// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database service (see SessionDbServer.h).

#include "SessionDbServer.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "DuckDbWrap.h"
#include "SessionDbLog.h"
#include "SessionDbSearch.h"
#include "SessionDbSummary.h"
#include "SessionDbTools.h"

#include "../ClaudeSpawn.h" // ClaudeProjectsDir, LogSwallowedException
#include "../Persistence.h" // LoadAppSettings
#include "../ProcessInspect.h" // SessionSummary
#include "../SessionDbProtocol.h"

namespace Agentmaster::SessionDb
{
    using json = nlohmann::json;
    namespace Proto = SessionDbProtocol;

    StoreConfig ServiceConfigFor(const std::wstring& profileDir)
    {
        StoreConfig cfg;
        cfg.profileDir = profileDir;
        cfg.projectsDir = ClaudeProjectsDir();
        // history.jsonl sits beside the projects folder in the Claude config folder.
        const size_t slash = cfg.projectsDir.find_last_of(L"\\/");
        cfg.historyPath = slash == std::wstring::npos ? std::wstring{} : cfg.projectsDir.substr(0, slash) + L"\\history.jsonl";
        const AppSettings settings = LoadAppSettings();
        cfg.inferGitRoot = settings.inferGitRoot;
        cfg.db.threads = std::to_string(ClampSessionDbThreads(settings.sessionDbThreads));
        cfg.db.memoryLimit = std::to_string(ClampSessionDbMemoryMb(settings.sessionDbMemoryMb)) + "MB";
        cfg.log = [](const std::wstring& line) { ServiceLog(line); };
        return cfg;
    }

    namespace
    {
        // Query connections in the pool (§9: one query connection per request, from a pool of 4).
        constexpr int kQueryWorkers = 4;
        // The deadline of a `sql` request and of every other query request (§9).
        constexpr int64_t kSqlDeadlineMs = 2500;
        constexpr int64_t kQueryDeadlineMs = 10000;
        // How long a worker waits for a client that stopped reading before it gives the write up.
        constexpr DWORD kWriteTimeoutMs = 30000;
        // Idle maintenance: history.jsonl at most this often; a re-cluster only after this long without a write job.
        constexpr int64_t kHistoryEveryMs = 60000;
        constexpr int64_t kReclusterIdleMs = 120000;
        // After a failed re-cluster the next try waits this long, doubling per failure up to the maximum; a
        // success (or a service restart) resets it.
        constexpr int64_t kReclusterBackoffFirstMs = 15ll * 60 * 1000;
        constexpr int64_t kReclusterBackoffMaxMs = 24ll * 60 * 60 * 1000;

        int64_t NowUnixMs()
        {
            FILETIME ft{};
            ::GetSystemTimeAsFileTime(&ft);
            const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            return static_cast<int64_t>(t / 10000ULL - 11644473600000ULL);
        }

        int64_t TickMs()
        {
            return static_cast<int64_t>(::GetTickCount64());
        }

        std::string Dump(const json& j)
        {
            return j.dump(-1, ' ', false, json::error_handler_t::replace);
        }

        // A handle closed when the last owner lets go.
        struct Handle
        {
            HANDLE h{ nullptr };
            explicit Handle(HANDLE v) :
                h(v)
            {
            }
            ~Handle()
            {
                if (h && h != INVALID_HANDLE_VALUE)
                {
                    ::CloseHandle(h);
                }
            }
            Handle(const Handle&) = delete;
            Handle& operator=(const Handle&) = delete;
        };

        // One connected client: its pipe, its write lock, its in-flight requests.
        struct Client
        {
            uint64_t id{ 0 };
            HANDLE pipe{ nullptr };
            HANDLE readEvent{ nullptr };
            HANDLE writeEvent{ nullptr };
            std::mutex writeMtx;
            std::atomic<bool> closed{ false };

            ~Client()
            {
                // The last owner (the reader thread, or a worker still answering) closes the pipe.
                if (pipe)
                {
                    ::DisconnectNamedPipe(pipe);
                    ::CloseHandle(pipe);
                }
                if (readEvent)
                {
                    ::CloseHandle(readEvent);
                }
                if (writeEvent)
                {
                    ::CloseHandle(writeEvent);
                }
            }
        };

        // A request in flight on a query worker: what `cancel` and the watchdog interrupt.
        struct InFlight
        {
            uint64_t clientId{ 0 };
            std::string reqId;
            int64_t deadlineTick{ 0 };
            std::mutex mtx; // guards con: set while the request's query may run
            Connection* con{ nullptr };
            std::atomic<bool> cancelled{ false }; // the client sent `cancel`
            std::atomic<bool> timedOut{ false }; // the watchdog stopped it at the deadline

            void Interrupt()
            {
                std::lock_guard lk{ mtx };
                if (con)
                {
                    con->Interrupt();
                }
            }
        };

        // The pipe's request id as the map key (a string or a number on the wire).
        std::string IdKey(const json& id)
        {
            return id.is_string() ? id.get<std::string>() : Dump(id);
        }

        class Service
        {
        public:
            Service(std::wstring profileDir, size_t sweepLimit) :
                _profile(std::move(profileDir)), _sweepLimit(sweepLimit)
            {
            }

            int Run();

        private:
            // --- lifecycle ---
            bool _openStore();
            void _writeDiscovery();
            void _requestStop(const wchar_t* why);
            bool _idleExitDue();

            // --- pipe I/O ---
            bool _readExact(Client& c, char* buf, DWORD n);
            bool _readFrame(Client& c, std::string& payload);
            bool _writeFrame(Client& c, const std::string& payload);
            void _respond(const std::shared_ptr<Client>& c, const json& id, json result);
            void _respondError(const std::shared_ptr<Client>& c, const json& id, int code, const std::string& message);
            void _clientLoop(std::shared_ptr<Client> c);

            // --- requests ---
            void _handle(const std::shared_ptr<Client>& c, const std::string& payload);
            void _postQuery(const std::shared_ptr<Client>& c, const json& id, std::string method, json params);
            void _postWrite(std::function<void()> job);
            json _runQuery(const std::string& method, const json& params, Connection& con, InFlight& f, int& errCode, std::string& errText);
            json _status(Connection& con);
            json _pageSearch(Connection& con, const json& params, InFlight& f);
            json _summary(Connection& con, const json& params, int& errCode, std::string& errText);
            json _rebuild(Connection& con, const json& params, int& errCode, std::string& errText);

            // --- threads ---
            void _queryWorker(int index);
            void _writerLoop();
            void _watchdog();
            void _maintenance();

