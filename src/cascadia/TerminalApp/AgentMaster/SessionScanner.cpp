// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and the
// standalone test harness compiles it directly). The transcript tail is raw Win32 file I/O.
#define NOMINMAX
#include "SessionScanner.h"

#include "ClaudeSpawn.h" // ResolveClaudeTranscriptPath, AppendStateLog
#include "Json.h"
#include "SessionRegistry.h"

#include <windows.h>

#include <algorithm>
#include <chrono>

namespace
{
    int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // FILETIME (100ns ticks since 1601-01-01) -> Unix epoch milliseconds.
    int64_t FiletimeToUnixMs(const FILETIME& ft)
    {
        ULARGE_INTEGER u{};
        u.HighPart = ft.dwHighDateTime;
        u.LowPart = ft.dwLowDateTime;
        // 11644473600 s between 1601 and 1970, in 100ns ticks == 116444736000000000.
        if (u.QuadPart < 116444736000000000ULL)
        {
            return 0;
        }
        return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
    }

    // UTF-8 byte span -> UTF-16 (mirrors HooksBridge's helper). Empty on failure.
    std::wstring Utf8ToUtf16(const char* data, int len)
    {
        if (len <= 0)
        {
            return {};
        }
        const int needed = ::MultiByteToWideChar(CP_UTF8, 0, data, len, nullptr, 0);
        if (needed <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<size_t>(needed), L'\0');
        const int written = ::MultiByteToWideChar(CP_UTF8, 0, data, len, out.data(), needed);
        if (written <= 0)
        {
            return {};
        }
        out.resize(static_cast<size_t>(written));
        return out;
    }

    bool EndsWithQuestion(std::wstring_view s)
    {
        size_t b = s.size();
        while (b > 0 && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n'))
        {
            --b;
        }
        return b > 0 && s[b - 1] == L'?';
    }

    // Read the `cwd` recorded in a transcript's FIRST JSON line (every Claude transcript line
    // carries the working dir). Reads only the head (one buffer), shares all access so it never
    // blocks claude's append. Empty if the file can't be read or has no cwd yet.
    std::wstring ReadTranscriptCwd(const std::wstring& path)
    {
        const HANDLE h = ::CreateFileW(path.c_str(),
                                       GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_FLAG_SEQUENTIAL_SCAN,
                                       nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return {};
        }
        std::string bytes;
        bytes.resize(16384);
        DWORD got = 0;
        const BOOL ok = ::ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr);
        ::CloseHandle(h);
        if (!ok || got == 0)
        {
            return {};
        }
        bytes.resize(got);
        size_t start = 0;
        while (start < bytes.size())
        {
            const size_t nl = bytes.find('\n', start);
            const size_t end = (nl == std::string::npos) ? bytes.size() : nl;
            if (end > start)
            {
                const std::wstring wide = Utf8ToUtf16(bytes.data() + start, static_cast<int>(end - start));
                const auto parsed = Agentmaster::json::Parse(wide);
                if (parsed && parsed->type == Agentmaster::json::Value::Type::Obj)
                {
                    const std::wstring cwd = parsed->StrAt(L"cwd");
                    if (!cwd.empty())
                    {
                        return cwd;
                    }
                }
            }
            if (nl == std::string::npos)
            {
                break;
            }
            start = nl + 1;
        }
        return {};
    }

    // Concatenate the text blocks of a Claude message `content` (string, or array of blocks).
    std::wstring CollectText(const Agentmaster::json::Value* content)
    {
        using Agentmaster::json::Value;
        if (!content)
        {
            return {};
        }
        if (content->type == Value::Type::Str)
        {
            return content->str;
        }
        if (content->type == Value::Type::Arr)
        {
            std::wstring text;
            for (const auto& blk : content->arr)
            {
                if (blk.type == Value::Type::Obj && blk.StrAt(L"type") == L"text")
                {
                    text += blk.StrAt(L"text");
                }
            }
            return text;
        }
        return {};
    }
}

