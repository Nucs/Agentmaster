// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TranscriptWatcher: the SessionScanner's change feed over Claude's transcript tree
// (HOOKS.md "Transcript watcher").
//
// WHY: the scanner used to find out about transcript changes by POLLING every live session, every
// pass (300 ms while anything runs). For each session it stat'ed the transcript, enumerated its
// `subagents` + `tool-results` side folders, and, for a session with NO transcript yet (launched but
// never prompted), re-enumerated EVERY project folder under ~/.claude/projects looking for
// `<id>.jsonl` (ResolveClaudeTranscriptPath: 203 folders on the measured fleet). On the 2026-09-25
// release instance (~60 live sessions) that loop held a scanner thread at ~35% of a core with 10,535 s
// of CPU in 12.8 h. A 19.5 s stack sample put 28% of it in ResolveClaudeTranscriptPath alone.
//
// WHAT: ONE ReadDirectoryChangesW watch on the PARENT folder (<claude config>\projects) with
// bWatchSubtree = TRUE, so every project folder, transcript and side file below it is covered by a
// single handle. From the notifications it keeps:
//   * an INDEX sessionId -> transcript path (one full enumeration at start, then incremental), which
//     replaces the per-pass folder walk for unresolved sessions with a map lookup;
//   * per-session CHANGE SEQUENCES (transcript / side folder), which let the scanner skip the stat
//     and the side-folder enumeration for a session whose files did not change since its last pass;
//   * an EPOCH that bumps on every full re-index (start, a notification-buffer overflow, a lost and
//     re-opened watch) — the "anything may have changed" boundary.
// It wakes the scanner (coalesced) so an append is processed at once instead of on the next poll.
//
// SAFETY: notifications for Claude's own write shape (open, append, close) land within ~0.4 ms
// (measured, transcripts and nested side files alike). A writer that keeps its handle open and never
// flushes produces none until it does, so the scanner keeps belts: it still stats demotion-sensitive
// sessions every pass, and re-stats every other session on a slow resync. When the watch cannot be
// established (no projects folder yet, an access error), Healthy() is false and the scanner falls back
// to its classic polling.
//
// Plain C++ + Win32 (no WinRT), like the rest of AgentMaster/, so the pure parts
// (ClassifyProjectsRelativePath, TranscriptIndex) are unit-tested standalone and the watcher itself is
// exercised end to end on a temp folder in the m5 harness.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Agentmaster
{
    // How a path RELATIVE to the projects root matters to the scanner.
    enum class ProjectsPathKind : uint8_t
    {
        Ignored, // nothing the scanner reads: a stray file at the root, a project's memory/ folder, deeper non-side content
        ProjectDir, // "<proj>" — a project folder itself (created / removed / renamed); its MODIFIED records mean nothing
        Transcript, // "<proj>\<id>.jsonl" — a conversation transcript (the file the scanner tails)
        SideFile, // "<proj>\<id>" (the session's side folder) or anything under "<proj>\<id>\subagents|tool-results\..."
    };

    // The verdict ClassifyProjectsRelativePath returns.
    struct ProjectsPathInfo
    {
        ProjectsPathKind kind{ ProjectsPathKind::Ignored };
        std::wstring projectDir; // the project folder component, ORIGINAL case ("" for Ignored)
        std::wstring fileName; // for Transcript: the "<id>.jsonl" leaf, ORIGINAL case (to rebuild the path); else ""
        std::wstring sessionId; // LOWERCASE session id (the transcript stem / side folder name); "" for ProjectDir/Ignored
    };

    // PURE + total: classify one ReadDirectoryChangesW name (relative to the watched projects root).
    // Separators '\' and '/' are both accepted (FILE_NOTIFY_INFORMATION uses '\'); empty components are
    // tolerated. Mirrors ResolveClaudeTranscriptPath's lookup rule — ANY "<proj>\<name>.jsonl" direct child
    // is a transcript named by its stem, whatever the stem looks like — so the index answers exactly what
    // the folder walk would have.
    //   relative — the record's FileName (not NUL-terminated in the record; pass its exact length).
    // Returns the kind plus its components; the session id is lowercased (Claude's ids are lowercase, and
    // Windows file names compare case-insensitively). Never throws (beyond std::bad_alloc).
    ProjectsPathInfo ClassifyProjectsRelativePath(std::wstring_view relative);

    // The FILE_ACTION_* codes a ReadDirectoryChangesW record carries, as a typed enum.
    enum class ProjectsChangeAction : uint8_t
    {
        Added, // FILE_ACTION_ADDED
        Removed, // FILE_ACTION_REMOVED
        Modified, // FILE_ACTION_MODIFIED (size / last-write changed)
        RenamedOld, // FILE_ACTION_RENAMED_OLD_NAME (the name that went away)
        RenamedNew, // FILE_ACTION_RENAMED_NEW_NAME (the name that appeared)
    };

    // What the scanner asks per session each pass. Plain data; read under the watcher's lock and copied out.
    struct TranscriptWatchStamp
    {
        bool healthy{ false }; // the index is AUTHORITATIVE right now (the watch is live and indexed)
        uint64_t epoch{ 0 }; // bumps on every full (re)index — compare to force a refresh of everything
        uint64_t transcriptSeq{ 0 }; // bumps on every change to this session's transcript (add / remove / modify)
        uint64_t sideSeq{ 0 }; // bumps on every change under this session's side folder (subagents / tool-results)
    };

    // PURE (not thread-safe — TranscriptWatcher guards it): the sessionId -> transcript-path index plus the
    // per-session change sequences. Separated from the Win32 watcher so its bookkeeping is unit-testable
    // without a filesystem.
    class TranscriptIndex
    {
    public:
        // Drop every indexed PATH while keeping each session's change sequences counting (a full re-index
        // must never hand the scanner a sequence it already saw for a different state of the file). Every
        // session that lost a path gets its transcript sequence bumped. Never throws.
        void ClearPaths() noexcept;

        // Index `fullPath` as a transcript of `sessionId` inside `projectDir`. Idempotent: re-adding the same
        // (project, id) replaces its path. Keeps each id's candidate list sorted case-insensitively, so
        // Resolve's pick is deterministic when the same id exists in two project folders (the legacy folder
        // walk returned the first match in NTFS order, which is the same case-insensitive order).
        //   projectDir — the project folder name (any case; compared case-insensitively).
        //   sessionId  — the transcript stem (lowercased by the caller: ClassifyProjectsRelativePath does).
        //   fullPath   — the absolute path to hand back from Resolve.
        // Bumps the id's transcript sequence. Never throws (beyond std::bad_alloc).
        void AddTranscript(std::wstring_view projectDir, std::wstring_view sessionId, std::wstring fullPath);

        // Forget the transcript of `sessionId` inside `projectDir` (a delete, or the old half of a rename).
        // Bumps the id's transcript sequence even when nothing was indexed (the scanner then re-stats, which
        // is what a vanished file needs). Never throws.
        void RemoveTranscript(std::wstring_view projectDir, std::wstring_view sessionId);

        // Forget every transcript that lives in `projectDir` (the folder was removed or renamed away — the
        // system reports only the folder, not its children). Bumps each affected id's transcript sequence.
        // Never throws.
        void RemoveProjectDir(std::wstring_view projectDir);

        // Record a content change to `sessionId`'s transcript (a MODIFIED record). Never throws (beyond
        // std::bad_alloc on first sight of the id).
        void TouchTranscript(std::wstring_view sessionId);

        // Record a change under `sessionId`'s side folder (subagents / tool-results). Never throws (beyond
        // std::bad_alloc on first sight of the id).
        void TouchSide(std::wstring_view sessionId);

        // Apply ONE notification record.
        //   action   — the record's action.
        //   relative — the record's name relative to `root`.
        //   root     — the watched projects root (to build absolute paths for new transcripts).
        // Returns the project folder the CALLER must (re-)enumerate because its contents changed without
        // records of their own (a folder created or renamed/moved into the tree), or "" when the record was
        // fully applied here. Never throws (beyond std::bad_alloc).
        std::wstring Apply(ProjectsChangeAction action, std::wstring_view relative, std::wstring_view root);

        // The indexed transcript path for `sessionId`, or "" when none is indexed. Pure lookup (any case).
        // When the id exists in several project folders, the case-insensitively smallest path wins (the
        // legacy walk's NTFS order). Never throws (beyond std::bad_alloc).
        std::wstring Resolve(std::wstring_view sessionId) const;

        // Copy out the id's two change sequences (0/0 for an id never seen). Throws only std::bad_alloc
        // (the case-folded lookup key is built on the fly).
        void SequencesFor(std::wstring_view sessionId, uint64_t& transcriptSeq, uint64_t& sideSeq) const;

        // How many transcript paths are indexed (tests + the watch log line). Never throws.
        size_t TranscriptCount() const noexcept;

        // --- the session database's change feed (SESSIONS_DB.md §7.6, D16) ---
        // While tracking is on, every change the index records (a transcript added / removed / modified, a
        // side file touched, a project folder removed) also puts its session id into a CHANGED set, which
        // TakeChanged hands out and empties. Off by default, so an install that does not run the database keeps
        // no set at all; turning it off drops the set. Never throws.
        void SetChangeTracking(bool on) noexcept;
        // The ids changed since the last take (lowercase, any order), and the set emptied. Never throws
        // (beyond std::bad_alloc).
        std::vector<std::wstring> TakeChanged();
        // Empty the changed set without handing it out: a full re-index made it meaningless (the epoch bump
        // tells takers to sweep everything instead). Never throws.
        void ClearChanged() noexcept;

    private:
        // Record `lowerId` in the changed set when tracking is on. Never throws (beyond std::bad_alloc).
        void _noteChanged(const std::wstring& lowerId);

        // One indexed transcript file of an id.
        struct Location
        {
            std::wstring projectKey; // lowercase project folder (the RemoveProjectDir / RemoveTranscript key)
            std::wstring path; // absolute path (original case)
        };
        // Everything the index knows about one session id.
        struct Entry
        {
            std::vector<Location> locations; // sorted case-insensitively by path (Resolve takes the first)
            uint64_t transcriptSeq{ 0 };
            uint64_t sideSeq{ 0 };
        };
        // Lowercase id -> entry. Entries are never erased (their sequences must keep counting across a
        // re-index); the id set is bounded by the transcripts that ever existed on this machine.
        std::unordered_map<std::wstring, Entry> _byId;
        size_t _paths{ 0 }; // total Location count across entries (TranscriptCount)
        bool _tracking{ false }; // the database's change feed is on (SetChangeTracking)
        std::unordered_set<std::wstring> _changed; // lowercase ids changed since the last TakeChanged
    };

    // What TranscriptWatcher::TakeChangedSessions hands the database's refresh tick (D16).
    struct ChangedSessions
    {
        bool healthy{ false }; // the watch is live: the set is complete (false => the tick must sweep instead)
        uint64_t epoch{ 0 }; // the index epoch: a value different from the previous take's => a full re-index
                             // happened, the set says nothing, and the tick sweeps (resync)
        std::vector<std::pair<std::wstring, std::wstring>> sessions; // (lowercase id, indexed transcript path or "")
    };

    // The live watcher: one background thread owning a ReadDirectoryChangesW watch on the projects root
    // (subtree ON) and the TranscriptIndex it feeds. All public methods are thread-safe.
    class TranscriptWatcher
    {
    public:
        // Called (on the watcher thread, outside its lock) after changes were applied — at most once per
        // kTranscriptWatchCoalesceMs while changes keep streaming. The scanner passes its Wake().
        using ChangeCallback = std::function<void()>;

        // `projectsDir` — the folder to watch (<claude config>\projects; ClaudeProjectsDir()). It does not
        // have to exist yet: the thread retries the open every kTranscriptWatchRetryMs until it does.
        explicit TranscriptWatcher(std::wstring projectsDir);

        // Stops the thread (Stop()) — never throws.
        ~TranscriptWatcher();

        TranscriptWatcher(const TranscriptWatcher&) = delete;
        TranscriptWatcher& operator=(const TranscriptWatcher&) = delete;

        // Start the watcher thread (idempotent: a second call is a no-op).
        //   onChange — invoked after applied changes (coalesced); may be empty.
        // The index becomes Healthy() asynchronously, after the first full enumeration on the thread; until
        // then the scanner uses its classic lookups. Throws std::system_error only if the OS refuses to
        // create the thread or the stop event (the engine's init net catches and logs it).
        void Start(ChangeCallback onChange);

        // Stop the thread and close the watch: signals the stop event, cancels the pending read, WAITS for the
        // kernel to finish with the buffer (freeing it while the read can still complete would be a
        // use-after-free), then joins. Idempotent. Never throws.
        void Stop() noexcept;

        // The scanner's per-session question: is the index authoritative, and what changed for `sessionId`?
        // Throws only std::bad_alloc (the case-folded lookup key); the scanner's pass net catches it.
        TranscriptWatchStamp Stamp(std::wstring_view sessionId) const;

        // The indexed transcript path for `sessionId` ("" when none). AUTHORITATIVE only while Healthy() —
        // call it after a Stamp(...).healthy check. Never throws (beyond std::bad_alloc).
        std::wstring Resolve(std::wstring_view sessionId) const;

        // True while the watch is live and indexed. Never throws.
        bool Healthy() const noexcept;

        // The session database's change feed (SESSIONS_DB.md §7.6, D16): turn the changed-session set on or off
        // (off drops it). The database's refresh tick turns it on while the database runs. Never throws.
        void SetChangeTracking(bool on) noexcept;
        // The sessions whose transcript or side files changed since the last take (each with its indexed path),
        // the watch's health and its epoch; the set is emptied. Throws only std::bad_alloc.
        ChangedSessions TakeChangedSessions();

        // The watched root, as passed to the constructor. Never throws.
        const std::wstring& Root() const noexcept
        {
            return _root;
        }

    private:
        // The watcher thread body: open + arm + index + event loop, with retry/backoff on any failure.
        void _run() noexcept;
        // Open the root for listing (FILE_FLAG_OVERLAPPED | BACKUP_SEMANTICS). False on failure (handle stays invalid).
        bool _open();
        // Issue the next ReadDirectoryChangesW into _buffer. False on failure (the caller re-opens).
        bool _arm();
        // Wait on {stop, the pending read}, apply each completed buffer, re-arm, and fire the coalesced
        // callback. Returns true when the stop event ended it, false when the watch broke (the caller then
        // backs off and re-opens). May throw std::bad_alloc (the caller's net logs it and re-opens).
        bool _eventLoop();
        // Close the directory handle after cancelling + draining any pending read. Never throws.
        void _closeWatch() noexcept;
        // Enumerate every project folder's *.jsonl into the index (paths cleared first), bump the epoch and
        // mark healthy. Enumeration runs OUTSIDE the lock; the swap is under it.
        void _fullIndex();
        // (Re-)enumerate one project folder's *.jsonl into the index.
        void _indexProjectDir(const std::wstring& projectDir);
        // Parse one completed notification buffer and apply its records. Returns true when anything applied.
        bool _applyBuffer(const unsigned char* data, unsigned long bytes);
        // Invoke the change callback now (outside the lock) and restart the coalescing clock.
        void _notifyNow() noexcept;

        std::wstring _root; // the watched folder
        ChangeCallback _onChange; // set once by Start, before the thread runs
        std::thread _thread;
        std::atomic<bool> _started{ false };
        void* _stopEvent{ nullptr }; // HANDLE (manual-reset): set by Stop()
        void* _ioEvent{ nullptr }; // HANDLE (manual-reset): the OVERLAPPED completion event
        void* _dir{ nullptr }; // HANDLE to the root (INVALID_HANDLE_VALUE / null when closed)
        std::vector<uint32_t> _buffer; // DWORD-aligned notification buffer (ReadDirectoryChangesW requires it)
        // Storage for the OVERLAPPED the pending read uses. Kept opaque (8-byte aligned, placement-constructed
        // in the .cpp) so this header stays free of <windows.h> and its min/max macros; the .cpp
        // static_asserts that an OVERLAPPED fits.
        alignas(8) unsigned char _overlapped[64]{};
        bool _pending{ false }; // a read is in flight on _dir (thread-confined)
        unsigned long _lastError{ 0 }; // Win32 error of the last failed open/arm — the outage log line's cause (thread-confined)

        mutable std::mutex _mtx; // guards _index, _epoch, _healthy
        TranscriptIndex _index;
        uint64_t _epoch{ 0 };
        bool _healthy{ false };

        int64_t _lastNotifyTick{ 0 }; // GetTickCount64 of the last callback (thread-confined)
        bool _notifyPending{ false }; // changes applied but not yet announced (coalescing; thread-confined)
    };

    // Minimum spacing between two change callbacks while notifications keep streaming. A burst of
    // appends across many sessions becomes ONE scanner wake per window instead of one per record.
    inline constexpr int64_t kTranscriptWatchCoalesceMs = 100;
    // How long the watcher waits before re-trying a failed open/arm (no projects folder yet, the folder
    // was deleted, an access error). While it waits, Healthy() is false and the scanner polls as before.
    inline constexpr int64_t kTranscriptWatchRetryMs = 5000;
    // The notification buffer: 256 KiB on a local volume (thousands of records between two reads), 64 KiB
    // on a network share (ReadDirectoryChangesW rejects larger there). An overflow is survivable (a full
    // re-index), just wasteful, so the local size errs large.
    inline constexpr unsigned long kTranscriptWatchBufferBytes = 256u * 1024u;
    inline constexpr unsigned long kTranscriptWatchNetworkBufferBytes = 64u * 1024u;
    // How many overflow (empty) reads in a row the watcher accepts before it treats the watch as broken. A
    // genuine overflow is followed by a normal read; a read that completes empty again and again means the
    // watch no longer delivers changes, and re-indexing on each one would spin a core. Past this streak the
    // watcher closes the handle and re-opens after kTranscriptWatchRetryMs, and the scanner polls meanwhile.
    inline constexpr int kTranscriptWatchMaxOverflowStreak = 4;
}