            std::wstring _profile;
            size_t _sweepLimit{ 0 }; // > 0: every sweep takes only that many newest changed sessions (diagnostics)
            std::wstring _pipeName;
            std::unique_ptr<Store> _store;
            std::unique_ptr<SearchEngine> _search;

            HANDLE _stopEvent{ nullptr };
            std::atomic<bool> _stopping{ false };
            int64_t _startedMs{ 0 };

            // clients
            std::atomic<int> _clients{ 0 };
            std::atomic<int64_t> _lastClientGoneTick{ 0 };
            std::atomic<uint64_t> _nextClientId{ 1 };
            // Client reader threads are detached (one per connection, a CLI makes many); this counts the live
            // ones so a stop can wait for them before the service object goes away.
            std::atomic<int> _clientThreadsAlive{ 0 };

            // in-flight query requests
            std::mutex _inflightMtx;
            std::map<std::pair<uint64_t, std::string>, std::shared_ptr<InFlight>> _inflight;

            // the query pool
            std::mutex _queryMtx;
            std::condition_variable _queryCv;
            std::deque<std::function<void(Connection&)>> _queryJobs;
            std::vector<std::thread> _queryThreads;

            // the writer
            std::mutex _writeMtx;
            std::condition_variable _writeCv;
            std::deque<std::function<void()>> _writeJobs;
            std::thread _writer;
            std::atomic<bool> _writerBusy{ false };
            std::atomic<int64_t> _lastWriteJobTick{ 0 };
            int64_t _lastHistoryTick{ 0 };
            // The re-cluster's back-off (writer thread only): no try before _reclusterRetryTick; the current
            // wait, 0 after a success.
            int64_t _reclusterRetryTick{ 0 };
            int64_t _reclusterBackoffMs{ 0 };
            // The sweep: pending at start (the catch-up) and after a `resync`; it yields to waiting write jobs
            // and resumes (cursor-based, so a cut sweep loses nothing).
            std::atomic<bool> _sweepPending{ true };
            std::atomic<int64_t> _sweepDone{ 0 };
            std::atomic<int64_t> _sweepTotal{ 0 };
            std::atomic<int64_t> _lastSweepMs{ 0 };
            std::mutex _sweepWaitMtx;
            std::vector<std::pair<std::shared_ptr<Client>, json>> _sweepWaiters; // resync requests answered when the sweep completes
            ResyncResult _sweepTally; // what the current sweep (with its yields) has done so far

            std::thread _watchdogThread;
        };

        // ============================================================================================
        // lifecycle
        // ============================================================================================

        bool Service::_openStore()
        {
            _store = std::make_unique<Store>(ServiceConfigFor(_profile));
            try
            {
                if (_store->Open() == SchemaState::TooNew)
                {
                    ServiceLog(L"[sessdb] the database belongs to a newer service; this one stops without touching it");
                    return false;
                }
            }
            catch (const std::exception& e)
            {
                // §11: a file that does not open (corrupt, or written by a newer DuckDB) is set aside and rebuilt.
                LogSwallowedException(L"sessiondb open");
                ServiceLog(L"[sessdb] the database did not open (" + FromUtf8(e.what()) + L"); setting it aside and building a new one");
                _store.reset();
                SYSTEMTIME st{};
                ::GetLocalTime(&st);
                wchar_t stamp[32];
                swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                const std::wstring db = Proto::DatabasePath(_profile);
                ::MoveFileExW(db.c_str(), (db + L".broken-" + stamp).c_str(), MOVEFILE_REPLACE_EXISTING);
                ::MoveFileExW((db + L".wal").c_str(), (db + L".wal.broken-" + stamp).c_str(), MOVEFILE_REPLACE_EXISTING);
                _store = std::make_unique<Store>(ServiceConfigFor(_profile));
                if (_store->Open() == SchemaState::TooNew)
                {
                    return false;
                }
            }
            _search = std::make_unique<SearchEngine>(*_store);
            return true;
        }

        void Service::_writeDiscovery()
        {
            json j = { { "pipe", ToUtf8(_pipeName) },
                       { "pid", ::GetCurrentProcessId() },
                       { "build", Proto::kServiceBuild },
                       { "schema", Proto::kSchemaVersion },
                       { "protocol", Proto::kProtocolVersion },
                       { "tools", Proto::kToolRegistryVersion },
                       { "startedAt", LocalTime(_startedMs) } };
            const std::string text = j.dump(2);
            const std::wstring path = Proto::DiscoveryPath(_profile);
            const std::wstring tmp = path + L".tmp";
            HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return;
            }
            DWORD w = 0;
            ::WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &w, nullptr);
            ::CloseHandle(h);
            ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        void Service::_requestStop(const wchar_t* why)
        {
            if (!_stopping.exchange(true))
            {
                ServiceLog(std::wstring{ L"[sessdb] stopping: " } + why);
                ::SetEvent(_stopEvent);
                _queryCv.notify_all();
                _writeCv.notify_all();
            }
        }

        bool Service::_idleExitDue()
        {
            if (_clients.load() > 0 || _writerBusy.load() || _sweepPending.load())
            {
                return false;
            }
            {
                std::lock_guard lk{ _writeMtx };
                if (!_writeJobs.empty())
                {
                    return false;
                }
            }
            const int64_t gone = _lastClientGoneTick.load();
            return TickMs() - std::max(gone, _lastWriteJobTick.load()) >= Proto::kIdleExitMs;
        }

        // ============================================================================================
        // pipe I/O (overlapped, so a stop or a cancel never waits on a client)
        // ============================================================================================

        bool Service::_readExact(Client& c, char* buf, DWORD n)
        {
            DWORD got = 0;
            while (got < n)
            {
                OVERLAPPED ov{};
                ov.hEvent = c.readEvent;
                DWORD r = 0;
                if (!::ReadFile(c.pipe, buf + got, n - got, &r, &ov))
                {
                    const DWORD e = ::GetLastError();
                    if (e != ERROR_IO_PENDING)
                    {
                        return false; // the client went away (ERROR_BROKEN_PIPE) or the pipe failed
                    }
                    HANDLE waits[2]{ c.readEvent, _stopEvent };
                    const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                    if (w != WAIT_OBJECT_0)
                    {
                        ::CancelIoEx(c.pipe, &ov);
                        ::GetOverlappedResult(c.pipe, &ov, &r, TRUE);
                        return false;
                    }
                    if (!::GetOverlappedResult(c.pipe, &ov, &r, FALSE))
                    {
                        return false;
                    }
                }
                if (r == 0)
                {
                    return false;
                }
                got += r;
            }
            return true;
        }

