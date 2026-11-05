// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — SessionSearchLog: the AI search's conversation log (SessionSearchLog.h has the design and the line format).

#include "SessionSearchLog.h"

#include "AgentHarness.h" // HarnessEvent / HarnessEventKind (types only: this unit needs no SDK)
#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException
#include "Json.h"
#include "StateBackups.h" // CurrentWriter / WriterText — the running build, on the "start" line

#include <windows.h>

#include <chrono>
#include <cstdio> // swprintf_s
#include <ctime> // _localtime64_s
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <utility>

namespace Agentmaster
{
    namespace
    {
        // The most requests a log keeps open at once. A request is open from its ask to its end, so more than a few means
        // ends that never came (a conversation torn down mid-request); past this the oldest is forgotten, so a log that
        // lives for days never grows without bound. Its lines stay in the file; only its "at_ms" bookkeeping goes.
        constexpr size_t kMaxOpenRequests = 64;
        constexpr int64_t kMsPerDay = 86'400'000;

        // The system clock, in unix ms. Never throws.
        int64_t SystemNowUnixMs() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        // `unixMs` as LOCAL calendar time: the files are dated, and the lines stamped, by the user's own day (the date they
        // will look for). A time the CRT cannot convert reads as a zeroed date ("0000-00-00"): its lines still land in a
        // file, under a name no prune ever matches. Never throws.
        std::tm LocalTimeOf(int64_t unixMs) noexcept
        {
            std::tm tm{};
            const __time64_t seconds = static_cast<__time64_t>(unixMs / 1000);
            if (_localtime64_s(&tm, &seconds) != 0)
            {
                tm = std::tm{};
                tm.tm_year = -1900; // year 0, so the zeroed date reads "0000-..." rather than "1900-..."
                tm.tm_mon = -1;
            }
            return tm;
        }

        // "yyyy-MM-dd", the local date of `unixMs`: the files' date part, and the prune's cutoff (ISO dates compare as text).
        //   throws - std::bad_alloc only.
        std::wstring LocalDateText(int64_t unixMs)
        {
            const std::tm tm = LocalTimeOf(unixMs);
            wchar_t buf[16]{};
            swprintf_s(buf, L"%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            return buf;
        }

        // "yyyy-MM-dd HH:mm:ss.mmm", the local time of `unixMs`: every line's "at", for a person reading the file ("t" keeps
        // the exact instant for a program).
        //   throws - std::bad_alloc only.
        std::wstring LocalStampText(int64_t unixMs)
        {
            const std::tm tm = LocalTimeOf(unixMs);
            const int ms = static_cast<int>(((unixMs % 1000) + 1000) % 1000); // a pre-1970 time stays in 0..999
            wchar_t buf[32]{};
            swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
            return buf;
        }

        // Whether `name` is one of THIS conversation's log files ("<conversation>-yyyy-MM-dd.jsonl"), and its date. The shape
        // is checked character by character, so the prune never deletes another conversation's files ("sessions-search" is
        // not a prefix match of "sessions-search-old-...") nor anything the user put in the folder.
        //   name         - a file name (no folder).
        //   conversation - the conversation's name.
        //   date         - out: the "yyyy-MM-dd" part when this returns true.
        //   returns      - true for exactly that shape.
        //   throws       - std::bad_alloc only.
        bool IsLogFileOf(std::wstring_view name, std::wstring_view conversation, std::wstring& date)
        {
            constexpr std::wstring_view kExtension = L".jsonl";
            constexpr size_t kDateLength = 10; // yyyy-MM-dd
            if (name.size() != conversation.size() + 1 + kDateLength + kExtension.size() || name.substr(0, conversation.size()) != conversation ||
                name[conversation.size()] != L'-' || name.substr(name.size() - kExtension.size()) != kExtension)
            {
                return false;
            }
            const std::wstring_view d = name.substr(conversation.size() + 1, kDateLength);
            for (size_t i = 0; i < d.size(); ++i)
            {
                const bool dash = (i == 4 || i == 7);
                if (dash ? d[i] != L'-' : (d[i] < L'0' || d[i] > L'9'))
                {
                    return false;
                }
            }
            date.assign(d);
            return true;
        }

        // Append ONE line's bytes to `path`, creating the file when it is missing. FILE_APPEND_DATA alone makes every write
        // land at the end of the file, and the share modes let a reader (the review script, an editor) and a delete (the user
        // clearing the folder) coexist with a log that is still writing.
        //   path    - the file.
        //   bytes   - the line, UTF-8, ending in "\n".
        //   error   - out: the Win32 error of a failure (0 on success).
        //   returns - true when every byte was written.
        //   throws  - nothing.
        bool AppendBytes(const std::wstring& path, const std::string& bytes, DWORD& error) noexcept
        {
            error = 0;
            const HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = ::GetLastError();
                return false;
            }
            DWORD written = 0;
            const BOOL ok = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
            if (!ok)
            {
                error = ::GetLastError();
            }
            ::CloseHandle(file);
            return ok && written == bytes.size();
        }

