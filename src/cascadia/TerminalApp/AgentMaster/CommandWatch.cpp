// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and the
// standalone test harness compiles it directly). See CommandWatch.h / COMMANDS.md for the design.
#define NOMINMAX
#include "CommandWatch.h"

#include "ClaudeSpawn.h" // AppendStateLog / ShortId (the [cmd] trace lines)

#include <windows.h>

#include <algorithm>
#include <optional>
#include <utility>

namespace
{
    wchar_t AsciiLower(wchar_t c) noexcept
    {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    }

    std::wstring_view TrimWs(std::wstring_view s) noexcept
    {
        size_t b = 0;
        size_t e = s.size();
        while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n'))
        {
            ++b;
        }
        while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n'))
        {
            --e;
        }
        return s.substr(b, e - b);
    }

    // The body between <tag>…</tag>, or nullopt when the open tag is absent. An unterminated tag
    // (no close) yields the remainder — tolerant, like every other transcript reader here.
    std::optional<std::wstring_view> TagBody(std::wstring_view text, std::wstring_view openTag, std::wstring_view closeTag)
    {
        const size_t open = text.find(openTag);
        if (open == std::wstring_view::npos)
        {
            return std::nullopt;
        }
        const size_t bodyStart = open + openTag.size();
        const size_t close = text.find(closeTag, bodyStart);
        return text.substr(bodyStart, close == std::wstring_view::npos ? std::wstring_view::npos : close - bodyStart);
    }

    // Case-insensitive (ASCII) "does `hay` contain `needle`?" for the leaf preference.
    bool ContainsCi(std::wstring_view hay, std::wstring_view needle)
    {
        if (needle.empty() || needle.size() > hay.size())
        {
            return needle.empty();
        }
        for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        {
            size_t j = 0;
            while (j < needle.size() && AsciiLower(hay[i + j]) == AsciiLower(needle[j]))
            {
                ++j;
            }
            if (j == needle.size())
            {
                return true;
            }
        }
        return false;
    }

    std::wstring_view PathLeaf(std::wstring_view path) noexcept
    {
        const size_t sep = path.find_last_of(L"/\\");
        return sep == std::wstring_view::npos ? path : path.substr(sep + 1);
    }
}

namespace Agentmaster
{
    bool ParseCommandEcho(std::wstring_view text, SlashCommand& out)
    {
        const auto nameBody = TagBody(text, L"<command-name>", L"</command-name>");
        if (!nameBody)
        {
            return false;
        }
        std::wstring_view name = TrimWs(*nameBody);
        while (!name.empty() && name.front() == L'/')
        {
            name.remove_prefix(1); // the echo carries "/handover"; bindings key on the bare word
        }
        if (name.empty())
        {
            return false;
        }
        out.name.assign(name);
        for (auto& c : out.name)
        {
            c = AsciiLower(c);
        }
        out.args.clear();
        if (const auto argsBody = TagBody(text, L"<command-args>", L"</command-args>"))
        {
            out.args.assign(TrimWs(*argsBody));
        }
        return true;
    }

    bool IsMarkdownPath(std::wstring_view path)
    {
        constexpr std::wstring_view kExt = L".md";
        if (path.size() < kExt.size())
        {
            return false;
        }
        const std::wstring_view tail = path.substr(path.size() - kExt.size());
        for (size_t i = 0; i < kExt.size(); ++i)
        {
            if (AsciiLower(tail[i]) != kExt[i])
            {
                return false;
            }
        }
        return true;
    }

    bool IsAbsolutePathForWatch(std::wstring_view path)
    {
        if (path.size() >= 2)
        {
            if (path[1] == L':') // drive-rooted X:\… / X:/…
            {
                return true;
            }
            if ((path[0] == L'\\' && path[1] == L'\\') || (path[0] == L'/' && path[1] == L'/'))
            {
                return true; // UNC
            }
        }
        return !path.empty() && (path[0] == L'\\' || path[0] == L'/'); // rooted on the current drive
    }

    std::wstring PickMarkdownWritePath(const std::vector<std::wstring>& paths, std::wstring_view preferLeafContains)
    {
        std::wstring firstMd;
        for (const auto& p : paths)
        {
            if (!IsMarkdownPath(p))
            {
                continue;
            }
            if (!preferLeafContains.empty() && ContainsCi(PathLeaf(p), preferLeafContains))
            {
                return p; // a preferred-name markdown in this batch outranks an incidental doc edit
            }
            if (firstMd.empty())
            {
                firstMd = p;
            }
        }
        return firstMd;
    }