        bool Service::_readFrame(Client& c, std::string& payload)
        {
            unsigned char header[4];
            if (!_readExact(c, reinterpret_cast<char*>(header), 4))
            {
                return false;
            }
            const uint32_t n = Proto::DecodeFrameLength(header);
            if (n > Proto::kMaxRequestBytes)
            {
                ServiceLog(L"[sessdb] client " + std::to_wstring(c.id) + L" sent a " + std::to_wstring(n) + L"-byte frame; dropping it");
                return false; // a well-formed client never sends one: the stream cannot be trusted any more
            }
            payload.resize(n);
            return n == 0 || _readExact(c, payload.data(), n);
        }

        bool Service::_writeFrame(Client& c, const std::string& payload)
        {
            std::lock_guard lk{ c.writeMtx };
            if (c.closed.load())
            {
                return false;
            }
            const std::string frame = Proto::EncodeFrame(payload);
            size_t done = 0;
            while (done < frame.size())
            {
                OVERLAPPED ov{};
                ov.hEvent = c.writeEvent;
                DWORD w = 0;
                const DWORD chunk = static_cast<DWORD>(std::min<size_t>(frame.size() - done, 1u << 20));
                if (!::WriteFile(c.pipe, frame.data() + done, chunk, &w, &ov))
                {
                    if (::GetLastError() != ERROR_IO_PENDING)
                    {
                        return false;
                    }
                    // A client that stopped reading must not hold a worker forever.
                    if (::WaitForSingleObject(c.writeEvent, kWriteTimeoutMs) != WAIT_OBJECT_0)
                    {
                        ::CancelIoEx(c.pipe, &ov);
                        ::GetOverlappedResult(c.pipe, &ov, &w, TRUE);
                        return false;
                    }
                    if (!::GetOverlappedResult(c.pipe, &ov, &w, FALSE))
                    {
                        return false;
                    }
                }
                done += w;
            }
            return true;
        }

        void Service::_respond(const std::shared_ptr<Client>& c, const json& id, json result)
        {
            json env = { { "id", id }, { "result", std::move(result) } };
            std::string text = Dump(env);
            if (text.size() > Proto::kMaxResponseBytes)
            {
                // §9: a response is cut at 16 MB (the tools cap their own results far below).
                env = { { "id", id }, { "error", { { "code", Proto::kErrInternal }, { "message", "The response was larger than 16 MB." } } } };
                text = Dump(env);
            }
            _writeFrame(*c, text);
        }

        void Service::_respondError(const std::shared_ptr<Client>& c, const json& id, int code, const std::string& message)
        {
            _writeFrame(*c, Dump({ { "id", id }, { "error", { { "code", code }, { "message", message } } } }));
        }

        void Service::_clientLoop(std::shared_ptr<Client> c)
        {
            ++_clients;
            std::string payload;
            while (!_stopping.load() && _readFrame(*c, payload))
            {
                try
                {
                    _handle(c, payload);
                }
                catch (...)
                {
                    LogSwallowedException(L"sessiondb request");
                    _respondError(c, nullptr, Proto::kErrInternal, "The request failed inside the service.");
                }
            }
            // The client went away: stop its in-flight queries (no one waits for them).
            c->closed.store(true);
            {
                std::lock_guard lk{ _inflightMtx };
                for (auto& [key, f] : _inflight)
                {
                    if (key.first == c->id)
                    {
                        f->cancelled.store(true);
                        f->Interrupt();
                    }
                }
            }
            _lastClientGoneTick.store(TickMs());
            --_clients;
        }

        // ============================================================================================
        // requests
        // ============================================================================================

        bool IsQueryMethod(std::string_view m)
        {
            return m == Proto::kStatus || m == Proto::kListSessions || m == Proto::kSearch || m == Proto::kPageSearch || m == Proto::kSessionDetail ||
                   m == Proto::kSessionSummary || m == Proto::kResolveIds ||
                   m == Proto::kMessages || m == Proto::kGrep || m == Proto::kFiles || m == Proto::kWhoTouched || m == Proto::kTimeline || m == Proto::kUsage ||
                   m == Proto::kRelated || m == Proto::kSql || m == Proto::kToolCall;
        }

        // The protocol method of a tool contract (§9: each tool maps to one method).
        const char* ToolOfMethod(std::string_view m)
        {
            if (m == Proto::kSearch || m == Proto::kListSessions)
            {
                return "search";
            }
            if (m == Proto::kSessionDetail)
            {
                return "session";
            }
            if (m == Proto::kMessages)
            {
                return "messages";
            }
            if (m == Proto::kGrep)
            {
                return "grep";
            }
            if (m == Proto::kFiles)
            {
                return "files";
            }
            if (m == Proto::kWhoTouched)
            {
                return "who_touched";
            }
            if (m == Proto::kTimeline)
            {
                return "timeline";
            }
            if (m == Proto::kUsage)
            {
                return "usage";
            }
            if (m == Proto::kRelated)
            {
                return "related";
            }
            if (m == Proto::kSql)
            {
                return "sql";
            }
            return nullptr;
        }

