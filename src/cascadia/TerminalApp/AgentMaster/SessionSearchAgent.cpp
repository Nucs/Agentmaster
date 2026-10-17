// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (vcxproj marks it NotUsing).
// The Sessions browser's AI search; see SessionSearchAgent.h and SESSIONS.md §9.
#include "SessionSearchAgent.h"

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException, ResolveClaudeTranscriptPath
#include "Engine.h" // SharedEngine — the shared agent's registry
#include "Persistence.h" // LoadAppSettings — the shared agent's persisted thinking level
#include "SessionDbClient.h" // SESSIONS_DB.md §8.2 — the database mode's tool calls
#include "SessionDbProtocol.h" // kToolCall / kResolveIds
#include "SessionRegistry.h" // LiveTabBindings / HeadlinesOf — the open sessions
#include "SessionStore.h" // WarmSessionStore — the store's first read, kept off the search's path
#include "TranscriptStore.h" // LoadOrRefreshSessionIndex — a shown session's row in the database mode

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace
{
    using namespace Agentmaster;

    int64_t NowUnixMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // The period a catalog is gathered for before any search asked for one: the search tool's
    // own default, so the first search of a request finds its catalog ready.
    constexpr int64_t kDefaultCatalogMs = static_cast<int64_t>(kSessionSearchDefaultDays) * 86400000LL;

    // One step of the index keeper reads sidecar files for this long, then publishes what it
    // has. Short enough that a search which waits for "a bit more index" gets it soon; long
    // enough that a step's fixed cost (the directory listing, the copies from the step before:
    // about 0.1 s for 1,500 sessions) stays a small part of it.
    constexpr int64_t kCatalogStepBudgetMs = 2500;

    // The FIRST step of a run, when there is no index at all: much shorter, so that the first
    // search has the newest sessions whatever the rest costs. Measured on a machine with 1,506
    // sessions in 90 days: a sidecar that Windows has cached reads in 0.3 ms, the first touch
    // of one takes 10 to 20 ms (an on-access scan), so a cold index needs 15 to 30 s. In 400 ms
    // the first step has the newest 20 to 40 sessions cold, and all of them warm.
    constexpr int64_t kCatalogFirstStepBudgetMs = 400;

    // How long a search waits for an index that covers its period and is fresh, when an older
    // or shorter one is there. After that it searches what there is, and the result says that
    // older sessions are still being indexed. The model, and behind it the user, waits here:
    // only the first search of a request waits (CatalogFor).
    constexpr int64_t kCatalogSearchWaitMs = 2000;

    // How long a search waits for the FIRST index of a run (there is nothing to search before
    // it). The first step, plus room for one very large transcript that a step cannot split
    // (a transcript with no sidecar is parsed whole: seconds for hundreds of megabytes).
    constexpr int64_t kCatalogFirstWaitMs = 15000;

    // ===== the session database mode (SESSIONS_DB.md §8.2) =====

    // How long one forwarded database call may take: the service ends a query at 10 s (`sql` at 2.5 s) and answers
    // 408, so this is only the belt for a service that stopped answering.
    constexpr uint32_t kDatabaseCallTimeoutMs = 20000;
    // How long a forwarded call waits for a service it had to start (the copy-out and the database's open).
    constexpr uint32_t kDatabaseStartWaitMs = 20000;

    // Connect a client to the profile's service, starting one when none runs. Returns false with `error` set.
    bool ConnectDatabase(SessionDb::ServiceClient& client, std::wstring& error)
    {
        return client.Connect(true, kDatabaseStartWaitMs, error);
    }

    // One database tool call through the service: its own connection (a ServiceClient makes one call at a time, and
    // the model may run several read-only calls of one step at once), `toolCall` with the tool's name and the
    // model's arguments, then the answer's text and error flag as the model reads them. Every failure becomes an
    // error result that tells the model what to do (it must not read "no session matched" into it).
    //   profile   — the profile whose service answers.
    //   tool      — a SessionDbToolDefs.h name.
    //   arguments — the model's argument object.
    // Returns the tool's result. Throws only std::bad_alloc (the harness answers the model with it).
    HarnessToolResult CallDatabaseTool(const std::wstring& profile, const std::string& tool, const json::Value& arguments)
    {
        SessionDb::ServiceClient client{ profile };
        std::wstring error;
        if (!ConnectDatabase(client, error))
        {
            AppendStateLog(L"hooks.log", L"[ai-search] the session database did not answer a " + HarnessFromUtf8(tool) + L" call: " + error + L"\n");
            return { L"The session database is not available (" + error + L"). Do not search again now and do not call show: reply that the session database is not running and that the user can ask again in a moment.", true };
        }
        json::Value params = json::Value::MkObj();
        params.Set(L"name", json::Value::MkStr(HarnessFromUtf8(tool)));
        params.Set(L"arguments", arguments.type == json::Value::Type::Obj ? arguments : json::Value::MkObj());
        std::string answer;
        if (!client.Call(SessionDbProtocol::kToolCall, HarnessToUtf8(json::Dump(params)), answer, kDatabaseCallTimeoutMs, error))
        {
            AppendStateLog(L"hooks.log", L"[ai-search] the session database's " + HarnessFromUtf8(tool) + L" call failed: " + error + L"\n");
            return { L"The session database did not answer (" + error + L"). Try the call once more; if it fails again, reply that the session database is not answering.", true };
        }
        const auto env = json::Parse(HarnessFromUtf8(answer));
        if (!env || env->type != json::Value::Type::Obj)
        {
            return { L"The session database sent an unreadable answer.", true };
        }
        if (const auto* e = env->Find(L"error"))
        {
            // 408 / 499 / 400 ...: the service's message says what happened and, for a bad argument, how to fix it.
            return { e->StrAt(L"message", L"The call failed."), true };
        }
        const auto* result = env->Find(L"result");
        if (!result || result->type != json::Value::Type::Obj)
        {
            return { L"The session database sent an empty answer.", true };
        }
        return { result->StrAt(L"text"), result->BoolAt(L"isError") };
    }

    // The whole session ids behind short ids, in order ("" for an unknown or shared one), in one `resolveIds` round
    // trip. Never throws beyond std::bad_alloc: a failure answers "" for every handle (show then reports them as
    // unknown ids, which the model can correct).
    std::vector<std::wstring> ResolveDatabaseIds(const std::wstring& profile, const std::vector<std::wstring>& handles)
    {
        std::vector<std::wstring> out(handles.size());
        if (handles.empty())
        {
            return out;
        }
        SessionDb::ServiceClient client{ profile };
        std::wstring error;
        if (!ConnectDatabase(client, error))
        {
            AppendStateLog(L"hooks.log", L"[ai-search] resolveIds: the session database did not answer: " + error + L"\n");
            return out;
        }
        json::Value list = json::Value::MkArr();
        for (size_t i = 0; i < handles.size() && i < 200; ++i)
        {
            list.Push(json::Value::MkStr(handles[i]));
        }
        json::Value params = json::Value::MkObj();
        params.Set(L"ids", std::move(list));
        std::string answer;
        if (!client.Call(SessionDbProtocol::kResolveIds, HarnessToUtf8(json::Dump(params)), answer, kDatabaseCallTimeoutMs, error))
        {
            AppendStateLog(L"hooks.log", L"[ai-search] resolveIds failed: " + error + L"\n");
            return out;
        }
        const auto env = json::Parse(HarnessFromUtf8(answer));
        const json::Value* result = env ? env->Find(L"result") : nullptr;
        const json::Value* ids = result ? result->Find(L"ids") : nullptr;
        if (!ids || ids->type != json::Value::Type::Obj)
        {
            return out;
        }
        for (size_t i = 0; i < handles.size(); ++i)
        {
            if (const auto* v = ids->Find(handles[i]); v && v->type == json::Value::Type::Str)
            {
                out[i] = v->str;
            }
        }
        return out;
    }

    // FILETIME (UTC, 100 ns since 1601) as Unix milliseconds; 0 for a time before 1970.
    int64_t FileTimeUnixMs(const FILETIME& ft) noexcept
    {
        const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        constexpr uint64_t kEpochDiff = 116444736000000000ull; // 1601 -> 1970 in 100 ns
        return t < kEpochDiff ? 0 : static_cast<int64_t>((t - kEpochDiff) / 10000);
    }

    // The index entry of one session by its whole id, for a row the page shows outside its own window (the database
    // mode has no catalog to take it from): the transcript found where Claude keeps it, then its sidecar index
    // (LoadOrRefreshSessionIndex: one small file read, or a parse of a transcript that grew). nullopt when the
    // transcript is gone (the page then shows no row for it, like for any deleted session). Never throws beyond
    // std::bad_alloc (a failed read is logged and reads as nullopt).
    std::optional<SessionIndexEntry> IndexEntryForSession(const std::wstring& sessionId)
    {
        try
        {
            const std::wstring path = ResolveClaudeTranscriptPath(sessionId);
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (path.empty() || !::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
            {
                return std::nullopt;
            }
            TranscriptRef ref;
            ref.sessionId = sessionId;
            ref.path = path;
            const size_t slash = path.find_last_of(L"\\/");
            if (slash != std::wstring::npos && slash > 0)
            {
                const size_t parent = path.find_last_of(L"\\/", slash - 1);
                ref.projectDirLeaf = path.substr(parent == std::wstring::npos ? 0 : parent + 1, slash - (parent == std::wstring::npos ? 0 : parent + 1));
            }
            ref.sizeBytes = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            ref.mtimeMs = FileTimeUnixMs(fad.ftLastWriteTime);
            ref.birthMs = FileTimeUnixMs(fad.ftCreationTime);
            return LoadOrRefreshSessionIndex(ref);
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchAgent: the index entry of a shown session");
            return std::nullopt;
        }
    }

    // The shown list as JSON (the "view" file, version 1): what a page needs to show the
    // conversation's last result again after a restart.
    std::wstring SerializeView(const SessionSearchSnapshot& s)
    {
        using json::Value;
        Value root = Value::MkObj();
        root.Set(L"version", Value::MkNum(1));
        root.Set(L"question", Value::MkStr(s.question));
        root.Set(L"note", Value::MkStr(s.note));
        root.Set(L"answer", Value::MkStr(s.answer));
        root.Set(L"answerIsError", Value::MkBool(s.answerIsError));
        root.Set(L"hasShown", Value::MkBool(s.hasShown));
        Value ids = Value::MkArr();
        for (const auto& id : s.shownIds)
        {
            ids.Push(Value::MkStr(id));
        }
        root.Set(L"shownIds", std::move(ids));
        // The evidence per shown session (SESSIONS.md 9.12), index-aligned with shownIds. Still version 1: an older
        // build reads the file and ignores the key; a file an older build wrote has none, and its rows show no why.
        Value reasons = Value::MkArr();
        for (size_t i = 0; i < s.shownIds.size(); ++i)
        {
            reasons.Push(Value::MkStr(i < s.shownReasons.size() ? s.shownReasons[i] : std::wstring{}));
        }
        root.Set(L"shownReasons", std::move(reasons));
        return json::Dump(root);
    }

    // Read a view file's text into `s`. Anything that is not a version-1 view leaves `s` as it is.
    void DeserializeViewInto(std::wstring_view text, SessionSearchSnapshot& s)
    {
        const auto parsed = json::Parse(text);
        if (!parsed || parsed->type != json::Value::Type::Obj || parsed->I64At(L"version") != 1)
        {
            return;
        }
        s.question = parsed->StrAt(L"question");
        s.note = parsed->StrAt(L"note");
        s.answer = parsed->StrAt(L"answer");
        s.answerIsError = parsed->BoolAt(L"answerIsError");
        s.hasShown = parsed->BoolAt(L"hasShown");
        s.shownIds.clear();
        s.shownReasons.clear();
        const auto* reasons = parsed->Find(L"shownReasons");
        const bool hasReasons = reasons && reasons->type == json::Value::Type::Arr;
        if (const auto* ids = parsed->Find(L"shownIds"); ids && ids->type == json::Value::Type::Arr)
        {
            for (size_t i = 0; i < ids->arr.size(); ++i)
            {
                const auto& id = ids->arr[i];
                if (id.type == json::Value::Type::Str && !id.str.empty())
                {
                    s.shownIds.push_back(id.str);
                    // Lined up with its id by position (SerializeView writes one entry per id; a skipped empty id
                    // skips its reason with it).
                    const bool reason = hasReasons && i < reasons->arr.size() && reasons->arr[i].type == json::Value::Type::Str;
                    s.shownReasons.push_back(reason ? reasons->arr[i].str : std::wstring{});
                }
            }
        }
    }

    // Write `text` to `path` as UTF-8, replacing the file through a temp file and a rename (a
    // crash leaves the old file or the new one).
    //   returns — true when the file now holds the text.
    bool WriteViewFile(const std::wstring& path, const std::wstring& text) noexcept
    {
        try
        {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path{ path }.parent_path(), ec);
            const std::string bytes = HarnessToUtf8(text);
            const std::wstring tmp = path + L".tmp." + std::to_wstring(::GetCurrentThreadId());
            const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            DWORD wrote = 0;
            const BOOL ok = bytes.empty() ? TRUE : ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
            ::CloseHandle(h);
            if (!ok || wrote != bytes.size() || !::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                ::DeleteFileW(tmp.c_str());
                return false;
            }
            return true;
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchAgent: write the view file");
            return false;
        }
    }

    std::wstring ReadViewFile(const std::wstring& path) noexcept
    {
        try
        {
            std::ifstream f(std::filesystem::path{ path }, std::ios::binary);
            if (!f)
            {
                return {};
            }
            const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return HarnessFromUtf8(bytes);
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchAgent: read the view file");
            return {};
        }
    }

    void LogAgent(const std::wstring& message) noexcept
    {
        try
        {
            AppendStateLog(L"hooks.log", L"[ai-search] " + message + L"\n");
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE (CLAUDE.md Rule #18, the logging-path exemption): this IS the
            // logging path; reporting its own failure would call AppendStateLog again.
        }
    }

    // ===== the window and tab tools over the app's windows (SESSIONS.md §9.14) =====

    // How long the tools wait for the windows' UI thread (every window runs on the Emperor's one UI thread). A census
    // is one pass over each strip; an open spawns a ConPTY and may wait behind a busy dispatcher; a park swaps a
    // terminal control. Past these the model is told that a window did not answer — better than a model that waits
    // on a wedged UI thread (a late open still lands, and the tool says so).
    constexpr uint32_t kWindowCensusTimeoutMs = 4000;
    constexpr uint32_t kWindowOpenTimeoutMs = 15000;
    constexpr uint32_t kWindowDeactivateTimeoutMs = 10000;

    /// <summary>The Triage Board's word for a state — what the model reads as a tab's `state`, and what the deactivate
    /// guard compares ("running" / "needs approval" mean a turn is in flight).</summary>
    /// <param name="state">The session's state.</param>
    /// <returns>A static, NUL-terminated word.</returns>
    const wchar_t* TabStateWord(SessionState state) noexcept
    {
        switch (state)
        {
        case SessionState::Running:
            return L"running";
        case SessionState::WaitingForInput:
            return L"waiting for you";
        case SessionState::NeedsApproval:
            return L"needs approval";
        case SessionState::Error:
            return L"error";
        case SessionState::Done:
            return L"done";
        case SessionState::Idle:
        default:
            return L"idle";
        }
    }

    /// <summary>
    /// Fill the session half of every tab of a census (WindowTabInfo) from the shared registry: one lean snapshot of
    /// the LIVE sessions (SnapshotLiveForCards: no queues, no transcript texts), and a queue-less read for a tab whose
    /// session is not in it (a tab mid-bind).
    /// </summary>
    /// <param name="windows">The census the windows answered (their strip halves).</param>
    /// <exception cref="std::bad_alloc">If the snapshot cannot be allocated.</exception>
    /// <remarks>
    /// Done here, on the tool-call thread, rather than in the windows: each window then only walks its strip on its UI
    /// thread, and a census of hundreds of tabs costs the UI thread no registry copy (UI_THREAD.md §10e — the full
    /// copies of every record were the release's measured cost).
    /// </remarks>
    void EnrichWindowCensus(std::vector<WindowCensus>& windows)
    {
        const auto registry = SharedEngine().registry;
        if (!registry)
        {
            return;
        }
        std::unordered_map<std::wstring, SessionInfo> live;
        for (auto& s : registry->SnapshotLiveForCards())
        {
            std::wstring id = s.id;
            live.emplace(std::move(id), std::move(s));
        }
        for (auto& window : windows)
        {
            for (auto& tab : window.tabs)
            {
                if (tab.sessionId.empty())
                {
                    continue; // a shell tab: no session facts
                }
                std::optional<SessionInfo> fallback;
                const SessionInfo* s = nullptr;
                if (const auto it = live.find(tab.sessionId); it != live.end())
                {
                    s = &it->second;
                }
                else
                {
                    fallback = registry->GetWithoutQueue(tab.sessionId);
                    if (fallback)
                    {
                        s = &*fallback;
                    }
                }
                if (!s)
                {
                    continue; // unknown to the registry (a tab whose binding is in flight): only its strip facts
                }
                tab.codex = s->kind == AgentKind::Codex;
                tab.state = TabStateWord(s->state);
                tab.started = s->started;
                tab.external = s->external;
                tab.dir = s->workingDir;
                tab.worksIn = s->inferredWorkingDir; // stored empty when it is the launch folder
                tab.branch = s->branch;
                tab.lastTurnUnixMs = s->turnStartUnixMs;
                // The newest activity of any kind: the transcript's own (line-derived), the decay anchor, a turn's end.
                tab.lastActivityUnixMs = (std::max)({ s->convLastActivityUnixMs, s->lastActivityUnixMs, s->turnEndUnixMs });
            }
        }
    }
}

namespace Agentmaster
{
    struct SessionSearchAgent::Impl
    {
        SessionSearchAgentOptions options;
        std::wstring viewPath; // empty when nothing is persisted

        // `mutex` guards everything from here to `keeper`.
        mutable std::mutex mutex;
        SessionSearchSnapshot snapshot;
        uint64_t latestTurn{ 0 }; // the newest request's turn id: only its end changes the answer
        struct ListenerEntry
        {
            uint64_t token{};
            Listener fn;
        };
        std::vector<ListenerEntry> listeners;
        uint64_t nextListenerToken{ 1 };
        std::wstring claudeExePath;
        bool shuttingDown{ false };
        // The window the RUNNING request came from (Ask's originWindowId; "" = the Telegram topic or none) — what the
        // tabs server reports as asked_from (SESSIONS.md §9.14). Replaced by every Ask, cleared by Reset.
        std::wstring requestOrigin;
        // The newest request's text blocks that no tool call has followed YET. A tool call moves them into
        // snapshot.narration (they were written before acting: the plan); the turn's end drops them (they are the
        // answer, which TurnCompleted carries).
        std::wstring pendingAssistantText;
        // ---- the index keeper (see KeeperRun) ----
        std::shared_ptr<const SessionCatalog> catalog; // the newest session index, or null
        // The earliest instant a search of this run asked for; 0 = nobody asked since the index
        // was released. It only moves back: the keeper reads the widest period ever asked, so
        // a search for a shorter one never throws a long index away.
        int64_t wantSince{ 0 };
        bool keeperRunning{ false }; // a keeper task is gathering (at most one at a time)
        // The request (a requestGeneration value) whose search already waited the whole
        // kCatalogSearchWaitMs for a longer index and did not get it. Its further searches do
        // not wait again: three searches of one request must not wait three times.
        uint64_t waitedGeneration{ ~uint64_t{ 0 } };
        std::future<void> keeper; // that task, kept so that Shutdown can wait for it

        // Raised when a new index was published or the keeper ended. Waited on with `mutex`.
        std::condition_variable catalogChanged;
        // Bumped to stop the keeper's running step (ReleaseCatalog, Shutdown). Atomic: the
        // gather reads it between two sidecar files, without the mutex.
        std::atomic<uint64_t> keeperEpoch{ 0 };
        // Bumped whenever the running request is not wanted any more (a newer request, an
        // interrupt, a reset, the shutdown). A search reads it when it starts and stops when it
        // changed (SessionSearchToolset::Callbacks::generation).
        std::atomic<uint64_t> requestGeneration{ 0 };

        std::unique_ptr<SessionSearchToolset> toolset;
        // LAST: its threads call the toolset and this object; Shutdown stops it first.
        std::unique_ptr<HarnessConversation> conversation;

        // -------------------------------------------------------------------------------

        SessionCatalogSources Sources() const
        {
            return SessionCatalogSources{ options.openSessions };
        }

        /// <summary>
        /// Append one text block to snapshot.narration, within kSessionSearchMaxNarrationChars: the START is what
        /// matters (the plan), so a block that would pass the cap is cut and later blocks are dropped.
        /// </summary>
        /// <param name="block">The text the model wrote before a tool call.</param>
        /// <exception cref="std::bad_alloc">If the narration cannot grow.</exception>
        /// <remarks>The caller holds `mutex`.</remarks>
        void AppendNarrationLocked(const std::wstring& block)
        {
            std::wstring& narration = snapshot.narration;
            if (block.empty() || narration.size() >= kSessionSearchMaxNarrationChars)
            {
                return;
            }
            if (!narration.empty())
            {
                narration += L"\n";
            }
            narration += block;
            if (narration.size() > kSessionSearchMaxNarrationChars)
            {
                size_t keep = kSessionSearchMaxNarrationChars - 3;
                // Never split a surrogate pair: a lone high surrogate would reach the page and the Telegram API.
                if (keep > 0 && narration[keep - 1] >= 0xD800 && narration[keep - 1] <= 0xDBFF)
                {
                    --keep;
                }
                narration.resize(keep);
                narration += L"...";
            }
        }

        // Tell every listener. Fills e.snapshot. Never throws: a listener that throws is logged.
        void Notify(SessionSearchEvent e) noexcept
        {
            try
            {
                std::vector<Listener> targets;
                {
                    std::lock_guard lock{ mutex };
                    e.snapshot = snapshot;
                    targets.reserve(listeners.size());
                    for (const auto& l : listeners)
                    {
                        targets.push_back(l.fn);
                    }
                }
                // Outside the lock: a listener may call back into the agent.
                for (const auto& fn : targets)
                {
                    try
                    {
                        fn(e);
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"SessionSearchAgent listener");
                    }
                }
            }
            catch (...)
            {
                LogSwallowedException(L"SessionSearchAgent::Notify");
            }
        }

        // Write (or, for an empty conversation, delete) the view file.
        void SaveView() noexcept
        {
            if (viewPath.empty())
            {
                return;
            }
            try
            {
                SessionSearchSnapshot copy;
                {
                    std::lock_guard lock{ mutex };
                    copy = snapshot;
                }
                if (copy.question.empty() && !copy.hasShown)
                {
                    ::DeleteFileW(viewPath.c_str());
                    return;
                }
                if (!WriteViewFile(viewPath, SerializeView(copy)))
                {
                    LogAgent(L"the view file was NOT saved: " + viewPath);
                }
            }
            catch (...)
            {
                LogSwallowedException(L"SessionSearchAgent::SaveView");
            }
        }

        // The index keeper: the ONE place that reads the session index for the AI search. It
        // runs as a background task while there is something to read, and reads in STEPS: each
        // step takes everything the step before had (no file read) and reads new sidecar files
        // for kCatalogStepBudgetMs, newest sessions first, then publishes the result. So:
        //   - a search never reads the disk itself; it takes the newest published index;
        //   - an index that needs minutes the first time (every transcript whose sidecar is
        //     missing or out of date is parsed once) is searchable after the first step, the
        //     newest sessions first, and reaches further back with every step;
        //   - a refresh of a complete index is one short step (the listing, plus the sessions
        //     that changed).
        // It ends when the index is complete for the widest period asked, when the index was
        // released and nobody asked since, when a gather failed, or at the shutdown.
        // Never throws: a task that threw would end silently with keeperRunning left true, and
        // no index would ever be read again.
        void KeeperRun() noexcept
        {
            try
            {
                const auto started = std::chrono::steady_clock::now();
                int steps = 0;
                bool announced = false; // the "being built" line was written for this run
                bool storeWarmTried = false; // the session store's first read was tried in this run (once is enough: a failure must not loop)
                for (;;)
                {
                    std::shared_ptr<const SessionCatalog> previous;
                    int64_t since = 0;
                    uint64_t epoch = 0;
                    {
                        std::lock_guard lock{ mutex };
                        if (shuttingDown || wantSince == 0)
                        {
                            keeperRunning = false;
                            break;
                        }
                        previous = catalog;
                        since = wantSince;
                        // Read under the lock, like ReleaseCatalog bumps it: the step belongs
                        // to exactly the state it copied.
                        epoch = keeperEpoch.load();
                    }
                    std::shared_ptr<const SessionCatalog> step;
                    try
                    {
                        SessionCatalogGatherOptions gather;
                        // No index yet: publish the newest sessions fast, then take normal steps.
                        gather.budgetMs = previous ? kCatalogStepBudgetMs : kCatalogFirstStepBudgetMs;
                        gather.previous = std::move(previous);
                        gather.cancelled = [this, epoch] { return keeperEpoch.load() != epoch; };
                        step = GatherSessionCatalog(since, Sources(), gather);
                        ++steps;
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"SessionSearchAgent: the session index gather");
                    }
                    if (step && keeperEpoch.load() == epoch) // not a step that a release cut short
                    {
                        // The log lines come BEFORE the last locked section: after it this task
                        // must return at once (EnsureCatalog may replace its future, which
                        // waits for the task, while it holds the mutex).
                        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                        if (!step->Complete() && !announced)
                        {
                            announced = true;
                            LogAgent(L"the session index is being built in the background: " + std::to_wstring(step->entries.size()) + L" session(s) so far, the last " +
                                     std::to_wstring((step->gatheredUnixMs - step->coveredSinceUnixMs) / 86400000LL) + L" of " +
                                     std::to_wstring((step->gatheredUnixMs - step->sinceUnixMs) / 86400000LL) + L" day(s) are searchable");
                        }
                        else if (step->Complete() && (announced || ms > 1500))
                        {
                            // Worth a line: time a search may have waited, or the end of a build.
                            LogAgent(L"the session index is complete: " + std::to_wstring(step->entries.size()) + L" session(s), " + std::to_wstring(steps) + L" step(s), " + std::to_wstring(ms) + L" ms");
                            announced = false;
                        }
                    }
                    // The step left the stored titles, tags and favorites out (the session store
                    // was not read yet in this process): it is published as it is, and the store
                    // is read AFTER the publish, here on this background thread, so the step
                    // after it has them. In the app the engine reads the store at start-up, so
                    // this runs only for a request made in the first seconds of a run.
                    const bool needStore = step && !step->storeOverlay && !storeWarmTried;
                    bool more = false;
                    {
                        std::lock_guard lock{ mutex };
                        // Released (or shut down) while the step ran: its result must not bring
                        // back the memory that ReleaseCatalog freed.
                        const bool released = keeperEpoch.load() != epoch;
                        if (step && !released)
                        {
                            catalog = step;
                        }
                        if (shuttingDown)
                        {
                            more = false;
                        }
                        else if (released)
                        {
                            more = wantSince != 0; // somebody asked again after the release: read it again
                        }
                        else if (!step)
                        {
                            more = false; // the gather failed: the next request starts a new keeper
                        }
                        else
                        {
                            // Not everything is read yet, the store is still to be read, or a
                            // search asked for a longer period while this step ran.
                            more = !step->Complete() || needStore || wantSince < step->sinceUnixMs;
                        }
                        if (!more)
                        {
                            keeperRunning = false;
                        }
                    }
                    catalogChanged.notify_all();
                    if (!more)
                    {
                        break;
                    }
                    if (needStore)
                    {
                        storeWarmTried = true;
                        try
                        {
                            const auto t0 = std::chrono::steady_clock::now();
                            const size_t records = WarmSessionStore();
                            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                            if (ms > 1500)
                            {
                                LogAgent(L"the session store was read for the index: " + std::to_wstring(records) + L" record(s), " + std::to_wstring(ms) + L" ms");
                            }
                        }
                        catch (...)
                        {
                            // The next steps then stay without the stored titles, tags and
                            // favorites; the next run of the keeper tries again.
                            LogSwallowedException(L"SessionSearchAgent: read the session store");
                        }
                    }
                }
            }
            catch (...)
            {
                LogSwallowedException(L"SessionSearchAgent::KeeperRun");
                try
                {
                    std::lock_guard lock{ mutex };
                    keeperRunning = false;
                }
                catch (...)
                {
                    LogSwallowedException(L"SessionSearchAgent::KeeperRun: clear the running flag");
                }
                catalogChanged.notify_all();
            }
        }

        // Make sure that a fresh index from `sinceUnixMs` on exists or is on its way: start the
        // keeper when none runs and the current index is too old or too short. Returns at once
        // (it is called from a UI thread, through Prewarm and Ask). Never throws.
        void EnsureCatalog(int64_t sinceUnixMs) noexcept
        {
            try
            {
                std::lock_guard lock{ mutex };
                // The database mode searches the session database: no index is read here (SESSIONS_DB.md §8.2).
                if (shuttingDown || !options.sessionDbProfile.empty())
                {
                    return;
                }
                if (wantSince == 0 || sinceUnixMs < wantSince)
                {
                    wantSince = sinceUnixMs;
                }
                if (keeperRunning)
                {
                    return; // it reads wantSince before every step
                }
                // Half the maximum age: an index read now must still be good when the model's
                // first request has come back (about a second later).
                if (catalog && catalog->coveredSinceUnixMs <= wantSince && NowUnixMs() - catalog->gatheredUnixMs <= kSessionCatalogMaxAgeMs / 2)
                {
                    return;
                }
                // Replacing `keeper` waits for the task before this one. That task has cleared
                // keeperRunning as its last act under this mutex, so it is returning: the wait
                // is a few instructions long, not a gather.
                keeperRunning = true;
                try
                {
                    keeper = std::async(std::launch::async, [this] { KeeperRun(); });
                }
                catch (...)
                {
                    keeperRunning = false;
                    throw;
                }
            }
            catch (...)
            {
                // No thread for the task. A search then uses the index there is, or reports
                // that there is none; the next request tries again.
                LogSwallowedException(L"SessionSearchAgent::EnsureCatalog");
            }
        }

        // The index for a search from `sinceUnixMs` on. Called on the tool-call thread, with
        // the model waiting. It reads no file: it asks the keeper for the index and waits a
        // bounded time for one that covers the period and is fresh.
        //   cancelled — empty = never; else true ends the wait (the search is not wanted).
        //   returns   — the newest index: complete for the period when the keeper got there in
        //               time, else a shorter or older one (the search result says what it
        //               lacks); null when there is none at all.
        std::shared_ptr<const SessionCatalog> CatalogFor(int64_t sinceUnixMs, const std::function<bool()>& cancelled) noexcept
        {
            try
            {
                EnsureCatalog(sinceUnixMs);
                const auto started = std::chrono::steady_clock::now();
                const uint64_t generation = requestGeneration.load();
                std::unique_lock lock{ mutex };
                for (;;)
                {
                    const bool good = catalog && catalog->coveredSinceUnixMs <= sinceUnixMs && NowUnixMs() - catalog->gatheredUnixMs <= kSessionCatalogMaxAgeMs;
                    if (good || shuttingDown || !keeperRunning)
                    {
                        break; // it is there, or nothing better is coming
                    }
                    if (cancelled && cancelled())
                    {
                        break;
                    }
                    if (catalog && waitedGeneration == generation)
                    {
                        break; // this request waited once already: search what there is
                    }
                    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                    if (waited >= (catalog ? kCatalogSearchWaitMs : kCatalogFirstWaitMs))
                    {
                        waitedGeneration = generation;
                        break;
                    }
                    // Short slices: `cancelled` has no notification of its own.
                    catalogChanged.wait_for(lock, std::chrono::milliseconds{ 100 });
                }
                return catalog;
            }
            catch (...)
            {
                LogSwallowedException(L"SessionSearchAgent::CatalogFor");
                return nullptr;
            }
        }

        // The index entries of `ids`, in order, without their touched-path lists.
        std::vector<SessionIndexEntry> EntriesFor(const std::vector<std::wstring>& ids) const
        {
            std::shared_ptr<const SessionCatalog> current;
            {
                std::lock_guard lock{ mutex };
                current = catalog;
            }
            std::vector<SessionIndexEntry> out;
            if (ids.empty())
            {
                return out;
            }
            if (!options.sessionDbProfile.empty())
            {
                // The database mode has no catalog: each listed session's row comes from its own sidecar index (the
                // page needs it only for a session outside its window; at most the 100 a show lists).
                out.reserve(ids.size());
                for (const auto& id : ids)
                {
                    if (auto entry = IndexEntryForSession(id))
                    {
                        entry->stats.pathsAccessed.clear();
                        entry->stats.pathsAccessed.shrink_to_fit();
                        out.push_back(std::move(*entry));
                    }
                }
                return out;
            }
            if (!current)
            {
                return out;
            }
            std::unordered_map<std::wstring, size_t> byId;
            byId.reserve(current->entries.size());
            for (size_t i = 0; i < current->entries.size(); ++i)
            {
                byId.emplace(current->entries[i].sessionId, i);
            }
            out.reserve(ids.size());
            for (const auto& id : ids)
            {
                if (const auto it = byId.find(id); it != byId.end())
                {
                    out.push_back(current->entries[it->second]);
                    // A row needs the title, folder, times and counts — not up to 512 paths.
                    out.back().stats.pathsAccessed.clear();
                    out.back().stats.pathsAccessed.shrink_to_fit();
                }
            }
            return out;
        }

        // ---- the toolset's callbacks (the SDK's reader thread) ----

        void OnSearching(const std::wstring& summary)
        {
            uint64_t turn = 0;
            {
                std::lock_guard lock{ mutex };
                snapshot.activity = L"Searching: " + summary;
                turn = latestTurn;
            }
            SessionSearchEvent e;
            e.kind = SessionSearchEventKind::Searching;
            e.turn = turn;
            e.text = summary;
            Notify(std::move(e));
        }

        void OnCandidates(const std::vector<std::wstring>& ids)
        {
            SessionSearchEvent e;
            e.kind = SessionSearchEventKind::Candidates;
            {
                std::lock_guard lock{ mutex };
                e.turn = latestTurn;
            }
            e.sessionIds = ids;
            e.entries = EntriesFor(ids);
            Notify(std::move(e));
        }

        void OnShow(const std::vector<std::wstring>& ids, const std::wstring& note, const std::vector<std::wstring>& whys)
        {
            SessionSearchEvent e;
            e.kind = SessionSearchEventKind::Shown;
            e.text = note;
            e.sessionIds = ids;
            e.reasons = whys;
            e.reasons.resize(ids.size()); // index-aligned whatever the toolset passed
            e.entries = EntriesFor(ids);
            size_t withWhy = 0;
            for (const auto& why : e.reasons)
            {
                withWhy += why.empty() ? 0 : 1;
            }
            {
                std::lock_guard lock{ mutex };
                snapshot.hasShown = true;
                snapshot.shownIds = ids;
                snapshot.shownReasons = e.reasons;
                snapshot.note = note;
                snapshot.activity.clear();
                e.turn = latestTurn;
            }
            // How many shown sessions came with evidence: a request answered with none is one where the model did
            // not follow rules 4 and 5 (read, then a why each) — the log is where a review of that starts.
            LogAgent(L"shown " + std::to_wstring(ids.size()) + L" session(s), " + std::to_wstring(withWhy) + L" with a why" + (note.empty() ? std::wstring{} : (L": " + note)));
            SaveView();
            Notify(std::move(e));
        }

        // ---- the conversation's events (its worker thread and the SDK's reader thread) ----

        void OnHarnessEvent(const HarnessEvent& h)
        {
            SessionSearchEvent e;
            e.turn = h.turn;
            bool notify = false;
            bool save = false;
            // A process that just came up (or answered its first turn) has reported the model's
            // effort levels: the page's thinking toggle offers only those. Read before our lock:
            // the inventory takes the conversation's own lock, and the conversation calls this
            // sink outside it.
            const bool processReported = (h.kind == HarnessEventKind::State && h.state == HarnessState::Ready) || h.kind == HarnessEventKind::Ready;
            std::vector<std::wstring> efforts;
            if (processReported && conversation)
            {
                efforts = conversation->Inventory().supportedEfforts;
            }
            {
                std::lock_guard lock{ mutex };
                if (h.epoch < snapshot.epoch)
                {
                    return; // an event of a conversation that was reset away
                }
                snapshot.state = h.state;
                snapshot.usage = h.usage;
                if (processReported)
                {
                    snapshot.supportedEfforts = std::move(efforts); // a new process's report replaces the old one
                }
                switch (h.kind)
                {
                case HarnessEventKind::State:
                    e.kind = SessionSearchEventKind::StateChanged;
                    notify = true;
                    break;
                case HarnessEventKind::Ready:
                    snapshot.model = h.model;
                    e.kind = SessionSearchEventKind::StateChanged;
                    notify = true;
                    break;
                case HarnessEventKind::Compacting:
                    snapshot.activity = L"Compacting the conversation\x2026";
                    e.kind = SessionSearchEventKind::Compacting;
                    notify = true;
                    break;
                case HarnessEventKind::Compacted:
                    ++snapshot.compactions;
                    snapshot.activity = L"Thinking\x2026";
                    e.kind = SessionSearchEventKind::StateChanged;
                    notify = true;
                    break;
                case HarnessEventKind::AssistantText:
                    // A text block of the newest request. Held until it is known whether a tool call follows it (then it
                    // was written BEFORE acting — the plan the system prompt asks for, shown as the narration) or the
                    // turn ends (then it is the answer, which TurnCompleted carries).
                    if (h.turn >= latestTurn && !h.text.empty())
                    {
                        if (!pendingAssistantText.empty())
                        {
                            pendingAssistantText += L"\n";
                        }
                        pendingAssistantText += h.text;
                    }
                    break;
                case HarnessEventKind::ToolCall:
                    // The text before this call is narration. (The toolset's callbacks report the call itself.)
                    if (h.turn >= latestTurn && !pendingAssistantText.empty())
                    {
                        e.kind = SessionSearchEventKind::Narration;
                        e.text = std::move(pendingAssistantText);
                        pendingAssistantText.clear();
                        AppendNarrationLocked(e.text);
                        notify = true;
                    }
                    break;
                case HarnessEventKind::TurnCompleted:
                    if (h.turn < latestTurn)
                    {
                        break; // an older request (interrupted or replaced): the newest one owns the answer
                    }
                    latestTurn = h.turn;
                    pendingAssistantText.clear(); // the turn's last text is its answer, not narration
                    snapshot.busy = false;
                    snapshot.activity.clear();
                    snapshot.answerIsError = h.isError;
                    snapshot.answer = !h.text.empty() ? h.text : (h.interrupted ? std::wstring{ L"Stopped." } : std::wstring{});
                    e.kind = h.isError ? SessionSearchEventKind::Failed : SessionSearchEventKind::Answer;
                    e.text = snapshot.answer;
                    notify = true;
                    save = true;
                    break;
                case HarnessEventKind::Error:
                    // A start failure. A request that waited for the process gets its own
                    // TurnCompleted right after; this covers a failed Prewarm.
                    snapshot.activity.clear();
                    snapshot.answerIsError = true;
                    snapshot.answer = h.text;
                    e.kind = SessionSearchEventKind::Failed;
                    e.text = h.text;
                    notify = true;
                    break;
                case HarnessEventKind::Notice:
                    if (h.isError)
                    {
                        snapshot.answerIsError = true;
                        snapshot.answer = h.text;
                        e.kind = SessionSearchEventKind::Failed;
                        e.text = h.text;
                        notify = true;
                    }
                    break;
                case HarnessEventKind::UserMessage: // Ask already reported it
                case HarnessEventKind::AssistantDelta:
                case HarnessEventKind::ToolResult: // the toolset's callbacks report what the calls did
                default:
                    break;
                }
            }
            if (save)
            {
                SaveView();
            }
            if (notify)
            {
                Notify(std::move(e));
            }
        }
    };

    SessionSearchAgent::SessionSearchAgent(SessionSearchAgentOptions options) :
        _impl{ std::make_unique<Impl>() }
    {
        Impl* const impl = _impl.get();
        impl->options = std::move(options);
        if (!IsValidHarnessName(impl->options.name))
        {
            throw std::invalid_argument("SessionSearchAgentOptions::name must be 1 to 48 characters of a-z, 0-9 and '-'.");
        }

        SessionSearchToolset::Callbacks callbacks;
        callbacks.catalog = [impl](int64_t since, const std::function<bool()>& cancelled) { return impl->CatalogFor(since, cancelled); };
        callbacks.generation = [impl] { return impl->requestGeneration.load(); };
        callbacks.onSearching = [impl](const std::wstring& summary) { impl->OnSearching(summary); };
        callbacks.onCandidates = [impl](const std::vector<std::wstring>& ids) { impl->OnCandidates(ids); };
        callbacks.onShow = [impl](const std::vector<std::wstring>& ids, const std::wstring& note, const std::vector<std::wstring>& whys) { impl->OnShow(ids, note, whys); };
        // The window and tab tools (SESSIONS.md §9.14): the host's implementations as they are, except the request's
        // origin, which only this object knows (Ask records it per request).
        callbacks.tabs = impl->options.tabTools;
        if (callbacks.tabs.census)
        {
            callbacks.tabs.originWindowId = [impl] {
                std::lock_guard lock{ impl->mutex };
                return impl->requestOrigin;
            };
        }
        const bool database = !impl->options.sessionDbProfile.empty();
        if (database)
        {
            // SESSIONS_DB.md §8.2: the database's read tools instead of the page search (each call forwarded to the
            // profile's service), and `show` resolves the short ids those tools print.
            const std::wstring profile = impl->options.sessionDbProfile;
            callbacks.database.call = [profile](const std::string& tool, const json::Value& arguments) { return CallDatabaseTool(profile, tool, arguments); };
            callbacks.database.resolve = [profile](const std::vector<std::wstring>& handles) { return ResolveDatabaseIds(profile, handles); };
            LogAgent(L"the AI search uses the session database (profile " + profile + L")");
        }
        impl->toolset = std::make_unique<SessionSearchToolset>(std::move(callbacks));

        HarnessOptions h;
        h.name = impl->options.name;
        h.claudeExe = impl->options.claudeExe ? impl->options.claudeExe : std::function<std::wstring()>{ [impl] {
            std::lock_guard lock{ impl->mutex };
            return impl->claudeExePath;
        } };
        h.model = impl->options.model;
        // The start level (SetThinkingLevel changes it later, live). Off = no thinking blocks
        // before a tool call, at effort low: the fastest answer.
        h.thinking = HarnessThinkingLevelThinks(impl->options.thinkingLevel) ? HarnessThinking::Adaptive : HarnessThinking::Disabled;
        h.effort = HarnessThinkingLevelEffort(impl->options.thinkingLevel);
        h.systemPrompt = database ? SessionSearchToolset::DatabaseSystemPrompt() : SessionSearchToolset::SystemPrompt();
        h.toolServers = impl->toolset->Servers();
        h.autoCompactTokens = impl->options.autoCompactTokens;
        h.persistRecord = impl->options.persist;
        // One request = the plan and its searches (maybe a few) + read the candidates (and a grep for
        // the user's own words) + show + the reply: about five model steps since the read-before-show
        // rule of 2026-10-17 (SESSIONS.md 9.12). A request that also opens a tab adds list + resume /
        // fork (SESSIONS.md 9.14), and a refused call (no window named, a turn in flight) one more
        // step, so the cap rose from 12 to 16. A model that keeps searching is stopped here, with
        // whatever it showed so far.
        h.maxTurnsPerMessage = 16;
        h.apiMaxRetries = 2;
        h.turnSilenceTimeoutMs = 90000;
        impl->conversation = std::make_unique<HarnessConversation>(std::move(h), [impl](const HarnessEvent& e) { impl->OnHarnessEvent(e); });

        // What the last run left: the conversation's record (loaded by the conversation) and,
        // beside it, the last shown list.
        const HarnessRecord record = impl->conversation->Record();
        std::lock_guard lock{ impl->mutex };
        impl->snapshot.epoch = impl->conversation->Epoch();
        impl->snapshot.turns = record.turns;
        impl->snapshot.usage = record.usage;
        impl->snapshot.model = record.model;
        impl->snapshot.compactions = record.compactions;
        impl->snapshot.thinkingLevel = impl->options.thinkingLevel;
        impl->latestTurn = record.turns;
        if (impl->options.persist)
        {
            impl->viewPath = HarnessStateDir() + L"\\" + impl->options.name + L".view.json";
            if (!record.Empty())
            {
                // Only together with its record: a view without the conversation it belongs to
                // would show a result nobody can refer to.
                DeserializeViewInto(ReadViewFile(impl->viewPath), impl->snapshot);
            }
        }
    }

    SessionSearchAgent::~SessionSearchAgent()
    {
        Shutdown();
    }

    uint64_t SessionSearchAgent::AddListener(Listener listener)
    {
        if (!listener)
        {
            return 0;
        }
        std::lock_guard lock{ _impl->mutex };
        const uint64_t token = _impl->nextListenerToken++;
        _impl->listeners.push_back(Impl::ListenerEntry{ token, std::move(listener) });
        return token;
    }

    void SessionSearchAgent::RemoveListener(uint64_t token) noexcept
    {
        if (token == 0)
        {
            return;
        }
        try
        {
            std::lock_guard lock{ _impl->mutex };
            auto& list = _impl->listeners;
            list.erase(std::remove_if(list.begin(), list.end(), [token](const Impl::ListenerEntry& l) { return l.token == token; }), list.end());
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchAgent::RemoveListener");
        }
    }

    void SessionSearchAgent::SetClaudeExePath(std::wstring path)
    {
        std::lock_guard lock{ _impl->mutex };
        _impl->claudeExePath = std::move(path);
    }

    void SessionSearchAgent::Prewarm(bool readIndex)
    {
        if (readIndex)
        {
            _impl->EnsureCatalog(NowUnixMs() - kDefaultCatalogMs);
        }
        _impl->conversation->Prewarm();
    }

    uint64_t SessionSearchAgent::Ask(std::wstring text, std::wstring originWindowId)
    {
        if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos)
        {
            return 0;
        }
        {
            std::lock_guard lock{ _impl->mutex };
            if (_impl->shuttingDown)
            {
                return 0;
            }
            _impl->snapshot.busy = true;
            _impl->snapshot.question = text;
            _impl->snapshot.answer.clear();
            _impl->snapshot.answerIsError = false;
            _impl->snapshot.activity = L"Thinking\x2026";
            // A new request: its own plan, and the window it came from (the tabs server's asked_from).
            _impl->snapshot.narration.clear();
            _impl->pendingAssistantText.clear();
            _impl->requestOrigin = std::move(originWindowId);
        }
        // The index is read while the model reads the request: by the time it calls the search
        // tool (about a second later) the catalog is there.
        _impl->EnsureCatalog(NowUnixMs() - kDefaultCatalogMs);
        // BEFORE the send: a search of the request that this one replaces must see the change
        // and return, or the interrupt that Send makes waits behind it.
        ++_impl->requestGeneration;
        const uint64_t turn = _impl->conversation->Send(text);
        {
            std::lock_guard lock{ _impl->mutex };
            _impl->latestTurn = (std::max)(_impl->latestTurn, turn);
            _impl->snapshot.turns = (std::max)(_impl->snapshot.turns, turn);
        }
        LogAgent(L"ask[" + std::to_wstring(turn) + L"]: " + text.substr(0, 200));
        SessionSearchEvent e;
        e.kind = SessionSearchEventKind::Asked;
        e.turn = turn;
        e.text = std::move(text);
        _impl->Notify(std::move(e));
        return turn;
    }

    void SessionSearchAgent::Interrupt()
    {
        ++_impl->requestGeneration; // first: a running search returns, so the interrupt can be delivered
        _impl->conversation->Interrupt();
    }

    void SessionSearchAgent::SetThinkingLevel(HarnessThinkingLevel level) noexcept
    {
        try
        {
            {
                std::lock_guard lock{ _impl->mutex };
                if (_impl->shuttingDown || _impl->snapshot.thinkingLevel == level)
                {
                    return; // nothing changes: no event, no log line (the pages seed the level at every AI-on)
                }
                _impl->snapshot.thinkingLevel = level;
                // Under our lock, so that two windows that set levels at the same moment leave the
                // snapshot and the conversation agreeing on the last one. Safe: the conversation
                // never calls into this object while it holds its own lock (its sink runs outside
                // it), so the two locks are only ever taken in this order.
                _impl->conversation->SetThinking(HarnessThinkingLevelThinks(level) ? HarnessThinking::Adaptive : HarnessThinking::Disabled, HarnessThinkingLevelEffort(level));
            }
            LogAgent(std::wstring{ L"thinking -> " } + HarnessThinkingLevelWord(level) + L" (from the next request)");
            SessionSearchEvent e;
            e.kind = SessionSearchEventKind::StateChanged; // every window repaints its strip from the snapshot
            _impl->Notify(std::move(e));
        }
        catch (...)
        {
            // Rule #18. Recovery: the level the snapshot holds stays in effect for the next request.
            LogSwallowedException(L"SessionSearchAgent::SetThinkingLevel");
        }
    }

    void SessionSearchAgent::Reset()
    {
        {
            std::lock_guard lock{ _impl->mutex };
            if (_impl->shuttingDown)
            {
                return;
            }
        }
        ++_impl->requestGeneration; // a running search returns at once and records nothing
        _impl->toolset->Reset();
        _impl->conversation->Reset(); // raises the epoch at once; the worker ends the process
        {
            std::lock_guard lock{ _impl->mutex };
            const std::wstring model = _impl->snapshot.model; // the next conversation uses the same one
            // The thinking level is the user's setting, not part of the conversation: a reset
            // keeps it (and the model's levels, which the next process reports again anyway).
            const HarnessThinkingLevel thinkingLevel = _impl->snapshot.thinkingLevel;
            std::vector<std::wstring> supportedEfforts = std::move(_impl->snapshot.supportedEfforts);
            _impl->snapshot = SessionSearchSnapshot{};
            _impl->snapshot.epoch = _impl->conversation->Epoch();
            _impl->snapshot.state = _impl->conversation->State();
            _impl->snapshot.model = model;
            _impl->snapshot.thinkingLevel = thinkingLevel;
            _impl->snapshot.supportedEfforts = std::move(supportedEfforts);
            _impl->latestTurn = 0;
            // The thrown-away conversation's half-written text and its request's window go with it.
            _impl->pendingAssistantText.clear();
            _impl->requestOrigin.clear();
        }
        _impl->SaveView(); // an empty conversation: deletes the file
        LogAgent(L"reset");
        SessionSearchEvent e;
        e.kind = SessionSearchEventKind::Reset;
        _impl->Notify(std::move(e));
    }

    void SessionSearchAgent::ReleaseCatalog()
    {
        std::shared_ptr<const SessionCatalog> old;
        {
            std::lock_guard lock{ _impl->mutex };
            old = std::move(_impl->catalog);
            _impl->catalog.reset();
            // Nobody wants an index now: the keeper stops at its next sidecar file and
            // publishes nothing. The sidecar files it wrote stay, so the next build continues
            // where this one stopped.
            _impl->wantSince = 0;
            ++_impl->keeperEpoch;
        }
        _impl->catalogChanged.notify_all();
        // `old` is released here, outside the lock: freeing a large catalog takes a moment.
    }

    void SessionSearchAgent::ParkProcessAfter(int64_t delayMs)
    {
        _impl->conversation->ParkAfter(delayMs);
    }

    void SessionSearchAgent::Shutdown() noexcept
    {
        try
        {
            std::future<void> pending;
            {
                std::lock_guard lock{ _impl->mutex };
                _impl->shuttingDown = true;
                ++_impl->keeperEpoch; // the keeper's running step stops at its next sidecar file
                pending = std::move(_impl->keeper);
            }
            ++_impl->requestGeneration; // a running search returns at once
            _impl->catalogChanged.notify_all(); // ... and so does one that waits for an index
            if (_impl->conversation)
            {
                _impl->conversation->Shutdown(); // no event and no tool call after this returns
            }
            if (pending.valid())
            {
                pending.wait(); // the keeper uses this object: it must be gone before the object is
            }
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchAgent::Shutdown");
        }
    }

    SessionSearchSnapshot SessionSearchAgent::Snapshot() const
    {
        std::lock_guard lock{ _impl->mutex };
        return _impl->snapshot;
    }

    std::vector<SessionIndexEntry> SessionSearchAgent::EntriesFor(const std::vector<std::wstring>& ids) const
    {
        return _impl->EntriesFor(ids);
    }

    // The three diagnostics read the conversation object directly: it exists from the
    // constructor to the destructor (Shutdown ends its process, not the object), and each of its
    // accessors takes the conversation's own lock.
    HarnessRecord SessionSearchAgent::Record() const
    {
        return _impl->conversation->Record();
    }

    HarnessInventory SessionSearchAgent::Inventory() const
    {
        return _impl->conversation->Inventory();
    }

    std::vector<std::wstring> SessionSearchAgent::DescribeCommandLine() const
    {
        return _impl->conversation->DescribeCommandLine();
    }

    SessionSearchAgent& SharedSessionSearchAgent()
    {
        // Leaked on purpose (see the header). A function-local static: created once, thread-safe.
        static SessionSearchAgent* const agent = [] {
            SessionSearchAgentOptions options;
            // The persisted thinking level (the Sessions page's strip toggle): the agent STARTS with
            // it, so its first process has it on the command line. An unknown word reads as Off.
            try
            {
                const AppSettings settings = LoadAppSettings();
                if (const auto level = ParseHarnessThinkingLevel(settings.sessionsAiThinkingLevel))
                {
                    options.thinkingLevel = *level;
                }
                // SESSIONS_DB.md §8.2: with the session database On, the AI search searches it (its read tools, no
                // index keeper). Read once: the agent lives for the process, so a later change applies at the next
                // start of the app (the cog's tooltip says so). Shadow keeps the page search (nothing on screen
                // changes in Shadow). A service that cannot start (no duckdb.dll beside it) keeps the page search too:
                // a conversation handed the database's tools would answer every request "the database is not running".
                options.sessionDbProfile = SearchDatabaseProfileIfUsable(settings.sessionDbMode == SessionDbMode::On, L"sessions-search");
            }
            catch (...)
            {
                // Rule #18. Recovery: the agent starts at Off with the page search; the page's toggle still sets a level.
                LogSwallowedException(L"SharedSessionSearchAgent: read the settings");
            }
            // SESSIONS.md §9.14: the window and tab tools over the app's windows (list, resume, fork, deactivate).
            options.tabTools = MakeEngineTabTools();
            options.openSessions = [] {
                std::vector<std::pair<std::wstring, std::wstring>> open;
                const auto registry = SharedEngine().registry;
                if (!registry)
                {
                    return open;
                }
                // The two lean registry reads: the live ids, then their titles — never a snapshot
                // of whole records (UI_THREAD.md §10a).
                std::vector<std::wstring> ids;
                for (const auto& binding : registry->LiveTabBindings())
                {
                    ids.push_back(binding.id);
                }
                for (auto& headline : registry->HeadlinesOf(ids))
                {
                    open.emplace_back(std::move(headline.id), std::move(headline.title));
                }
                return open;
            };
            return new SessionSearchAgent{ std::move(options) };
        }();
        return *agent;
    }

    SessionSearchToolset::Callbacks::TabTools MakeEngineTabTools()
    {
        SessionSearchToolset::Callbacks::TabTools tools;
        tools.census = [] {
            // The strips from the windows (their UI thread), the session facts from the registry (here).
            std::vector<WindowCensus> windows = CollectWindowCensus(kWindowCensusTimeoutMs);
            EnrichWindowCensus(windows);
            return windows;
        };
        tools.open = [](const std::wstring& windowId, const WindowToolRequest& request) -> WindowToolOutcome {
            WindowToolRequest filled = request;
            // The folder and the title a launch needs, read HERE (the tool-call thread) so the window's UI thread does
            // no file work: the registry's record when it knows the session, else the session's own transcript index (a
            // session that was never managed, found by a search). A known session's record wins in the window anyway.
            const auto registry = SharedEngine().registry;
            const std::optional<SessionInfo> known = registry ? registry->GetWithoutQueue(filled.sessionId) : std::nullopt;
            if (known)
            {
                if (filled.dir.empty())
                {
                    filled.dir = known->workingDir;
                }
                if (filled.title.empty())
                {
                    filled.title = known->title;
                }
            }
            else if (const auto entry = IndexEntryForSession(filled.sessionId))
            {
                if (filled.dir.empty())
                {
                    filled.dir = entry->stats.cwd;
                }
                if (filled.title.empty())
                {
                    // The Sessions page's precedence; "" lets the window derive a name from the folder.
                    filled.title = !entry->liveTitle.empty() ? entry->liveTitle : PickDisplayTitle(entry->stats.customTitle, entry->stats.aiTitle, entry->stats.summary, entry->stats.firstUserPrompt);
                }
            }
            if (!known && filled.dir.empty())
            {
                WindowToolOutcome refused;
                refused.handled = true;
                refused.message = L"its folder is not known: the session's transcript was not found on this computer (it may have been deleted), so it cannot be opened.";
                return refused;
            }
            const auto outcome = RunWindowTool(windowId, filled, kWindowOpenTimeoutMs);
            LogAgent(std::wstring{ filled.kind == WindowToolRequest::Kind::Fork ? L"tool fork " : L"tool resume " } + ShortId(filled.sessionId) + L" -> window " + ShortId(windowId) +
                     (outcome ? (outcome->ok ? L": opened " + ShortId(outcome->sessionId) + L" at index " + std::to_wstring(outcome->index) : L": refused (" + outcome->message + L")") : std::wstring{ L": no answer in time" }));
            return outcome ? *outcome : WindowToolOutcome{};
        };
        tools.deactivate = [](const std::wstring& sessionId) -> WindowToolOutcome {
            WindowToolRequest request;
            request.kind = WindowToolRequest::Kind::Deactivate;
            request.sessionId = sessionId;
            const auto outcome = RunWindowToolOnHost(request, kWindowDeactivateTimeoutMs);
            LogAgent(L"tool deactivate " + ShortId(sessionId) + (outcome ? (outcome->ok ? std::wstring{ L": parked" } : L": refused (" + outcome->message + L")") : std::wstring{ L": no window hosts it, or none answered in time" }));
            return outcome ? *outcome : WindowToolOutcome{};
        };
        tools.focus = [](const std::wstring& sessionId) {
            // Every window's activate sink; the one that hosts the tab selects it and comes to the front. Fire-and-
            // forget: the tool says it asked, and the next list shows the result.
            ActivateSessionInOtherWindows(sessionId, std::wstring{});
            return true;
        };
        return tools;
    }

    std::wstring SearchDatabaseProfileIfUsable(bool databaseModeOn, std::wstring_view who)
    {
        if (!databaseModeOn)
        {
            return {};
        }
        const std::wstring profile = AgentmasterStateDir();
        if (SessionDb::SessionDbServiceInstalled(profile))
        {
            return profile;
        }
        // The mode is On but the service cannot start: the page search keeps the AI search useful, and the log says why.
        LogAgent(std::wstring{ who } + L": the session database mode is On, but its service is not installed (agentmaster-sessiondb.exe with duckdb.dll beside it, in the package or in " + profile +
                 L"\\sessiondb\\bin): this conversation reads the transcripts directly");
        return {};
    }
}
