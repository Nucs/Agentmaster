// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the STATE FILE GUARD (PERSISTENCE.md "Unreadable state files"): how the profile's state files are read,
// and the one check before any of them is replaced, so that a file Agentmaster could not read is never overwritten
// without its bytes being kept first.
//
// Why it exists: every loader FAILS OPEN. A sessions.json, settings.json, window record or session-store record that
// cannot be opened (a lock), or whose text does not parse (a BOM, a torn write, NUL bytes from a power loss, a "nan" an
// older build wrote), loads as "nothing saved", and the next save then writes that nothing over the user's file. At
// startup this is the whole workspace: an empty fleet re-homes no tab, each reopened window deletes its record as
// "Manager-only", and the next registry save writes an empty sessions.json. The guard closes the last step:
//   * ReadStateFileUtf8 tells "missing" from "unreadable", rides out a brief lock (an antivirus scan, a writer's rename)
//     with short retries, drops a UTF-8 BOM, and logs a file that stays unreadable ([persist-read-fail]).
//   * PrepareStateFileReplace runs before every replace (Persistence's WriteAllUtf8 and SessionStore's AtomicWriteUtf8):
//     the first time this process replaces a file — or the first time after someone else changed it — it reads the file;
//     a file it cannot read is NOT replaced (the write is refused and retried by the caller's next save), and one whose
//     text is not JSON is first copied aside as "<name>.could-not-read-<yyyyMMdd-HHmmss>" ([persist-guard]).
//   * A loader that could not use a file it depends on (the startup fleet read) marks it, so its first replace keeps a
//     copy whatever the file then holds — a lock that ended after the read must not let an empty fleet replace a good one.
//     Every Unreadable read through ReadStateFileUtf8 marks its file the same way (PERSISTENCE.md §16), so the rule holds
//     for each loader and read-modify-write, not only the fleet's.
//
// Implemented in Persistence.cpp, which every build that compiles SessionStore.cpp also compiles (TerminalAppLib, the
// session-db service, the m5 harness, the probes). Std-only declarations, so a hot TU can include it cheaply.

#pragma once

#include <string>
#include <string_view>

namespace Agentmaster
{
    // How a state-file read went (ReadStateFileUtf8).
    enum class StateFileStatus
    {
        Missing, // no such file (or folder): "nothing saved" is the truth — a fresh profile, a file never written
        Read, // the whole file was read; its text may still fail to parse, which the caller judges
        Unreadable, // the file exists but could not be opened or read, even after the retries
    };

    // The result of ReadStateFileUtf8.
    struct StateFileRead
    {
        StateFileStatus status{ StateFileStatus::Missing };
        std::wstring text; // the file as UTF-16, a leading UTF-8 BOM removed; empty unless status == Read
        unsigned long error{ 0 }; // the last Win32 error (GetLastError) when status == Unreadable, else 0
    };

    // A normal read's retry budget: enough for an antivirus scan or another writer's rename (milliseconds in practice),
    // short enough that a UI-thread caller (LoadAppSettings is read on demand) never stalls noticeably.
    inline constexpr unsigned kStateReadRetryBudgetMs = 400;
    // The startup fleet read's budget (LoadSessionsForStartup): one read per process, and the whole workspace depends on
    // it, so it waits longer for a lock to clear before it gives up and starts without the saved sessions.
    inline constexpr unsigned kStartupStateReadRetryBudgetMs = 3000;
    // How long a file stays "recently unreadable": a read inside this window makes one attempt and logs nothing, so a
    // file that stays locked or denied costs one failed open per read instead of the whole retry budget each time
    // (settings.json is read on demand from UI paths), and hooks.log gets one line per episode instead of a flood.
    inline constexpr unsigned kStateUnreadableQuietMs = 30000;