        // A JSON number from an integer count (json::Value holds doubles; every count here is far below 2^53).
        json::Value Num(int64_t n)
        {
            return json::Value::MkNum(static_cast<double>(n));
        }
    }

    std::wstring SessionSearchLogFileName(std::wstring_view conversation, int64_t unixMs)
    {
        return std::wstring{ conversation } + L"-" + LocalDateText(unixMs) + L".jsonl";
    }

    struct SessionSearchLog::Impl
    {
        // One tool call that has no result yet: its number within the request, and when it started.
        struct OpenCall
        {
            uint32_t seq{ 0 };
            int64_t startUnixMs{ 0 };
        };

        // One request between its ask and its end.
        struct Request
        {
            int64_t askedUnixMs{ 0 }; // 0 = not seen (a call of a request this log was never told about): no at_ms, no usage share
            HarnessUsage usageAtAsk; // the conversation's totals at the ask: the base of the request's share
            uint32_t nextSeq{ 1 };
            // Per tool, its calls that wait for their result, oldest first: the SDK's reader thread runs a tool's calls in
            // order, so the oldest open one is the call a result answers.
            std::map<std::wstring, std::deque<OpenCall>> open;
        };

        Options options;
        std::mutex mutex; // guards everything below AND the file writes, so lines stay whole and in order
        bool started{ false }; // the "start" line and the prune ran (the first write of this object)
        bool failureReported{ false }; // a failed write was reported to hooks.log (once: a full disk must not flood it)
        std::map<std::pair<uint64_t, uint64_t>, Request> requests; // (epoch, turn) -> the open request

        // The clock (Options::nowUnixMs, else the system's). Never throws past the test clock's own.
        int64_t Now() const
        {
            return options.nowUnixMs ? options.nowUnixMs() : SystemNowUnixMs();
        }

        // A line's common head: the format, the kind, the local stamp and the instant, the conversation, the epoch, the turn.
        //   throws - std::bad_alloc only.
        json::Value Head(const wchar_t* kind, uint64_t epoch, uint64_t turn, int64_t now) const
        {
            json::Value o = json::Value::MkObj();
            o.Set(L"v", Num(kSessionSearchLogFormat));
            o.Set(L"kind", json::Value::MkStr(kind));
            o.Set(L"at", json::Value::MkStr(LocalStampText(now)));
            o.Set(L"t", Num(now));
            o.Set(L"conv", json::Value::MkStr(options.conversation));
            o.Set(L"epoch", Num(static_cast<int64_t>(epoch)));
            o.Set(L"turn", Num(static_cast<int64_t>(turn)));
            return o;
        }

        // The open request (epoch, turn), or null. Under the mutex.
        Request* FindLocked(uint64_t epoch, uint64_t turn)
        {
            const auto it = requests.find({ epoch, turn });
            return it != requests.end() ? &it->second : nullptr;
        }

        // The open request (epoch, turn), opened unseen when this log was never told about it (askedUnixMs 0). Under the mutex.
        //   throws - std::bad_alloc only.
        Request& EnsureLocked(uint64_t epoch, uint64_t turn)
        {
            if (Request* found = FindLocked(epoch, turn))
            {
                return *found;
            }
            if (requests.size() >= kMaxOpenRequests)
            {
                requests.erase(requests.begin()); // the oldest (the smallest epoch, then turn): see kMaxOpenRequests
            }
            return requests[{ epoch, turn }];
        }

        // "at_ms" of a line of `request` written at `now`, when the request's ask was seen. Under the mutex.
        static void SetAtMs(json::Value& line, const Request* request, int64_t now)
        {
            if (request && request->askedUnixMs > 0)
            {
                line.Set(L"at_ms", Num(now - request->askedUnixMs));
            }
        }

