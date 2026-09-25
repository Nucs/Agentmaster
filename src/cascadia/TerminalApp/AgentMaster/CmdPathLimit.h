// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — keep PATH within cmd.exe's reach (header-only, plain Win32; no WinRT, no engine deps).
//
// WHY (the Ctrl+G incident, 2026-09-25 — doc/agentmaster/CMD_PATH_LIMIT.md): cmd.exe resolves NO external
// command at all once the PATH it inherits is longer than 8191 characters — not "the tail is ignored":
// nothing, System32 included, wherever it sits in the list (measured on the incident box: 8173 chars still
// resolves `hostname`, 8227 answers "'hostname' is not recognized" with exit code 1). Claude Code launches its
// external editor (Ctrl+G, `chat:externalEditor`) through `cmd.exe /d /s /c "<EDITOR> "<tmp>""` inside its own
// alternate screen, so a bloated PATH turns Ctrl+G into a one-frame "flash" (cmd's error, printed into that
// alternate screen) followed by "Edit quit unexpectedly (exit code 1)". The same wall breaks every `cmd /c`
// a session runs and Agentmaster's own am-update.cmd / am-uninstall.cmd (bare `powershell`). The bloat that
// triggered it: 101 DEAD `%TEMP%\docfx-hub-runtime-tests\<guid>\data\dotnet-home\.dotnet\tools` entries the
// .NET SDK's first-run experience appended to the persistent USER Path — one per test run that used a fresh
// DOTNET_CLI_HOME — pushing every process on the machine to ~14,300 chars. (It surfaced right after the side
// panel shipped, which is why it was first blamed on the panel; the panel has nothing to do with it.)
//
// THE RULE (CompactOverlongPath) — intervene only when the PATH is at risk, and remove only what cannot matter:
//   1. a PATH at or under kCompactTriggerChars comes back UNTOUCHED, so a healthy machine never sees a change;
//   2. otherwise drop EMPTY entries and later DUPLICATES (first occurrence wins — it is the only copy a lookup
//      can ever reach, so this pass alone is resolution-identical);
//   3. only if that still leaves it over the trigger, drop entries whose directory PROVABLY does not exist
//      (a lookup through a missing directory finds nothing), never probing a network / device / relative entry.
//   A LIVE directory is never dropped: when the list is genuinely too long the result stays over the limit and
//   is REPORTED (stillOverCmdLimit) for the caller to log — guessing which live entry matters least would
//   silently change which binary a bare command resolves to.
//
// WHERE: WindowEmperor::HandleCommandlineArgs compacts the PROCESS environment first thing — before the single-
// instance handoff serializes it to the running instance, before the Emperor snapshots it for new windows'
// tabs, before any .cmd launcher runs — and ConptyConnection::_LaunchAttachedClient applies the same rule to
// every tab's FINAL environment block (the belt for a profile that regenerates its env from the registry).
// One rule, one header, both seams; the persistent (registry) PATH is NEVER written — that is the user's.