        void Service::_handle(const std::shared_ptr<Client>& c, const std::string& payload)
        {
            const json req = json::parse(payload, nullptr, false);
            if (req.is_discarded() || !req.is_object() || !req.contains("method") || !req["method"].is_string())
            {
                _respondError(c, req.is_object() && req.contains("id") ? req["id"] : json(nullptr), Proto::kErrBadRequest,
                              "A request is {v, id, method, params} as JSON.");
                return;
            }
            const json id = req.contains("id") ? req["id"] : json(nullptr);
            const std::string method = req["method"].get<std::string>();
            const json params = req.contains("params") && req["params"].is_object() ? req["params"] : json::object();
            if (req.contains("v") && req["v"].is_number_integer() && req["v"].get<int>() != Proto::kProtocolVersion)
            {
                _respondError(c, id, Proto::kErrBadRequest,
                              "Protocol version " + std::to_string(req["v"].get<int>()) + " is not " + std::to_string(Proto::kProtocolVersion) + ".");
                return;
            }

            if (method == Proto::kCancel)
            {
                // No answer: the cancelled request answers 499 itself.
                const std::string target = params.contains("id") ? IdKey(params["id"]) : std::string{};
                std::lock_guard lk{ _inflightMtx };
                const auto it = _inflight.find({ c->id, target });
                if (it != _inflight.end())
                {
                    it->second->cancelled.store(true);
                    it->second->Interrupt();
                }
                return;
            }
            if (method == Proto::kHello)
            {
                _respond(c, id,
                         { { "service", Proto::kServiceBuild },
                           { "schema", Proto::kSchemaVersion },
                           { "protocol", Proto::kProtocolVersion },
                           { "tools", Proto::kToolRegistryVersion },
                           { "pid", ::GetCurrentProcessId() },
                           { "duckdb", duckdb_library_version() },
                           { "startedAt", LocalTime(_startedMs) },
                           { "profile", ToUtf8(_profile) } });
                return;
            }
            if (method == Proto::kToolList)
            {
                _respond(c, id, { { "tools", ToolDefinitionsJson() }, { "version", Proto::kToolRegistryVersion } });
                return;
            }
            if (method == Proto::kShutdown)
            {
                _respond(c, id, { { "stopping", true } });
                _requestStop(L"a client asked (shutdown)");
                return;
            }
            if (IsQueryMethod(method))
            {
                _postQuery(c, id, method, params);
                return;
            }

            // --- the writer's requests ---
            if (method == Proto::kUpsertSessions)
            {
                if (!params.contains("sessions") || !params["sessions"].is_array())
                {
                    _respondError(c, id, Proto::kErrBadRequest, "upsertSessions takes {sessions: [{id, path}]}.");
                    return;
                }
                std::vector<SessionRef> refs;
                for (const json& s : params["sessions"])
                {
                    if (s.is_object() && s.contains("id") && s["id"].is_string())
                    {
                        refs.push_back(SessionRef{ FromUtf8(s["id"].get<std::string>()),
                                                   s.contains("path") && s["path"].is_string() ? FromUtf8(s["path"].get<std::string>()) : std::wstring{} });
                    }
                }
                _postWrite([this, c, id, refs = std::move(refs)]() {
                    json out = json::array();
                    try
                    {
                        const auto results = _store->UpsertSessions(refs, [this] { return _stopping.load(); });
                        for (const auto& r : results)
                        {
                            json j = { { "id", ToUtf8(r.id) }, { "cursor", r.cursor }, { "lines", r.lines }, { "ok", r.ok } };
                            if (r.missing)
                            {
                                j["missing"] = true;
                            }
                            if (!r.ok)
                            {
                                j["error"] = ToUtf8(r.error);
                            }
                            out.push_back(std::move(j));
                        }
                        _respond(c, id, { { "sessions", out } });
                    }
                    catch (const std::exception& e)
                    {
                        LogSwallowedException(L"sessiondb upsertSessions");
                        _respondError(c, id, Proto::kErrInternal, std::string{ "upsertSessions failed: " } + e.what());
                    }
                });
                return;
            }
            if (method == Proto::kResync)
            {
                // Answered when the sweep completes (it yields to the refresh tick's upserts meanwhile).
                {
                    std::lock_guard lk{ _sweepWaitMtx };
                    _sweepWaiters.emplace_back(c, id);
                }
                _sweepPending.store(true);
                _writeCv.notify_all();
                return;
            }
            if (method == Proto::kFactsChanged)
            {
                _postWrite([this, c, id]() {
                    try
                    {
                        _store->RefreshFacts();
                        _search->Invalidate();
                        _respond(c, id, json::object());
                    }
                    catch (const std::exception& e)
                    {
                        LogSwallowedException(L"sessiondb factsChanged");
                        _respondError(c, id, Proto::kErrInternal, std::string{ "factsChanged failed: " } + e.what());
                    }
                });
                return;
            }
            if (method == Proto::kOpenChanged)
            {
                std::vector<std::wstring> ids;
                if (params.contains("ids") && params["ids"].is_array())
                {
                    for (const json& v : params["ids"])
                    {
                        if (v.is_string())
                        {
                            ids.push_back(FromUtf8(v.get<std::string>()));
                        }
                    }
                }
                _postWrite([this, c, id, ids = std::move(ids)]() {
                    try
                    {
                        _store->SetOpenSessions(ids);
                        _respond(c, id, json::object());
                    }
                    catch (const std::exception& e)
                    {
                        LogSwallowedException(L"sessiondb openChanged");
                        _respondError(c, id, Proto::kErrInternal, std::string{ "openChanged failed: " } + e.what());
                    }
                });
                return;
            }
            if (method == Proto::kRebuild)
            {
                if (params.value("index", false))
                {
                    // The whole index, re-clustered (merge first: the hot tier joins the rewrite).
                    _postWrite([this, c, id]() {
                        try
                        {
                            const auto t0 = TickMs();
                            _store->Merge([this] { return _stopping.load(); });
                            _store->Recluster([this] { return _stopping.load(); });
                            _respond(c, id, { { "reclustered", true }, { "ms", TickMs() - t0 } });
                        }
                        catch (const std::exception& e)
                        {
                            LogSwallowedException(L"sessiondb rebuild index");
                            _respondError(c, id, Proto::kErrInternal, std::string{ "rebuild failed: " } + e.what());
                        }
                    });
                    return;
                }
                _postQuery(c, id, method, params); // a transcript from the lossless store: a read
                return;
            }
            _respondError(c, id, Proto::kErrBadRequest, "Unknown method \"" + method + "\".");
        }

        void Service::_postQuery(const std::shared_ptr<Client>& c, const json& id, std::string method, json params)
        {
            auto f = std::make_shared<InFlight>();
            f->clientId = c->id;
            f->reqId = IdKey(id);
            const bool sql = method == Proto::kSql || (method == Proto::kToolCall && params.value("name", std::string{}) == "sql");
            f->deadlineTick = TickMs() + (sql ? kSqlDeadlineMs : kQueryDeadlineMs);
            {
                std::lock_guard lk{ _inflightMtx };
                _inflight[{ c->id, f->reqId }] = f;
            }
            {
                std::lock_guard lk{ _queryMtx };
                _queryJobs.emplace_back([this, c, id, f, method = std::move(method), params = std::move(params)](Connection& con) {
                    int code = 0;
                    std::string message;
                    json result;
                    if (!f->cancelled.load() && !c->closed.load())
                    {
                        {
                            std::lock_guard lk{ f->mtx };
                            f->con = &con;
                        }
                        try
                        {
                            result = _runQuery(method, params, con, *f, code, message);
                        }
                        catch (const DbError& e)
                        {
                            const std::string what = e.what();
                            code = what.find("nterrupt") != std::string::npos ? Proto::kErrTimeout : Proto::kErrInternal;
                            message = what;
                            if (code == Proto::kErrInternal)
                            {
                                LogSwallowedException(L"sessiondb query");
                            }
                        }
                        catch (const std::exception& e)
                        {
                            LogSwallowedException(L"sessiondb query");
                            code = Proto::kErrInternal;
                            message = e.what();
                        }
                        {
                            std::lock_guard lk{ f->mtx };
                            f->con = nullptr;
                        }
                    }
                    else
                    {
                        code = Proto::kErrCancelled;
                    }
                    // An interrupt says why it happened: the client's cancel (499) or the deadline (408).
                    if (f->cancelled.load())
                    {
                        code = Proto::kErrCancelled;
                        message = "Cancelled.";
                    }
                    else if (f->timedOut.load() && code != 0)
                    {
                        code = Proto::kErrTimeout;
                        message = "The request ran past its time budget and was stopped.";
                    }
                    {
                        std::lock_guard lk{ _inflightMtx };
                        _inflight.erase({ f->clientId, f->reqId });
                    }
                    if (code != 0)
                    {
                        _respondError(c, id, code, message);
                    }
                    else
                    {
                        _respond(c, id, std::move(result));
                    }
                });
            }
            _queryCv.notify_one();
        }

