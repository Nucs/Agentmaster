// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — CommandWatch: bind to SLASH COMMANDS the user types into a managed Claude
// session, and AWAIT the session's follow-up activity asynchronously (COMMANDS.md).
//
// The Fleet's PULL lane already tails every live session's transcript (SessionScanner ->
// ParseTranscriptDelta), and a typed slash command leaves a deterministic echo there — a
// `type:"user"` line whose string content carries `<command-name>/x</command-name>` +
// `<command-args>…</command-args>` (current Claude Code writes even built-ins this way; the
// older strata used a `type:"system","subtype":"local_command"` line with the same tags). The
// parser surfaces those echoes as ORDERED `TranscriptEvent::Kind::Command` events (they remain
// NOISE for the state machine — never a turn event, exactly as before), and the scanner feeds
// them here, along with the two follow-up signals a binding can await: file-writing tool_use
// blocks (Write/Edit `file_path`s) and turn boundaries (a terminal stop_reason / an interrupt).
//
// A BINDING says "when /name is sighted, await X, then fire". The one await shape v1 ships is
// the MARKDOWN await (the /handover integration): after the command, watch for the session
// writing a `.md` file (the Write/Edit tool_use names the path; the DISK tells us when the file
// actually exists — a tool_use line only proves the request, the write may still be pending
// approval), then fire the bound handler with (sessionId, mdPath, args). Every await is BOUNDED
// so state can never leak: a sighting expires after kCommandAwaitMaxTurnEnds turn boundaries
// with no match (the command's own turn + one clarification turn), and unconditionally at
// kCommandAwaitDeadlineMs; sightings are FIFO per session (a second /handover before the first
// resolves arms its own await; one md write satisfies the oldest unmatched), and a session's
// pendings are capped. Sightings are accepted only when FRESH (the line's own `timestamp` within
// kCommandSightingFreshMs of now) so a history replay — a restored/adopted session's initial
// backlog read, a truncation rewind — can never re-fire an old command.
//
// Threading: the scanner WORKER thread is the sole feeder (OnCommandSighting / OnFileToolWrite /
// OnTurnEnd / Tick, all in transcript order); bindings are registered once at engine init before
// the scanner starts. Handlers FIRE ON THE SCANNER THREAD — the engine's binding fans out to
// per-window sinks that marshal onto their own UI dispatchers (the activate-sink idiom), so no
// UI work happens here. All state is mutex-guarded anyway (cheap; the class stays correct if a
// second feeder ever appears). Plain C++ + Win32 (no WinRT), unit-testable standalone: the disk
// probe is injectable (SetFileProbe) and the clock is a parameter.

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Agentmaster
{
    // --- tunables (DecideAdvance-style: explicit, header-visible, static_assert-able) ---
    // A command sighting is armed only when the transcript line's OWN timestamp is within this
    // window of now — the replay guard: primed-cursor gating alone cannot reject a small adopted
    // transcript's one-shot history read (it primes during the same pass), but an old command's
    // own stamp can. Generous vs the scanner cadence (<=2.5s ticks) so a stalled scan still arms.
    inline constexpr int64_t kCommandSightingFreshMs = 60'000;
    // Hard ceiling on any pending await's lifetime (arm -> fire), whatever else happens: a
    // rejected/never-executed write, a session that stops emitting turn boundaries, a stuck file.
    inline constexpr int64_t kCommandAwaitDeadlineMs = 15 * 60'000;
    // An UNMATCHED sighting survives this many turn boundaries: the command's own turn plus one
    // clarification round. Scoping the await to the command's vicinity is what keeps an unrelated
    // `.md` edit three turns later from ever satisfying a forgotten /handover (spawning a
    // spurious tab is worse than missing one — the user just re-runs the command). A MATCHED
    // sighting (>=1 path collected) never ages by turns — the FIRST turn end after a match SEALS
    // it instead (the collection boundary, below), and only the deadline bounds it from there (a
    // write may be sitting behind a permission approval across turn boundaries).
    inline constexpr int kCommandAwaitMaxTurnEnds = 2;
    // Per-session pending cap (bounded memory; oldest evicted). Generous: even chained handovers
    // resolve within a turn, so two concurrently-pending sightings is already unusual.
    inline constexpr size_t kCommandMaxPendingPerSession = 4;
    // MULTI-FILE collection settle (the no-turn-end fallback): a matched pending normally SEALS —
    // stops collecting further markdown writes and becomes fire-eligible — at the first turn end
    // after its match (the command's definition ends the turn right after writing, so every file
    // of one command lands in ONE turn). When no turn end ever arrives (the session died mid-turn,
    // a truncated tail), this much write-silence after the LAST collected path seals it instead,
    // so a matched await can still fire without waiting out the full deadline. Comfortably above
    // the scanner cadence (<=2.5s) and any realistic gap between one command's several writes.
    inline constexpr int64_t kCommandMatchSettleMs = 20'000;

    // A typed slash command extracted from its transcript echo. `name` is the bare command word —
    // leading '/' stripped, ASCII-lowercased (bindings match case-insensitively; Claude Code
    // command names are ASCII slugs). `args` is the <command-args> body verbatim (may be
    // multi-line), outer whitespace trimmed; empty when the tag is absent/empty.
    struct SlashCommand
    {
        std::wstring name;
        std::wstring args;
    };

    // PURE: extract a SlashCommand from a command-echo body — the `<command-name>…</command-name>`
    // (+ optional `<command-args>…</command-args>`) tag pair found in a `type:"user"` line's string
    // content OR a `type:"system","subtype":"local_command"` line's `content`. Order-AGNOSTIC over
    // the tags: the June-2026 strata wrote <command-message> first, current Claude Code writes
    // <command-name> first — both parse (and a raw future shape keeps parsing as long as the
    // name tag survives). False when no non-empty <command-name> tag is present.
    bool ParseCommandEcho(std::wstring_view text, SlashCommand& out);

    // PURE: is this a markdown file path (".md", case-insensitive)? The markdown await's filter.
    bool IsMarkdownPath(std::wstring_view path);

    // PURE: absolute-vs-relative for the tool-input path resolve (drive-rooted `X:\…`/`X:/…`, a
    // UNC `\\server\…`, or a rooted slash form). A relative Write/Edit file_path is resolved
    // against the session's working dir before probing the disk.
    bool IsAbsolutePathForWatch(std::wstring_view path);

    // PURE: pick which of a message's file-write paths a markdown await should match — the FIRST
    // markdown path whose leaf contains `preferLeafContains` (case-insensitive; the /handover
    // binding passes "handover" so its own `HANDOVER-*.md` outranks an incidental doc edit in the
    // same message), else the FIRST markdown path, else "". Batch-scoped: across separate
    // messages, first-seen still wins (we cannot hold an armed await hostage to a name).
    std::wstring PickMarkdownWritePath(const std::vector<std::wstring>& paths, std::wstring_view preferLeafContains);

    // PURE (safeguard): is this a path we are willing to MATCH, probe, and hand to an action —
    // non-empty, bounded (<= kWatchMaxPathChars), and free of control characters, double quotes,
    // and pipes? A matched path flows into a log line, a disk probe, and ultimately the
    // successor's single-line launch prompt — a transcript field carrying a newline / quote / NUL
    // (malformed or adversarial tool input; none is legal in a real Windows path) must be
    // rejected at the MATCH, not discovered downstream. `|` is rejected for the same "never legal
    // in a real Windows path" reason AND because it is the multi-path fan-out separator
    // (JoinWatchPaths), so a hostile path can never smuggle a fake second path through the
    // payload. Applied to the resolved path in OnFileToolWrite.
    inline constexpr size_t kWatchMaxPathChars = 4096;
    bool IsSaneWatchPath(std::wstring_view path);

    // PURE: the fan-out payload form of a fired await's PATH SET. A fire can carry several
    // markdown files (one /handover writing HANDOVER-a.md + HANDOVER-b.md — the multi-file
    // collection below), but the per-window command-action sink payload is ONE string — so the
    // paths ride '|'-joined ('|' is illegal in a real Windows path and IsSaneWatchPath rejects it
    // per-path, making the separator unambiguous). SplitWatchPaths tolerates empty segments
    // (dropped) so a hand-built payload can't produce phantom empties.
    std::wstring JoinWatchPaths(const std::vector<std::wstring>& paths);
    std::vector<std::wstring> SplitWatchPaths(std::wstring_view payload);

    // Durable per-session COMMAND PROGRESS (the restart-resilience state; COMMANDS.md §3a) — what
    // survives an app restart so a transcript-history replay can neither DOUBLE-PROCESS a command
    // nor lose one that was mid-await:
    //   * processedMs — the FIRED watermark: the max transcript-line timestamp of every sighting
    //     this session has already fired. A replayed echo at/under it never re-arms (the
    //     double-fire guard: resuming a handed-over session moments later, a crash right after a
    //     fire, a truncation rewind — all replay the echo fresh enough to pass the 60s gate).
    //   * armed — the (command, lineTsMs) markers of sightings that ARMED but have not resolved
    //     (fired/expired) yet. A replayed echo matching a marker REVIVES its await even when the
    //     stamp is long past the freshness window (the crash/shutdown-mid-await case), with the
    //     ORIGINAL timestamp as its arm time so the 15-min deadline keeps its absolute meaning —
    //     a marker older than the deadline is pruned at load and never revives. An echo that is
    //     neither fresh nor marked never arms (an adopted/foreign transcript's deep history stays
    //     inert, exactly as before).
    // Persisted per session through an injectable store seam (SetProgressStore below; the engine
    // wires it to the SessionStore KV — session-store/<sid>.json, key kSessionStoreCommandProgressKey);
    // encode/decode are PURE and tolerant (malformed/unknown input decodes to the empty default).
    struct CommandProgress
    {
        int64_t processedMs{ 0 };
        std::vector<std::pair<std::wstring, int64_t>> armed; // (bare lowercase command, lineTsMs)
    };
    std::wstring EncodeCommandProgress(const CommandProgress& p); // "" for the empty default (the store removes the key — sparse)
    CommandProgress DecodeCommandProgress(std::wstring_view encoded);

    // Binds slash commands to awaited follow-up activity. One process-wide instance, owned by the
    // Engine beside the scanner that feeds it (Engine::commandWatch).
    //
    // EXCEPTION CONTAINMENT (safeguard): every public feed (OnCommandSighting / OnFileToolWrite /
    // OnTurnEnd / Tick / DropSession) is SELF-CONTAINED — its body runs under try/catch and a
    // failure is logged (LogSwallowedException, the never-lose-a-swallowed-exception policy) and
    // swallowed, so the SCANNER WORKER can never lose a reconcile pass (or the process a thread)
    // to a watch bug, a throwing injected probe, or a throwing bound handler. A bound handler is
    // additionally caught PER FIRE (_fire), so one bad handler cannot block a later fire, and a
    // throwing injected file probe reads as "file absent" (retried next Tick) rather than
    // propagating. The scanner's own feed sites therefore need no guards of their own.
    class CommandWatch
    {
    public:
        // sessionId, the resolved absolute md paths (each verified present on disk, in the order
        // written — ONE command may produce several HANDOVER-*.md files), the command's args.
        using MarkdownReadyHandler = std::function<void(const std::wstring& sessionId, const std::vector<std::wstring>& mdPaths, const std::wstring& args)>;

        // Register the MARKDOWN await for `commandName` (bare, lowercase — "handover"): when the
        // command is sighted in any managed session, the `.md` Write/Edits after it are COLLECTED
        // (every markdown whose leaf contains `preferLeafContains`; a batch's first markdown as
        // the fallback while nothing collected yet) until the turn ends (the SEAL — plus the
        // kCommandMatchSettleMs no-turn-end fallback); once every collected file exists on disk
        // the handler fires ONCE with all of them (scanner thread). One binding per name (last
        // wins; lookup is name-EXACT — no prefix aliasing). Register at engine init BEFORE the
        // scanner starts — the feeder assumes the binding set is stable.
        void BindMarkdownAwait(std::wstring commandName, std::wstring preferLeafContains, MarkdownReadyHandler handler);

        // Durable progress store (COMMANDS.md §3a — restart resilience): `load` returns the
        // session's encoded CommandProgress ("" == none), `save` persists it ("" == remove). The
        // engine wires these to the SessionStore KV; tests inject an in-memory map; UNSET == the
        // pre-persistence semantics (freshness gate only, nothing durable). Set before feeding.
        using ProgressLoadFn = std::function<std::wstring(const std::wstring& sessionId)>;
        using ProgressSaveFn = std::function<void(const std::wstring& sessionId, const std::wstring& encoded)>;
        void SetProgressStore(ProgressLoadFn load, ProgressSaveFn save);

        // --- scanner feeds (transcript order within a session; scanner worker thread) ---
        // A command echo. `lineTsMs` is the transcript line's own timestamp (0 == absent — never
        // armed). Arms when FRESH (within kCommandSightingFreshMs) or REVIVED (a persisted armed
        // marker matches — the restart-mid-await case); an echo at/under the session's fired
        // watermark never re-arms (the durable double-processing guard). SAME-FAMILY SUPERSEDE:
        // bindings sharing a leaf hint (e.g. /handover + /handover-here, both "handover") await
        // the same indistinguishable file family, so a new family sighting RE-AIMS the await —
        // an older UNSEALED family pending with no collected paths is superseded (removed, its
        // marker retired: the newest handling path wins); one that did collect is defensively
        // sealed (fires with its own files); sealed pendings are untouched. At most ONE unsealed
        // family pending exists per session, so a family write has exactly one possible owner —
        // the two commands can never race each other's files. Unbound names are ignored (no
        // state, no logs, no progress reads).
        void OnCommandSighting(const std::wstring& sessionId, const SlashCommand& cmd, int64_t lineTsMs, int64_t nowMs);
        // The file-writing tool_use paths of one assistant message (Write/Edit file_path values,
        // block order). `sessionCwd` resolves a relative path. The oldest UNSEALED pending of the
        // session collects the batch's qualifying markdowns (multi-file: every hint-matching one;
        // the batch's first markdown only while the pending has none).
        void OnFileToolWrite(const std::wstring& sessionId, const std::vector<std::wstring>& paths, const std::wstring& sessionCwd, int64_t nowMs);
        // A turn boundary (terminal stop_reason / user interrupt): a MATCHED pending SEALS (its
        // collection is complete — fires as soon as every file is on disk, often immediately
        // here); unmatched pendings age one turn and expire past kCommandAwaitMaxTurnEnds.
        void OnTurnEnd(const std::wstring& sessionId);
        // Periodic sweep (every scanner pass): deadline expiry, the settle-seal fallback, + the
        // disk poll that fires a sealed await the moment its last file lands.
        void Tick(int64_t nowMs);
        // The session left the live set (archived/removed) — drop its in-memory pendings + cached
        // progress. The PERSISTED progress (watermark + armed markers) deliberately survives:
        // it is what makes a later resume/replay of this session safe (no re-fire) and able to
        // revive a mid-await command.
        void DropSession(const std::wstring& sessionId);

        // Test seam: replace the "does this file exist with content?" probe (default: a real
        // GetFileAttributesExW size>0 check). Set before feeding; not thread-synchronized against
        // in-flight feeds.
        void SetFileProbe(std::function<bool(const std::wstring&)> probe);

        // Diagnostics / tests: the number of armed awaits currently pending.
        size_t PendingCount() const;

    private:
        struct Binding
        {
            std::wstring command; // bare lowercase name
            std::wstring preferLeafContains;
            MarkdownReadyHandler onReady;
        };
        struct Pending
        {
            uint64_t id{ 0 };
            std::wstring sessionId;
            std::wstring command;
            std::wstring args;
            int64_t lineTsMs{ 0 }; // the echo line's own timestamp — the durable identity (watermark/marker key)
            int64_t armedMs{ 0 }; // deadline anchor (a REVIVED pending anchors at its ORIGINAL lineTsMs)
            int turnEnds{ 0 };
            std::vector<std::wstring> matchedPaths; // collected in write order; empty until the first match
            bool sealed{ false }; // collection complete (turn end / settle) — fire once all paths present
            int64_t lastMatchMs{ 0 }; // when the newest path was collected (the settle fallback's anchor)
        };

        // Locked helpers (callers hold _mtx). _takeReady pops every SEALED pending whose files are
        // all on disk (probe true) into fire-able (pending, handler) pairs — invoked OUTSIDE the
        // lock — marking each fired sighting into the session's durable progress (watermark +
        // marker removal) BEFORE the handler ever runs (at-most-once across a restart).
        std::vector<std::pair<Pending, MarkdownReadyHandler>> _takeReadyLocked(int64_t nowMs);
        const Binding* _findBindingLocked(std::wstring_view command) const;
        bool _probeFile(const std::wstring& path) const;
        static void _fire(const std::vector<std::pair<Pending, MarkdownReadyHandler>>& ready);
        // Progress plumbing (callers hold _mtx). _progressLocked lazily loads + caches a
        // session's persisted CommandProgress (pruning armed markers past the deadline when
        // nowMs > 0); _saveProgressLocked persists the cache entry ("" when empty — the store
        // removes the key); _dropArmedMarkerLocked removes one (command, ts) marker + saves.
        CommandProgress& _progressLocked(const std::wstring& sessionId, int64_t nowMs);
        void _saveProgressLocked(const std::wstring& sessionId);
        void _dropArmedMarkerLocked(const std::wstring& sessionId, const std::wstring& command, int64_t lineTsMs);

        mutable std::mutex _mtx;
        std::vector<Binding> _bindings;
        std::vector<Pending> _pending; // FIFO per session (global order == arm order)
        uint64_t _nextPendingId{ 1 };
        std::function<bool(const std::wstring&)> _fileProbe; // empty => real disk probe
        ProgressLoadFn _progressLoad; // empty => no persistence (in-memory progress only)
        ProgressSaveFn _progressSave;
        std::unordered_map<std::wstring, CommandProgress> _progress; // per-session cache over the store
    };
}