    void CommandWatch::BindMarkdownAwait(std::wstring commandName, std::wstring preferLeafContains, MarkdownReadyHandler handler)
    {
        for (auto& c : commandName)
        {
            c = AsciiLower(c);
        }
        std::lock_guard lk{ _mtx };
        for (auto& b : _bindings)
        {
            if (b.command == commandName)
            {
                b.preferLeafContains = std::move(preferLeafContains);
                b.onReady = std::move(handler);
                return; // one binding per name — last wins
            }
        }
        _bindings.push_back(Binding{ std::move(commandName), std::move(preferLeafContains), std::move(handler) });
    }

    const CommandWatch::Binding* CommandWatch::_findBindingLocked(std::wstring_view command) const
    {
        for (const auto& b : _bindings)
        {
            if (b.command == command)
            {
                return &b;
            }
        }
        return nullptr;
    }

    bool CommandWatch::_probeFile(const std::wstring& path) const
    {
        if (_fileProbe)
        {
            return _fileProbe(path);
        }
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        {
            return false;
        }
        if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            return false;
        }
        return fad.nFileSizeLow != 0 || fad.nFileSizeHigh != 0; // present AND non-empty (a just-created 0-byte file is still mid-write)
    }

    void CommandWatch::OnCommandSighting(const std::wstring& sessionId, const SlashCommand& cmd, int64_t lineTsMs, int64_t nowMs)
    {
        if (sessionId.empty() || cmd.name.empty())
        {
            return;
        }
        // The replay guard: only a line stamped within the freshness window arms. An absent stamp
        // (0) or an old one — a restored/adopted session's initial history read, a truncation
        // rewind — is silently ignored: an old command must never re-fire on reopen. A slightly
        // FUTURE stamp (clock skew) passes (the difference is negative, under the window).
        if (lineTsMs <= 0 || (nowMs - lineTsMs) > kCommandSightingFreshMs)
        {
            return;
        }
        {
            std::lock_guard lk{ _mtx };
            if (!_findBindingLocked(cmd.name))
            {
                return; // unbound command (/model, /compact, …): no state, no logs
            }
            // Per-session cap (bounded memory): evict the OLDEST pending of this session.
            size_t mine = 0;
            for (const auto& p : _pending)
            {
                if (p.sessionId == sessionId)
                {
                    ++mine;
                }
            }
            if (mine >= kCommandMaxPendingPerSession)
            {
                const auto oldest = std::find_if(_pending.begin(), _pending.end(), [&](const Pending& p) { return p.sessionId == sessionId; });
                if (oldest != _pending.end())
                {
                    _pending.erase(oldest);
                }
            }
            Pending p;
            p.id = _nextPendingId++;
            p.sessionId = sessionId;
            p.command = cmd.name;
            p.args = cmd.args;
            p.armedMs = nowMs;
            _pending.push_back(std::move(p));
        }
        std::wstring argsPreview = cmd.args.substr(0, 120);
        for (auto& c : argsPreview)
        {
            if (c == L'\r' || c == L'\n' || c == L'\t')
            {
                c = L' '; // keep the log line single-line
            }
        }
        AppendStateLog(L"hooks.log", L"[cmd] /" + cmd.name + L" sighted " + ShortId(sessionId) + L" args=\"" + argsPreview + L"\" (awaiting md)\n");
    }

    void CommandWatch::OnFileToolWrite(const std::wstring& sessionId, const std::vector<std::wstring>& paths, const std::wstring& sessionCwd, int64_t nowMs)
    {
        if (sessionId.empty() || paths.empty())
        {
            return;
        }
        std::vector<std::pair<Pending, MarkdownReadyHandler>> ready;
        std::wstring matchedLog;
        {
            std::lock_guard lk{ _mtx };
            // Oldest unmatched pending of this session takes this message's markdown write (FIFO —
            // a second /handover armed before the first resolved waits for the NEXT write).
            const auto it = std::find_if(_pending.begin(), _pending.end(), [&](const Pending& p) { return p.sessionId == sessionId && p.matchedPath.empty(); });
            if (it != _pending.end())
            {
                const Binding* b = _findBindingLocked(it->command);
                std::wstring pick = PickMarkdownWritePath(paths, b ? std::wstring_view{ b->preferLeafContains } : std::wstring_view{});
                if (!pick.empty())
                {
                    if (!IsAbsolutePathForWatch(pick) && !sessionCwd.empty())
                    {
                        std::wstring joined = sessionCwd;
                        if (!joined.empty() && joined.back() != L'\\' && joined.back() != L'/')
                        {
                            joined += L'\\';
                        }
                        joined += pick;
                        pick = std::move(joined);
                    }
                    it->matchedPath = std::move(pick);
                    matchedLog = L"[cmd] /" + it->command + L" md matched " + ShortId(sessionId) + L" path=" + it->matchedPath + L"\n";
                }
            }
            ready = _takeReadyLocked(nowMs); // the common case: the file is already on disk by parse time
        }
        if (!matchedLog.empty())
        {
            AppendStateLog(L"hooks.log", matchedLog);
        }
        _fire(ready);
    }

    void CommandWatch::OnTurnEnd(const std::wstring& sessionId)
    {
        std::vector<std::wstring> expired;
        {
            std::lock_guard lk{ _mtx };
            for (auto it = _pending.begin(); it != _pending.end();)
            {
                if (it->sessionId == sessionId && it->matchedPath.empty())
                {
                    // Only an UNMATCHED sighting ages by turns — a matched one is bounded by the
                    // deadline alone (its write may sit behind an approval across boundaries).
                    if (++it->turnEnds >= kCommandAwaitMaxTurnEnds)
                    {
                        expired.push_back(L"[cmd-expire] /" + it->command + L" " + ShortId(it->sessionId) + L" (no md within " + std::to_wstring(kCommandAwaitMaxTurnEnds) + L" turns)\n");
                        it = _pending.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }
        for (const auto& line : expired)
        {
            AppendStateLog(L"hooks.log", line);
        }
    }

    void CommandWatch::Tick(int64_t nowMs)
    {
        std::vector<std::pair<Pending, MarkdownReadyHandler>> ready;
        std::vector<std::wstring> expired;
        {
            std::lock_guard lk{ _mtx };
            if (_pending.empty())
            {
                return; // steady state: one empty-check, no disk I/O, no allocs
            }
            ready = _takeReadyLocked(nowMs);
            for (auto it = _pending.begin(); it != _pending.end();)
            {
                if (nowMs - it->armedMs >= kCommandAwaitDeadlineMs)
                {
                    expired.push_back(L"[cmd-expire] /" + it->command + L" " + ShortId(it->sessionId) +
                                      (it->matchedPath.empty() ? L" (deadline, no md write seen)\n" : (L" (deadline, file never appeared: " + it->matchedPath + L")\n")));
                    it = _pending.erase(it);
                    continue;
                }
                ++it;
            }
        }
        for (const auto& line : expired)
        {
            AppendStateLog(L"hooks.log", line);
        }
        _fire(ready);
    }

    void CommandWatch::DropSession(const std::wstring& sessionId)
    {
        std::lock_guard lk{ _mtx };
        _pending.erase(std::remove_if(_pending.begin(), _pending.end(), [&](const Pending& p) { return p.sessionId == sessionId; }),
                       _pending.end());
    }

    void CommandWatch::SetFileProbe(std::function<bool(const std::wstring&)> probe)
    {
        std::lock_guard lk{ _mtx };
        _fileProbe = std::move(probe);
    }

    size_t CommandWatch::PendingCount() const
    {
        std::lock_guard lk{ _mtx };
        return _pending.size();
    }

    std::vector<std::pair<CommandWatch::Pending, CommandWatch::MarkdownReadyHandler>> CommandWatch::_takeReadyLocked(int64_t /*nowMs*/)
    {
        std::vector<std::pair<Pending, MarkdownReadyHandler>> ready;
        for (auto it = _pending.begin(); it != _pending.end();)
        {
            if (!it->matchedPath.empty() && _probeFile(it->matchedPath))
            {
                if (const Binding* b = _findBindingLocked(it->command); b && b->onReady)
                {
                    ready.emplace_back(std::move(*it), b->onReady);
                }
                it = _pending.erase(it); // fired (or unbound-raced): consumed either way
                continue;
            }
            ++it;
        }
        return ready;
    }

    void CommandWatch::_fire(const std::vector<std::pair<Pending, MarkdownReadyHandler>>& ready)
    {
        // Invoked OUTSIDE the lock (the registry's _notify pattern): the handler fans out to
        // per-window sinks that marshal into UI dispatchers — never hold engine state across that.
        for (const auto& [p, handler] : ready)
        {
            AppendStateLog(L"hooks.log", L"[cmd-fire] /" + p.command + L" " + ShortId(p.sessionId) + L" md=" + p.matchedPath + L"\n");
            if (handler)
            {
                try
                {
                    handler(p.sessionId, p.matchedPath, p.args);
                }
                catch (...)
                {
                    LogSwallowedException(L"CommandWatch::_fire");
                }
            }
        }
    }
}