        void Service::_postWrite(std::function<void()> job)
        {
            {
                std::lock_guard lk{ _writeMtx };
                _writeJobs.push_back(std::move(job));
            }
            _writeCv.notify_all();
        }

        json Service::_runQuery(const std::string& method, const json& params, Connection& con, InFlight& f, int& errCode, std::string& errText)
        {
            if (method == Proto::kStatus)
            {
                return _status(con);
            }
            if (method == Proto::kSessionSummary)
            {
                return _summary(con, params, errCode, errText);
            }
            if (method == Proto::kRebuild)
            {
                return _rebuild(con, params, errCode, errText);
            }
            if (method == Proto::kPageSearch)
            {
                return _pageSearch(con, params, f);
            }
            if (method == Proto::kResolveIds)
            {
                // The maps answer it (no query): every id the request names, mapped to its whole id or null.
                if (!params.contains("ids") || !params["ids"].is_array() || params["ids"].size() > 200)
                {
                    errCode = Proto::kErrBadRequest;
                    errText = "resolveIds takes {ids: [up to 200 short ids or UUIDs]}.";
                    return nullptr;
                }
                json ids = json::object();
                for (const auto& v : params["ids"])
                {
                    if (!v.is_string())
                    {
                        continue;
                    }
                    const std::string text = v.get<std::string>();
                    int matches = 0;
                    const auto key = _store->ResolveSession(FromUtf8(text), matches);
                    ids[text] = key ? json(ToUtf8(key->sessionId)) : json(nullptr);
                }
                return { { "ids", std::move(ids) } };
            }
            ToolContext ctx{ *_store, *_search, con, [&f] { return f.cancelled.load() || f.timedOut.load(); }, NowUnixMs() };
            std::string tool;
            json args = params;
            if (method == Proto::kToolCall)
            {
                // MCP's tools/call: the result is the tool's text and its error flag (the MCP side wraps it).
                tool = params.value("name", std::string{});
                args = params.contains("arguments") ? params["arguments"] : json::object();
                const ToolOutcome o = CallTool(ctx, tool, args);
                if (f.cancelled.load())
                {
                    errCode = Proto::kErrCancelled;
                    errText = "Cancelled.";
                    return nullptr;
                }
                return { { "isError", o.isError }, { "text", o.text } };
            }
            tool = ToolOfMethod(method);
            if (method == Proto::kListSessions && !args.contains("sort"))
            {
                args["sort"] = "active"; // a listing (no query) reads newest first
            }
            ToolOutcome o = CallTool(ctx, tool, args);
            if (o.isError)
            {
                errCode = o.code ? o.code : Proto::kErrInternal;
                errText = o.text;
                return nullptr;
            }
            return std::move(o.result);
        }

        json Service::_status(Connection& con)
        {
            const StoreStatus st = _store->Status(con);
            json j = { { "service", Proto::kServiceBuild },
                       { "schema", Proto::kSchemaVersion },
                       { "pid", ::GetCurrentProcessId() },
                       { "startedAt", LocalTime(_startedMs) },
                       { "uptimeMs", NowUnixMs() - _startedMs },
                       { "clients", _clients.load() },
                       { "sessions", st.sessions },
                       { "files", st.files },
                       { "lines", st.lines },
                       { "hotLines", st.hotLines },
                       { "blocks", st.docs },
                       { "hotBlocks", st.hotDocs },
                       { "terms", st.terms },
                       { "dbBytes", st.dbBytes },
                       { "lastWrite", LocalTime(st.lastWriteMs) },
                       { "coldBuildDone", st.coldBuildDone },
                       { "sweeping", _sweepPending.load() },
                       { "lastSweep", LocalTime(_lastSweepMs.load()) } };
            if (_sweepPending.load() && _sweepTotal.load() > 0)
            {
                j["sweepProgress"] = { { "done", _sweepDone.load() }, { "total", _sweepTotal.load() } };
            }
            if (!st.coldBuildDone && st.coveredSinceMs > 0)
            {
                j["coveredSince"] = LocalTime(st.coveredSinceMs);
            }
            {
                std::lock_guard lk{ _writeMtx };
                j["writeQueue"] = _writeJobs.size();
            }
            return j;
        }

        json Service::_pageSearch(Connection& con, const json& params, InFlight& f)
        {
            // The page's own request: its scope switches, its window (Unix ms), and how much to return.
            SearchRequest req;
            req.query = FromUtf8(params.value("query", std::string{}));
            const json scopes = params.contains("scopes") && params["scopes"].is_object() ? params["scopes"] : json::object();
            req.scopeTitle = scopes.value("title", true);
            req.scopeDirs = scopes.value("dirs", true);
            req.scopeFiles = scopes.value("files", true);
            req.scopePrompt = scopes.value("prompt", true);
            req.scopeReply = scopes.value("reply", false);
            req.scopeTool = scopes.value("tool", false);
            req.scopeSubagent = scopes.value("subagent", false);
            req.fuzzy = params.value("fuzzy", false);
            req.afterMs = params.value("afterMs", static_cast<int64_t>(0));
            req.beforeMs = params.value("beforeMs", static_cast<int64_t>(0));
            req.limit = static_cast<size_t>(std::clamp<int64_t>(params.value("limit", static_cast<int64_t>(2000)), 1, 5000));
            req.snippetsPerRow = static_cast<size_t>(std::clamp<int64_t>(params.value("snippets", static_cast<int64_t>(3)), 0, 6));
            req.snippetChars = 160;
            req.cancel = [&f] { return f.cancelled.load() || f.timedOut.load(); };
            const SearchResult r = _search->Search(con, req);
            json rows = json::array();
            for (const SearchRow& row : r.rows)
            {
                const SessionCard& c = r.cards->cards[r.cards->bySid.at(row.sidKey)];
                json snips = json::array();
                for (const SearchSnippet& s : row.snippets)
                {
                    snips.push_back({ { "role", s.role }, { "history", s.history }, { "text", ToUtf8(s.text) } });
                }
                rows.push_back({ { "id", ToUtf8(c.sessionId) }, { "tier", row.tier }, { "hits", row.hits }, { "score", row.score }, { "snippets", std::move(snips) } });
            }
            json out = { { "matched", r.matched }, { "searched", r.searched }, { "rows", std::move(rows) }, { "timings", ToUtf8(r.timings) } };
            if (r.partial)
            {
                out["partial"] = true;
            }
            if (!r.note.empty())
            {
                out["note"] = ToUtf8(r.note);
            }
            return out;
        }

