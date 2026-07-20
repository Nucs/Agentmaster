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

    bool IsSaneWatchPath(std::wstring_view path)
    {
        if (path.empty() || path.size() > kWatchMaxPathChars)
        {
            return false;
        }
        for (const wchar_t c : path)
        {
            if (c < 0x20 || c == L'"' || c == L'|')
            {
                // control char / quote / pipe: never a real Windows path — reject at the match,
                // not downstream ('|' doubles as the JoinWatchPaths fan-out separator, so a
                // hostile path can never smuggle a fake second path through the payload).
                return false;
            }
        }
        return true;
    }

    std::wstring JoinWatchPaths(const std::vector<std::wstring>& paths)
    {
        std::wstring out;
        for (const auto& p : paths)
        {
            if (p.empty())
            {
                continue;
            }
            if (!out.empty())
            {
                out.push_back(L'|');
            }
            out += p;
        }
        return out;
    }

    std::vector<std::wstring> SplitWatchPaths(std::wstring_view payload)
    {
        std::vector<std::wstring> out;
        size_t pos = 0;
        while (pos <= payload.size())
        {
            const size_t sep = payload.find(L'|', pos);
            const std::wstring_view part = payload.substr(pos, sep == std::wstring_view::npos ? std::wstring_view::npos : sep - pos);
            if (!part.empty())
            {
                out.emplace_back(part);
            }
            if (sep == std::wstring_view::npos)
            {
                break;
            }
            pos = sep + 1;
        }
        return out;
    }

    std::wstring EncodeCommandProgress(const CommandProgress& p)
    {
        // "v1;p=<processedMs>;a=<cmd>@<ts>,<cmd>@<ts>" — a compact single line (the SessionStore
        // value is a string; command names here are OUR binding slugs — lowercase ASCII words with
        // no ';'/','/'@' — so the separators are unambiguous by construction). The empty default
        // encodes to "" so the store's remove-on-empty keeps session files sparse.
        if (p.processedMs <= 0 && p.armed.empty())
        {
            return {};
        }
        std::wstring out = L"v1;p=" + std::to_wstring(p.processedMs > 0 ? p.processedMs : 0) + L";a=";
        bool first = true;
        for (const auto& [cmd, ts] : p.armed)
        {
            if (cmd.empty() || ts <= 0)
            {
                continue;
            }
            if (!first)
            {
                out.push_back(L',');
            }
            out += cmd + L"@" + std::to_wstring(ts);
            first = false;
        }
        return out;
    }

    CommandProgress DecodeCommandProgress(std::wstring_view encoded)
    {
        // Tolerant: anything malformed/unknown decodes to the empty default (the store value is
        // ours, but a hand-edited/corrupted file must never wedge the watch — worst case the
        // session loses its durable progress and falls back to the freshness gate).
        CommandProgress out;
        constexpr std::wstring_view kPrefix = L"v1;p=";
        if (encoded.size() < kPrefix.size() || encoded.substr(0, kPrefix.size()) != kPrefix)
        {
            return out;
        }
        size_t pos = kPrefix.size();
        int64_t processed = 0;
        while (pos < encoded.size() && encoded[pos] >= L'0' && encoded[pos] <= L'9')
        {
            processed = processed * 10 + (encoded[pos] - L'0');
            ++pos;
        }
        out.processedMs = processed;
        constexpr std::wstring_view kArmed = L";a=";
        if (pos + kArmed.size() > encoded.size() || encoded.substr(pos, kArmed.size()) != kArmed)
        {
            return out; // no armed section (or trailing garbage) — the watermark alone still holds
        }
        pos += kArmed.size();
        while (pos < encoded.size())
        {
            const size_t comma = encoded.find(L',', pos);
            const std::wstring_view entry = encoded.substr(pos, comma == std::wstring_view::npos ? std::wstring_view::npos : comma - pos);
            const size_t at = entry.find(L'@');
            if (at != std::wstring_view::npos && at > 0)
            {
                int64_t ts = 0;
                bool numeric = at + 1 < entry.size();
                for (size_t i = at + 1; i < entry.size(); ++i)
                {
                    if (entry[i] < L'0' || entry[i] > L'9')
                    {
                        numeric = false;
                        break;
                    }
                    ts = ts * 10 + (entry[i] - L'0');
                }
                if (numeric && ts > 0)
                {
                    out.armed.emplace_back(std::wstring{ entry.substr(0, at) }, ts);
                }
            }
            if (comma == std::wstring_view::npos)
            {
                break;
            }
            pos = comma + 1;
        }
        return out;
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
            // Safeguard: an injected probe (tests, future callers) that THROWS reads as "file
            // absent" — the pending stays and is re-probed next Tick / swept by the deadline —
            // instead of unwinding into the feed (whose own catch would abort the whole batch).
            try
            {
                return _fileProbe(path);
            }
            catch (...)
            {
                LogSwallowedException(L"CommandWatch::_probeFile");
                return false;
            }
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
    try
    {
        if (sessionId.empty() || cmd.name.empty())
        {
            return;
        }
        // An absent stamp (0) never arms — the durable identity below IS the line timestamp.
        if (lineTsMs <= 0)
        {
            return;
        }
        bool revived = false;
        {
            std::lock_guard lk{ _mtx };
            if (!_findBindingLocked(cmd.name))
            {
                return; // unbound command (/model, /compact, …): no state, no logs, no progress reads
            }
            // The durable replay guards (COMMANDS.md §3a). Precedence matters:
            //   1. a persisted ARMED marker for exactly this (command, ts) REVIVES the await —
            //      even past the freshness window (the restart/shutdown-mid-await case), and even
            //      at/under the watermark (an out-of-order fire — a later sighting fired while
            //      this one still awaited its file — must not orphan the earlier one);
            //   2. else at/under the FIRED watermark ⇒ already processed — never re-arms (the
            //      double-fire guard: a resume/crash-restart replays the echo fresh enough to
            //      pass the 60s gate, but the watermark remembers);
            //   3. else only a FRESH stamp arms (adopted/foreign deep history stays inert; a
            //      slightly FUTURE stamp — clock skew — passes, the difference is negative).
            auto& prog = _progressLocked(sessionId, nowMs);
            for (const auto& [an, ats] : prog.armed)
            {
                if (an == cmd.name && ats == lineTsMs)
                {
                    revived = true;
                    break;
                }
            }
            if (!revived)
            {
                if (lineTsMs <= prog.processedMs)
                {
                    return; // already fired in a prior run — the restart double-processing guard
                }
                if ((nowMs - lineTsMs) > kCommandSightingFreshMs)
                {
                    return; // neither fresh nor marked — a history replay never arms
                }
            }
            // Idempotence: this exact sighting already armed in THIS run (an overlapping replay /
            // truncation rewind re-feeding the same line) — never double-arm one echo.
            for (const auto& p : _pending)
            {
                if (p.sessionId == sessionId && p.command == cmd.name && p.lineTsMs == lineTsMs)
                {
                    return;
                }
            }
            // Per-session cap (bounded memory): evict the OLDEST pending of this session — and
            // retire its durable marker with it (an evicted await must not revive on replay).
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
                    _dropArmedMarkerLocked(sessionId, oldest->command, oldest->lineTsMs);
                    _pending.erase(oldest);
                }
            }
            Pending p;
            p.id = _nextPendingId++;
            p.sessionId = sessionId;
            p.command = cmd.name;
            p.args = cmd.args;
            p.lineTsMs = lineTsMs;
            // A revived await anchors its deadline at the ORIGINAL typing time, so the 15-min
            // bound is absolute across the restart (a marker just under the deadline revives with
            // only its remaining budget; one past it was already pruned at load and never gets here).
            p.armedMs = revived ? lineTsMs : nowMs;
            _pending.push_back(std::move(p));
            if (!revived)
            {
                prog.armed.emplace_back(cmd.name, lineTsMs);
                _saveProgressLocked(sessionId); // durable: a restart mid-await can revive it
            }
        }
        std::wstring argsPreview = cmd.args.substr(0, 120);
        for (auto& c : argsPreview)
        {
            if (c == L'\r' || c == L'\n' || c == L'\t')
            {
                c = L' '; // keep the log line single-line
            }
        }
        AppendStateLog(L"hooks.log", L"[cmd] /" + cmd.name + L" sighted " + ShortId(sessionId) + L" args=\"" + argsPreview + L"\"" + (revived ? L" (revived after restart, awaiting md)" : L" (awaiting md)") + L"\n");
    }
    catch (...)
    {
        LogSwallowedException(L"CommandWatch::OnCommandSighting"); // self-contained: the scanner pass survives any watch failure
    }

    void CommandWatch::OnFileToolWrite(const std::wstring& sessionId, const std::vector<std::wstring>& paths, const std::wstring& sessionCwd, int64_t nowMs)
    try
    {
        if (sessionId.empty() || paths.empty())
        {
            return;
        }
        std::vector<std::pair<Pending, MarkdownReadyHandler>> ready;
        std::vector<std::wstring> logLines;
        {
            std::lock_guard lk{ _mtx };
            // The oldest UNSEALED pending of this session COLLECTS this message's markdown writes
            // (FIFO across commands — a pending seals at its turn end, so a second /handover's
            // writes can never bleed into the first). MULTI-FILE: one command may write several
            // HANDOVER-*.md files (the definitions permit splitting) — every markdown whose leaf
            // contains the binding's hint is collected, in write order; a batch's FIRST markdown
            // is the fallback pick only while the pending has collected nothing (the legacy
            // mis-named-single-file tolerance) — an incidental non-hint doc edit never rides
            // along once the real handover files are in.
            const auto it = std::find_if(_pending.begin(), _pending.end(), [&](const Pending& p) { return p.sessionId == sessionId && !p.sealed; });
            if (it != _pending.end())
            {
                const Binding* b = _findBindingLocked(it->command);
                const std::wstring_view hint = b ? std::wstring_view{ b->preferLeafContains } : std::wstring_view{};
                std::vector<std::wstring> picks;
                for (const auto& raw : paths)
                {
                    if (IsMarkdownPath(raw) && !hint.empty() && ContainsCi(PathLeaf(raw), hint))
                    {
                        picks.push_back(raw); // hint-matching markdown — the command's own file family
                    }
                }
                if (picks.empty() && it->matchedPaths.empty())
                {
                    // No hint match in this batch and nothing collected yet — the legacy fallback:
                    // the batch's first markdown (PickMarkdownWritePath's else-branch, verbatim).
                    const std::wstring first = PickMarkdownWritePath(paths, {});
                    if (!first.empty())
                    {
                        picks.push_back(first);
                    }
                }
                for (auto& pick : picks)
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
                    // Safeguard: the RESOLVED path flows into a log line, the disk probe, the
                    // '|'-joined fan-out payload, and the successor's launch prompt — a control
                    // char / quote / pipe / unbounded length (malformed or adversarial tool
                    // input; none is a legal Windows path) is rejected HERE, leaving the pending
                    // collecting for a later, sane write.
                    if (!IsSaneWatchPath(pick))
                    {
                        logLines.push_back(L"[cmd] /" + it->command + L" md match REJECTED " + ShortId(sessionId) + L" (insane path: control char/quote/pipe/oversize)\n");
                        continue;
                    }
                    // Dedup (case-insensitive): a Write then Edit of the same file collects once.
                    const bool dup = std::any_of(it->matchedPaths.begin(), it->matchedPaths.end(), [&](const std::wstring& have) {
                        return have.size() == pick.size() && ContainsCi(have, pick); // equal length + CI-contains == CI-equal
                    });
                    if (dup)
                    {
                        continue;
                    }
                    it->matchedPaths.push_back(std::move(pick));
                    it->lastMatchMs = nowMs;
                    logLines.push_back(L"[cmd] /" + it->command + L" md matched " + ShortId(sessionId) + L" path=" + it->matchedPaths.back() + L" (" + std::to_wstring(it->matchedPaths.size()) + L" file" + (it->matchedPaths.size() == 1 ? L"" : L"s") + L")\n");
                }
            }
            ready = _takeReadyLocked(nowMs); // an earlier SEALED await whose last file just landed
        }
        for (const auto& line : logLines)
        {
            AppendStateLog(L"hooks.log", line);
        }
        _fire(ready);
    }
    catch (...)
    {
        LogSwallowedException(L"CommandWatch::OnFileToolWrite"); // self-contained: the scanner pass survives any watch failure
    }

    void CommandWatch::OnTurnEnd(const std::wstring& sessionId)
    try
    {
        std::vector<std::wstring> lines;
        std::vector<std::pair<Pending, MarkdownReadyHandler>> ready;
        {
            std::lock_guard lk{ _mtx };
            for (auto it = _pending.begin(); it != _pending.end();)
            {
                if (it->sessionId == sessionId && !it->matchedPaths.empty())
                {
                    // MATCHED: the first turn end after a match SEALS the collection — the
                    // command's definition ends the turn right after writing its file(s), so
                    // everything belonging to this command is in. From here only the disk gate
                    // (all files present) and the deadline apply — never turn aging (a write may
                    // sit behind a permission approval across boundaries).
                    if (!it->sealed)
                    {
                        it->sealed = true;
                        lines.push_back(L"[cmd] /" + it->command + L" sealed " + ShortId(it->sessionId) + L" files=" + std::to_wstring(it->matchedPaths.size()) + L" (turn end)\n");
                    }
                }
                else if (it->sessionId == sessionId)
                {
                    // UNMATCHED: ages one turn; expires past the window (command turn + one
                    // clarification round). The durable armed marker retires with it — an
                    // expired await must not revive on a later replay.
                    if (++it->turnEnds >= kCommandAwaitMaxTurnEnds)
                    {
                        lines.push_back(L"[cmd-expire] /" + it->command + L" " + ShortId(it->sessionId) + L" (no md within " + std::to_wstring(kCommandAwaitMaxTurnEnds) + L" turns)\n");
                        _dropArmedMarkerLocked(it->sessionId, it->command, it->lineTsMs);
                        it = _pending.erase(it);
                        continue;
                    }
                }
                ++it;
            }
            // A just-sealed pending whose files are all on disk fires right here (the common
            // case: the writes landed during the turn, the end_turn seals + releases it).
            ready = _takeReadyLocked(0);
        }
        for (const auto& line : lines)
        {
            AppendStateLog(L"hooks.log", line);
        }
        _fire(ready);
    }
    catch (...)
    {
        LogSwallowedException(L"CommandWatch::OnTurnEnd"); // self-contained: the scanner pass survives any watch failure
    }

    void CommandWatch::Tick(int64_t nowMs)
    try
    {
        std::vector<std::pair<Pending, MarkdownReadyHandler>> ready;
        std::vector<std::wstring> lines;
        {
            std::lock_guard lk{ _mtx };
            if (_pending.empty())
            {
                return; // steady state: one empty-check, no disk I/O, no allocs
            }
            // Settle-seal fallback (multi-file): a matched pending whose turn end never arrived
            // (the session died mid-turn / a truncated tail) seals after kCommandMatchSettleMs of
            // write-silence, so it can still fire without waiting out the whole deadline.
            for (auto& p : _pending)
            {
                if (!p.sealed && !p.matchedPaths.empty() && p.lastMatchMs > 0 && nowMs - p.lastMatchMs >= kCommandMatchSettleMs)
                {
                    p.sealed = true;
                    lines.push_back(L"[cmd] /" + p.command + L" sealed " + ShortId(p.sessionId) + L" files=" + std::to_wstring(p.matchedPaths.size()) + L" (settle - no turn end seen)\n");
                }
            }
            ready = _takeReadyLocked(nowMs);
            for (auto it = _pending.begin(); it != _pending.end();)
            {
                if (nowMs - it->armedMs >= kCommandAwaitDeadlineMs)
                {
                    lines.push_back(L"[cmd-expire] /" + it->command + L" " + ShortId(it->sessionId) +
                                    (it->matchedPaths.empty() ? L" (deadline, no md write seen)\n" : (L" (deadline, file(s) never appeared: " + JoinWatchPaths(it->matchedPaths) + L")\n")));
                    _dropArmedMarkerLocked(it->sessionId, it->command, it->lineTsMs); // a dead await must not revive on a later replay
                    it = _pending.erase(it);
                    continue;
                }
                ++it;
            }
        }
        for (const auto& line : lines)
        {
            AppendStateLog(L"hooks.log", line);
        }
        _fire(ready);
    }
    catch (...)
    {
        LogSwallowedException(L"CommandWatch::Tick"); // self-contained: the scanner pass survives any watch failure
    }

    void CommandWatch::DropSession(const std::wstring& sessionId)
    try
    {
        std::lock_guard lk{ _mtx };
        _pending.erase(std::remove_if(_pending.begin(), _pending.end(), [&](const Pending& p) { return p.sessionId == sessionId; }),
                       _pending.end());
        // Drop the CACHE only — the persisted progress (watermark + armed markers) deliberately
        // survives the session leaving the live set: it is what makes a later resume/replay both
        // safe (the watermark blocks a re-fire) and able to revive a mid-await command (the
        // markers). A fresh load re-populates the cache on next contact.
        _progress.erase(sessionId);
    }
    catch (...)
    {
        LogSwallowedException(L"CommandWatch::DropSession"); // self-contained: the scanner pass survives any watch failure
    }

    void CommandWatch::SetFileProbe(std::function<bool(const std::wstring&)> probe)
    {
        std::lock_guard lk{ _mtx };
        _fileProbe = std::move(probe);
    }

    void CommandWatch::SetProgressStore(ProgressLoadFn load, ProgressSaveFn save)
    {
        std::lock_guard lk{ _mtx };
        _progressLoad = std::move(load);
        _progressSave = std::move(save);
        _progress.clear(); // a re-pointed store invalidates the cache (tests; the engine sets it once)
    }

    size_t CommandWatch::PendingCount() const
    {
        std::lock_guard lk{ _mtx };
        return _pending.size();
    }

    CommandProgress& CommandWatch::_progressLocked(const std::wstring& sessionId, int64_t nowMs)
    {
        const auto cached = _progress.find(sessionId);
        if (cached != _progress.end())
        {
            return cached->second;
        }
        CommandProgress loaded;
        if (_progressLoad)
        {
            // Safeguard: a throwing injected loader reads as "no stored progress" — the session
            // falls back to the freshness gate — instead of unwinding into the feed.
            try
            {
                loaded = DecodeCommandProgress(_progressLoad(sessionId));
            }
            catch (...)
            {
                LogSwallowedException(L"CommandWatch::_progressLocked load");
                loaded = {};
            }
        }
        // Load-prune: an armed marker past the await deadline can never legitimately revive
        // (a revived pending anchors its deadline at the marker's own timestamp), so retire it
        // here — this is also what garbage-collects markers of sessions that died mid-await and
        // were only ever resumed much later.
        if (nowMs > 0 && !loaded.armed.empty())
        {
            const size_t before = loaded.armed.size();
            loaded.armed.erase(std::remove_if(loaded.armed.begin(), loaded.armed.end(), [&](const auto& m) { return nowMs - m.second >= kCommandAwaitDeadlineMs; }),
                               loaded.armed.end());
            if (loaded.armed.size() != before)
            {
                auto& entry = _progress[sessionId] = std::move(loaded);
                _saveProgressLocked(sessionId); // persist the prune (the stale markers stay gone)
                return entry;
            }
        }
        return _progress[sessionId] = std::move(loaded);
    }

    void CommandWatch::_saveProgressLocked(const std::wstring& sessionId)
    {
        if (!_progressSave)
        {
            return; // no store wired (tests without persistence) — in-memory progress only
        }
        const auto it = _progress.find(sessionId);
        // Safeguard: a throwing saver is logged and swallowed — the in-memory progress still
        // guards this run; only the durable copy is stale (the next successful save heals it).
        try
        {
            _progressSave(sessionId, it == _progress.end() ? std::wstring{} : EncodeCommandProgress(it->second));
        }
        catch (...)
        {
            LogSwallowedException(L"CommandWatch::_saveProgressLocked");
        }
    }

    void CommandWatch::_dropArmedMarkerLocked(const std::wstring& sessionId, const std::wstring& command, int64_t lineTsMs)
    {
        auto& prog = _progressLocked(sessionId, 0);
        const size_t before = prog.armed.size();
        prog.armed.erase(std::remove_if(prog.armed.begin(), prog.armed.end(), [&](const auto& m) { return m.first == command && m.second == lineTsMs; }),
                         prog.armed.end());
        if (prog.armed.size() != before)
        {
            _saveProgressLocked(sessionId);
        }
    }

    std::vector<std::pair<CommandWatch::Pending, CommandWatch::MarkdownReadyHandler>> CommandWatch::_takeReadyLocked(int64_t /*nowMs*/)
    {
        std::vector<std::pair<Pending, MarkdownReadyHandler>> ready;
        for (auto it = _pending.begin(); it != _pending.end();)
        {
            const bool allPresent = it->sealed && !it->matchedPaths.empty() &&
                                    std::all_of(it->matchedPaths.begin(), it->matchedPaths.end(), [&](const std::wstring& p) { return _probeFile(p); });
            if (allPresent)
            {
                // Mark the durable progress BEFORE the handler ever runs (at-most-once across a
                // restart): advance the fired watermark + retire the armed marker. A crash after
                // this write but before the action loses the fire (the user re-runs the command)
                // — never doubles it.
                auto& prog = _progressLocked(it->sessionId, 0);
                if (it->lineTsMs > prog.processedMs)
                {
                    prog.processedMs = it->lineTsMs;
                }
                prog.armed.erase(std::remove_if(prog.armed.begin(), prog.armed.end(), [&](const auto& m) { return m.first == it->command && m.second == it->lineTsMs; }),
                                 prog.armed.end());
                _saveProgressLocked(it->sessionId);
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
            AppendStateLog(L"hooks.log", L"[cmd-fire] /" + p.command + L" " + ShortId(p.sessionId) + L" md=" + JoinWatchPaths(p.matchedPaths) + L"\n");
            if (handler)
            {
                try
                {
                    handler(p.sessionId, p.matchedPaths, p.args);
                }
                catch (...)
                {
                    LogSwallowedException(L"CommandWatch::_fire");
                }
            }
        }
    }
}