#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Agentmaster::PathLimit
{
    /// <summary>
    /// cmd.exe's hard ceiling on a variable it can use: a PATH longer than this makes cmd.exe resolve NO external
    /// command (every bare name reports "is not recognized", exit code 1) — the failure this header exists to prevent.
    /// </summary>
    /// <remarks>
    /// Documented as cmd.exe's 8191-character string limit; measured as a cliff between 8173 (resolves) and
    /// 8227 (fails) with System32 both first and last in the list, so position does not help.
    /// </remarks>
    inline constexpr size_t kCmdExeMaxVariableChars = 8191;

    /// <summary>
    /// Growth budget for what our CHILDREN prepend on their own before they run cmd.exe — the pwsh host that wraps
    /// every managed session adds its $PSHOME (~31 chars), Git Bash its MSYS dirs (~130), the engine's claude shim
    /// ~35. A PATH that fits in OUR process but not after those prepends would still break Ctrl+G inside the
    /// session, so compaction starts this far below the ceiling.
    /// </summary>
    inline constexpr size_t kDownstreamHeadroomChars = 1024;

    /// <summary>
    /// Compaction runs only for a PATH LONGER than this (the ceiling minus the downstream headroom); anything at
    /// or below it is returned byte-identical, which is what keeps the guard invisible on a healthy machine.
    /// </summary>
    inline constexpr size_t kCompactTriggerChars = kCmdExeMaxVariableChars - kDownstreamHeadroomChars;

    /// <summary>
    /// How many dropped-as-missing entries a result keeps verbatim for its log line: enough to name the polluter
    /// (the incident's 101 entries all shared one prefix), few enough that the line stays one screen wide.
    /// </summary>
    /// <remarks>
    /// The sample is the LAST missing entries in PATH order, not the first: installers and tools APPEND to PATH,
    /// so runaway pollution piles up at the tail (the incident's first missing entries were three unrelated
    /// one-offs, its last ones the 101-strong docfx-hub pile).
    /// </remarks>
    inline constexpr size_t kMissingSampleMax = 3;

    /// <summary>
    /// The outcome of one compaction: the PATH value to use plus exactly what was removed and whether cmd.exe can
    /// use the result — the caller logs it (this header never logs: it runs where no logger exists).
    /// </summary>
    struct PathCompaction
    {
        /// <summary>The PATH value to use. Byte-identical to the input whenever <see cref="changed"/> is false.</summary>
        std::wstring path;

        /// <summary>Length of the input PATH in characters (0 when there was no PATH to examine).</summary>
        size_t originalChars = 0;

        /// <summary>Entries dropped for being empty (a doubled, leading or trailing ';').</summary>
        size_t removedEmpty = 0;

        /// <summary>Entries dropped as later duplicates of an earlier entry (case / slash / trailing-'\' variants).</summary>
        size_t removedDuplicates = 0;

        /// <summary>Entries dropped because their directory provably does not exist (only when dedupe was not enough).</summary>
        size_t removedMissing = 0;

        /// <summary>
        /// The last (up to) <see cref="kMissingSampleMax"/> missing entries, verbatim and in PATH order, so the log
        /// line names the polluter (see kMissingSampleMax for why the tail).
        /// </summary>
        std::vector<std::wstring> missingSample;

        /// <summary>True when at least one entry was removed — i.e. <see cref="path"/> differs from the input.</summary>
        bool changed = false;

        /// <summary>
        /// True when even the result is longer than <see cref="kCmdExeMaxVariableChars"/>: every remaining entry is live
        /// (or a network / relative entry the probe never judges), so cmd.exe stays broken and only the USER can fix it
        /// (shorten PATH) — log it loudly.
        /// </summary>
        bool stillOverCmdLimit = false;

        /// <summary>
        /// Set only by <see cref="CompactProcessPathIfOverlong"/>: true once the compacted value was written back into
        /// this process's environment block (false when nothing changed or SetEnvironmentVariableW refused).
        /// </summary>
        bool applied = false;
    };

    namespace detail
    {
        /// <summary>True for an ASCII letter — the only characters a drive letter can be.</summary>
        /// <param name="ch">The candidate drive letter.</param>
        /// <returns>True for 'A'..'Z' / 'a'..'z'.</returns>
        constexpr bool IsAsciiLetter(wchar_t ch) noexcept
        {
            return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
        }

        /// <summary>
        /// The directory an entry names, as cmd.exe reads it: every double quote removed (cmd strips them from a
        /// PATH entry) and surrounding spaces/tabs trimmed. Case and separators are left alone — this is the probe
        /// spelling, not the comparison key.
        /// </summary>
        /// <param name="entry">One raw PATH entry, as <see cref="SplitPathEntries"/> returned it.</param>
        /// <returns>The unquoted, trimmed directory; empty when the entry holds nothing but quotes/whitespace.</returns>
        /// <exception cref="std::bad_alloc">When the copy cannot be allocated.</exception>
        inline std::wstring UnquotedTrimmedEntry(std::wstring_view entry)
        {
            std::wstring dir;
            dir.reserve(entry.size());
            for (const auto ch : entry)
            {
                if (ch != L'"')
                {
                    dir.push_back(ch);
                }
            }
            const auto first = dir.find_first_not_of(L" \t");
            if (first == std::wstring::npos)
            {
                return {};
            }
            const auto last = dir.find_last_not_of(L" \t");
            return dir.substr(first, last - first + 1);
        }

        /// <summary>Length the entries occupy once re-joined with ';' — the size cmd.exe will actually see.</summary>
        /// <param name="entries">The entries to measure.</param>
        /// <returns>Sum of the entry lengths plus one separator between each pair (0 for no entries).</returns>
        inline size_t JoinedLength(const std::vector<std::wstring_view>& entries) noexcept
        {
            size_t total = 0;
            for (const auto entry : entries)
            {
                total += entry.size();
            }
            return entries.empty() ? 0 : total + (entries.size() - 1);
        }

        /// <summary>Re-joins entries with ';', each spelled exactly as it was in the input (quotes and all).</summary>
        /// <param name="entries">The kept entries, in their original order.</param>
        /// <returns>The PATH value.</returns>
        /// <exception cref="std::bad_alloc">When the result cannot be allocated.</exception>
        inline std::wstring JoinPathEntries(const std::vector<std::wstring_view>& entries)
        {
            std::wstring joined;
            joined.reserve(JoinedLength(entries));
            for (size_t i = 0; i < entries.size(); ++i)
            {
                if (i != 0)
                {
                    joined.push_back(L';');
                }
                joined.append(entries[i]);
            }
            return joined;
        }
    }

    /// <summary>
    /// Splits a PATH value on ';' the way cmd.exe reads it: a ';' inside double quotes belongs to the entry
    /// (`"C:\odd;dir"` is ONE directory), so a naive split would mangle such an entry into two dead halves.
    /// </summary>
    /// <param name="path">The PATH value (just the value, not "PATH=...").</param>
    /// <returns>
    /// Every entry in order, VERBATIM (quotes and whitespace kept) and including the empty ones a leading,
    /// trailing or doubled ';' produces — so re-joining all of them with ';' reproduces the input exactly.
    /// The views alias <paramref name="path"/>: they dangle once that storage is freed or modified.
    /// </returns>
    /// <exception cref="std::bad_alloc">When the result vector cannot grow.</exception>
    inline std::vector<std::wstring_view> SplitPathEntries(std::wstring_view path)
    {
        std::vector<std::wstring_view> entries;
        size_t start = 0;
        bool inQuotes = false;
        for (size_t i = 0; i < path.size(); ++i)
        {
            if (path[i] == L'"')
            {
                inQuotes = !inQuotes;
            }
            else if (path[i] == L';' && !inQuotes)
            {
                entries.push_back(path.substr(start, i - start));
                start = i + 1;
            }
        }
        // The final entry (possibly empty after a trailing ';'). An UNBALANCED quote simply runs to the end,
        // which is also where cmd.exe's own parser ends it.
        entries.push_back(path.substr(start));
        return entries;
    }

    /// <summary>
    /// The comparison key that decides whether two entries are the SAME directory: quotes stripped, trimmed, '/'
    /// folded to '\', trailing '\' dropped (but a bare drive root keeps its slash — "C:" alone means the drive's
    /// CURRENT directory, a different place from "C:\"), ASCII letters upper-cased.
    /// </summary>
    /// <param name="entry">One raw PATH entry.</param>
    /// <returns>The key; empty for an entry that names nothing (only quotes/whitespace).</returns>
    /// <remarks>
    /// Folding is ASCII-only on purpose: two spellings that differ in a non-ASCII letter's case are simply NOT
    /// merged — the safe direction, since a missed duplicate costs a few characters while a wrong merge would
    /// drop a distinct directory. `%VAR%` text is compared literally (never expanded here).
    /// </remarks>
    /// <exception cref="std::bad_alloc">When the key cannot be allocated.</exception>
    inline std::wstring PathEntryKey(std::wstring_view entry)
    {
        auto key = detail::UnquotedTrimmedEntry(entry);
        for (auto& ch : key)
        {
            if (ch == L'/')
            {
                ch = L'\\';
            }
            else if (ch >= L'a' && ch <= L'z')
            {
                ch = static_cast<wchar_t>(ch - (L'a' - L'A'));
            }
        }
        // "C:\Tools\" and "C:\Tools" are one directory; stop at 3 chars so "C:\" (the root) survives intact.
        while (key.size() > 3 && key.back() == L'\\')
        {
            key.pop_back();
        }
        return key;
    }

    /// <summary>
    /// The Win32 existence probe <see cref="CompactOverlongPath"/> uses in production: answers "keep this entry?"
    /// and only ever says NO for an absolute local directory the file system PROVABLY does not have.
    /// </summary>
    /// <param name="entry">One raw PATH entry (quotes and surrounding whitespace allowed).</param>
    /// <returns>
    /// False only for: an entry that names nothing; a drive letter with no volume behind it; or a path the file
    /// system reports as not found / invalid. True (keep) for everything else — an existing directory, an existing
    /// FILE (not ours to judge), a UNC / `\\?\` / `\\.\` entry and a mapped NETWORK drive (never probed: an
    /// offline share stalls the probe for seconds, and this runs on the launch path), a relative or drive-relative
    /// entry (it depends on each child's current directory), and any probe error that does not prove absence
    /// (access denied, device not ready, name too long, ...).
    /// </returns>
    /// <remarks>
    /// The probe runs with the "insert a disk" / "drive not ready" critical-error dialogs suppressed for this
    /// thread only (SetThreadErrorMode), then restores the previous mode — it can run on a UI thread, where a
    /// modal system dialog would freeze the window.
    /// </remarks>
    /// <exception cref="std::bad_alloc">When the unquoted copy of the entry cannot be allocated.</exception>
    inline bool IsPathEntryLiveOrUnprobeable(std::wstring_view entry)
    {
        const auto dir = detail::UnquotedTrimmedEntry(entry);
        if (dir.empty())
        {
            return false;
        }
        const auto isSeparator = [](wchar_t ch) noexcept { return ch == L'\\' || ch == L'/'; };
        if (dir.size() >= 2 && isSeparator(dir[0]) && isSeparator(dir[1]))
        {
            return true; // UNC / \\?\ / \\.\ — a network or device path is never probed
        }
        const bool driveQualified = dir.size() >= 3 && detail::IsAsciiLetter(dir[0]) && dir[1] == L':' && isSeparator(dir[2]);
        if (!driveQualified)
        {
            return true; // relative ("bin", "."), drive-relative ("C:tools") or rooted-without-drive ("\tools")
        }

        const wchar_t root[] = { dir[0], L':', L'\\', L'\0' };
        switch (::GetDriveTypeW(root))
        {
        case DRIVE_REMOTE:
            return true; // a disconnected mapped drive can stall GetFileAttributesW for seconds — keep, don't ask
        case DRIVE_NO_ROOT_DIR:
            return false; // no volume behind that letter right now: nothing under it can resolve
        default:
            break;
        }

        // Suppress the critical-error dialogs for THIS probe only; read the error BEFORE restoring the mode
        // (SetThreadErrorMode may overwrite the thread's last-error value).
        DWORD previousMode = 0;
        const bool modeSet = ::SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previousMode) != FALSE;
        const DWORD attributes = ::GetFileAttributesW(dir.c_str());
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? ::GetLastError() : ERROR_SUCCESS;
        if (modeSet)
        {
            ::SetThreadErrorMode(previousMode, nullptr);
        }

        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            return true;
        }
        switch (error)
        {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_INVALID_DRIVE:
            return false; // provably absent
        default:
            return true; // absence NOT proven (denied / not ready / too long / ...) — keep
        }
    }

    /// <summary>
    /// THE RULE: returns a PATH that cmd.exe can use, removing only what cannot matter — untouched at or under
    /// <see cref="kCompactTriggerChars"/>; above it, empties and later duplicates go first (resolution-identical),
    /// and only if that is not enough, entries whose directory provably does not exist.
    /// </summary>
    /// <typeparam name="EntryIsLive">
    /// A callable <c>bool(std::wstring_view rawEntry)</c> answering "keep this entry?" — production passes
    /// <see cref="IsPathEntryLiveOrUnprobeable"/>; tests pass a fake so the rule is checked without a file system.
    /// </typeparam>
    /// <param name="path">The PATH value to examine.</param>
    /// <param name="entryIsLive">
    /// The existence probe. Called only when the dedupe pass left the PATH over the trigger, at most once per
    /// remaining entry, in order; a healthy PATH never calls it (so it costs nothing on a normal machine).
    /// </param>
    /// <returns>
    /// The outcome. Kept entries keep their exact original spelling and relative order, so the first directory
    /// holding any given command is unchanged. <see cref="PathCompaction::stillOverCmdLimit"/> reports a PATH that
    /// is genuinely too long (all live, or unjudgeable) — this function never drops a live directory to force a fit.
    /// </returns>
    /// <exception cref="std::bad_alloc">When a working copy cannot be allocated.</exception>
    /// <exception>Anything <paramref name="entryIsLive"/> throws propagates unchanged (the production probe throws
    /// only std::bad_alloc).</exception>
    template<typename EntryIsLive>
    PathCompaction CompactOverlongPath(std::wstring_view path, EntryIsLive&& entryIsLive)
    {
        PathCompaction result;
        result.originalChars = path.size();
        if (path.size() <= kCompactTriggerChars)
        {
            result.path.assign(path);
            return result;
        }

        const auto entries = SplitPathEntries(path);

        // Pass 1 — empties + later duplicates. Free (no I/O) and resolution-identical: a lookup walks the list in
        // order and stops at the first hit, so a second copy of a directory can never be the one that answers.
        std::vector<std::wstring_view> kept;
        kept.reserve(entries.size());
        std::unordered_set<std::wstring> seen;
        seen.reserve(entries.size());
        for (const auto entry : entries)
        {
            auto key = PathEntryKey(entry);
            if (key.empty())
            {
                ++result.removedEmpty;
                continue;
            }
            if (!seen.insert(std::move(key)).second)
            {
                ++result.removedDuplicates;
                continue;
            }
            kept.push_back(entry);
        }

        // Pass 2 — only when dedupe was not enough: drop directories that provably do not exist. Deliberately
        // gated, because this is the one pass with a (theoretical) behavior change: a directory created LATER in
        // a child's life would no longer be on its PATH. With cmd.exe otherwise unable to resolve anything, that
        // edge is the far smaller cost.
        if (detail::JoinedLength(kept) > kCompactTriggerChars)
        {
            std::vector<std::wstring_view> live;
            live.reserve(kept.size());
            for (const auto entry : kept)
            {
                if (entryIsLive(entry))
                {
                    live.push_back(entry);
                    continue;
                }
                ++result.removedMissing;
                // Keep a sliding window of the LAST few (pollution accumulates at the tail — kMissingSampleMax).
                if (result.missingSample.size() == kMissingSampleMax)
                {
                    result.missingSample.erase(result.missingSample.begin());
                }
                result.missingSample.emplace_back(entry);
            }
            kept = std::move(live);
        }

        result.changed = result.removedEmpty + result.removedDuplicates + result.removedMissing != 0;
        if (result.changed)
        {
            result.path = detail::JoinPathEntries(kept);
        }
        else
        {
            // Nothing removable: hand back the input verbatim rather than a re-join (identical today, but this
            // keeps "changed == false => byte-identical" true by construction, not by the splitter's round-trip).
            result.path.assign(path);
        }
        result.stillOverCmdLimit = result.path.size() > kCmdExeMaxVariableChars;
        return result;
    }

    /// <summary>
    /// One human-readable log line for a compaction outcome (no tag, no timestamp — the caller's logger adds
    /// those), written so that the line alone tells the reader what broke, what was done, and who polluted PATH.
    /// </summary>
    /// <param name="result">A result from <see cref="CompactOverlongPath"/> or <see cref="CompactProcessPathIfOverlong"/>.</param>
    /// <returns>The description; for an unchanged, healthy PATH a short "healthy" note (callers normally skip logging then).</returns>
    /// <exception cref="std::bad_alloc">When the text cannot be allocated.</exception>
    inline std::wstring DescribePathCompaction(const PathCompaction& result)
    {
        const auto limit = std::to_wstring(kCmdExeMaxVariableChars);
        if (!result.changed && !result.stillOverCmdLimit)
        {
            return L"PATH is " + std::to_wstring(result.originalChars) + L" chars \x2014 within cmd.exe's reach, left untouched";
        }

        std::wstring text;
        if (result.changed)
        {
            text = L"PATH " + std::to_wstring(result.originalChars) + L" -> " + std::to_wstring(result.path.size()) +
                   L" chars (cmd.exe resolves NO command past " + limit + L" \x2014 e.g. Claude's Ctrl+G editor launch): dropped " +
                   std::to_wstring(result.removedDuplicates) + L" duplicate, " + std::to_wstring(result.removedMissing) +
                   L" missing, " + std::to_wstring(result.removedEmpty) + L" empty entries";
            if (!result.missingSample.empty())
            {
                text += L" (missing e.g. ";
                for (size_t i = 0; i < result.missingSample.size(); ++i)
                {
                    text += (i == 0 ? L"" : L" | ") + result.missingSample[i];
                }
                text += L")";
            }
            if (result.applied)
            {
                text += L"; applied to this process";
            }
        }
        else
        {
            text = L"PATH is " + std::to_wstring(result.originalChars) + L" chars and has no duplicate, empty or missing entry to drop";
        }
        if (result.stillOverCmdLimit)
        {
            text += L" \x2014 STILL over cmd.exe's " + limit + L"-char limit: every remaining entry is live (or a network/relative entry "
                                                               L"that is never judged), so `cmd /c` launches (Claude's Ctrl+G editor, .cmd tools and "
                                                               L"scripts) cannot resolve commands until PATH is shortened";
        }
        return text;
    }

    /// <summary>
    /// Applies <see cref="CompactOverlongPath"/> to THIS process's PATH and writes the result back, so every child
    /// launched afterwards — and every environment block later serialized from this process (the single-instance
    /// handoff, the Emperor's new-window snapshot) — inherits a PATH cmd.exe can use.
    /// </summary>
    /// <returns>
    /// The outcome with <see cref="PathCompaction::applied"/> set when the process environment was updated; a
    /// default (unchanged, originalChars 0) result when PATH is unset/empty or changed under us mid-read.
    /// </returns>
    /// <remarks>
    /// Call it FIRST THING at process start, before anything spawns a child or captures the environment block:
    /// later calls cannot fix what was already handed out. Never touches the persistent (registry) PATH.
    /// </remarks>
    /// <exception cref="std::bad_alloc">When a working copy cannot be allocated (the PATH is then left untouched).</exception>
    inline PathCompaction CompactProcessPathIfOverlong()
    {
        const DWORD needed = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (needed <= 1)
        {
            return {}; // unset (0) or empty (just the terminator) — nothing to protect
        }
        std::wstring current(needed, L'\0');
        const DWORD got = ::GetEnvironmentVariableW(L"PATH", current.data(), needed);
        if (got == 0 || got >= needed)
        {
            return {}; // another thread changed PATH between the two reads — leave it alone rather than guess
        }
        current.resize(got);

        auto result = CompactOverlongPath(current, IsPathEntryLiveOrUnprobeable);
        if (result.changed)
        {
            result.applied = ::SetEnvironmentVariableW(L"PATH", result.path.c_str()) != FALSE;
        }
        return result;
    }
}