        json Service::_summary(Connection& con, const json& params, int& errCode, std::string& errText)
        {
            const std::string idText = params.value("id", std::string{});
            int matches = 0;
            auto key = _store->ResolveSession(FromUtf8(idText), matches);
            if (!key)
            {
                errCode = matches > 1 ? Proto::kErrConflict : Proto::kErrNotFound;
                errText = matches > 1 ? "\"" + idText + "\" is shared by " + std::to_string(matches) + " sessions." : "Unknown session \"" + idText + "\".";
                return nullptr;
            }
            const auto toJson = [](const std::wstring& shortId, const std::wstring& uuid, const SessionSummary& s) {
                const auto list = [](const std::vector<std::wstring>& v) {
                    json a = json::array();
                    for (const auto& x : v)
                    {
                        a.push_back(ToUtf8(x));
                    }
                    return a;
                };
                json segs = json::array();
                for (const auto& seg : s.previousSegments)
                {
                    segs.push_back({ { "label", ToUtf8(seg.label) }, { "prompts", list(seg.userMsgs) } });
                }
                return json{ { "id", ToUtf8(shortId) },
                             { "uuid", ToUtf8(uuid) },
                             { "found", s.found },
                             { "prompts", list(s.userMsgs) },
                             { "previousSegments", segs },
                             { "compacted", s.compacted },
                             { "filesRead", list(s.filesRead) },
                             { "filesCreated", list(s.filesCreated) },
                             { "filesEdited", list(s.filesEdited) },
                             { "skills", list(s.skillsLoaded) },
                             { "branch", ToUtf8(s.branch) },
                             { "recap", ToUtf8(s.awaySummary) },
                             { "first", ToUtf8(s.firstTs) },
                             { "last", ToUtf8(s.lastTs) },
                             { "lastUser", ToUtf8(s.lastUserTs) },
                             { "tasks", { { "done", s.tasksCompleted }, { "pending", s.tasksPending } } },
                             { "plan", { { "exit", s.hasExitPlanMode }, { "content", s.hasPlanContent }, { "file", ToUtf8(s.planFilePath) }, { "filesRead", list(s.planFilesRead) } } },
                             { "parent", ToUtf8(s.parentSessionId) } };
            };
            SessionSummary s;
            LoadSummary(con, key->sidKey, s);
            json out = toJson(key->shortId, key->sessionId, s);
            if (params.value("chain", false))
            {
                // The sessions it continues (fork parent, plan parent, /clear and /handover links), upstream only.
                json chain = json::array();
                std::wstring next = key->sessionId;
                for (int depth = 0; depth < 16; ++depth)
                {
                    auto p = con.Prepare("SELECT coalesce(s.forked_from, ''), coalesce(sm.parent, ''), coalesce((SELECT from_id FROM am_link WHERE session_id = s.session_id LIMIT 1), '') "
                                         "FROM session s LEFT JOIN summary sm USING (sid_key) WHERE s.session_id = $1");
                    p.Bind(1, std::string_view{ ToUtf8(next) });
                    auto r = p.Execute();
                    if (!r.Next())
                    {
                        break;
                    }
                    std::string up = r.String(0);
                    up = up.empty() ? r.String(1) : up;
                    up = up.empty() ? r.String(2) : up;
                    if (up.empty())
                    {
                        break;
                    }
                    int m = 0;
                    const auto upKey = _store->ResolveSession(FromUtf8(up), m);
                    if (!upKey)
                    {
                        chain.push_back({ { "uuid", up }, { "found", false } });
                        break;
                    }
                    SessionSummary us;
                    LoadSummary(con, upKey->sidKey, us);
                    chain.push_back(toJson(upKey->shortId, upKey->sessionId, us));
                    next = upKey->sessionId;
                }
                out["chain"] = chain;
            }
            return out;
        }

        json Service::_rebuild(Connection& con, const json& params, int& errCode, std::string& errText)
        {
            // A transcript from the lossless store, byte for byte (§5.5): its lines in order, each followed by '\n'.
            // Paged: one answer carries about 7 MB of raw lines (JSON escaping can nearly double that, and an
            // answer must stay under 16 MB); `next_line` continues at the first line it did not carry.
            constexpr size_t kPageBytes = 7u * 1024u * 1024u;
            const std::string idText = params.value("id", std::string{});
            int matches = 0;
            auto key = _store->ResolveSession(FromUtf8(idText), matches);
            if (!key)
            {
                errCode = matches > 1 ? Proto::kErrConflict : Proto::kErrNotFound;
                errText = "Unknown session \"" + idText + "\".";
                return nullptr;
            }
            const int64_t fromLine = std::max<int64_t>(1, params.value("from_line", static_cast<int64_t>(1)));
            const int64_t lo = (static_cast<int64_t>(key->fileId) << 32) | fromLine;
            const int64_t hi = (static_cast<int64_t>(key->fileId) << 32) | 0xFFFFFFFFLL;
            auto p = con.Prepare("SELECT line_no, raw, raw_bin FROM (SELECT line_id, line_no, raw, raw_bin FROM line WHERE line_id BETWEEN $1 AND $2 "
                                 "UNION ALL SELECT line_id, line_no, raw, raw_bin FROM line_hot WHERE line_id BETWEEN $1 AND $2) ORDER BY line_id");
            p.Bind(1, lo).Bind(2, hi);
            auto r = p.Execute();
            std::string text;
            int64_t lines = 0;
            int64_t nextLine = 0;
            bool binary = false;
            while (r.Next())
            {
                // raw holds a UTF-8 line; raw_bin holds the bytes of one that is not valid UTF-8 (JSON then
                // carries it with replacement characters, so such a page is marked `binary`).
                const bool bin = r.IsNull(1);
                const std::string_view line = bin ? r.Str(2) : r.Str(1);
                if (!text.empty() && text.size() + line.size() + 1 > kPageBytes)
                {
                    nextLine = r.Int64(0);
                    break;
                }
                binary = binary || bin;
                text.append(line);
                text.push_back('\n');
                ++lines;
            }
            json out = { { "id", ToUtf8(key->shortId) }, { "from_line", fromLine }, { "lines", lines }, { "bytes", text.size() }, { "text", std::move(text) } };
            if (nextLine)
            {
                out["next_line"] = nextLine;
            }
            if (binary)
            {
                out["binary"] = true;
            }
            return out;
        }