        // Delete this conversation's files dated before the cutoff (today less keepDays). Only the exact file shape
        // (IsLogFileOf) is ever touched. A failed delete is reported and left for the next process. Under the mutex.
        //   throws - std::bad_alloc only (a filesystem error is an error code here, never an exception).
        void PruneLocked(int64_t now)
        {
            if (options.keepDays <= 0)
            {
                return;
            }
            const std::wstring cutoff = LocalDateText(now - static_cast<int64_t>(options.keepDays) * kMsPerDay);
            std::error_code ec;
            std::filesystem::directory_iterator it{ options.dir, ec };
            const std::filesystem::directory_iterator end;
            size_t deleted = 0;
            while (!ec && it != end)
            {
                std::wstring date;
                const std::wstring name = it->path().filename().wstring();
                if (IsLogFileOf(name, options.conversation, date) && date < cutoff)
                {
                    std::error_code removeError;
                    if (std::filesystem::remove(it->path(), removeError))
                    {
                        ++deleted;
                    }
                    else if (removeError)
                    {
                        AppendStateLog(L"hooks.log", L"[ai-search] " + options.conversation + L": the conversation log could not delete " + name + L" (" + std::to_wstring(removeError.value()) + L")\n");
                    }
                }
                it.increment(ec);
            }
            if (deleted > 0)
            {
                AppendStateLog(L"hooks.log", L"[ai-search] " + options.conversation + L": the conversation log deleted " + std::to_wstring(deleted) + L" file(s) dated before " + cutoff + L" (kept: " +
                                                 std::to_wstring(options.keepDays) + L" days)\n");
            }
        }

        // Append `line` to today's file. The first write of this object first creates the folder, prunes old files and writes
        // the "start" line. A failure is reported once to hooks.log; the line is then lost (the search goes on). Under the
        // mutex.
        //   throws - std::bad_alloc only.
        void WriteLocked(const json::Value& line, int64_t now)
        {
            if (!started)
            {
                started = true; // first: a failure below must not retry the prune and the start line for every line
                std::error_code ec;
                std::filesystem::create_directories(options.dir, ec); // an existing folder is no error
                PruneLocked(now);
                json::Value start = Head(L"start", 0, 0, now);
                start.Set(L"build", json::Value::MkStr(StateBackups::WriterText(StateBackups::CurrentWriter())));
                start.Set(L"pid", Num(static_cast<int64_t>(::GetCurrentProcessId())));
                start.Set(L"keep_days", Num(options.keepDays));
                AppendLocked(start, now);
            }
            AppendLocked(line, now);
        }

        // One line to the file of `now`'s local day. Under the mutex.
        //   throws - std::bad_alloc only.
        void AppendLocked(const json::Value& line, int64_t now)
        {
            const std::wstring path = options.dir + L"\\" + SessionSearchLogFileName(options.conversation, now);
            // json::Dump writes ONE line: it escapes every control character, a line break included.
            const std::string bytes = HarnessToUtf8(json::Dump(line)) + "\n";
            DWORD error = 0;
            if (!AppendBytes(path, bytes, error) && !failureReported)
            {
                failureReported = true;
                AppendStateLog(L"hooks.log", L"[ai-search] " + options.conversation + L": the conversation log could not be written to " + path + L" (error " + std::to_wstring(error) +
                                                 L"); its lines are lost until the next start\n");
            }
        }

        // The head of the "end" line of request (epoch, turn): its `status` and, when its ask was seen, "elapsed_ms". The caller
        // adds its own fields, writes the line, and then forgets the request (requests.erase). Under the mutex.
        //   throws - std::bad_alloc only.
        json::Value EndHeadLocked(uint64_t epoch, uint64_t turn, const wchar_t* status, int64_t now)
        {
            json::Value line = Head(L"end", epoch, turn, now);
            line.Set(L"status", json::Value::MkStr(status));
            if (const Request* request = FindLocked(epoch, turn); request && request->askedUnixMs > 0)
            {
                line.Set(L"elapsed_ms", Num(now - request->askedUnixMs));
            }
            return line;
        }
    };

    SessionSearchLog::SessionSearchLog(Options options) :
        _impl{ std::make_unique<Impl>() }
    {
        _impl->options = std::move(options);
    }

    SessionSearchLog::~SessionSearchLog() = default;

    void SessionSearchLog::Asked(uint64_t epoch, uint64_t turn, std::wstring_view text, std::wstring_view origin, std::wstring_view thinking, std::wstring_view model, const HarnessUsage& usageNow) noexcept
    {
        if (turn == 0)
        {
            return; // an ignored request (an empty text, a shut-down agent): nothing was sent
        }
        try
        {
            std::lock_guard lock{ _impl->mutex };
            const int64_t now = _impl->Now();
            Impl::Request& request = _impl->EnsureLocked(epoch, turn);
            request.askedUnixMs = now;
            request.usageAtAsk = usageNow;
            json::Value line = _impl->Head(L"ask", epoch, turn, now);
            line.Set(L"text", json::Value::MkStr(std::wstring{ text }));
            line.Set(L"origin", json::Value::MkStr(std::wstring{ origin }));
            line.Set(L"thinking", json::Value::MkStr(std::wstring{ thinking }));
            line.Set(L"model", json::Value::MkStr(std::wstring{ model }));
            _impl->WriteLocked(line, now);
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchLog::Asked"); // the line is lost; the search goes on
        }
    }