namespace Agentmaster
{
    TranscriptParse ParseTranscriptDelta(std::wstring_view chunk)
    {
        TranscriptParse out;
        size_t lineStart = 0;
        for (size_t i = 0; i < chunk.size(); ++i)
        {
            if (chunk[i] != L'\n')
            {
                continue;
            }
            std::wstring_view line = chunk.substr(lineStart, i - lineStart);
            out.consumed = i + 1; // a partial trailing line (after the last '\n') is left unconsumed
            lineStart = i + 1;

            // Trim a trailing CR (CRLF transcripts) and skip blank lines cheaply.
            while (!line.empty() && line.back() == L'\r')
            {
                line.remove_suffix(1);
            }
            bool blank = true;
            for (const wchar_t c : line)
            {
                if (c != L' ' && c != L'\t')
                {
                    blank = false;
                    break;
                }
            }
            if (blank)
            {
                continue;
            }

            const auto parsed = json::Parse(line);
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                continue; // not a JSON object line (tolerate anything)
            }
            const auto& obj = *parsed;
            const std::wstring type = obj.StrAt(L"type");

            if (type == L"assistant")
            {
                const auto* msg = obj.Find(L"message");
                if (!msg || msg->type != json::Value::Type::Obj)
                {
                    continue;
                }
                TranscriptEvent ev;
                ev.kind = TranscriptEvent::Kind::Assistant;
                ev.stopReason = msg->StrAt(L"stop_reason");
                ev.text = CollectText(msg->Find(L"content"));
                out.events.push_back(std::move(ev));
            }
            else if (type == L"user")
            {
                // Skip meta / synthetic user lines (command echoes, injected reminders, etc.).
                if (obj.BoolAt(L"isMeta"))
                {
                    continue;
                }
                const auto* msg = obj.Find(L"message");
                if (!msg || msg->type != json::Value::Type::Obj)
                {
                    continue;
                }
                const auto* content = msg->Find(L"content");
                if (!content)
                {
                    continue;
                }
                std::wstring prompt;
                if (content->type == json::Value::Type::Str)
                {
                    prompt = content->str;
                }
                else if (content->type == json::Value::Type::Arr)
                {
                    // A pure-text user message is a human prompt; ANY tool_result block means this
                    // is a tool turn, not something the human typed -> skip (zero false positives).
                    bool hasToolResult = false;
                    std::wstring text;
                    for (const auto& blk : content->arr)
                    {
                        if (blk.type != json::Value::Type::Obj)
                        {
                            continue;
                        }
                        const std::wstring bt = blk.StrAt(L"type");
                        if (bt == L"tool_result")
                        {
                            hasToolResult = true;
                            break;
                        }
                        if (bt == L"text")
                        {
                            text += blk.StrAt(L"text");
                        }
                    }
                    if (!hasToolResult)
                    {
                        prompt = text;
                    }
                }
                if (!prompt.empty())
                {
                    TranscriptEvent ev;
                    ev.kind = TranscriptEvent::Kind::UserPrompt;
                    ev.text = std::move(prompt);
                    out.events.push_back(std::move(ev));
                }
            }
        }
        return out;
    }

    SessionScanner::SessionScanner(std::shared_ptr<SessionRegistry> registry) :
        _registry{ std::move(registry) }
    {
    }

    SessionScanner::~SessionScanner()
    {
        Stop();
    }

    void SessionScanner::Start()
    {
        bool expected = false;
        if (!_running.compare_exchange_strong(expected, true))
        {
            return;
        }
        _thread = std::thread([this]() noexcept { _worker(); });
    }

    void SessionScanner::Stop() noexcept
    {
        if (_running.exchange(false))
        {
            {
                std::lock_guard lk{ _mtx };
                _woken = true;
            }
            _cv.notify_all();
            if (_thread.joinable())
            {
                _thread.join();
            }
        }
    }

    void SessionScanner::Wake() noexcept
    {
        {
            std::lock_guard lk{ _mtx };
            _woken = true;
        }
        _cv.notify_one();
    }

    LivenessToken SessionScanner::AddLivenessProbe(LivenessProbe probe)
    {
        std::lock_guard lk{ _mtx };
        const LivenessToken token = _nextProbeId++;
        _probes.emplace_back(token, std::move(probe));
        return token;
    }

    void SessionScanner::RemoveLivenessProbe(LivenessToken token)
    {
        std::lock_guard lk{ _mtx };
        _probes.erase(
            std::remove_if(_probes.begin(), _probes.end(), [token](const auto& p) { return p.first == token; }),
            _probes.end());
    }

    void SessionScanner::_worker() noexcept
    {
        for (;;)
        {
            int64_t sleepMs;
            try
            {
                sleepMs = _scanOnce();
            }
            catch (...)
            {
                sleepMs = kScanLiveIdleMs;
            }

            std::unique_lock lk{ _mtx };
            if (!_running.load())
            {
                return;
            }
            if (sleepMs < 0)
            {
                // Nothing live: sleep at zero cost until Wake()/Stop().
                _cv.wait(lk, [this] { return !_running.load() || _woken; });
            }
            else
            {
                _cv.wait_for(lk, std::chrono::milliseconds(sleepMs), [this] { return !_running.load() || _woken; });
            }
            _woken = false;
            if (!_running.load())
            {
                return;
            }
        }
    }

    int64_t SessionScanner::_scanOnce()
    {
        const int64_t now = NowMs();

        // Discovery (the PULL detection of un-hooked, hand-typed claudes): refresh the recent-
        // transcript index while armed, rate-limited to its own cadence so a fast Running-cadence
        // tick (300ms) doesn't re-enumerate the projects tree. Independent of any live session —
        // this is how a `claude` started in a fresh tab (no managed session yet) gets noticed.
        const bool armed = _discoverArmed.load();
        if (armed && (now - _lastDiscoverMs) >= kScanDiscoverMs)
        {
            _discoverOnce();
            _lastDiscoverMs = now;
        }

        const auto sessions = _registry->Snapshot();

        bool anyLive = false;
        bool anyRunning = false;
        for (const auto& s : sessions)
        {
            if (!s.live)
            {
                continue; // archived: no running claude, the transcript is static -> skip
            }
            anyLive = true;
            if (s.state == SessionState::Running)
            {
                anyRunning = true;
            }
            _reconcileSession(s);
        }

        // Drop tail cursors for sessions that are gone / archived (bounded memory).
        if (!_scan.empty())
        {
            for (auto it = _scan.begin(); it != _scan.end();)
            {
                bool live = false;
                for (const auto& s : sessions)
                {
                    if (s.live && s.id == it->first)
                    {
                        live = true;
                        break;
                    }
                }
                it = live ? std::next(it) : _scan.erase(it);
            }
        }

        // Run the app-layer probes when armed even with nothing live, so each window's probe can
        // correlate a freshly discovered transcript to one of its tabs (the probe alone knows tabs).
        _maybeSweepLiveness(now, anyLive || armed);

        if (!anyLive)
        {
            // Keep ticking on the discovery cadence while armed (so new transcripts are noticed);
            // otherwise sleep at zero cost until a registry observer Wake()s us.
            return armed ? kScanDiscoverMs : -1;
        }
        return anyRunning ? kScanRunningMs : kScanLiveIdleMs;
    }

    void SessionScanner::_reconcileSession(const SessionInfo& s)
    {
        ScanState& st = _scan[s.id];
        if (st.path.empty())
        {
            st.path = ResolveClaudeTranscriptPath(s.id);
            if (st.path.empty())
            {
                return; // no transcript yet (never prompted) — nothing to tail
            }
        }

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!::GetFileAttributesExW(st.path.c_str(), GetFileExInfoStandard, &fad))
        {
            st.path.clear(); // vanished / rotated -> re-resolve next pass
            return;
        }
        const int64_t size = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | static_cast<int64_t>(fad.nFileSizeLow);

        if (size < st.offset)
        {
            // Truncated / rewritten under us (shouldn't happen for an append-only transcript, but
            // be safe): restart the tail from the top.
            st.offset = 0;
            st.lastSize = -1;
        }
        if (size != st.lastSize)
        {
            _readDelta(st, s, size); // advances st.offset past the complete lines it consumed
            st.lastSize = size;
        }

        // Missed-Stop reconciliation: the transcript's last assistant message ended the turn
        // (stop_reason == "end_turn") and the file has gone quiescent, yet we are STILL Running —
        // the Stop hook was dropped. Synthesize a Stop identical to the real one (-> WaitingForInput
        // + the question-guard + the Autopilot advance). The Running gate (re-checked against the
        // freshest state right before firing) makes a real Stop that already landed win, so this
        // never double-fires.
        if (s.state == SessionState::Running && st.lastStopReason == L"end_turn")
        {
            const int64_t quietForMs = NowMs() - FiletimeToUnixMs(fad.ftLastWriteTime);
            if (quietForMs >= kScanStopQuiescenceMs)
            {
                const auto fresh = _registry->Get(s.id);
                if (fresh && fresh->state == SessionState::Running)
                {
                    HookMessage stop;
                    stop.event = HookEvent::Stop;
                    stop.sessionId = s.id;
                    stop.cwd = s.workingDir;
                    stop.ts = NowMs();
                    stop.lastMessageIsQuestion = EndsWithQuestion(st.lastAssistantText);
                    _registry->OnHookEvent(stop);
                    AppendStateLog(L"scanner.log",
                                   L"[recon-stop] " + s.id + L" q=" + (stop.lastMessageIsQuestion ? L"1" : L"0") + L"\n");
                }
            }
        }
    }

    void SessionScanner::_readDelta(ScanState& st, const SessionInfo& s, int64_t size)
    {
        const int64_t avail = size - st.offset;
        if (avail <= 0)
        {
            return;
        }
        const DWORD want = static_cast<DWORD>(avail < kScanMaxDeltaBytes ? avail : kScanMaxDeltaBytes);

        // Share everything: claude has the file open for append; never block its writes.
        const HANDLE h = ::CreateFileW(st.path.c_str(),
                                       GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_FLAG_SEQUENTIAL_SCAN,
                                       nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return;
        }
        LARGE_INTEGER li{};
        li.QuadPart = st.offset;
        if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
        {
            ::CloseHandle(h);
            return;
        }
        std::string bytes;
        bytes.resize(want);
        DWORD got = 0;
        const BOOL ok = ::ReadFile(h, bytes.data(), want, &got, nullptr);
        ::CloseHandle(h);
        if (!ok || got == 0)
        {
            return;
        }
        bytes.resize(got);

        // Consume only up to the last newline; a trailing partial line is an append in flight and
        // is re-read whole next tick. (Splitting on the '\n' BYTE is UTF-8-safe: 0x0A is never a
        // continuation byte.)
        const size_t nl = bytes.rfind('\n');
        if (nl == std::string::npos)
        {
            // No complete line in this window. Guard against a corrupt/binary run wedging the
            // cursor forever: if the unterminated run is pathologically large, skip past it.
            if (avail > kScanForceConsumeBytes)
            {
                st.offset = size;
            }
            return;
        }
        const size_t completeBytes = nl + 1;
        const std::wstring wide = Utf8ToUtf16(bytes.data(), static_cast<int>(completeBytes));
        st.offset += static_cast<int64_t>(completeBytes);

        const auto parsed = ParseTranscriptDelta(wide);
        for (const auto& ev : parsed.events)
        {
            if (ev.kind == TranscriptEvent::Kind::Assistant)
            {
                st.lastStopReason = ev.stopReason; // latest assistant line wins (tool_use -> not done)
                if (!ev.text.empty())
                {
                    st.lastAssistantText = ev.text;
                    // Mirror into the model for a future Flight-Plan "peek". QUIET: streamed text
                    // must not trigger the persist / UI / scheduler cascade on every line.
                    const std::wstring text = ev.text;
                    _registry->UpdateQuiet(s.id, [&text](SessionInfo& ss) { ss.lastAssistantText = text; });
                }
            }
            else // UserPrompt: a new human turn began
            {
                // CRITICAL: a new user message starts a fresh turn, so the PRIOR assistant
                // end_turn no longer marks the CURRENT turn complete. Clear the tracked
                // stop_reason, else the missed-Stop backstop could fire on that stale end_turn
                // while claude is mid-(new-)turn — declaring turn-complete and draining the plan
                // into a running turn. (tool_result user lines don't reach here — they're filtered
                // in ParseTranscriptDelta — so this only resets on a genuine human prompt.)
                st.lastStopReason.clear();
                _registry->NoteExternalPrompt(s.id, ev.text); // idempotent by text — back-fills a dropped hook
            }
        }
    }

    void SessionScanner::_maybeSweepLiveness(int64_t nowMs, bool anyLive)
    {
        if (!anyLive)
        {
            return; // nothing to probe
        }
        if (_lastSweepMs != 0 && (nowMs - _lastSweepMs) < kScanSweepMs)
        {
            return; // rate-limited: a sweep marshals to the UI thread, so don't do it every fast tick
        }
        _lastSweepMs = nowMs;

        std::vector<LivenessProbe> probes;
        {
            std::lock_guard lk{ _mtx };
            probes.reserve(_probes.size());
            for (const auto& p : _probes)
            {
                probes.push_back(p.second);
            }
        }
        for (auto& pr : probes)
        {
            if (pr)
            {
                try
                {
                    pr();
                }
                catch (...)
                {
                }
            }
        }
    }

    void SessionScanner::ArmDiscovery()
    {
        // Only discover sessions that start AFTER this point — the user's pre-existing claude
        // history (and our own just-restored archived sessions) must not flood in.
        _discoverSinceMs = NowMs();
        _discoverArmed.store(true);
        Wake(); // break the idle wait so discovery starts ticking immediately
    }

    std::vector<DiscoveredTranscript> SessionScanner::RecentTranscripts() const
    {
        std::lock_guard lk{ _discMtx };
        return _recent;
    }

    void SessionScanner::_discoverOnce()
    {
        const std::wstring projects = ClaudeProjectsDir();
        if (projects.empty())
        {
            return;
        }

        std::vector<DiscoveredTranscript> found;

        const std::wstring dirPattern = projects + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE dh = ::FindFirstFileW(dirPattern.c_str(), &fd);
        if (dh == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                continue;
            }
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }

            const std::wstring sub = projects + L"\\" + name;
            const std::wstring filePattern = sub + L"\\*.jsonl";
            WIN32_FIND_DATAW ff{};
            HANDLE fh = ::FindFirstFileW(filePattern.c_str(), &ff);
            if (fh == INVALID_HANDLE_VALUE)
            {
                continue;
            }
            do
            {
                if (ff.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    continue;
                }
                const int64_t mtime = FiletimeToUnixMs(ff.ftLastWriteTime);
                if (mtime < _discoverSinceMs)
                {
                    continue; // predates this run — not something the user just started
                }
                std::wstring leaf = ff.cFileName; // "<id>.jsonl"
                if (leaf.size() <= 6)
                {
                    continue;
                }
                const std::wstring id = leaf.substr(0, leaf.size() - 6); // strip ".jsonl"

                // cwd: read the transcript head ONCE per id, then cache (even if empty — don't
                // re-open a still-header-less file every tick).
                std::wstring cwd;
                if (const auto it = _cwdById.find(id); it != _cwdById.end())
                {
                    cwd = it->second;
                }
                else
                {
                    cwd = ReadTranscriptCwd(sub + L"\\" + leaf);
                    _cwdById[id] = cwd;
                }
                if (cwd.empty())
                {
                    continue;
                }
                found.push_back(DiscoveredTranscript{ id, cwd, mtime });
            } while (::FindNextFileW(fh, &ff));
            ::FindClose(fh);
        } while (::FindNextFileW(dh, &fd));
        ::FindClose(dh);

        std::lock_guard lk{ _discMtx };
        _recent.swap(found);
    }
}