        // ============================================================================================
        // threads
        // ============================================================================================

        void Service::_queryWorker(int index)
        {
            ::SetThreadDescription(::GetCurrentThread(), (L"sessiondb query " + std::to_wstring(index)).c_str());
            std::unique_ptr<Connection> con;
            try
            {
                con = std::make_unique<Connection>(_store->Db());
            }
            catch (const std::exception& e)
            {
                LogSwallowedException(L"sessiondb query worker connect");
                ServiceLog(L"[sessdb] query worker " + std::to_wstring(index) + L" could not connect: " + FromUtf8(e.what()));
                return;
            }
            for (;;)
            {
                std::function<void(Connection&)> job;
                {
                    std::unique_lock lk{ _queryMtx };
                    _queryCv.wait(lk, [this] { return _stopping.load() || !_queryJobs.empty(); });
                    if (_queryJobs.empty())
                    {
                        return; // stopping, nothing left
                    }
                    job = std::move(_queryJobs.front());
                    _queryJobs.pop_front();
                }
                try
                {
                    job(*con);
                }
                catch (...)
                {
                    LogSwallowedException(L"sessiondb query job");
                }
            }
        }

        void Service::_watchdog()
        {
            ::SetThreadDescription(::GetCurrentThread(), L"sessiondb watchdog");
            while (::WaitForSingleObject(_stopEvent, 50) == WAIT_TIMEOUT)
            {
                const int64_t now = TickMs();
                std::lock_guard lk{ _inflightMtx };
                for (auto& [key, f] : _inflight)
                {
                    if (now > f->deadlineTick && !f->timedOut.load())
                    {
                        f->timedOut.store(true);
                        f->Interrupt();
                    }
                }
            }
            // Stopping: interrupt whatever still runs so the workers can finish.
            std::lock_guard lk{ _inflightMtx };
            for (auto& [key, f] : _inflight)
            {
                f->cancelled.store(true);
                f->Interrupt();
            }
        }

        void Service::_maintenance()
        {
            // Background priority (§10): lower CPU and I/O priority while maintenance runs.
            ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
            const auto yield = [this] {
                if (_stopping.load())
                {
                    return true;
                }
                std::lock_guard lk{ _writeMtx };
                return !_writeJobs.empty(); // a waiting request goes first; the maintenance resumes later
            };
            try
            {
                if (_store->MergeDue())
                {
                    _store->Merge(yield);
                }
                if (TickMs() - _lastHistoryTick >= kHistoryEveryMs)
                {
                    _lastHistoryTick = TickMs();
                    _store->IngestHistory();
                }
            }
            catch (const std::exception& e)
            {
                LogSwallowedException(L"sessiondb maintenance");
                ServiceLog(L"[sessdb] maintenance failed: " + FromUtf8(e.what()));
            }
            // The re-cluster on its own, behind a back-off: a failure (out of memory, a full disk) leaves it due,
            // and without the back-off every maintenance tick retried it (2026-10-15: every ~20 s, each try a
            // full read of the database, for as long as the service ran).
            if (TickMs() >= _reclusterRetryTick && TickMs() - _lastWriteJobTick.load() >= kReclusterIdleMs && !yield())
            {
                try
                {
                    if (_store->ReclusterDue())
                    {
                        _store->Merge(yield);
                        _store->Recluster(yield);
                    }
                    _reclusterBackoffMs = 0;
                }
                catch (const std::exception& e)
                {
                    _reclusterBackoffMs = _reclusterBackoffMs == 0 ? kReclusterBackoffFirstMs : std::min(_reclusterBackoffMs * 2, kReclusterBackoffMaxMs);
                    _reclusterRetryTick = TickMs() + _reclusterBackoffMs;
                    LogSwallowedException(L"sessiondb re-cluster");
                    ServiceLog(L"[sessdb] re-cluster failed (next try in " + std::to_wstring(_reclusterBackoffMs / 60000) + L" min): " + FromUtf8(e.what()));
                }
            }
            ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        }