    void SessionSearchLog::Narration(uint64_t epoch, uint64_t turn, std::wstring_view text) noexcept
    {
        try
        {
            std::lock_guard lock{ _impl->mutex };
            const int64_t now = _impl->Now();
            json::Value line = _impl->Head(L"narration", epoch, turn, now);
            Impl::SetAtMs(line, _impl->FindLocked(epoch, turn), now);
            line.Set(L"text", json::Value::MkStr(std::wstring{ text }));
            _impl->WriteLocked(line, now);
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchLog::Narration");
        }
    }

    void SessionSearchLog::Harness(const HarnessEvent& event) noexcept
    {
        try
        {
            std::lock_guard lock{ _impl->mutex };
            const int64_t now = _impl->Now();
            switch (event.kind)
            {
            case HarnessEventKind::ToolCall:
            {
                Impl::Request& request = _impl->EnsureLocked(event.epoch, event.turn);
                const uint32_t seq = request.nextSeq++;
                request.open[event.toolName].push_back(Impl::OpenCall{ seq, now });
                json::Value line = _impl->Head(L"call", event.epoch, event.turn, now);
                line.Set(L"seq", Num(seq));
                line.Set(L"tool", json::Value::MkStr(event.toolName));
                line.Set(L"arguments", json::Value::MkStr(event.argumentsJson));
                Impl::SetAtMs(line, &request, now);
                _impl->WriteLocked(line, now);
                break;
            }
            case HarnessEventKind::ToolResult:
            {
                Impl::Request* request = _impl->FindLocked(event.epoch, event.turn);
                json::Value line = _impl->Head(L"result", event.epoch, event.turn, now);
                // The oldest open call of this tool is the one answered (see Impl::Request::open); a result with no open call
                // (its call came before this log existed) gets seq 0 and no duration.
                uint32_t seq = 0;
                if (request)
                {
                    if (const auto it = request->open.find(event.toolName); it != request->open.end() && !it->second.empty())
                    {
                        const Impl::OpenCall call = it->second.front();
                        it->second.pop_front();
                        seq = call.seq;
                        line.Set(L"ms", Num(now - call.startUnixMs));
                    }
                }
                line.Set(L"seq", Num(seq));
                line.Set(L"tool", json::Value::MkStr(event.toolName));
                Impl::SetAtMs(line, request, now);
                line.Set(L"error", json::Value::MkBool(event.isError));
                line.Set(L"chars", Num(static_cast<int64_t>(event.text.size())));
                line.Set(L"result", json::Value::MkStr(event.text)); // WHOLE: the text the model read
                _impl->WriteLocked(line, now);
                break;
            }
            case HarnessEventKind::TurnCompleted:
            {
                const wchar_t* status = event.interrupted ? L"interrupted" : (event.isError ? L"failed" : L"answered");
                json::Value line = _impl->EndHeadLocked(event.epoch, event.turn, status, now);
                line.Set(L"answer", json::Value::MkStr(event.text));
                line.Set(L"turn_ms", Num(event.durationMs));
                line.Set(L"thinking_blocks", Num(event.thinkingBlocks));
                line.Set(L"thinking_tokens", Num(event.thinkingTokens));
                // The request's share of the conversation's totals: the totals now less those at its ask (a total can only
                // grow; a smaller one — a counter that started over — counts as 0, never as a negative share).
                json::Value usage = json::Value::MkObj();
                const Impl::Request* request = _impl->FindLocked(event.epoch, event.turn);
                if (request && request->askedUnixMs > 0)
                {
                    const HarnessUsage& a = request->usageAtAsk;
                    const HarnessUsage& b = event.usage;
                    const auto share = [](int64_t before, int64_t after) { return after > before ? after - before : 0; };
                    usage.Set(L"input", Num(share(a.inputTokens, b.inputTokens)));
                    usage.Set(L"output", Num(share(a.outputTokens, b.outputTokens)));
                    usage.Set(L"cache_read", Num(share(a.cacheReadTokens, b.cacheReadTokens)));
                    usage.Set(L"cache_write", Num(share(a.cacheCreationTokens, b.cacheCreationTokens)));
                    usage.Set(L"cost_usd", json::Value::MkNum(b.costUsd > a.costUsd ? b.costUsd - a.costUsd : 0.0));
                }
                usage.Set(L"context", Num(event.usage.contextTokens));
                usage.Set(L"window", Num(event.usage.contextWindow));
                line.Set(L"usage", std::move(usage));
                _impl->WriteLocked(line, now);
                _impl->requests.erase({ event.epoch, event.turn });
                break;
            }
            case HarnessEventKind::Ready:
            {
                json::Value line = _impl->Head(L"process", event.epoch, event.turn, now);
                line.Set(L"model", json::Value::MkStr(event.model));
                line.Set(L"cli", json::Value::MkStr(event.cliVersion));
                line.Set(L"permission", json::Value::MkStr(event.permissionMode));
                _impl->WriteLocked(line, now);
                break;
            }
            case HarnessEventKind::Compacted:
            {
                json::Value line = _impl->Head(L"compacted", event.epoch, event.turn, now);
                line.Set(L"pre", Num(event.preTokens));
                line.Set(L"post", Num(event.postTokens));
                line.Set(L"ms", Num(event.durationMs));
                _impl->WriteLocked(line, now);
                break;
            }
            case HarnessEventKind::Error:
            case HarnessEventKind::Notice:
            {
                if (event.kind == HarnessEventKind::Notice && !event.isError)
                {
                    break; // an informational notice (a restart said in words): the request's own lines tell the story
                }
                json::Value line = _impl->Head(L"failure", event.epoch, event.turn, now);
                Impl::SetAtMs(line, _impl->FindLocked(event.epoch, event.turn), now);
                line.Set(L"text", json::Value::MkStr(event.text));
                _impl->WriteLocked(line, now);
                break;
            }
            case HarnessEventKind::State:
            case HarnessEventKind::UserMessage:
            case HarnessEventKind::AssistantDelta:
            case HarnessEventKind::AssistantText:
            case HarnessEventKind::Compacting:
            default:
                break; // the agent reports the request and the narration itself; a state change says nothing new
            }
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchLog::Harness");
        }
    }