    // Read a state file whole, as UTF-16.
    //   path          - the file.
    //   retryBudgetMs - how long to keep retrying an open or read that fails with a sharing violation, a lock violation
    //                   or access denied (a file being replaced reads as "delete pending" for an instant), 50 ms apart;
    //                   0 = one attempt. Other errors are not retried. Within kStateUnreadableQuietMs of an earlier
    //                   failure on the same path only one attempt is made.
    //   returns       - Missing (no file / no folder), Read (the text; a UTF-8 BOM dropped; bytes that are not UTF-8
    //                   become U+FFFD, as MultiByteToWideChar decodes them), or Unreadable (with the Win32 error; the
    //                   first failure of an episode logs "[persist-read-fail] <path> ..." to hooks.log).
    // An Unreadable read also MARKS the file keep-before-replace (MarkStateFileKeepBeforeReplace): the caller now holds
    // nothing in place of its content, and a lock that ends before the caller's save must not let that save replace the
    // file without its bytes being kept (PERSISTENCE.md §16). A file that stays unreadable is still refused outright.
    // Never throws: an allocation failure or any other exception is logged (Rule #18) and reported as Unreadable.
    StateFileRead ReadStateFileUtf8(const std::wstring& path, unsigned retryBudgetMs = kStateReadRetryBudgetMs) noexcept;

    // The check before replacing a state file (WriteAllUtf8, SessionStore's AtomicWriteUtf8 — call it before writing
    // the temp file). Cheap in the steady state: one stat, matched against the stamp of this process's last write.
    //   path    - the file about to be replaced.
    //   returns - true when the replace may go ahead: the file is missing; or it is the one this process last wrote
    //             (same size and time); or a loader marked it safe; or it was read and is blank or valid JSON; or a copy
    //             of it was kept (it does not parse, or a loader marked it "keep before replace"). False when it must
    //             NOT be replaced: it exists but cannot be read (its bytes could not be checked or kept), or a copy that
    //             was needed could not be made. A refusal is logged ([persist-guard] ... NOT replaced); the caller
    //             treats it as a failed write, so a save retries at its next turn.
    // Never throws (failures are logged and refuse the replace).
    bool PrepareStateFileReplace(const std::wstring& path) noexcept;

    // Record that `path` now holds bytes that are safe to replace: this process just wrote it, or a loader read and
    // parsed it, or its bytes were kept elsewhere (a recovery snapshot). Stamps the file's current size and time, so the
    // next PrepareStateFileReplace takes the fast path until someone else changes the file.
    //   path - the file. A file that is missing now forgets its entry.
    // Never throws.
    void MarkStateFileSafeToReplace(const std::wstring& path) noexcept;

    // Record that this process could NOT use `path`'s content (a loader read nothing, or text it could not parse), so the
    // in-memory state that will be saved over it did not come from it: its first replace must keep a copy of whatever it
    // then holds, even text that parses (a lock that ended after the startup read would otherwise let an empty fleet
    // replace a good one).
    //   path - the file.
    // Never throws.
    void MarkStateFileKeepBeforeReplace(const std::wstring& path) noexcept;

    // Copy `path` aside as "<path>.could-not-read-<yyyyMMdd-HHmmss>" ("-2", "-3", ... when that exists), beside it. The
    // name ends in no ".json", so no scan of the folder (windows\*.json, the session store) ever reads the copy as live
    // state. Logs "[persist-guard] kept a copy of <path> as <name> (<reason>)".
    //   path   - the file to keep.
    //   reason - why, for the log line.
    //   returns - the copy's full path, or "" when the copy failed (the source could not be read, the disk is full).
    // Never throws.
    std::wstring KeepStateFileCopy(const std::wstring& path, std::wstring_view reason) noexcept;

    // TEST SEAM: forget every path the guard has seen (stamps, keep marks, recent read failures), so a harness test that
    // rewrites a file behind the guard's back starts from a clean slate. Never called by the app.
    void ResetStateFileGuardForTests() noexcept;
}