        void Service::_writerLoop()
        {
            ::SetThreadDescription(::GetCurrentThread(), L"sessiondb writer");
            // Agentmaster's own facts and history.jsonl first: cheap, and the search's names need them.
            try
            {
                _store->RefreshFacts();
                _store->IngestHistory();
                _lastHistoryTick = TickMs();
            }
            catch (const std::exception& e)
            {
                LogSwallowedException(L"sessiondb startup facts");
                ServiceLog(L"[sessdb] startup facts failed: " + FromUtf8(e.what()));
            }
            while (!_stopping.load())
            {
                std::function<void()> job;
                {
                    std::unique_lock lk{ _writeMtx };
                    if (_writeJobs.empty() && !_sweepPending.load())
                    {
                        _writeCv.wait_for(lk, std::chrono::seconds(2), [this] { return _stopping.load() || !_writeJobs.empty() || _sweepPending.load(); });
                    }
                    if (!_writeJobs.empty())
                    {
                        job = std::move(_writeJobs.front());
                        _writeJobs.pop_front();
                    }
                }
                if (_stopping.load())
                {
                    break;
                }
                if (job)
                {
                    _writerBusy.store(true);
                    try
                    {
                        job();
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"sessiondb write job");
                    }
                    _lastWriteJobTick.store(TickMs());
                    _writerBusy.store(false);
                    continue;
                }
                if (_sweepPending.load())
                {
                    // The sweep (the start's catch-up, or a resync): it yields to waiting write jobs (the refresh
                    // tick's upserts) and resumes from the cursors, so a cut sweep loses nothing.
                    _writerBusy.store(true);
                    ResyncResult r;
                    try
                    {
                        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
                        r = _store->Resync(
                            [this] {
                                if (_stopping.load())
                                {
                                    return true;
                                }
                                std::lock_guard lk{ _writeMtx };
                                return !_writeJobs.empty();
                            },
                            [this](int64_t done, int64_t total) {
                                _sweepDone.store(done);
                                _sweepTotal.store(total);
                            },
                            _sweepLimit);
                        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
                        if (_store->MergeDue())
                        {
                            _store->Merge([this] { return _stopping.load(); });
                        }
                    }
                    catch (const std::exception& e)
                    {
                        ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
                        LogSwallowedException(L"sessiondb sweep");
                        ServiceLog(L"[sessdb] sweep failed: " + FromUtf8(e.what()));
                        r.cancelled = true;
                        // A failing sweep is retried later, not in a hot loop.
                        std::unique_lock lk{ _writeMtx };
                        _writeCv.wait_for(lk, std::chrono::seconds(30), [this] { return _stopping.load() || !_writeJobs.empty(); });
                    }
                    _sweepTally.filesSeen = r.filesSeen;
                    _sweepTally.filesChanged += r.filesChanged;
                    _sweepTally.sessionsUpserted += r.sessionsUpserted;
                    _sweepTally.linesIngested += r.linesIngested;
                    _sweepTally.bytesRead += r.bytesRead;
                    _sweepTally.ms += r.ms;
                    _writerBusy.store(false);
                    if (!r.cancelled)
                    {
                        _sweepPending.store(false);
                        _lastSweepMs.store(NowUnixMs());
                        const json result = { { "filesSeen", _sweepTally.filesSeen },
                                              { "filesChanged", _sweepTally.filesChanged },
                                              { "sessionsUpserted", _sweepTally.sessionsUpserted },
                                              { "linesIngested", _sweepTally.linesIngested },
                                              { "bytesRead", _sweepTally.bytesRead },
                                              { "ms", _sweepTally.ms } };
                        _sweepTally = ResyncResult{};
                        std::vector<std::pair<std::shared_ptr<Client>, json>> waiters;
                        {
                            std::lock_guard lk{ _sweepWaitMtx };
                            waiters.swap(_sweepWaiters);
                        }
                        for (auto& [c, id] : waiters)
                        {
                            _respond(c, id, result);
                        }
                    }
                    continue;
                }
                _maintenance();
            }
            // Stopping: answer the resync waiters (the sweep did not finish).
            std::lock_guard lk{ _sweepWaitMtx };
            for (auto& [c, id] : _sweepWaiters)
            {
                _respondError(c, id, Proto::kErrUnavailable, "The service is stopping.");
            }
            _sweepWaiters.clear();
        }

        // ============================================================================================
        // Run
        // ============================================================================================

        int Service::Run()
        {
            _startedMs = NowUnixMs();
            _pipeName = Proto::PipeName(_profile);
            // ONE service per profile (§4.2).
            const Handle mutex{ ::CreateMutexW(nullptr, TRUE, Proto::MutexName(_profile).c_str()) };
            if (!mutex.h || ::GetLastError() == ERROR_ALREADY_EXISTS)
            {
                ServiceLog(L"[sessdb] another service owns this profile; exiting");
                return 0;
            }
            if (!_openStore())
            {
                return 3;
            }
            _stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            const Handle stopEvent{ _stopEvent };
            _writeDiscovery();
            ServiceLog(L"[sessdb] service " + std::to_wstring(Proto::kServiceBuild) + L" serving " + _pipeName + L" (pid " + std::to_wstring(::GetCurrentProcessId()) +
                       L", duckdb " + FromUtf8(duckdb_library_version()) + L")");

            _writer = std::thread([this] { _writerLoop(); });
            for (int i = 0; i < kQueryWorkers; ++i)
            {
                _queryThreads.emplace_back([this, i] { _queryWorker(i); });
            }
            _watchdogThread = std::thread([this] { _watchdog(); });
            _lastClientGoneTick.store(TickMs());

            // The accept loop: one pipe instance waits for the next client while the others are served.
            const Handle connectEvent{ ::CreateEventW(nullptr, TRUE, FALSE, nullptr) };
            bool first = true;
            while (!_stopping.load())
            {
                HANDLE pipe = ::CreateNamedPipeW(_pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 64 * 1024,
                                                 64 * 1024, 0, nullptr);
                if (pipe == INVALID_HANDLE_VALUE)
                {
                    const DWORD e = ::GetLastError();
                    ServiceLog(L"[sessdb] could not create the pipe (" + std::to_wstring(e) + L")");
                    if (first)
                    {
                        _requestStop(L"the pipe name is taken");
                        break;
                    }
                    ::WaitForSingleObject(_stopEvent, 1000);
                    continue;
                }
                first = false;
                OVERLAPPED ov{};
                ov.hEvent = connectEvent.h;
                ::ResetEvent(connectEvent.h);
                bool connected = false;
                if (::ConnectNamedPipe(pipe, &ov))
                {
                    connected = true;
                }
                else
                {
                    const DWORD e = ::GetLastError();
                    if (e == ERROR_PIPE_CONNECTED)
                    {
                        connected = true;
                    }
                    else if (e == ERROR_IO_PENDING)
                    {
                        HANDLE waits[2]{ connectEvent.h, _stopEvent };
                        for (;;)
                        {
                            const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, 1000);
                            if (w == WAIT_OBJECT_0)
                            {
                                DWORD n = 0;
                                connected = ::GetOverlappedResult(pipe, &ov, &n, FALSE) != 0;
                                break;
                            }
                            if (w == WAIT_OBJECT_0 + 1)
                            {
                                ::CancelIoEx(pipe, &ov);
                                DWORD n = 0;
                                ::GetOverlappedResult(pipe, &ov, &n, TRUE);
                                break;
                            }
                            if (_idleExitDue())
                            {
                                _requestStop(L"no client for 10 minutes");
                            }
                        }
                    }
                }
                if (!connected)
                {
                    ::CloseHandle(pipe);
                    continue;
                }
                auto c = std::make_shared<Client>();
                c->id = _nextClientId++;
                c->pipe = pipe;
                c->readEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                c->writeEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                ++_clientThreadsAlive;
                try
                {
                    std::thread([this, c] {
                        ::SetThreadDescription(::GetCurrentThread(), L"sessiondb client");
                        _clientLoop(c);
                        --_clientThreadsAlive;
                    }).detach();
                }
                catch (...)
                {
                    LogSwallowedException(L"sessiondb client thread");
                    --_clientThreadsAlive; // no thread: the client's pipe closes with its last owner (here)
                }
            }

            // Stop: the client readers see the stop event, the workers drain, the writer finishes its step.
            _requestStop(L"shutting down");
            // Every reader waits on the stop event too, so they end at once; 10 s is a belt.
            for (int i = 0; i < 200 && _clientThreadsAlive.load() > 0; ++i)
            {
                ::Sleep(50);
            }
            _watchdogThread.join();
            for (auto& t : _queryThreads)
            {
                t.join();
            }
            _writer.join();
            _search.reset();
            _store->Close();
            ::DeleteFileW(Proto::DiscoveryPath(_profile).c_str());
            ServiceLog(L"[sessdb] service stopped");
            return 0;
        }
    }

    int RunServe(const std::wstring& profileDir, size_t sweepLimit)
    {
        Service s{ profileDir, sweepLimit };
        return s.Run();
    }
}