    void SessionSearchLog::Shown(uint64_t epoch, uint64_t turn, const std::vector<std::wstring>& ids, const std::vector<std::wstring>& whys, std::wstring_view note) noexcept
    {
        try
        {
            std::lock_guard lock{ _impl->mutex };
            const int64_t now = _impl->Now();
            json::Value line = _impl->Head(L"show", epoch, turn, now);
            Impl::SetAtMs(line, _impl->FindLocked(epoch, turn), now);
            json::Value idList = json::Value::MkArr();
            json::Value whyList = json::Value::MkArr();
            for (size_t i = 0; i < ids.size(); ++i)
            {
                idList.Push(json::Value::MkStr(ids[i]));
                whyList.Push(json::Value::MkStr(i < whys.size() ? whys[i] : std::wstring{})); // index-aligned with the ids
            }
            line.Set(L"ids", std::move(idList));
            line.Set(L"whys", std::move(whyList));
            line.Set(L"note", json::Value::MkStr(std::wstring{ note }));
            _impl->WriteLocked(line, now);
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchLog::Shown");
        }
    }

    void SessionSearchLog::Reset(uint64_t newEpoch) noexcept
    {
        try
        {
            std::lock_guard lock{ _impl->mutex };
            const int64_t now = _impl->Now();
            // Every request of the thrown-away conversation ends here: its own end would arrive with the old epoch, which the
            // agent drops (a reset makes every later event of that epoch stale).
            for (auto it = _impl->requests.begin(); it != _impl->requests.end();)
            {
                if (it->first.first < newEpoch)
                {
                    _impl->WriteLocked(_impl->EndHeadLocked(it->first.first, it->first.second, L"reset", now), now);
                    it = _impl->requests.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            _impl->WriteLocked(_impl->Head(L"reset", newEpoch, 0, now), now);
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchLog::Reset");
        }
    }

    void SessionSearchLog::ThinkingChanged(std::wstring_view level) noexcept
    {
        try
        {
            std::lock_guard lock{ _impl->mutex };
            const int64_t now = _impl->Now();
            json::Value line = _impl->Head(L"thinking", 0, 0, now);
            line.Set(L"level", json::Value::MkStr(std::wstring{ level }));
            _impl->WriteLocked(line, now);
        }
        catch (...)
        {
            LogSwallowedException(L"SessionSearchLog::ThinkingChanged");
        }
    }

    const std::wstring& SessionSearchLog::Directory() const noexcept
    {
        return _impl->options.dir;
    }
}
