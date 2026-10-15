// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — SessionScanner: the interval reconciler (the PULL half of the design; the
// HooksBridge is the PUSH half). Hooks give low-latency, authoritative state, but they can be
// DROPPED (a missed `Stop` strands a session in Running forever) and they carry no assistant
// output text. A low-priority worker thread tails each LIVE session's transcript .jsonl and
// reconciles what the push path missed — capturing the latest assistant message, back-filling a
// human prompt whose UserPromptSubmit hook never arrived, and synthesizing a missed Stop — and
// it fans out a periodic liveness sweep so a dead claude.exe (crash / `/exit` with no SessionEnd)
// gets archived.
//
// SMART + PERFORMANT (the explicit ask): scope to live, non-archived sessions only; a cheap
// size stat (plus the unread-backlog flag — ShouldReadTranscriptDelta) gates any read; only the
// byte DELTA past a saved offset is parsed, and a FIRST-SIGHT cursor on a big transcript starts at
// its tail window instead of replaying the whole history (FirstSightReadOffset); one coalesced
// pass per tick; cadence is ADAPTIVE (fast while a turn is Running, slow while Idle/Waiting) and
// the loop SLEEPS on a condvar at zero cost when nothing is live — a registry observer Wake()s
// it the instant a session goes live.
//
// EVENT-DRIVEN (HOOKS.md "Transcript watcher", the 2026-09-25 35%-of-a-core finding): a
// TranscriptWatcher holds ONE ReadDirectoryChangesW watch on <claude config>\projects (subtree on).
// It answers "where is this session's transcript" from an index instead of walking every project
// folder, tells each pass WHICH sessions' files changed (so an unchanged cold session costs no stat
// and no side-folder walk), and Wake()s the worker the moment a transcript is appended. Sessions whose
// tail can DEMOTE them (Running / NeedsApproval / a sent-but-unechoed prompt) keep their per-pass
// stat as a belt, and every other session is re-stat'ed on a slow resync; if the watch cannot be
// established the scanner simply polls as it always did.
//
// Plain C++ + Win32 (no WinRT), like the rest of AgentMaster/, so it is unit-testable standalone.
// The transcript parse is a PURE function (ParseTranscriptDelta); the thread wraps it with the
// filesystem read, the registry writes, and the liveness fan-out. The liveness CHECK itself is
// WinRT (it walks XAML tabs), so it is delegated to app-layer probes the scanner merely TICKS.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "SessionModels.h" // SessionInfo (passed to the per-session reconcile)

namespace Agentmaster
{
    class SessionRegistry;
    class CommandWatch; // COMMANDS.md — the slash-command binding/await engine the scanner feeds
    class TranscriptWatcher; // HOOKS.md "Transcript watcher" — the ReadDirectoryChangesW change feed (TranscriptWatcher.h)
    struct ChangedSessions; // the session database's change feed (TranscriptWatcher.h, SESSIONS_DB.md §7.6)

    // --- cadence + bound tunables (see DecideAdvance-style separation: values are explicit) ---
    inline constexpr int64_t kScanRunningMs = 300; // a turn is in progress: poll quickly
    inline constexpr int64_t kScanLiveIdleMs = 2500; // live but Idle/Waiting: poll lazily
    inline constexpr int64_t kScanSweepMs = 2500; // min interval between liveness-probe ticks
    inline constexpr int64_t kScanStopQuiescenceMs = 2000; // transcript must be this quiet before a synthesized Stop
    inline constexpr int64_t kScanPresenceIdleQuiescenceMs = 5000; // presence-idle release BASE floor (ShouldSynthesizeStopFromPresenceIdle): the transcript must be quiet THIS long — longer than kScanStopQuiescenceMs, to outlast the ~2s S-lane presence-refresh lag — before claude's at-rest ("idle") heartbeat releases a NON-terminal-tail turn. Used directly for NeedsApproval; a RUNNING cleared-tail turn needs the longer kScanPresenceIdleRunningQuiescenceMs below (its shape is ambiguous with an API-retry pause)
    inline constexpr int64_t kScanPresenceIdleRunningQuiescenceMs = 30000; // RUNNING-state presence-idle release floor (the idle<->running flap fix): a Running session with a CLEARED (empty) stop_reason must be quiet — NO transcript append AND NO "busy" heartbeat, BOTH of which reset quietForMs in _reconcileSession — for THIS long before its "idle" heartbeat releases it. FAR longer than the base floor, because a cleared-tail Running turn is AMBIGUOUS: a genuine no-op/finished turn is indistinguishable at an instant from a turn merely PAUSED behind a "No response from API · Retrying" backoff / slow first token / streaming stall (claude reports "idle" in both). The long floor lets a real pause ride — it appends or flips "busy" within the window — while still releasing a session truly at rest, killing the Running<->WaitingForInput flap vs recon-run (the reported "card bg" flap)
    inline constexpr int64_t kScanMaxDeltaBytes = 1 << 20; // read at most 1 MiB of new transcript per tick
    inline constexpr int64_t kScanForceConsumeBytes = 4 << 20; // a 4 MiB run with no newline -> skip it (corrupt/binary guard)
    inline constexpr int64_t kScanDiscoverMs = 1500; // idle keep-ticking cadence (drives each window's observer probe + liveness sweep when nothing is live)
    inline constexpr int64_t kScanRunRepairFreshMs = 15000; // a consumed turn event must be this FRESH (file mtime) to synthesize a missed UserPromptSubmit — a stalled/late scan must not revive an old write (belt+suspenders BEHIND the primed-cursor gate below, which is what actually blocks the history replay: mtime alone cannot — a window closed mid-turn and reopened within the window replays a FRESH file)
    inline constexpr int64_t kScanSubagentFreshMs = 15000; // a SUBAGENT/tool-result side file written within this window == the turn is actively working inside a Task/Agent subagent (the parent <id>.jsonl is quiescent) -> hold/synthesize Running (SubagentActivityUnixMs, ProcessInspect)
    inline constexpr int64_t kScanExternalWorkGraceMs = 20000; // "OUTLIVED the turn" proof margin: external work counts as OUTLIVING a FINISHED/aborted turn only when it postdates the parent transcript's last write by MORE than this — separating genuinely ongoing background work (an in-process TEAMMATE / a run_in_background Agent writing subagents/*.jsonl for minutes-hours after the lead's end_turn; a live SHELL job keeping presence "shell"/"busy") from turn-tail RESIDUE (a classic subagent's final flush lands us-before end_turn and post-Esc dying subagents keep flushing up to kScanSubagentFreshMs; "busy" lingers a ~2s S-lane tick after a real Stop). Must exceed kScanSubagentFreshMs so an interrupted turn's dying flushes can NEVER read as outliving it (the 291 MB-log oscillation class)
    static_assert(kScanExternalWorkGraceMs > kScanSubagentFreshMs, "outlive margin must exceed the dying-subagent flush window, or post-Esc residue promotes Waiting->Running and oscillates against recon-stop");
    // Agentmaster (the TURN-START RACE — HOOKS.md "State machine", NewerPromptNotYetInTranscript): the
    // longest a submitted prompt that the transcript has not recorded yet may hold the tail-judging synths
    // (recon-stop, recon-stop-idle, recon-error), counted from the prompt's hook stamp.
    // It must outlast a legitimate LANDING — claude writes the prompt line only after its UserPromptSubmit
    // hooks finish:
    //   * normally ~0.2-0.5 s (the exec-form forwarder);
    //   * up to the forwarder's own 10 s timeout (kHookForwarderTimeoutSeconds) if it hangs;
    //   * longer only with a slow user/project hook. Seen: a 15 s "cancel window" hook (2026-09-10), and a
    //     35 s landing in the shell-form era, whose forwarder timeout was 30 s.
    // It also caps the cost of a prompt that NEVER lands (a submit aborted with Esc during the hook phase,
    // or re-submitted before the first one took: 17 in 97 days of the release log). Such a session reads
    // Running this long before the classic verdict runs. The same order as
    // kScanPresenceIdleRunningQuiescenceMs, the existing "how long a Running session with no evidence may
    // sit" policy.
    inline constexpr int64_t kScanPromptLandingGraceMs = 30000;

    // --- the TranscriptWatcher-driven refresh (HOOKS.md "Transcript watcher") ---
    // A COLD session (see ScanNeedsFreshTail) is re-stat'ed and has its side folders re-walked only when the
    // watcher reports a change for it — plus this slow resync, the belt against a notification that never
    // came (a writer that holds its handle open without flushing produces none until it does).
    inline constexpr int64_t kScanColdResyncMs = 30000;
    // A HOT session's side-folder activity (SubagentActivityUnixMs) is re-walked on a watcher change or at
    // least this often. Its transcript stat stays per-pass; the side folders only feed the quiescence clock,
    // whose shortest floor (kScanStopQuiescenceMs, 2 s) this belt stays well inside.
    inline constexpr int64_t kScanHotSideRefreshMs = 1000;
    // With NO healthy watcher (no projects folder yet, an access error), an unresolved session retries the
    // classic every-project-folder walk (ResolveClaudeTranscriptPath) at most this often instead of every
    // pass. That walk was 28% of the scanner's time on the 60-session release fleet.
    inline constexpr int64_t kScanResolveRetryMs = 3000;
    // With a healthy watcher, an unresolved session cross-checks the index against the classic walk at most
    // this often — the belt against an index bug (a hit is logged as "[watch] index missed").
    inline constexpr int64_t kScanIndexCrossCheckMs = 60000;
    // The minimum gap between two passes, even when woken. A burst of wakes (registry notifies from dozens of
    // sessions, watcher batches) collapses into ONE follow-up pass instead of back-to-back passes.
    inline constexpr int64_t kScanMinPassGapMs = 100;

    // PURE: must this pass re-stat the session's transcript (GetFileAttributesEx), or may it reuse the
    // cached size + last-write time from the last stat?
    //   watcherHealthy  — the TranscriptWatcher's index is authoritative (false => the classic every-pass stat).
    //   statValid       — a cached stat exists for the current path.
    //   epochChanged    — the watcher re-indexed since the cached stat (start / overflow / a re-opened watch).
    //   seqChanged      — the watcher saw a change to this session's transcript since the cached stat.
    //   hot             — ScanNeedsFreshTail: the session's tail can DEMOTE it, so it keeps a per-pass stat.
    //   sinceLastStatMs — ms since the cached stat was taken (the cold resync).
    // Returns true when any of those says the cache may be stale; false only for a cold, unchanged session
    // inside its resync window. Never throws.
    inline bool ShouldRefreshTranscriptStat(bool watcherHealthy, bool statValid, bool epochChanged, bool seqChanged, bool hot, int64_t sinceLastStatMs) noexcept
    {
        if (!watcherHealthy || !statValid || epochChanged || seqChanged || hot)
        {
            return true;
        }
        return sinceLastStatMs >= kScanColdResyncMs;
    }

    // PURE: must this pass re-walk the session's side folders (SubagentActivityUnixMs — two directory
    // enumerations), or may it reuse the cached newest side-file write?
    //   watcherHealthy  — as above (false => the classic every-pass walk).
    //   sideValid       — a cached value exists for the current path.
    //   epochChanged    — the watcher re-indexed since the cached walk.
    //   seqChanged      — the watcher saw a change under this session's subagents/ or tool-results/.
    //   hot             — ScanNeedsFreshTail (its belt is kScanHotSideRefreshMs, not the cold resync).
    //   sinceLastScanMs — ms since the cached walk.
    // Returns true when the cache may be stale. Never throws.
    inline bool ShouldRefreshSideActivity(bool watcherHealthy, bool sideValid, bool epochChanged, bool seqChanged, bool hot, int64_t sinceLastScanMs) noexcept
    {
        if (!watcherHealthy || !sideValid || epochChanged || seqChanged)
        {
            return true;
        }
        return sinceLastScanMs >= (hot ? kScanHotSideRefreshMs : kScanColdResyncMs);
    }

    // PURE: is this session HOT — can a stale view of its transcript tail make a synth act WRONGLY on it?
    // True while a turn can still be DEMOTED from the tail (Running / NeedsApproval: recon-stop, the
    // presence-idle release, recon-block and recon-error all read the tail's quiet time), and while a sent
    // prompt awaits its echo (the lost-send verdict fails it 15 s after delivery unless the echo line is read).
    // A cold session's synths only PROMOTE on new lines, so a late notification can at worst delay them.
    //   s — the pass snapshot of the session.
    // Returns true for a hot session (it keeps the classic per-pass stat). Never throws.
    inline bool ScanNeedsFreshTail(const SessionInfo& s) noexcept
    {
        if (s.state == SessionState::Running || s.state == SessionState::NeedsApproval)
        {
            return true;
        }
        for (const auto& p : s.queue)
        {
            if (p.status == PromptStatus::Sent && !p.echoed)
            {
                return true; // an unacknowledged delivery: its echo must be read the moment it lands
            }
        }
        return false;
    }

    // PURE: is this session DORMANT — a live session whose tab WE host but whose connection has never
    // started? That is a window-restored tab nobody has entered yet, and (since Deactivate parks a tab in
    // exactly that state) a tab parked by the tab menu's "Deactivate ▸". No process of ours can be running a
    // turn in a dormant session, so the scanner must never PROMOTE one to Running — recon-run,
    // recon-resume, recon-resume-presence and recon-subagent are held for it. Every signal those synths read
    // can only be residue of a claude that is already gone: the last transcript lines the claude a
    // Deactivate just closed wrote (consumed by a cursor that was primed while it ran), its subagents'
    // final side-file flushes, a heartbeat it published while dying. A window-restored tab never shows
    // that residue — its cursor primes on history and nothing writes — and the parked tab must read the
    // same. The at-rest synths (recon-stop, recon-block, recon-error and their presence twins) are NOT
    // gated, so both shapes keep refining their at-rest display state from the tail exactly as before.
    // ADOPTED (external) sessions are excluded: `started` is never maintained for a session we only observe
    // (its ConPTY is the user's shell, or no tab of ours hosts it at all), so it reads false for a session
    // that is in fact running, and holding its promotions would freeze its card at rest.
    //   s — the pass snapshot of the session.
    // Returns true when the session is live, ours (not adopted) and not started. Never throws.
    inline bool ScanSessionIsDormant(const SessionInfo& s) noexcept
    {
        return s.live && !s.started && !s.external;
    }

    // PURE + total (the 2026-08-14 stuck-Running WEDGE, session 6434e5f8): what to do when a delta
    // window read found NO newline. A SINGLE transcript line can outgrow the 1 MiB read window — a
    // huge pasted / tool_result user line (the live wedge: two PDF-read tool_results of 1.51 MiB
    // and 2.0 MiB) — and the old handling then consumed NOTHING while the remainder stayed under
    // the 4 MiB force-consume guard: the cursor froze at the same offset on EVERY pass, the parse
    // fold (lastStopReason / interrupted / pendingInteractiveTool) stayed mid-turn forever, and
    // every pull release was structurally dead — recon-stop needs a terminal tail, recon-stop-idle
    // REFUSES a pending non-terminal stop_reason, recon-block needs an interactive tool — so the
    // session sat Running 17+ minutes with claude provably idle, its real Stop already consumed by
    // a type-ahead phantom (whose documented safety net is exactly this scanner's quiescent synth).
    // Inputs: got = bytes this read returned; avail = bytes past the cursor at the pass's stat.
    //   * PartialInFlight — the read reached the stat'd end: the whole remainder is ONE
    //     unterminated append in flight; caught up on complete lines (primed), re-read whole once
    //     its newline lands.
    //   * Grow — more bytes exist past this read and the force cap allows a bigger window: RE-READ
    //     at min(avail, kScanForceConsumeBytes) so a 1-4 MiB line parses like any other line.
    //   * SkipRead — a full force-cap read with no newline: a corrupt/binary run. Advance the
    //     cursor past the PROVEN newline-less bytes only (never teleport to the file end — complete
    //     lines beyond the run must still parse on later passes).
    enum class DeltaNoNewline
    {
        PartialInFlight,
        Grow,
        SkipRead,
    };
    inline DeltaNoNewline DecideDeltaNoNewline(int64_t got, int64_t avail) noexcept
    {
        if (got >= avail)
        {
            return DeltaNoNewline::PartialInFlight; // read to the stat'd end — nothing more to grow into
        }
        if (got < kScanForceConsumeBytes)
        {
            return DeltaNoNewline::Grow; // the FIRST unconsumed line outgrew this window — a bigger read can finish it
        }
        return DeltaNoNewline::SkipRead; // a force-cap read verified newline-less — skip the run, not the file
    }

    // PURE + total (the 2026-09-25 stuck-Running report, release 0.6.8.11, session d1f3f198 — the
    // CATCH-UP STALL): should _reconcileSession READ the transcript on this pass at all?
    //
    // The read is WINDOWED (kScanMaxDeltaBytes, 1 MiB, per pass), but the gate in front of it used to
    // be `size != lastSize` ALONE — and lastSize was stamped with the new size after EVERY read,
    // including one the window CAPPED. So any read that stopped short of the stat'd end left complete,
    // unread lines past the cursor that no later pass would ever look at — until the file happened to
    // grow again. Three shapes hit it, all "the next pass retries" promises the size gate silently
    // broke: (1) the initial replay of a >1 MiB history (a restored / resumed / adopted / forked
    // session), (2) a >1 MiB append burst, (3) a transient failure (the file would not open, a grown
    // re-read came back short). A FINISHED turn writes nothing more, so the one append that could
    // unstick it never comes. The tail facts then stay mid-HISTORY for good; the live case parked on a
    // `stop_reason: tool_use` line from two days earlier. recon-stop needs a terminal tail, and the
    // presence-idle release deliberately never releases a Running session on a pending tool_use
    // (the API-retry flap fix). So claude sat provably idle while the session showed Running for 7+ hours.
    //
    // Read when EITHER the size changed (new bytes landed) OR the previous read did not reach the
    // size it was handed. Only a read that reached the stat'd end may go quiet; the remainder is then
    // at most one unterminated line in flight, which the next append completes. Steady state stays one
    // stat per pass and no read. A persistently failing open retries once per pass, which is cheap,
    // bounded and self-healing, and far better than freezing on stale facts.
    //   size               — the transcript's byte size from THIS pass's stat.
    //   lastSize           — the size stamped by the previous read (-1 before the first read).
    //   lastReadReachedEnd — whether that previous read covered every byte up to the size it was handed
    //                        (ScanState::readReachedEnd; false before the first read).
    // Returns true when this pass must call _readDelta. Never throws.
    inline bool ShouldReadTranscriptDelta(int64_t size, int64_t lastSize, bool lastReadReachedEnd) noexcept
    {
        return size != lastSize || !lastReadReachedEnd;
    }

    // PURE + total (FIRST-SIGHT TAIL SEEK — the companion of ShouldReadTranscriptDelta): the byte
    // offset a FRESH cursor starts reading a transcript at (a restored / resumed / adopted / forked
    // session, or a truncation rewind).
    //
    // Every scanner decision reads LATEST-WINS tail facts: the last assistant stop_reason, whether the
    // last human line was an interrupt, a pending AskUserQuestion, an unrecovered API error, the active
    // leaf, the context size, the current model, the newest recap. The last `window` bytes establish all
    // of those, so replaying the whole history from byte 0 buys nothing, and it is expensive. The
    // measured release fleet had 403 live transcripts totalling 1.3 GB (293 over 1 MiB, the largest
    // 87 MB), all read at startup. The old replay also re-fed ANCIENT history into the Auto-Testing
    // back-fill, and while it was still catching up it let tail-judging synths act on mid-history facts.
    // Starting at the tail window costs exactly what today's first pass already costs (≤ one window per
    // session) and yields the CORRECT facts on the very first pass.
    //
    // Tradeoffs, all deliberate:
    //   * prompts older than the window are not back-filled as Typed rows. The push hooks recorded them
    //     live, and the bounded queue history (kMaxQueueHistoryEntries) would trim them anyway.
    //   * a recap buried deeper than the window stays unseen until the next away_summary. Measured:
    //     the median last recap sits ~1 KiB from EOF, so this is rare.
    //   * CommandWatch / interrupt-hold feeds see this window exactly as they already saw the FINAL
    //     catch-up chunk; their own line-timestamp freshness gates keep replayed lines inert.
    // The caller aligns to the first COMPLETE line inside the window (FirstCompleteLineStart) and
    // falls back to the classic replay from 0 when the window lies inside a single line.
    //   size   — the transcript's byte size at first sight.
    //   window — the tail window to start inside (kScanMaxDeltaBytes; overridable for tests).
    // Returns 0 when the whole file fits the window (read it all — the classic behavior, unchanged
    // for every transcript ≤ 1 MiB) or either input is non-positive; else `size - window`. Never throws.
    inline int64_t FirstSightReadOffset(int64_t size, int64_t window = kScanMaxDeltaBytes) noexcept
    {
        if (size <= 0 || window <= 0 || size <= window)
        {
            return 0;
        }
        return size - window;
    }

    // PURE + total: the index of the first COMPLETE line inside a tail-seek window. A first-sight read
    // starts at an arbitrary byte (FirstSightReadOffset), so the window opens mid-line. Its leading
    // fragment must be dropped: it is garbage to the JSON parser, and a fragment of an embedded escaped
    // object must never be taken for a real line.
    //   window — the raw bytes read from the seek point (UTF-8; '\n' is never a continuation byte, so a
    //            byte search is UTF-8-safe).
    // Returns the offset just past the window's first '\n', where the first complete line begins.
    // Returns npos when there is no usable boundary: no newline at all, or the only newline is the
    // window's LAST byte (nothing follows it). In both cases the whole window lies inside one line that
    // began before the seek point (a >1 MiB final line), so its facts cannot be read from here. The
    // caller then falls back to the classic replay from byte 0. Never throws.
    inline size_t FirstCompleteLineStart(std::string_view window) noexcept
    {
        const size_t nl = window.find('\n');
        if (nl == std::string_view::npos || nl + 1 >= window.size())
        {
            return std::string_view::npos;
        }
        return nl + 1;
    }

    // One reconciled record extracted from a transcript .jsonl line (the PURE parser's output).
    struct TranscriptEvent
    {
        enum class Kind
        {
            UserPrompt, // a human message (content is a plain string / pure-text block; NOT a tool_result)
            Assistant, // an assistant message (carries its concatenated text + message.stop_reason)
            ToolResult, // a user message carrying a tool_result block — a tool completed (NOT a typed prompt)
            // The {"type":"last-prompt","leafUuid":…} ACTIVE-LEAF pointer. NOT a turn event — Claude writes
            // it whenever the conversation's active branch head moves, notably a double-ESC REWIND that
            // repoints the leaf BACKWARD with no new user/assistant line. Emitted as an ordered event (so
            // the scanner sees its position relative to an API-error line) carrying the leafUuid in `text`.
            // Lets the scanner tell that an API error is no longer "the last message" (the leaf moved off
            // it) and leave Error — see errorEpochLeaf / ApiErrorIsActiveLeaf / ShouldReleaseErrorOnLeafMove.
            // Only emitted when leafUuid is non-empty (a last-prompt for the trust dialog has none).
            LeafMarker,
            // A non-turn BOOKKEEPING line carrying only LINEAGE — its `uuid` + `parentUuid` (a
            // system/turn_duration child, an away_summary, a progress line, …). NOT a turn event (it never
            // touches state). Claude appends such children AFTER an API error (the error -> a
            // system/turn_duration child -> an away_summary), and the post-error last-prompt marker points
            // at that turn_duration CHILD — a DESCENDANT of the error, not a rewind off it. The scanner
            // folds these into the error's forward descendant frontier (errorBranchUuids) so
            // ApiErrorIsActiveLeaf recognizes the advanced leaf as still being the error tail. Emitted only
            // when both uuid + parentUuid are present (mode/permission-mode config lines carry neither).
            Node,
            // Agentmaster (COMMANDS.md): a typed SLASH-COMMAND echo — a `type:"user"` line whose
            // string content carries `<command-name>/x</command-name>` (+ `<command-args>`), or the
            // older `type:"system","subtype":"local_command"` shape with the same tags. NOT a turn
            // event (the echo stays NOISE for the state machine, exactly as when it was dropped
            // outright — it never clears tail facts, never counts as a human prompt); emitted as an
            // ORDERED event so the CommandWatch can bind to it and await the session's follow-up
            // activity (the /handover integration). Carries commandName/commandArgs + lineTsMs.
            Command,
        };
        Kind kind{ Kind::Assistant };
        std::wstring text; // UserPrompt: the prompt body. Assistant: concatenated text blocks (may be empty). LeafMarker: the leafUuid.
        std::wstring stopReason; // Assistant only: message.stop_reason ("end_turn" / "tool_use" / ...).
        // Assistant: this message's own uuid (used to SEED the API-error descendant frontier — the error
        // line's uuid is the root of its bookkeeping chain). Node: the bookkeeping line's uuid. "" otherwise.
        std::wstring uuid;
        // Node only: the bookkeeping line's parentUuid (the chain link the scanner walks to extend the
        // error's descendant frontier — a line whose parent is already on the frontier is itself on it).
        std::wstring parentUuid;
        // Assistant only: the name of an INTERACTIVE tool_use block in this message (one that blocks
        // on the user — AskUserQuestion), else "". Lets the scanner tell a session that is BLOCKED
        // waiting for the user to answer (-> NeedsApproval) from one genuinely working (a long Bash).
        std::wstring toolName;
        // Assistant only: this message's context occupancy = message.usage
        // input_tokens + cache_creation_input_tokens + cache_read_input_tokens + output_tokens
        // (≈ the size of the request that produced it). 0 when no usage block. The NEWEST assistant
        // line wins -> SessionInfo.contextTokens -> the board card's context-% adornment.
        int64_t tokens{};
        // Assistant only: this message's `message.model` — the model that actually produced it (e.g.
        // "claude-fable-5"). The NEWEST real assistant line wins -> SessionInfo.currentModel (the
        // board card / per-tab overlay / tab tooltip model adornment). "" when absent; the synthetic
        // API-error line carries the pseudo-model "<synthetic>", which the scanner fold skips (it is
        // not a model the session runs on).
        std::wstring model;
        // Assistant only: this message is the synthetic API-error turn-ender — Claude Code wrote it
        // with a top-level isApiErrorMessage:true ("API Error: …" / a rate or usage limit / "Prompt is
        // too long" / a 4xx-5xx / a dropped connection). The turn DIED here; the scanner synthesizes
        // SessionState::Error while this remains the transcript tail (ShouldSynthesizeError).
        bool apiError{ false };
        // Assistant only: the API error's HTTP status code from the transcript's top-level
        // apiErrorStatus (429 / 529 / 500 / 404 / 401), or 0 for a client-side error that carries
        // none ("Prompt is too long"). Preserved onto SessionInfo.errorStatus for the Error card.
        int apiErrorStatus{ 0 };
        // Command only (COMMANDS.md): the bare command word (leading '/' stripped, ASCII-lowered —
        // "handover") + the <command-args> body verbatim (outer-trimmed; "" when absent).
        std::wstring commandName;
        std::wstring commandArgs;
        // Command + UserPrompt: the transcript line's own `timestamp` -> Unix ms (0 when absent).
        // Command: the CommandWatch's replay guard — only a fresh stamp arms a sighting, so a
        // history replay (a restored/adopted session's initial read, a truncation rewind) can never
        // re-fire an old command. UserPrompt: the PULL echo-consume's staleness filter
        // (DELIVERY_PLAN.md R1 — NoteExternalPrompt's observedUnixMs): a replayed OLD user line
        // whose text matches a NEW Sent prompt must not vouch for that send, while a late-READ
        // fresh line still consumes — the line's own time decides, not our read time. Deliberately
        // NOT filled for the remaining kinds (per-line parse cost for nothing).
        int64_t lineTsMs{ 0 };
        // Assistant only (COMMANDS.md): the file-WRITING tool_use paths of this message — each
        // Write/Edit (and legacy MultiEdit) block's input.file_path, in block order. Feeds the
        // CommandWatch's markdown await (the /handover "look for the .md being written" signal);
        // empty for a message with no file-writing tool_use. Never affects the state machine.
        std::vector<std::wstring> fileWritePaths;
        // Agentmaster (CUT-OFF TURNS — HOOKS.md "Cut-off turns"). Assistant only: this line is Claude
        // Code's RESUME PLACEHOLDER (IsResumePlaceholder) — the synthetic "No response requested."
        // message a resuming claude appends when the conversation it loaded ends on a user line. It is
        // NOT model output and it ends no turn: when it follows a turn that was still in flight, that
        // turn was cut off. The scanner fold therefore keeps the open turn through it instead of reading
        // its terminal stop_reason as "the turn finished".
        bool resumePlaceholder{ false };
        // Agentmaster (CUT-OFF TURNS). ToolResult + UserPrompt: the line's top-level
        // interruptedByShutdown flag — Claude Code's OWN statement that a shutdown, not the user, aborted
        // the turn. It rides the tool_result of a tool the shutdown aborted and the
        // "[Request interrupted by user…]" marker that follows. Read from the 2.1.291 bundle; no line
        // carrying it existed in the 4,407 local transcripts on 2026-10-09, so the path is pinned by
        // synthetic fixtures only.
        bool shutdownInterrupt{ false };
    };

    struct TranscriptParse
    {
        std::vector<TranscriptEvent> events;
        size_t consumed{ 0 }; // wide-char count up to and INCLUDING the last '\n' (a partial tail is left)
        // Agentmaster: the Claude Code idle RECAP — the LAST {"type":"system","subtype":"away_summary"}
        // line seen IN THIS CHUNK, normalized (NormalizeRecapText: the "(disable recaps in /config)" hint
        // stripped). Empty if this chunk carried no recap. The scanner mirrors a non-empty value onto
        // SessionInfo.recap (the Triage-Board card tooltip); the agentmaster-cli `show` reader emits it.
        // NOT a turn event — it never affects the state machine (its presence is orthogonal to events).
        std::wstring recap;
        // Agentmaster (HOOKS.md "Compaction" — the stale "ctx N" after a /compact): the context size a
        // system/compact_boundary in THIS chunk reset the conversation to (its compactMetadata.postTokens),
        // reported ONLY when that boundary is the chunk's LAST context-bearing line — an assistant line
        // carrying usage AFTER it supersedes it (that line's own usage is the newer truth), so the value
        // is 0 then. 0 too when the chunk has no boundary, or the boundary predates postTokens (an older
        // Claude Code). The scanner folds a non-zero value into SessionInfo.contextTokens AFTER the event
        // loop: without it the board kept showing the PRE-compaction size (e.g. "ctx 991k") until the next
        // assistant line — which, after a manual /compact at rest, only comes with the user's NEXT prompt.
        // Deliberately a side field, NOT a new TranscriptEvent kind: the scanner's event chain ends in a
        // bare `else` that treats any unlisted kind as a HUMAN PROMPT (a turn event that clears the tail).
        int64_t compactPostTokens{ 0 };
    };

    // PURE + total (never throws): parse a UTF-16 chunk of NEW transcript text into ordered
    // reconciled events. Each complete (newline-terminated) line is one JSON object (Claude Code
    // transcript JSONL). A trailing line WITHOUT a newline is an append in flight — it is left
    // unconsumed (reflected in `consumed`) so the next read re-sees it whole. User-prompt
    // extraction is deliberately CONSERVATIVE (top-level string / pure-text content only,
    // `isMeta` + `isCompactSummary` lines skipped — the latter the synthetic "This session is
    // being continued…" /compact bridge every other transcript reader already skips — and
    // machine-injected CONTROL MARKERS — a local slash command's
    // NON-meta "<command-name>…"/"<local-command-stdout>…" echoes, `!` bash passthrough echoes,
    // injected reminders, teammate/agent wrappers; IsNoiseUserPrompt, exempting the interrupt
    // marker — skipped too) so a tool_result / meta / control line is never mistaken for a typed
    // prompt: a mere `/model` in an idle session used to read as a turn START and recon-run lit
    // it Running (the /model false-Running fix). The UserPromptSubmit hook is the primary path
    // and this only back-fills a dropped one.
    TranscriptParse ParseTranscriptDelta(std::wstring_view chunk);

    // PURE: does this assistant stop_reason mark the TURN as complete? "end_turn" is the common
    // case, but "stop_sequence" / "max_tokens" / "refusal" equally end the turn (nothing further
    // is coming without new input) — gating the reconcilers on end_turn ONLY left a turn that
    // ended any other way stuck Running forever (the missed-Stop backstop never fired) and let
    // the run-repair read its tail as "turn in progress". "tool_use" (mid-turn), "" (a user line
    // cleared it / none seen yet), and any UNKNOWN future reason read as in-flight — that is the
    // pre-existing default (everything != end_turn), so a new reason degrades to the old
    // behavior instead of inventing a turn boundary.
    inline bool IsTerminalStopReason(std::wstring_view reason) noexcept
    {
        return reason == L"end_turn" || reason == L"stop_sequence" || reason == L"max_tokens" || reason == L"refusal";
    }

    // PURE + total: should the reconciler synthesize a missed/folded UserPromptSubmit (-> Running)?
    // The Running MIRROR of the missed-Stop synthesis. A dropped UserPromptSubmit hook used to be
    // half-repaired: the scanner back-filled the PROMPT (NoteExternalPrompt) but never the STATE, so
    // the whole turn ran showing WaitingForInput/Idle — and with the session never Running, the
    // missed-Stop backstop (gated on Running) was disarmed too, so the turn's END also went
    // unnoticed. Same for the FOLDED variant (the next prompt's UserPromptSubmit lands mid-turn /
    // before the prior turn's late Stop, which then flips Running back to Waiting mid-turn).
    // Fires only when: this pass consumed ≥1 turn event (a human prompt or an assistant line — the
    // repair piggybacks on transcript appends, never on a quiet file); the cursor was PRIMED before
    // this pass (`primedBeforePass`: the tail had already caught up with the file end, so the
    // consumed events are a LIVE append — the initial history replay of a restored/adopted session
    // reads its tail window (FirstSightReadOffset; the WHOLE file when it fits one window, or on the
    // alignment fallback), and a window closed mid-turn leaves that history
    // ending "turn in progress" with a FRESH mtime, so without this gate a just-resumed, idle
    // claude lit up Running and STUCK there — recon-stop needs a terminal-stop tail to clear
    // it); the tail says a turn is IN PROGRESS (!IsTerminalStopReason(lastStopReason) — a user
    // line cleared it / an assistant line is mid-turn; a terminal tail is the missed-Stop's
    // territory); the write is FRESH
    // (sinceWriteMs <= kScanRunRepairFreshMs — a stalled scan must not revive an old write); and
    // the session sits in one of the states a missed prompt strands it in: Idle / WaitingForInput
    // (Running needs no repair) PLUS Error — a fresh turn event after an API error is the user
    // retrying, so it is the pull half of "come out of Error on the first change" (the push half is a
    // real UserPromptSubmit -> Running). NeedsApproval / Done are still excluded — "needs you / ended"
    // states a mere transcript line must never clear (a real hook still can, through the state machine).
    bool ShouldSynthesizeRunning(SessionState state, bool consumedTurnEvent, bool primedBeforePass, std::wstring_view lastStopReason, int64_t sinceWriteMs) noexcept;

    // PURE: is this user-message text the Claude Code marker for a turn the user ABORTED (Esc)?
    // An interrupt fires NO clean Stop hook, and the marker line clears the tracked stop_reason —
    // so without recognizing it, a Running session whose turn the user killed stayed Running
    // forever (the missed-Stop backstop, gated on a terminal stop_reason, could never fire). The
    // marker is authoritative "the turn is over NOW" (no more assistant output is coming), so it
    // is treated as a turn-ender. Matches both "[Request interrupted by user]" and the
    // "…for tool use]" variant via the stable prefix.
    inline bool IsUserInterruptMarker(std::wstring_view text) noexcept
    {
        constexpr std::wstring_view kPrefix = L"[Request interrupted by user";
        return text.size() >= kPrefix.size() && text.substr(0, kPrefix.size()) == kPrefix;
    }

    // PURE: does this tool_use name BLOCK on the user (the agent cannot proceed until the user
    // answers)? AskUserQuestion is the built-in interactive question tool. An unanswered one with a
    // quiescent transcript means the session is "needs you", NOT working — distinct from a pending
    // NON-interactive tool (a long Bash / web fetch) whose transcript is ALSO momentarily quiescent
    // but is genuinely running. Conservative allowlist (evidence-based): only names we KNOW block.
    inline bool IsInteractiveTool(std::wstring_view toolName) noexcept
    {
        return toolName == L"AskUserQuestion";
    }

    // PURE + total: has the user submitted a prompt that the transcript has NOT recorded yet? While that
    // holds, the parsed tail still describes the PREVIOUS turn, so it must not be judged as the end of the
    // current one: recon-stop, recon-stop-idle and recon-error all hold.
    //
    // THE TURN-START RACE (HOOKS.md "State machine"). Measured 2026-10-01: 1,840 false Waitings over 97 days
    // of the release log, 92% of that day's false Waitings, about 29% of that day's prompts.
    //   1. A prompt-carrying UserPromptSubmit sets Running, and every registry notify Wake()s the scanner,
    //      so a pass runs ~6 ms later (p90 83 ms).
    //   2. Claude writes the prompt line only after its UserPromptSubmit hooks finish. The line's own
    //      `timestamp` is the submit time, but its bytes land ~0.2-0.5 s later.
    //   3. So that pass sees the previous turn's terminal stop_reason, a transcript quiet for minutes, and
    //      a heartbeat not yet refreshed off "idle" (the ~2 s S-lane): the exact missed-Stop shape.
    //   4. recon-stop fired, the session read Waiting for ~220 ms, and recon-run undid it when the prompt
    //      line landed. A slash command is worse: its echo is not a turn event, so only the 20 s
    //      outlived-work promotion undid it.
    //
    // The proof is two timestamps whose order cannot lie:
    //   lastPromptUnixMs — TurnAccounting's stamp of the newest PROMPT-CARRYING UserPromptSubmit: the
    //                      forwarder's start time ($ts is taken before it even reads stdin), so it precedes
    //                      every byte claude can write for that prompt. An EMPTY twin, or the recon-run
    //                      synth, never stamps it (DELIVERY_PLAN.md R3).
    //   tailWriteUnixMs  — the transcript's last-write time at this pass's stat: the newest byte the parse
    //                      can possibly reflect.
    // lastPrompt STRICTLY newer than tailWrite means not one byte was written since the prompt was submitted,
    // so every turn end in the tail belongs to an EARLIER turn. A genuine missed Stop is never held: its
    // turn's end line was written after that turn's prompt, so tailWrite > lastPrompt.
    //
    // BOUNDED by kScanPromptLandingGraceMs after the prompt. A submit claude never records (Esc during the
    // hook phase, or a re-submit before the first one took) would otherwise hold Running forever, the
    // d1f3f198 stuck-Running shape. When the hold expires, the classic verdict runs.
    //   lastPromptUnixMs — the stamp above (0 = no prompt-carrying hook ever seen: a hookless session).
    //   tailWriteUnixMs  — the stat's last-write time (0 = unknown: no stat yet, or the file vanished).
    //   nowUnixMs        — the pass's clock. 0 skips the bound; that is the legacy, unbounded recon-error
    //                      behavior its pre-existing callers relied on.
    // Either stamp at 0 disables the guard (never suppress blind). Returns true while the synths must hold.
    // Never throws.
    inline bool NewerPromptNotYetInTranscript(int64_t lastPromptUnixMs, int64_t tailWriteUnixMs, int64_t nowUnixMs) noexcept
    {
        if (lastPromptUnixMs <= 0 || tailWriteUnixMs <= 0 || lastPromptUnixMs <= tailWriteUnixMs)
        {
            return false; // no prompt stamp, an unknown tail, or the transcript already moved past the prompt
        }
        if (nowUnixMs > 0 && nowUnixMs - lastPromptUnixMs >= kScanPromptLandingGraceMs)
        {
            return false; // the prompt never reached the transcript within the grace: stop holding the verdict
        }
        return true;
    }

    // PURE + total: should the reconciler synthesize a missed/forced Stop (-> WaitingForInput)? The
    // missed-Stop backstop, generalized. Fires from Running OR NeedsApproval — a session blocked on
    // the user (a real permission approval, OR a synthesized AskUserQuestion block below) whose turn
    // then ENDED must also be released; recon-stop being Running-ONLY left an approved session stuck
    // in NeedsApproval when its post-approval Stop hook was dropped. The turn is OVER when the tail
    // is a TERMINAL stop_reason (end_turn / …) or the user INTERRUPTED it. Requires the transcript
    // to have gone quiescent first (a mid-turn pause is not the end).
    // `externalWorkOngoing` (default false == the classic behavior): external work — an in-process
    // TEAMMATE / background Agent still writing side files, or a live SHELL job per claude's own
    // heartbeat — has provably OUTLIVED the turn's end (the kScanExternalWorkGraceMs proof, computed
    // once in _reconcileSession and shared with ShouldSynthesizeRunningFromExternalWork so the two
    // gates key on the SAME expression and stay MUTUALLY EXCLUSIVE — promotion active <=> demotion
    // held, never both). While it holds, a RUNNING session is NOT demoted to WaitingForInput even on a
    // terminal tail: "a shell or agent or teammate still running" reads Running. Scoped to Running
    // ONLY — the NeedsApproval release (an answered/approved session whose post-turn Stop was dropped)
    // still fires, landing Waiting, from where the external-work promotion re-lights Running on the
    // next pass; holding orange "needs you" for the whole background run would be worse than a
    // one-pass bounce.
    // `lastPromptUnixMs` / `tailWriteUnixMs` / `nowUnixMs` (all default 0 == the classic behavior) feed
    // the TURN-START RACE guard: the synth holds while NewerPromptNotYetInTranscript says the user
    // submitted a prompt the transcript has not recorded yet. In that window the terminal tail is the
    // PREVIOUS turn's; firing on it read Waiting ~220 ms into a turn that had just started.
    // Returns true when the pass should synthesize the Stop. Never throws.
    inline bool ShouldSynthesizeStop(SessionState state, std::wstring_view lastStopReason, bool interrupted, int64_t quietForMs, bool externalWorkOngoing = false, int64_t lastPromptUnixMs = 0, int64_t tailWriteUnixMs = 0, int64_t nowUnixMs = 0) noexcept
    {
        if (state != SessionState::Running && state != SessionState::NeedsApproval)
        {
            return false; // only a turn-in-progress / blocked-on-user state has a turn to end
        }
        if (state == SessionState::Running && externalWorkOngoing)
        {
            return false; // a teammate / background agent / live shell OUTLIVES the turn — keep Running (the promotion's hold mirror)
        }
        if (NewerPromptNotYetInTranscript(lastPromptUnixMs, tailWriteUnixMs, nowUnixMs))
        {
            return false; // the turn-start race: the tail is an EARLIER turn's — the prompt just submitted has not landed yet
        }
        if (quietForMs < kScanStopQuiescenceMs)
        {
            return false; // not quiet long enough — could be a mid-turn pause
        }
        return interrupted || IsTerminalStopReason(lastStopReason);
    }

    // PURE + total: is the API error the ACTIVE LEAF — the genuine LAST message on the active branch —
    // rather than an error the conversation has since moved PAST or REWOUND away from? The user's exact
    // ask: distinguish "the error is the last message in the chain" from one stranded mid-history.
    //   * lastWasApiError is the BYTE-POSITIONAL signal (the newest TURN event is an unrecovered error);
    //     it is cleared by any later user/assistant/tool_result line, so a fast retry never reaches here.
    //   * The leaf signals distinguish a forward post-error advance from a backward REWIND. After an API
    //     error Claude appends BOOKKEEPING children — a system/turn_duration line, then an away_summary —
    //     and rewrites {"type":"last-prompt","leafUuid":…} to name that turn_duration CHILD, advancing the
    //     active leaf FORWARD onto the error's OWN descendant chain (NOT a rewind). A double-ESC REWIND
    //     instead repoints the leaf BACKWARD to an earlier prompt — OFF the error's branch — with no turn
    //     event, so lastWasApiError stays true. The error is still the active leaf iff the leaf is:
    //       - "" (no last-prompt marker ever seen — older/subagent transcripts: positional flag governs), OR
    //       - == errorEpochLeaf (the leaf has not moved at all since the error appended — the common case), OR
    //       - in errorBranchUuids (the leaf advanced onto the error's bookkeeping descendant chain — the
    //         error's uuid + every later line whose parent is already on the frontier, accumulated in
    //         _readDelta from Node events). A leaf that is NONE of these is a genuine rewind off the error.
    // (Equality with errorEpochLeaf was the ORIGINAL test; it MISSED the forward-advance case because the
    //  post-error last-prompt names the turn_duration child, not the pre-error anchor — so the error was
    //  wrongly read as rewound and never entered Error. The frontier closes that gap.)
    inline bool ApiErrorIsActiveLeaf(bool lastWasApiError,
                                     const std::wstring& activeLeafUuid,
                                     const std::wstring& errorEpochLeaf,
                                     const std::unordered_set<std::wstring>& errorBranchUuids) noexcept
    {
        if (!lastWasApiError)
        {
            return false; // a later turn event already superseded the error -> not the tail
        }
        if (activeLeafUuid.empty() || activeLeafUuid == errorEpochLeaf)
        {
            return true; // no marker seen (positional governs), or the leaf has not moved since the error
        }
        return errorBranchUuids.count(activeLeafUuid) != 0; // the leaf advanced onto the error's OWN descendant chain (post-error bookkeeping) -> still the active leaf
    }

    // PURE + total: should the reconciler synthesize SessionState::Error? Claude Code records an API
    // failure (a rate/usage limit, "Prompt is too long", a 4xx/5xx, a dropped connection, an
    // overloaded server, …) as a SYNTHETIC assistant message with a top-level isApiErrorMessage:true:
    // the turn DIED. It fires no clean Stop, and its TERMINAL stop_reason would otherwise make the
    // missed-Stop backstop (ShouldSynthesizeStop) read it as a normal turn-complete -> WaitingForInput,
    // HIDING the failure. This fires INSTEAD — and the caller checks it BEFORE recon-stop so it wins.
    // `errorIsActiveLeaf` (ApiErrorIsActiveLeaf above) is the tail predicate: the error is the genuine
    // LAST message on the active branch (positionally newest AND the active leaf has not rewound off it),
    // so a fast retry (whose new prompt clears the positional flag) OR a double-ESC rewind PAST the error
    // (which moves the leaf) never reaches Error. Fires from any live non-Error state — Running is the
    // usual one; WaitingForInput/Idle defensively (the real Stop hook for the errored turn, or an earlier
    // pass, may already have moved it there) — and Error is idempotent (never re-fires). Requires the
    // SAME quiescence as the missed-Stop (a settle window). The session LEAVES Error on the first new
    // turn event: a real UserPromptSubmit (push) lands Running, and ShouldSynthesizeRunning (which now
    // includes Error among its recoverable states) is the pull backstop — together the "come out of
    // Error on the first change" edge — while a rewind-and-SIT (no turn event) leaves via
    // ShouldReleaseErrorOnLeafMove below.
    //
    // DONE FIRES TOO (it did not, until the model-404 report — session 33f54bdd, 2026-07-25). "Done"
    // means the SessionEnd hook fired, NOT that the session ended cleanly: Claude Code emits SessionEnd
    // when it ABORTS a conversation fatally — a bogus `--model` 404s, it writes the API-error line, ends
    // the session, and STAYS UP telling you to run /model. SessionEnd (t+1.5s) beat this synth's 2s
    // quiescence window by ~1.2s, so `Done` latched first and then permanently vetoed the Error — the
    // failure read as a green "Done" card on a live, alive tab, and NOTHING could correct it (Done is the
    // one state no pull path accepts: recon-run, recon-subagent, recon-stop, recon-idle, recon-block and
    // recon-resume all exclude it, so only a push UserPromptSubmit could ever leave). An unrecovered
    // API-error tail is POSITIVE PROOF the session did not end cleanly, so it outranks the SessionEnd
    // label. Safe by construction: an ARCHIVED session is never reconciled at all (the caller's
    // `if (!s.live) continue;`), so this only ever reaches a LIVE session sitting in Done — exactly the
    // pathological case; and the triage "Move to Idle/Done" ack on an Error card lands in **Idle** with
    // errorDismissed set, so THAT path was never what this arm protected (errorDismissed is, at the call
    // site). Flipping to Error also restores the recovery edge, since ShouldSynthesizeRunning accepts
    // Error but not Done.
    //
    // PROMPT-SUPERSESSION GUARD (the 2b34b07f stale-snapshot race, 2026-08-07). The two evidence
    // sources this predicate joins are read at DIFFERENT instants: the transcript tail (parsed up to
    // the pass's attribute read) and the session STATE (which a real push hook mutates concurrently).
    // A user RETRY threads the needle: the UserPromptSubmit hook lands (Error -> Running — state no
    // longer Error, so the idempotence arm passes) while the retry's user line has not yet been
    // FLUSHED to the transcript (lastWasApiError still latched from the pre-retry tail) — and the
    // error was re-asserted 38ms after the real prompt, then stood for the whole 35-min retry turn
    // (compounded by the recon-run call-site gap, fixed alongside). `lastPromptUnixMs` (the newest
    // REAL prompt-carrying UserPromptSubmit's wire ts — TurnAccounting) STRICTLY NEWER than
    // `tailWriteUnixMs` (the transcript's last-write time at this pass's read — the newest byte the
    // parse can possibly reflect) is positive proof the parsed tail is superseded: the user already
    // retried, the new turn just hasn't hit the file yet -> hold the synth (the next pass consumes
    // the retry line, which clears lastWasApiError anyway; a retry that ITSELF dies writes a NEW
    // error line whose flush moves tailWrite past the prompt stamp, so a real re-error still fires).
    // A hookless session never stamps lastPromptUnixMs (0 -> guard inert), and tailWriteUnixMs == 0
    // (unknown — defaulted legacy callers) self-disables the guard rather than suppressing blind.
    // Since 2026-10-01 this is the SHARED NewerPromptNotYetInTranscript, the same predicate recon-stop and
    // recon-stop-idle now hold on, and it is BOUNDED by `nowUnixMs` (default 0 = the old unbounded hold).
    // All three gates hold together and release together. A retry that never reached the transcript
    // re-asserts Error once kScanPromptLandingGraceMs passes; with this gate unbounded and recon-stop
    // bounded, recon-stop would land Waiting on the errored tail first and hide the failure.
    // Returns true when the pass should synthesize Error. Never throws.
    inline bool ShouldSynthesizeError(SessionState state, bool errorIsActiveLeaf, int64_t quietForMs, int64_t lastPromptUnixMs = 0, int64_t tailWriteUnixMs = 0, int64_t nowUnixMs = 0) noexcept
    {
        if (!errorIsActiveLeaf)
        {
            return false; // the active branch's last message is not an (unrecovered) API error
        }
        if (state == SessionState::Error)
        {
            return false; // already Error — idempotent, never re-fires off the unchanged tail
        }
        if (NewerPromptNotYetInTranscript(lastPromptUnixMs, tailWriteUnixMs, nowUnixMs))
        {
            return false; // a REAL prompt postdates every parsed transcript byte -> the error is superseded (retry in flight)
        }
        return quietForMs >= kScanStopQuiescenceMs;
    }

    // PURE + total: should the reconciler RELEASE a session from Error because a double-ESC REWIND moved
    // the active leaf OFF the error WITHOUT a turn event clearing the positional flag? The error is still
    // physically last in the file (lastWasApiError true), but a later last-prompt marker repointed the
    // active leaf to a uuid that is NEITHER the error, its unmoved anchor, NOR on its bookkeeping
    // descendant chain (i.e. !ApiErrorIsActiveLeaf) — the user went BACK to an earlier point and is
    // waiting to re-prompt, so the error is no longer "the last message". Releases to WaitingForInput (a
    // quiescentStop). Distinct from the recovery paths recon-run / push, which own the user-RETRIED case
    // (a fresh turn event -> Running, with lastWasApiError already cleared): this requires lastWasApiError
    // STILL true (a pure rewind, no turn event) and a definite move OFF the error branch, so the two never
    // overlap. Crucially, the FORWARD advance onto the error's own turn_duration child (normal post-error
    // bookkeeping) keeps ApiErrorIsActiveLeaf true, so it does NOT trigger a release. Only from Error;
    // quiescence-gated like the missed-Stop (the rewind just wrote the marker — let it settle).
    inline bool ShouldReleaseErrorOnLeafMove(SessionState state, bool lastWasApiError, const std::wstring& activeLeafUuid, const std::wstring& errorEpochLeaf, const std::unordered_set<std::wstring>& errorBranchUuids, int64_t quietForMs) noexcept
    {
        if (state != SessionState::Error)
        {
            return false; // only a stuck-in-Error session has anything to release
        }
        if (!lastWasApiError)
        {
            return false; // the positional flag cleared -> a turn event recovered it (recon-run / push owns that)
        }
        if (ApiErrorIsActiveLeaf(lastWasApiError, activeLeafUuid, errorEpochLeaf, errorBranchUuids))
        {
            return false; // the error is still the active leaf (unmoved / a forward descendant advance) — stay Error
        }
        return quietForMs >= kScanStopQuiescenceMs;
    }

    // ============================================================================================
    // CUT-OFF TURNS (HOOKS.md "Cut-off turns") — a turn that never finished and that no process can
    // be running any more.
    //
    // THE INCIDENT (2026-10-09, release 0.6.8.18, session c47e90a2). The app died while the session
    // was mid-turn (its last transcript line: a tool result). On reopen the record was normalized
    // Running -> Idle (RestoredSessionState). When its tab was opened, the resumed claude appended its
    // placeholder line "No response requested." (a TERMINAL stop_reason), so the tail read as a
    // finished turn. Idle is READY for the autorunner: 40 s later it sent the next queued prompt
    // into a session whose previous turn had been cut off. Nothing in the engine could say "this turn
    // did not finish".
    //
    // WHAT THE TRANSCRIPT SHOWS. A turn is IN FLIGHT at the tail when the last turn line is
    //   - a tool result with no assistant line after it,
    //   - an assistant line that does not end the turn (tool_use / a null stop_reason), or
    //   - a human prompt with nothing after it.
    // In a live session that is normal: the turn is running. It is a CUT-OFF only when no process can
    // be running that turn. The fold (SessionScanner::_readDelta) tracks the open turn, and
    // ClassifyCutOffTurn below joins it with what the engine knows about the process.
    //
    // WHAT CLAUDE CODE WRITES ON A RESUME (read from the 2.1.291 bundle, checked against the local
    // transcripts): its loader repairs the conversation in memory and may append, at load time,
    //   - "[Tool call interrupted…" tool_result lines for tool calls that never returned (a ToolResult
    //     event here),
    //   - a meta user line "Continue from where you left off." (isMeta: the parser skips it),
    //   - the synthetic assistant line "No response requested." after a tail user line.
    // None of them is progress of the turn. With CLAUDE_CODE_RESUME_INTERRUPTED_TURN set, the same
    // claude then CONTINUES the turn by itself: real assistant lines follow and its heartbeat reads
    // "busy" — that is the session coming back to Running (recon-run / recon-subagent).
    // ============================================================================================

    // The text of Claude Code's resume placeholder line, and the pseudo-model every line Claude Code
    // writes ITSELF carries. Both are constants of the Claude Code bundle (2.1.291: `hH` and `sa`); a
    // release that renames either makes IsResumePlaceholder answer false, which only weakens ONE of the
    // cut-off evidences (the others — a dormant tab, the restore-time hint — do not depend on any text).
    inline constexpr std::wstring_view kResumePlaceholderText = L"No response requested.";
    inline constexpr std::wstring_view kSyntheticModelName = L"<synthetic>";

    // The message a cut-off session's Error card shows (SessionInfo::errorMessage; errorStatus 0). Kept
    // under the card's 200-char bound, and the recon-run log line recognizes a cut-off Error by it.
    inline constexpr std::wstring_view kTurnCutOffErrorMessage =
        L"The last turn did not finish: the session was shut down while the turn was running. Send a message to continue it.";

    // PURE + total: is this assistant line Claude Code's RESUME PLACEHOLDER? The mirror of the bundle's
    // own test (2.1.291 `DO`): an assistant line that is not an API error, written under the pseudo-model
    // "<synthetic>", whose whole text is "No response requested.". The model check is what tells it from
    // a REAL reply: a model does sometimes answer with exactly that sentence.
    //   model    — the line's message.model.
    //   apiError — the line's top-level isApiErrorMessage.
    //   text     — the line's concatenated text blocks.
    // Returns true for the placeholder. Never throws.
    inline bool IsResumePlaceholder(std::wstring_view model, bool apiError, std::wstring_view text) noexcept
    {
        return !apiError && model == kSyntheticModelName && text == kResumePlaceholderText;
    }

    // What kind of line a turn that is still in flight at the transcript tail stopped on (the fold's
    // ScanState::openTurn).
    enum class OpenTurnKind : uint8_t
    {
        None, // no turn is open: nothing read yet, or the last turn ended (a terminal stop_reason, an API error, a user interrupt)
        Prompt, // a human prompt with nothing after it — AMBIGUOUS by itself (see ClassifyCutOffTurn)
        ToolResult, // a tool result with no assistant line after it
        Assistant, // an assistant line that does not end the turn (tool_use / null / an unknown stop_reason)
    };

    // The log word for an OpenTurnKind. Never returns null; never throws.
    inline const wchar_t* OpenTurnKindText(OpenTurnKind kind) noexcept
    {
        switch (kind)
        {
        case OpenTurnKind::Prompt:
            return L"a prompt with no reply";
        case OpenTurnKind::ToolResult:
            return L"a tool result";
        case OpenTurnKind::Assistant:
            return L"an unfinished assistant step";
        case OpenTurnKind::None:
        default:
            return L"no open turn";
        }
    }

    // Everything ClassifyCutOffTurn reads. A plain value, so the rule is testable without a scanner.
    struct CutOffEvidence
    {
        // The fold: what the unfinished turn at the tail stopped on (None == the tail is closed).
        OpenTurnKind openTurn{ OpenTurnKind::None };
        // The fold: Claude Code's resume placeholder arrived while that turn was open. A resuming claude
        // writes it only for a conversation it did not produce in its own process, so the open turn
        // belongs to a process that is gone.
        bool resumeMarkerOnOpenTurn{ false };
        // The fold: the tail carries Claude Code's own interruptedByShutdown flag.
        bool shutdownInterrupt{ false };
        // ScanSessionIsDormant: the tab is ours and its connection never started (a restored tab nobody
        // entered, or a deactivated one). No process of ours can be running a turn in it.
        bool dormant{ false };
        // The scanner saw THIS open turn while the session was dormant, and no turn line replaced it
        // since. It carries the verdict across the moment the tab starts: `dormant` flips false then,
        // but the open turn is as dead as before.
        bool openTurnSeenDormant{ false };
        // SessionInfo::midTurnAtRestore: the record said a turn was in flight when it was last
        // normalized for a (re)host, and the session has not been Running since.
        bool midTurnAtRestore{ false };
        // The session is a fork that has not run a turn of its own yet (SessionInfo::forkParentId is
        // still set). Its transcript, once it exists, begins with lines COPIED from the source — an
        // open turn among them is the source's, not something that was cut off here.
        bool forkNotYetMessaged{ false };
    };

    // Why a turn counts as cut off (CutOffReason::None == it does not). The value names the STRONGEST
    // evidence found, for the log line.
    enum class CutOffReason : uint8_t
    {
        None, // not cut off (or not provable)
        ShutdownFlag, // Claude Code itself marked the tail interruptedByShutdown
        NoProcess, // an open turn in a dormant tab: nothing can be running it
        ResumePlaceholder, // a resuming claude answered the open turn with its placeholder line
        SeenWithoutProcess, // the open turn was seen while the tab was dormant, and nothing replaced it since
        MidTurnAtRestore, // the record said a turn was in flight when it was last restored / restarted / parked
    };

    // The log text for a CutOffReason. Never returns null; never throws.
    inline const wchar_t* CutOffReasonText(CutOffReason reason) noexcept
    {
        switch (reason)
        {
        case CutOffReason::ShutdownFlag:
            return L"claude marked the turn interrupted by a shutdown";
        case CutOffReason::NoProcess:
            return L"the tab is dormant, so no process can be running it";
        case CutOffReason::ResumePlaceholder:
            return L"the resumed claude answered it with its placeholder line";
        case CutOffReason::SeenWithoutProcess:
            return L"it was already open while the tab was dormant";
        case CutOffReason::MidTurnAtRestore:
            return L"the session was mid-turn when it was last restored, restarted or parked";
        case CutOffReason::None:
        default:
            return L"not cut off";
        }
    }

    // PURE + total: was the turn at the transcript tail CUT OFF — unfinished, with no process that can
    // still be running it? Returns the strongest evidence, or CutOffReason::None.
    //
    // The rule, in order:
    //   1. A fork that has not run a turn of its own is never cut off: its open turn was copied from
    //      the source (every evidence below would be the source's).
    //   2. Claude Code's own interruptedByShutdown flag at the tail -> cut off, whatever else holds.
    //   3. No open turn -> not cut off. A finished turn, an API error (recon-error's domain) and a
    //      turn the USER interrupted all close the tail.
    //   4. A BARE PROMPT is ambiguous: "the user pressed Esc before any reply" leaves exactly the same
    //      tail as "the host died seconds after the submit", and the first is ordinary (eight tabs of
    //      the 2026-10-09 release fleet sat on it, some for weeks). Only the restore-time hint settles
    //      it: the record still said a turn was in flight when the host ended. Neither a dormant tab
    //      nor the placeholder line proves anything here — a resumed claude writes the placeholder
    //      after BOTH.
    //   5. A tool result or an unfinished assistant step is in flight by construction — claude always
    //      writes something after them when a turn ends normally. It is cut off as soon as ONE of
    //      these says no process can be running it: the tab is dormant; the placeholder line closed
    //      it; it was seen while the tab was dormant; the restore-time hint.
    //   e — the evidence (see CutOffEvidence).
    // Never throws.
    // ⚠ A started tab with an open turn and none of the four evidences is a turn that is RUNNING. That
    // is the common case, and the reason the open turn alone must never count.
    inline CutOffReason ClassifyCutOffTurn(const CutOffEvidence& e) noexcept
    {
        if (e.forkNotYetMessaged)
        {
            return CutOffReason::None; // the tail is the fork source's copy, not this session's own turn
        }
        if (e.shutdownInterrupt)
        {
            return CutOffReason::ShutdownFlag; // claude's own word: a shutdown aborted the turn
        }
        if (e.openTurn == OpenTurnKind::None)
        {
            return CutOffReason::None; // the tail is closed: finished, errored (recon-error) or interrupted by the user
        }
        if (e.openTurn == OpenTurnKind::Prompt)
        {
            // The bare-prompt ambiguity (rule 4): only the record's own "a turn was in flight" settles it.
            return e.midTurnAtRestore ? CutOffReason::MidTurnAtRestore : CutOffReason::None;
        }
        if (e.dormant)
        {
            return CutOffReason::NoProcess;
        }
        if (e.resumeMarkerOnOpenTurn)
        {
            return CutOffReason::ResumePlaceholder;
        }
        if (e.openTurnSeenDormant)
        {
            return CutOffReason::SeenWithoutProcess;
        }
        if (e.midTurnAtRestore)
        {
            return CutOffReason::MidTurnAtRestore;
        }
        return CutOffReason::None; // an open turn in a started tab with no evidence against it: it is running
    }

    // PURE + total: should the reconciler synthesize SessionState::Error for a cut-off turn?
    // Fires from the states a cut-off session is stranded in:
    //   Idle            — the usual one: the restore normalized Running to Idle;
    //   WaitingForInput — a record promoted by hand, or cut off before this rule existed;
    //   NeedsApproval   — the turn was blocked on a permission prompt or a question when its host
    //                     ended. That prompt died with the process: nothing is left to approve, so
    //                     "needs approval" would be false (the stale Needs-approval of a parked or
    //                     restored tab);
    //   Done            — the SessionEnd of the dying process landed first. Like the API error
    //                     (ShouldSynthesizeError), an unfinished turn outranks the "ended" label.
    // NEVER from Running: a Running session has a turn in a live process by a turn event, and the tail
    // may simply not show that turn yet (the turn-start race). NEVER from Error (idempotent).
    // No quiescence floor, on purpose: the verdict rests on evidence (no process), not on a quiet
    // file, and waiting would let the resumed claude's "busy" heartbeat promote the session to Running
    // first (NextSessionState keeps Error across the resume only once it IS Error).
    //   state            — the session's current state.
    //   reason           — ClassifyCutOffTurn's verdict for this pass.
    //   lastPromptUnixMs — TurnAccounting's newest prompt stamp (0 = none): with tailWriteUnixMs and
    //   tailWriteUnixMs    nowUnixMs it feeds the shared turn-start guard (NewerPromptNotYetInTranscript):
    //   nowUnixMs          a prompt the transcript has not recorded yet means the tail is an OLDER
    //                      turn's, so the verdict holds. All three default 0 == the guard is inert.
    // Returns true when the pass should synthesize the cut-off Error. Never throws.
    inline bool ShouldSynthesizeCutOff(SessionState state, CutOffReason reason, int64_t lastPromptUnixMs = 0, int64_t tailWriteUnixMs = 0, int64_t nowUnixMs = 0) noexcept
    {
        if (reason == CutOffReason::None)
        {
            return false; // nothing was cut off (or it cannot be proven)
        }
        if (NewerPromptNotYetInTranscript(lastPromptUnixMs, tailWriteUnixMs, nowUnixMs))
        {
            return false; // the user just submitted a prompt the tail does not show yet: the open turn is not the current one
        }
        switch (state)
        {
        case SessionState::Idle:
        case SessionState::WaitingForInput:
        case SessionState::NeedsApproval:
        case SessionState::Done:
            return true;
        case SessionState::Running: // a turn event says a turn is in flight NOW — never overrule it from the tail
        case SessionState::Error: // already Error — idempotent
        default:
            return false;
        }
    }

    // PURE + total: should the reconciler RELEASE a session from Error because the transcript tail no
    // longer shows ANY error — neither an API error nor a cut-off turn? Error is LEVEL-derived: it
    // stands only while its evidence stands. Since 2026-10-09 a resume keeps Error
    // (NextSessionState, source "resume"), so the reset that used to clear a stale Error on every
    // SessionStart is gone, and this release replaces it. A session lands here when the evidence was
    // superseded WITHOUT a state change: a turn that started and finished between two passes with its
    // hooks lost, an API-error line that a resume placeholder followed, a cut-off whose last evidence
    // went away. Releases to WaitingForInput (a quiescent Stop).
    // Disjoint from the other Error exits: recon-run / a real UserPromptSubmit own "a turn started"
    // (they leave Error for Running), and ShouldReleaseErrorOnLeafMove owns the rewind, which keeps
    // lastWasApiError TRUE — this one requires it FALSE.
    //   state           — the session's current state.
    //   lastWasApiError — the fold's positional flag: an unrecovered API error is still the newest turn line.
    //   cutOffReason    — ClassifyCutOffTurn's verdict for this pass.
    //   quietForMs      — the pass's quiescence clock (0 while claude's heartbeat reads working, so a
    //                     turn that is starting is never released to Waiting under it).
    // Returns true when the pass should release the Error. Never throws.
    inline bool ShouldReleaseStaleError(SessionState state, bool lastWasApiError, CutOffReason cutOffReason, int64_t quietForMs) noexcept
    {
        if (state != SessionState::Error)
        {
            return false; // only a session in Error has anything to release
        }
        if (lastWasApiError || cutOffReason != CutOffReason::None)
        {
            return false; // the tail still shows the error: Error stands
        }
        return quietForMs >= kScanStopQuiescenceMs; // let a line that is just landing settle first
    }

    // PURE + total: should the reconciler synthesize a "needs you" state (-> NeedsApproval) because
    // the latest assistant message is an UNANSWERED interactive tool_use (AskUserQuestion) and the
    // transcript has gone quiescent? The session is blocked waiting for the user to answer — it must
    // NOT show as Running (the Triage Board's whole job is to surface who needs you). Only from
    // Running (idempotent — once NeedsApproval it stays until the answer + the turn's end release it
    // via ShouldSynthesizeStop). An interrupt takes precedence (the question is gone -> a Stop).
    inline bool ShouldSynthesizeBlockedOnUser(SessionState state, std::wstring_view pendingInteractiveTool, bool interrupted, int64_t quietForMs) noexcept
    {
        if (state != SessionState::Running)
        {
            return false; // entered once from Running; never re-fire while already NeedsApproval
        }
        if (interrupted)
        {
            return false; // an interrupt ends the turn (ShouldSynthesizeStop owns it), not a block
        }
        if (quietForMs < kScanStopQuiescenceMs)
        {
            return false; // the user may answer within the settle window — don't flicker
        }
        return IsInteractiveTool(pendingInteractiveTool);
    }

    // PURE + total: should the reconciler release a NeedsApproval session back to Running because the
    // user ANSWERED the blocking question / approval and the agent has RESUMED working? NeedsApproval
    // is entered by a real permission Notification OR the synthesized AskUserQuestion block above, and
    // until now its ONLY pull-side exit was -> WaitingForInput at the turn's END (ShouldSynthesizeStop)
    // — so a session that was answered and KEPT WORKING (more assistant output, not yet end-of-turn)
    // showed "needs you" (orange) for the whole rest of the turn. It has RESUMED when a fresh
    // assistant/user line appended THIS pass (consumedTurnEvent) on an already-PRIMED cursor (the
    // initial history replay never counts — exactly as ShouldSynthesizeRunning), the blocking
    // interactive tool is no longer pending (answered / moved on; a NEW AskUserQuestion keeps it set ->
    // stay blocked), the user did NOT interrupt, and the tail is NOT a terminal stop_reason (a finished
    // turn is ShouldSynthesizeStop's job -> Waiting, never a Running blip). The interrupt + terminal
    // guards make this MUTUALLY EXCLUSIVE with ShouldSynthesizeStop (no single pass satisfies both), so
    // the two never fight regardless of the fresh/quiescent window overlap. It is the NeedsApproval
    // mirror of ShouldSynthesizeRunning (which stays Idle/Waiting-ONLY — Running needs no repair); the
    // caller synthesizes it as tool ACTIVITY (PostToolUse), NOT a UserPromptSubmit, because a
    // UserPromptSubmit from NeedsApproval would wrongly ++queuedPrompts (the type-ahead accounting).
    // `answeredPendingQuestion` (Agentmaster): this pass consumed the tool_result that ANSWERED the
    // pending interactive tool_use — i.e. the literal "the user answered" line. It is accepted as an
    // ALTERNATIVE to consumedTurnEvent because a ToolResult deliberately never sets that flag (a bare
    // tool_result must never synthesize Running from Idle/Waiting — ShouldSynthesizeRunning's
    // invariant, which is untouched: this belt is NeedsApproval-only). Without it the release waited
    // for the agent's NEXT assistant append, measured 19.8s / 45.3s later, even though the answer was
    // already on disk (ed5a48a4: the answering tool_result was readable ~0.8s after it was written).
    // The pendingInteractiveTool check below still passes — the ToolResult branch cleared it — so the
    // two conditions are consistent, not contradictory.
    inline bool ShouldSynthesizeResumed(SessionState state, bool consumedTurnEvent, bool primedBeforePass, std::wstring_view pendingInteractiveTool, std::wstring_view lastStopReason, bool interrupted, int64_t sinceWriteMs, bool answeredPendingQuestion = false) noexcept
    {
        if (state != SessionState::NeedsApproval)
        {
            return false; // only NeedsApproval lacks a resume edge; Idle/Waiting -> ShouldSynthesizeRunning
        }
        if ((!consumedTurnEvent && !answeredPendingQuestion) || !primedBeforePass)
        {
            return false; // no live append this pass (or still replaying history) -> not a resume
        }
        if (!pendingInteractiveTool.empty())
        {
            return false; // still blocked on a (possibly NEW) question -> stay NeedsApproval
        }
        if (interrupted || IsTerminalStopReason(lastStopReason))
        {
            return false; // the turn ENDED -> ShouldSynthesizeStop's job (-> Waiting), not a resume
        }
        return sinceWriteMs <= kScanRunRepairFreshMs; // a live, fresh turn (not an old write surfacing late)
    }

    // PURE: is Claude's presence heartbeat reporting this session as actively WORKING? The presence
    // file (~/.claude/sessions/<pid>.json "status") is claude's OWN self-report, and it (a) FOLLOWS
    // the live conversation across /fork, /clear, /resume — it is pid-keyed, so it names the id the
    // process is ACTUALLY in, not the stale launch id (a /compact keeps the SAME id: it is in-place,
    // verified over 429 boundaries) — and (b) stays "busy" for the WHOLE
    // turn, including while a Task/Agent subagent runs and the main transcript is quiescent. So it is
    // the one signal that covers BOTH spawn cases. The S-lane validates it against pid liveness before
    // publishing it onto SessionInfo.presenceStatus (Rule #13: a display FACT — the OBSERVER never
    // sets state), which is exactly why the SCANNER — the engine's state authority — may read it as a
    // state INPUT here without violating that rule. "idle"/"waiting"/"shell"/"" are NOT a TURN in
    // progress — but see PresenceIsWorking below for the broader "actively doing something" signal.
    inline bool PresenceIsBusy(std::wstring_view presenceStatus) noexcept
    {
        return presenceStatus == L"busy";
    }

    // PURE: is Claude's presence heartbeat reporting this session actively DOING SOMETHING — a turn in
    // flight ("busy") OR a live SHELL job ("shell")? claude sets status "shell" while it owns a running
    // shell task (measured live: sessions carrying hours-old cmd.exe/bash.exe children report "shell"
    // with a quiet transcript), so "a shell is still running" is claude's OWN pid-validated fact, not an
    // inference of ours. This is the scanner's ACTIVITY signal — the quiescence fold, the external-work
    // promotion, and the Waiting->Idle decay block all key on it — while PresenceIsBusy above stays the
    // narrow turn-in-flight predicate and PresenceIsAtRest below the release ("idle"-only, so a "shell"
    // heartbeat still never releases a turn). "waiting" (claude asking the USER to answer/approve) is
    // deliberately NOT working — it overlaps the NeedsApproval "needs you" semantics.
    inline bool PresenceIsWorking(std::wstring_view presenceStatus) noexcept
    {
        return presenceStatus == L"busy" || presenceStatus == L"shell";
    }

    // PURE: is Claude's presence heartbeat reporting this session AT REST — "idle" (no turn in
    // progress, nothing pending)? The RELEASE mirror of PresenceIsBusy. claude's pid-keyed self-report
    // flips to "busy" the instant a turn starts and back to "idle" when it fully ends, so an explicit
    // "idle" is authoritative "no turn is running". Deliberately NARROW — only "idle": "waiting" (the
    // user is being asked to answer/approve) overlaps the NeedsApproval / recon-block "needs you"
    // semantics and is left to those paths; "shell" / "" are not at-rest claude-turn signals (a shell
    // job, or no live presence file at all). The S-lane validates the backing pid is a live claude.exe
    // before publishing this onto SessionInfo.presenceStatus (Rule #13: a FACT the scanner may consume).
    // PURE: is Claude's presence heartbeat reporting this session BLOCKED ON THE USER? claude sets
    // status "waiting" (with a `waitingFor` detail, e.g. "input needed") the instant it parks a turn
    // on the user — a pending AskUserQuestion — and rewrites the file again the instant that clears.
    // This is claude's OWN first-class declaration, and measurement says it is the EARLIEST and most
    // reliable of the three signals we have for the blocked-on-user lifecycle:
    //   * vs the Notification HOOK — presence flipped to "waiting" 6.4s BEFORE the hook arrived
    //     (14:04:47.872 vs 14:04:54.260, session 7b40d6cf), and the hook is fire-and-forget
    //     (forwarder-errors.log has recorded drops); a dropped one leaves NOTHING behind.
    //   * vs the TRANSCRIPT — a pending AskUserQuestion tool_use line was measured STILL UNWRITTEN
    //     10+ minutes into the question (7b40d6cf), so recon-block/recon-resume, which key on that
    //     line, can be arbitrarily late or never fire at all.
    // Consumed by the scanner as a state INPUT under the same Rule #13 licence PresenceIsAtRest
    // already uses (the S-lane validates the backing pid is a live claude.exe before publishing it).
    inline bool PresenceIsAwaitingUser(std::wstring_view presenceStatus) noexcept
    {
        return presenceStatus == L"waiting";
    }

    inline bool PresenceIsAtRest(std::wstring_view presenceStatus) noexcept
    {
        return presenceStatus == L"idle";
    }

    // Agentmaster (the COMPACTION WINDOW — HOOKS.md "Compaction"): the hard cap on how long a PreCompact
    // with no closing hook may hold the reconcilers. Far above the longest compaction measured on this
    // machine (durationMs 313 s over 429 boundaries) so a slow summarization is never cut short, yet a
    // LOST PostCompact/SessionStart (a dropped hook — the forwarder is fire-and-forget) can only hold for
    // this long even when claude's heartbeat never reads "idle" to release it sooner.
    inline constexpr int64_t kCompactionHoldMaxMs = 15 * 60 * 1000;
    // How long after PreCompact claude's own "idle" heartbeat is trusted to END the window early (a
    // compaction the user CANCELLED with Esc, or one that failed, fires no PostCompact). Covers ~2 S-lane
    // surveys (kObserverHeartbeatMs 2 s), so the stale pre-compaction "idle" still published in the first
    // moments of the window — before the survey sees claude flip to "busy" — can never release it.
    inline constexpr int64_t kCompactionPresenceSettleMs = 5000;

    // PURE + total: is a compaction IN FLIGHT for this session right now, i.e. must the scanner treat the
    // (silent) transcript as WORKING rather than quiescent? `compactingSinceMs` is SessionInfo::
    // compactingSinceUnixMs (the PreCompact wire ts, 0 when no window is open). A compaction writes
    // NOTHING to the transcript until it finishes (the manual /compact echo included), and the tail it
    // leaves meanwhile is the previous turn's TERMINAL stop_reason — exactly recon-stop's "a Stop was
    // missed" shape — so the caller forces its quiescence clock to 0 while this holds, the same
    // treatment a "busy" heartbeat gets (every quiescence-gated synth — recon-stop, the presence-idle
    // release, recon-block, recon-error, the error release — then stands down together). Ends when:
    //   * no window is open (0) — the normal case, and after PostCompact / SessionStart closed it;
    //   * the window is older than kCompactionHoldMaxMs — the lost-closing-hook belt;
    //   * claude's own heartbeat reads "idle" (PresenceIsAtRest) at least kCompactionPresenceSettleMs
    //     after the window opened — a cancelled/failed compaction is back at the prompt.
    // A negative age (a ts from a skewed clock) counts as a just-opened window: hold.
    inline bool CompactionInFlight(int64_t compactingSinceMs, int64_t nowMs, std::wstring_view presenceStatus) noexcept
    {
        if (compactingSinceMs <= 0)
        {
            return false;
        }
        const int64_t ageMs = nowMs - compactingSinceMs;
        if (ageMs < 0)
        {
            return true; // the window opened "in the future" (clock skew) — treat as just opened
        }
        if (ageMs >= kCompactionHoldMaxMs)
        {
            return false; // the closing hook was lost — stop holding
        }
        if (ageMs >= kCompactionPresenceSettleMs && PresenceIsAtRest(presenceStatus))
        {
            return false; // claude is back at the prompt (cancelled / failed compaction — no PostCompact)
        }
        return true;
    }

    // PURE + total: should the reconciler mark this session BLOCKED ON THE USER purely from claude's
    // heartbeat? The presence-driven ENTRY — the transcript-free twin of ShouldSynthesizeBlockedOnUser.
    // recon-block needs the AskUserQuestion tool_use LINE, which was measured still unwritten 10+ minutes
    // into a live question; the Notification hook covers that case today but is droppable, and when BOTH
    // miss, the session shows Running forever with a question on screen and nothing ever recovers it.
    // claude's own "waiting" is the third, independent belt that closes that hole.
    // Fires from the states a pending question can be MISREAD as: Running (the usual — the turn looks
    // in flight) and Idle/WaitingForInput (a question raised on a session the tail had already settled).
    // NOT from NeedsApproval (already there — idempotent), Error, or Done. `interrupted` blocks it: an
    // Esc'd turn is recon-stop's -> Waiting, and a lingering "waiting" must not re-park it.
    inline bool ShouldSynthesizeBlockedFromPresence(SessionState state, std::wstring_view presenceStatus, bool interrupted) noexcept
    {
        if (interrupted)
        {
            return false; // the user ABORTED the turn -> recon-stop's job, never "needs you"
        }
        if (!PresenceIsAwaitingUser(presenceStatus))
        {
            return false;
        }
        // Running (the usual — the turn LOOKS in flight, which is the misread this fixes) and Idle
        // (a question raised on a tail we had already settled). DELIBERATELY NOT WaitingForInput,
        // and that exclusion is a TERMINATION argument, not a preference: recon-stop can fire from
        // NeedsApproval on a TERMINAL tail (a stale one, since the whole premise here is that the
        // transcript may be minutes behind) and lands WaitingForInput. Were Waiting in this set, a
        // persistent "waiting" heartbeat would drive NeedsApproval -> (recon-stop) -> Waiting ->
        // (here) -> NeedsApproval forever — a per-pass state flap, the exact oscillation class the
        // externalOutlivesTurn hold exists to prevent elsewhere. Excluding it makes every path settle
        // in at most one transition, and costs nothing real: Waiting is ALREADY a "needs you" state,
        // so the card is on the right side of the board either way.
        return state == SessionState::Running || state == SessionState::Idle;
    }

    // PURE + total: should the reconciler release a NeedsApproval session back to Running because
    // claude's heartbeat LEFT "waiting" for actual work? The presence-driven EXIT, and the fix for the
    // one genuinely broken transition in this area: the user ANSWERS and the agent KEEPS WORKING.
    // (The other three exits already work off a real Stop hook — cancel 112ms, "Chat about this" 2.2s,
    // answer-then-end-turn 1.5s, all measured.) That shape has NO push event — PostToolUse is
    // deliberately not hooked (a forwarder per tool call) and Stop won't fire until the turn truly ends
    // — so ShouldSynthesizeResumed was the only exit, and it cannot fire off the ANSWER itself (a
    // ToolResult is not a turn event). Measured cost: 19.8s (ed5a48a4) and 45.3s (d2e77e90) of a card
    // showing orange "needs you" while the agent was demonstrably working.
    //
    // `sawWaiting` is an EDGE latch (ScanState::presenceWasWaiting), and it is load-bearing, not
    // decoration: releasing on a bare `presence==busy` while NeedsApproval would instantly undo any
    // NeedsApproval whose underlying block does NOT set "waiting" — a real permission prompt being the
    // open question (unverifiable on this machine: skipPermissions:true means permission prompts are
    // auto-approved and never surface, so `cfa0daa7` produced no "waiting" to sample). Requiring that
    // we OBSERVED "waiting" first makes the predicate correct under BOTH answers: if permission prompts
    // do set "waiting" they are covered free; if they don't, the latch never arms and this path simply
    // stays out of the way — it can never fight them.
    inline bool ShouldSynthesizeResumedFromPresence(SessionState state, std::wstring_view presenceStatus, bool sawWaiting, std::wstring_view lastStopReason, bool interrupted) noexcept
    {
        if (state != SessionState::NeedsApproval)
        {
            return false; // only NeedsApproval lacks a resume edge (mirrors ShouldSynthesizeResumed)
        }
        if (!sawWaiting)
        {
            return false; // we never saw claude declare "waiting" -> this block is not ours to release
        }
        if (!PresenceIsWorking(presenceStatus))
        {
            return false; // still "waiting" (unanswered), or "idle"/"" — at rest, which is recon-stop's -> Waiting
        }
        if (interrupted || IsTerminalStopReason(lastStopReason))
        {
            return false; // the turn ENDED/aborted -> ShouldSynthesizeStop's -> Waiting, never Running
        }
        return true;
    }

    // PURE + total: should the reconciler synthesize a missed Stop because Claude's OWN presence
    // heartbeat says the session is at rest ("idle") while we are STILL Running / NeedsApproval, even
    // though the transcript tail is NON-terminal? The missing IDLE half of the presence signal
    // (PresenceIsBusy is the BUSY-hold half — it only ever KEPT a session Running; nothing released one
    // on the strength of claude's own "I'm idle"). The plain missed-Stop backstop (ShouldSynthesizeStop)
    // needs a TERMINAL stop_reason or an interrupt in the tail — but a turn can end with NEITHER: the
    // last transcript line is a bare user prompt whose UserPromptSubmit cleared the tracked stop_reason
    // (_readDelta) and which then produced NO assistant output and fired NO Stop hook (dropped, or a
    // no-op turn), stranding the session Running forever while claude is demonstrably idle. claude's
    // pid-validated heartbeat is the authority here: "idle" => the turn is OVER. Fires only in exactly
    // that GAP — a NON-terminal, NON-interrupted tail (a terminal/interrupted tail is
    // ShouldSynthesizeStop's territory, so the two are mutually exclusive and never double-fire) — from
    // Running / NeedsApproval, and only after the transcript has been quiet LONGER than the normal stop
    // quiescence (kScanPresenceIdleQuiescenceMs). The longer window closes the only race: right after a
    // turn STARTS claude's file is already "busy" but the ~2s S-lane survey may not have refreshed
    // SessionInfo.presenceStatus off the prior "idle" yet — by the time the parent transcript has been
    // quiet kScanPresenceIdleQuiescenceMs with presence STILL "idle", the survey has re-read it, and a
    // genuine in-flight turn (incl. an extended-thinking pause, which keeps the heartbeat "busy") reads
    // "busy". The caller additionally gates this on no pending interactive tool so it never pre-empts
    // recon-block's blocked-on-question -> NeedsApproval path.
    //
    // Agentmaster (idle<->running flap fix, TWO-part): a RUNNING session's "idle" heartbeat must clear
    // TWO hurdles, because "idle heartbeat + non-terminal tail + quiet transcript" ALSO describes a turn
    // merely PAUSED behind a "No response from API · Retrying" backoff / a slow first token / a streaming
    // stall — claude stops generating, so it reports "idle" and the file sits quiet, yet the turn is NOT
    // over. (1) A PENDING tool_use tail (non-empty, non-terminal stop_reason) is NEVER released: claude
    // is mid-tool (a long Bash/build) or waiting on that tool's API call. (2) A CLEARED (empty)
    // stop_reason — a bare user prompt with no assistant output yet — is the no-op-turn case this
    // backstop was built for, BUT it is ALSO exactly what a turn looks like while its FIRST API call is
    // retrying (the original fix wrongly assumed "No response from API" only ever coincides with a
    // pending tool_use — the live "card bg" flap proved it fires with a CLEARED tail too). The two are
    // indistinguishable at an instant, so a cleared-tail Running turn is released only after the LONG
    // quiescence floor (kScanPresenceIdleRunningQuiescenceMs): any transcript append OR any "busy"
    // heartbeat inside the window resets quietForMs (see _reconcileSession), so a real retry — which
    // does at least one of those repeatedly — never reaches the floor, while a genuinely finished turn
    // (no writes, no "busy") does. Together these kill the Running<->WaitingForInput oscillation against
    // recon-run (the reported "card bg" flap + "jumps to idle/waiting while the API is retrying" bug).
    // NeedsApproval is unaffected: it keeps the SHORT base floor (its pending tool_use is the
    // AskUserQuestion — once answered + idle, that turn really is settled, with no API-retry ambiguity).
    // `lastPromptUnixMs` / `tailWriteUnixMs` / `nowUnixMs` (all default 0 == the classic behavior) feed
    // the TURN-START RACE guard, NewerPromptNotYetInTranscript. A prompt submitted after the tail's no-op
    // turn meets every condition above at once: a long-quiet transcript, a cleared tail, and a heartbeat
    // still reading "idle" from before the submit. It held Waiting ~220 ms into the new turn (72 times
    // in 97 days of the release log). Hold until the prompt lands, or the grace says it never will.
    // Returns true when the pass should synthesize the Stop. Never throws.
    inline bool ShouldSynthesizeStopFromPresenceIdle(SessionState state, std::wstring_view presenceStatus, std::wstring_view lastStopReason, bool interrupted, int64_t quietForMs, int64_t lastPromptUnixMs = 0, int64_t tailWriteUnixMs = 0, int64_t nowUnixMs = 0) noexcept
    {
        if (state != SessionState::Running && state != SessionState::NeedsApproval)
        {
            return false; // only a turn-in-progress / blocked-on-user state has a turn to end
        }
        if (NewerPromptNotYetInTranscript(lastPromptUnixMs, tailWriteUnixMs, nowUnixMs))
        {
            return false; // the turn-start race: the cleared tail is the EARLIER no-op turn's — the new prompt has not landed yet
        }
        if (interrupted || IsTerminalStopReason(lastStopReason))
        {
            return false; // a terminal / interrupted tail is the plain missed-Stop's job (no double-fire)
        }
        if (state == SessionState::Running && !lastStopReason.empty())
        {
            return false; // hurdle 1: a PENDING tool_use tail == mid-tool / waiting on its API call — never a turn-end
        }
        // hurdle 2 — the quiescence floor. NeedsApproval uses the short base; a Running cleared-tail turn
        // uses the LONG floor, because that shape is indistinguishable from an in-flight API-retry /
        // streaming pause (which appends or flips "busy" within the window, resetting quietForMs, so it
        // never reaches the floor). Only a session truly at rest for the WHOLE long window is released.
        const int64_t quiescenceFloor = (state == SessionState::Running) ? kScanPresenceIdleRunningQuiescenceMs : kScanPresenceIdleQuiescenceMs;
        if (quietForMs < quiescenceFloor)
        {
            return false;
        }
        return PresenceIsAtRest(presenceStatus);
    }

    // PURE + total: should the reconciler synthesize Running because EXTERNAL work is active while the
    // session sits Idle / WaitingForInput? The SUBAGENT/FORK mirror of ShouldSynthesizeRunning (which
    // keys on a PARENT-transcript append): a tab whose turn delegated to a subagent — so the parent
    // <id>.jsonl is quiescent while <id>/subagents/*.jsonl grows — or whose live conversation forked to
    // a new id we have not yet re-bound, must read Running, not idle. Two signals of "still working":
    //   * subagentActive — a subagent/tool-result side file was written within kScanSubagentFreshMs
    //     (SubagentActivityUnixMs). Fires regardless of cursor priming — it is a CURRENT filesystem
    //     fact, not a transcript-replay artifact, so the primed-cursor gate the parent-append repairs
    //     need does not apply.
    //   * presenceWorking — claude's heartbeat self-reports "busy" (it follows a live /fork to a new
    //     id) OR "shell" (a live shell job it owns — PresenceIsWorking).
    //   * externalOutlivesTurn — the caller's kScanExternalWorkGraceMs proof that the external work
    //     postdates the parent transcript's last write by more than the grace margin: an in-process
    //     TEAMMATE or run_in_background Agent still writing subagents/*.jsonl minutes-hours after the
    //     lead's end_turn, or a shell job still alive long after the turn settled. THIS arm alone may
    //     promote past a terminal/interrupted tail (below) — "a shell or agent or teammate still
    //     running means Running, not idle/done/waiting-for-you".
    // The first two arms are gated on a NON-TERMINAL, NON-INTERRUPTED tail: a TERMINAL stop_reason (end_turn / stop_sequence /
    // max_tokens / refusal) means the turn is OVER, and recent side-file activity / a lingering "busy"
    // is then the TAIL END of the turn that just finished — the subagent's last write lands µs BEFORE
    // the parent's end_turn (so it is always "fresh" at turn-end), and "busy" lingers a tick after a
    // real Stop — NOT new work, so it must never bounce a settled (WaitingForInput) session back to
    // Running. An INTERRUPT (user Esc) is likewise a turn-ender owned by recon-stop (ShouldSynthesizeStop
    // -> Waiting): after an Esc the dying subagents keep flushing side files for up to kScanSubagentFreshMs
    // and claude's heartbeat lingers "busy" for a tick, so WITHOUT an interrupt guard recon-stop
    // (Running -> Waiting on interrupted) and THIS promotion (Waiting -> Running on that stale activity)
    // FLIP-FLOP every tick — a Running<->Waiting oscillation that floods scanner/autorunner/hooks logs and
    // the UI until the freshness window expires (in the field it ballooned hooks.log to ~291 MB and wedged
    // the hosting window). Guarding on `interrupted` makes the two MUTUALLY EXCLUSIVE — the same discipline
    // ShouldSynthesizeResumed / ShouldSynthesizeBlockedOnUser / ShouldSynthesizeStopFromPresenceIdle already
    // follow. The latch is NOT sticky past a real resume: the parser clears st.interrupted the instant a new
    // turn appends (fresh assistant output OR a new non-interrupt user prompt), so a genuinely resumed
    // session promotes to Running on its next pass. During a LIVE (un-interrupted) subagent the parent tail
    // is the pending Task/Agent tool_use (non-terminal), so genuine in-flight work is unaffected. Only from the two states a missed turn-start strands a
    // session in (Idle / WaitingForInput); Running needs no repair, and NeedsApproval / Error / Done are
    // "needs you / ended" a mere activity signal must not clear. The caller synthesizes it as tool
    // ACTIVITY (PostToolUse -> Running), NOT a UserPromptSubmit (which would inflate the type-ahead
    // queue accounting, ++queuedPrompts).
    // `cutOffErrorStanding` (Agentmaster, CUT-OFF TURNS — default false == the classic behavior): the
    // session is in Error BECAUSE its last turn was cut off (the caller passes state == Error AND a
    // ClassifyCutOffTurn verdict for this pass). Such an Error is the third state a working session can
    // be stranded in. With CLAUDE_CODE_RESUME_INTERRUPTED_TURN set, a resumed claude CONTINUES the cut-off
    // turn by itself; its heartbeat reads "busy" at once, while its first transcript line lands only when
    // the first content block is complete (measured 16.7 s on session 9aa2cb78, 2026-10-09). The heartbeat
    // is therefore what brings the card back to Running without that delay ("when the observer sees that
    // the session resumed, it is Running"). An API-error Error is NOT covered: the flag is false for it,
    // so "needs you / ended" keeps its meaning there. Every other guard applies unchanged, so the resume
    // placeholder's terminal stop_reason (written when claude does NOT continue the turn) blocks the
    // promotion exactly as it blocks one from Waiting.
    //   state                — the session's current state.
    //   subagentActive       — a side file was written within kScanSubagentFreshMs.
    //   presenceWorking      — claude's heartbeat reads busy / shell.
    //   lastStopReason       — the fold's newest assistant stop_reason.
    //   interrupted          — the fold's "the user aborted the turn" flag.
    //   externalOutlivesTurn — the caller's kScanExternalWorkGraceMs proof (see above).
    //   cutOffErrorStanding  — see above.
    // Returns true when the pass should synthesize Running. Never throws.
    inline bool ShouldSynthesizeRunningFromExternalWork(SessionState state, bool subagentActive, bool presenceWorking, std::wstring_view lastStopReason, bool interrupted, bool externalOutlivesTurn = false, bool cutOffErrorStanding = false) noexcept
    {
        const bool strandedAtRest = state == SessionState::Idle || state == SessionState::WaitingForInput ||
                                    (state == SessionState::Error && cutOffErrorStanding);
        if (!strandedAtRest)
        {
            return false;
        }
        if (interrupted || IsTerminalStopReason(lastStopReason))
        {
            // A COMPLETED or INTERRUPTED turn: recent side-file activity / a lingering "busy" is
            // normally its TAIL END, not new work (an interrupt is recon-stop's job -> Waiting;
            // promoting on residue would oscillate against it) — EXCEPT work that provably OUTLIVED
            // the turn's end by the kScanExternalWorkGraceMs proof: a teammate / background agent
            // still writing side files, or a live shell job, long AFTER the end_turn / interrupt
            // marker is BY DESIGN still running, so the session reads Running. Sub-margin residue
            // (a final flush landing us before end_turn, post-Esc dying flushes <=
            // kScanSubagentFreshMs, a one-tick "busy" linger) never qualifies — the classic settled /
            // interrupt behavior is byte-identical — and recon-stop is HELD on the SAME expression
            // (ShouldSynthesizeStop's externalWorkOngoing), so promotion and demotion stay mutually
            // exclusive.
            return externalOutlivesTurn;
        }
        return subagentActive || presenceWorking;
    }

    // --- System-notification (toast) gates — the NOTIFICATION mirror of the outlived-turn pair ---
    // A completion toast fires on the Running -> X push edge, BEFORE the outlived-turn promotion
    // above can veto the transition: the promotion's presence arm needs the parent transcript quiet
    // PAST kScanExternalWorkGraceMs, so a session with a live background shell re-lights Running
    // ~20-24s AFTER the real Stop — and a toast, unlike the flash ring, cannot be recalled (proven
    // live on session 513d1366: "waiting for you" toasted at the Stop edge, recon-subagent promoted
    // presence=shell back to Running 20s later). The hosting window therefore HOLDS an
    // Idle/WaitingForInput toast while the external-work signal (PresenceIsWorking / fresh side
    // files) is present, DROPS it when the session re-lights Running (spurious confirmed), and
    // FIRES it when the signal clears with the session still at rest (the common one-survey-tick
    // "busy" linger after a real Stop costs exactly one ~2.5s sweep of toast latency).
    inline constexpr int64_t kNotifyExternalHoldCapMs = 30000; // fire a HELD toast at latest here — the backstop for a signal that never clears while the promotion never lands (a starved scanner / flapping heartbeat); must OUTLAST the promotion's worst-case latency (grace + scanner ticks), or the backstop fires the spurious toast right before the promotion would have dropped it
    inline constexpr int64_t kNotifyDuplicateToastMs = 20000; // per-session double-toast guard: at most one shown toast per session per this window — a Waiting -> Running -> Waiting flap otherwise toasts on EVERY Waiting entry (the Action Center Tag+Group replace dedupes the pile, not the interruptions)
    // The completion toast's LINE 3 — how much of the just-finished prompt to carry (NOTIFICATIONS.md
    // §3; the text goes through ::Agentmaster::PromptPreviewLine, so it is the FIRST line + "..." when
    // there is more). Deliberately well ABOVE what a toast BANNER renders (~1 line, which the shell
    // ellipsizes itself at whatever width/DPI it has — guessing that width here would truncate twice
    // and lose text on a wide screen) and well BELOW the prompt bodies that actually occur, so the
    // ACTION CENTER entry — which shows considerably more, and is where you read a toast you missed —
    // carries a genuinely useful amount. The overlay's row 3 caps at 300 for a HUD row that WRAPS;
    // this line cannot wrap, hence the tighter number.
    inline constexpr size_t kNotifyPromptPreviewChars = 120;
    static_assert(kNotifyExternalHoldCapMs > kScanExternalWorkGraceMs + 2 * kScanSweepMs,
                  "the hold cap must outlast the outlived-turn promotion latency (grace + sweep ticks), else the backstop fires the spurious toast right before the promotion re-lights Running");

    // PURE: should a just-detected Running -> `target` completion toast be HELD instead of shown?
    // Only the two states the outlived-turn promotion can veto (Idle / WaitingForInput) ever hold;
    // NeedsApproval / Error / Done fire immediately — external work can't answer a question, undo a
    // failure, or revive an exited claude, so those genuinely need the user NOW.
    inline bool ShouldHoldCompletionToast(SessionState target, bool presenceWorking, bool subagentFresh) noexcept
    {
        if (target != SessionState::Idle && target != SessionState::WaitingForInput)
        {
            return false;
        }
        return presenceWorking || subagentFresh;
    }

    // Agentmaster (System notifications — MINIMUM COMPLETION SPAN): a completion whose observed Running
    // span is SHORT yields NO toast at all. One threshold covers two motivations:
    //   (1) the "0 seconds complete but still running" BUG — a slow-path Stop hook lands ~20ms after the
    //       NEXT turn's UserPromptSubmit (the same slow-Stop race NextSessionStateOrdered's stale guard
    //       targets, seen here as a genuine sub-second registry dip because the guard let it through) and
    //       the session re-lights Running right after, so the toast claimed a completion that never
    //       happened. Proven live on 6e2d0b48 / 532dc9ac / e7fa7fcc / 09e226cb: UPS->Running, a Stop
    //       16-42ms later ->Waiting, then another UPS->Running within ~0.2-1.3s.
    //   (2) SIGNAL over noise — a turn that finished in a few seconds never needed an interruption; you
    //       were almost certainly still looking at it. The toast earns its interruption on LONG work.
    // This is deliberately a HARD FLOOR, not the deferred/confirming hold it started as: below it there
    // is no toast EVER, not a late one. (A hold cannot express this — a held toast's span keeps growing,
    // so it would eventually cross any threshold and fire, and "no notification" must never decay into
    // "a delayed notification".)
    inline constexpr int64_t kNotifyMinCompletionSpanMs = 10000; // a Running span under this yields NO completion toast at all — bug (1) + noise (2)
    // PURE: should a Running -> `target` completion be SUPPRESSED outright because its observed Running
    // span was too short to be trustworthy as (or worth) an interruption? Only Idle / WaitingForInput are
    // floored — the two SOFT "the turn ended" states, and the only ones a re-light can veto.
    // NeedsApproval / Error / Done are NEVER suppressed: an approval request routinely blocks seconds
    // into a turn (flooring it would silently hide the one state most needing you), and a failure or an
    // exited claude needs you regardless of how briefly it ran. runningSpanMs == 0 means the Running
    // ENTRY edge was never observed (adopted / moved in mid-turn) -> the span is UNKNOWN, not short:
    // never suppressed on this basis (it may have run for hours, and its toast honestly omits the
    // "after <span>" clause rather than claiming a duration).
    inline bool ShouldSuppressShortCompletionToast(SessionState target, int64_t runningSpanMs) noexcept
    {
        if (target != SessionState::Idle && target != SessionState::WaitingForInput)
        {
            return false;
        }
        return runningSpanMs > 0 && runningSpanMs < kNotifyMinCompletionSpanMs;
    }

    // Agentmaster (the presence/answer resume synth's one side effect). Releasing NeedsApproval ->
    // Running the instant the user ANSWERS means the "answered, agent replies briefly, turn ends"
    // shape — measured 1.06s of work on bee54051, 1.55s on 98d6805f ("Chat about this") — now passes
    // THROUGH Running instead of going NeedsApproval -> WaitingForInput directly. That is honest (the
    // agent really did run), but it converts a non-edge into a flash-ring EDGE: Running -> needs-you
    // flashes, target -> target does not. Left alone, EVERY answered question would newly flash its
    // tab. This floor suppresses exactly that blip and nothing else.
    inline constexpr int64_t kFlashMinRunSpanAfterAnswerMs = 3000; // a post-answer Running span under this does not flash
    // PURE: should the unvisited-tab flash be suppressed for a Running -> `target` edge whose Running
    // span was just the post-answer blip above? DELIBERATELY NARROW on three axes, so no pre-existing
    // edge changes behavior at all:
    //   * runEnteredFromNeedsApproval — only a Running entered FROM NeedsApproval (i.e. the answer
    //     release) is floored. An ordinary short turn (Waiting -> Running -> Waiting) still flashes
    //     exactly as it always did.
    //   * target — only the SOFT "turn ended" states. A post-answer NeedsApproval (the agent asked a
    //     SECOND question) / Error / Done always flashes, however brief the run.
    //   * runningSpanMs > 0 — an UNOBSERVED entry edge (span unknown, e.g. adopted mid-turn) is never
    //     suppressed on this basis, mirroring ShouldSuppressShortCompletionToast's reasoning.
    // Above the floor the flash is CORRECT and wanted: you answered, walked away, it worked, it now
    // needs you again — exactly what the ring is for.
    inline bool ShouldSuppressAnswerBlipFlash(SessionState target, bool runEnteredFromNeedsApproval, int64_t runningSpanMs) noexcept
    {
        if (!runEnteredFromNeedsApproval)
        {
            return false;
        }
        if (target != SessionState::Idle && target != SessionState::WaitingForInput)
        {
            return false;
        }
        return runningSpanMs > 0 && runningSpanMs < kFlashMinRunSpanAfterAnswerMs;
    }

    // PURE: the per-sweep ruling on one HELD toast. Drop == the completion proved spurious (the
    // session re-lit Running — the outlived-turn promotion or a real new turn) or the session left
    // the fleet (archived); Fire == the completion stands (signal cleared, or the session moved to a
    // hard needs-you state mid-hold, or the cap backstop elapsed); Keep == still ambiguous, sweep
    // again next tick. The caller fires with the CURRENT state (the body says what the session is
    // NOW) and re-applies the cog switches / focused-skip / duplicate guard at fire time.
    enum class HeldToastVerdict
    {
        Keep,
        Fire,
        Drop,
    };
    inline HeldToastVerdict DecideHeldToast(SessionState current, bool live, bool signalPresent, int64_t heldForMs) noexcept
    {
        if (!live)
        {
            return HeldToastVerdict::Drop;
        }
        if (current == SessionState::Running)
        {
            return HeldToastVerdict::Drop;
        }
        if (current != SessionState::Idle && current != SessionState::WaitingForInput)
        {
            return HeldToastVerdict::Fire; // moved to NeedsApproval / Error / Done mid-hold: needs you regardless of background work
        }
        if (!signalPresent)
        {
            return HeldToastVerdict::Fire;
        }
        if (heldForMs >= kNotifyExternalHoldCapMs)
        {
            return HeldToastVerdict::Fire;
        }
        return HeldToastVerdict::Keep;
    }

    // PURE: the per-session double-toast guard — suppress a toast shown within
    // kNotifyDuplicateToastMs of the session's previous SHOWN toast (lastShownMs == 0 => never shown).
    inline bool ShouldSuppressDuplicateToast(int64_t nowMs, int64_t lastShownMs) noexcept
    {
        return lastShownMs > 0 && (nowMs - lastShownMs) < kNotifyDuplicateToastMs;
    }

    // PURE + total: should the reconciler demote a WaitingForInput session to Idle now? (Agentmaster
    // Waiting-for-you "unread" model — the replacement for the old "decay after the 5-minute cache
    // window" rule.) A session leaves Waiting-for-you for Idle only when ALL hold:
    //   * decay is enabled (minutes != 0; 0 == the cog's "Never"),
    //   * it is actually WaitingForInput,
    //   * it is NOT manually "Mark Unread"-ed (a manual mark is sticky — only a visit/archive clears it),
    //   * claude is NOT self-reporting work — "busy" (a long Task/Agent subagent keeps the heartbeat busy
    //     while the parent transcript is quiescent — the recon-subagent promotion's target; don't race it
    //     to Idle) or "shell" (a live shell job — PresenceIsWorking),
    //   * there IS an activity anchor (lastActivityMs > 0),
    //   * the timeout has elapsed (now - lastActivityMs >= minutes), AND
    //   * the session has been READ since that activity (readUnixMs >= lastActivityMs) — an unread,
    //     past-timeout session keeps waiting-for-you until the user reads (visits) it.
    // So Waiting-for-you persists for the FULL timeout regardless of reading, and past the timeout it
    // persists further while still unread; the demote happens at max(timeout, read-time). presenceWorking
    // is passed in (the caller computes PresenceIsWorking) so this stays a pure, testable predicate.
    inline bool ShouldDecayWaitingToIdle(SessionState state, int64_t lastActivityMs, int64_t readUnixMs, bool manualUnread, bool presenceWorking, uint32_t minutes, int64_t nowMs) noexcept
    {
        if (minutes == 0 || state != SessionState::WaitingForInput || manualUnread || presenceWorking || lastActivityMs <= 0)
        {
            return false;
        }
        const int64_t timeoutMs = static_cast<int64_t>(minutes) * 60000;
        if (nowMs - lastActivityMs < timeoutMs)
        {
            return false; // still inside the waiting window — always waits the full timeout
        }
        return readUnixMs >= lastActivityMs; // past the timeout: decay only once READ, else keep waiting
    }

    // Ticked on the scanner thread on the slow cadence; the probe marshals to ITS OWN UI thread
    // and archives any of its claude tabs whose ConPTY connection has Closed. One per window (M9).
    using LivenessProbe = std::function<void()>;
    using LivenessToken = uint64_t;

    class SessionScanner
    {
    public:
        explicit SessionScanner(std::shared_ptr<SessionRegistry> registry);
        // Stops the worker and the transcript watcher (Stop()). Defined in the .cpp, where TranscriptWatcher
        // is a complete type (the unique_ptr member needs it). Never throws.
        ~SessionScanner();

        SessionScanner(const SessionScanner&) = delete;
        SessionScanner& operator=(const SessionScanner&) = delete;

        // Start the worker thread, and before it the TranscriptWatcher on ClaudeProjectsDir() (HOOKS.md
        // "Transcript watcher") — its change callback is this scanner's Wake(). A watcher that cannot start
        // (the OS refused its thread/event) is logged and skipped: the scanner then polls exactly as it did
        // before the watcher existed. Idempotent. Throws std::system_error only if the WORKER thread cannot
        // be created (the engine's init net catches it).
        void Start();
        // Stop the worker (join) and then the watcher, in that order: the worker reads the watcher on every
        // pass, and the watcher's callback only ever touches Wake(). Idempotent. Never throws.
        void Stop() noexcept;

        // Break the idle wait early (e.g. a session just went live). Wired to a registry observer
        // in Engine so a newly-launched/adopted session is picked up immediately, not after a
        // timeout. Cheap + idempotent.
        void Wake() noexcept;

        // The session database's change feed (SESSIONS_DB.md §7.6, D16), forwarded to the transcript watcher:
        // turn the changed-session set on or off, and take the sessions changed since the last take. A scanner
        // whose watcher could not start answers "not healthy" (out.healthy false), which tells the refresh tick
        // to sweep instead. Call only between Start() and Stop(): the engine stops its refresher first.
        // SetTranscriptChangeTracking never throws; TakeChangedTranscripts throws only std::bad_alloc.
        void SetTranscriptChangeTracking(bool on) noexcept;
        void TakeChangedTranscripts(ChangedSessions& out);

        // Register / detach an app-layer liveness probe (one per window; M9 fan-out, mirroring the
        // registry's adoption handlers). Tokens are monotonic and never reused, so removing a stale
        // one is a no-op. The window detaches via RemoveLivenessProbe on teardown.
        LivenessToken AddLivenessProbe(LivenessProbe probe);
        void RemoveLivenessProbe(LivenessToken token);

        // Call once at engine init to keep the worker TICKING even with no live sessions, so each
        // window's liveness probe + the Fleet Observer's per-window roster publish keep running (a
        // hand-typed `claude` in a fresh tab is then correlated out-of-band by the observer). The
        // transcript-discovery enumeration this used to also start is retired (O7). Name kept for now.
        void ArmDiscovery();

        // Agentmaster (Waiting-for-you "unread" model): how long a session may sit in WaitingForInput
        // before the scanner is allowed to demote it to Idle — gated additionally on read-state (see
        // ShouldDecayWaitingToIdle: it demotes only once the timeout has elapsed AND the session has
        // been read, and never while manually Mark-Unread-ed). 0 == never decay. Default 60 (1 hour).
        // Seeded from AppSettings at engine init and re-pushed when the Settings cog saves. Atomic —
        // the cog writes from a UI thread while the worker reads.
        void SetWaitingDecayMinutes(uint32_t minutes) noexcept
        {
            _waitingDecayMinutes.store(minutes);
        }

        // Agentmaster (COMMANDS.md): attach the process-wide CommandWatch the delta parse feeds —
        // command sightings, file-writing tool_use paths, turn boundaries, and the per-pass Tick.
        // Set ONCE at engine init BEFORE Start() (the worker reads it unsynchronized; the thread
        // creation is the happens-before). Null == no command bindings (tests / headless).
        void SetCommandWatch(std::shared_ptr<CommandWatch> watch) noexcept
        {
            _commandWatch = std::move(watch);
        }

    private:
        // Per-session tail cursor (owned solely by the worker thread — no lock needed).
        struct ScanState
        {
            std::wstring path; // resolved transcript path ("" until found / after it vanishes)
            // Agentmaster (HOOKS.md "Transcript watcher"): the CACHED stat of `path` — reused while the watcher
            // reports no change for a cold session (ShouldRefreshTranscriptStat), so an unchanged session costs
            // no GetFileAttributesEx. The seen* stamps record the watcher state the stat covers; they are taken
            // BEFORE the stat, so a change landing after it bumps the sequence again and re-stats next pass.
            bool statValid{ false }; // a stat of the CURRENT path is cached
            int64_t statSize{ 0 }; // the transcript's size at that stat
            int64_t statWriteUnixMs{ 0 }; // its last-write time at that stat (Unix ms) — every quiet-time computation reads this
            int64_t lastStatMs{ 0 }; // when that stat was taken (the cold resync clock)
            uint64_t seenStatEpoch{ 0 }; // watcher epoch the stat covers
            uint64_t seenTranscriptSeq{ 0 }; // watcher transcript sequence the stat covers
            // The CACHED newest side-file write (SubagentActivityUnixMs: subagents/ + tool-results/), with the
            // same watcher bookkeeping (ShouldRefreshSideActivity).
            bool sideValid{ false };
            int64_t subagentActivityMs{ 0 };
            int64_t lastSideScanMs{ 0 };
            uint64_t seenSideEpoch{ 0 };
            uint64_t seenSideSeq{ 0 };
            // When the classic every-project-folder resolve may run next for this unresolved session: the
            // retry back-off with no healthy watcher (kScanResolveRetryMs), the index cross-check with one
            // (kScanIndexCrossCheckMs). 0 == now.
            int64_t nextResolveMs{ 0 };
            int64_t offset{ 0 }; // BYTE offset already consumed (only complete lines advance it)
            int64_t lastSize{ -1 }; // file size stamped by the last read — the cheap change-gate, paired with readReachedEnd (ShouldReadTranscriptDelta): alone it froze a window-capped catch-up
            std::wstring lastAssistantText; // latest assistant text seen (for the missed-Stop question)
            std::wstring lastStopReason; // latest assistant stop_reason (IsTerminalStopReason => turn complete)
            // Has the cursor ever CAUGHT UP with the file end? False while the initial backlog is
            // still being consumed — a restored/adopted session's tail window (FirstSightReadOffset),
            // or, on the alignment fallback, its whole history from offset 0 in 1 MiB chunks, one per
            // pass; true from the first pass that reached the current end. Only
            // events consumed AFTER priming are a live append — the run-repair gate
            // (ShouldSynthesizeRunning) reads the value from BEFORE the pass, so the pass that
            // finishes the replay cannot itself synthesize. Reset with the truncation rewind.
            bool primed{ false };
            // Agentmaster (the 2026-09-25 CATCH-UP STALL, session d1f3f198 — ShouldReadTranscriptDelta):
            // did the LAST _readDelta cover every byte up to the size it was handed? True only when the
            // read reached the stat'd end, possibly leaving one unterminated line in flight. False after a
            // window-capped read (a backlog of complete lines remains), a failed open/seek/read, a grown
            // re-read that came back short, or the first-sight alignment fallback. Two consumers: false
            // forces the next pass to read even though the size did not change (without that, a finished
            // turn's quiet file froze the cursor mid-history for good), and it is the "tail facts are
            // CURRENT" gate that keeps every tail-judging synth from acting on facts that describe a line
            // somewhere mid-file. Starts false: nothing has been read yet. Reset with the truncation rewind.
            bool readReachedEnd{ false };
            // Agentmaster (FIRST-SIGHT TAIL SEEK — FirstSightReadOffset): has this cursor's starting point
            // been decided? Latched by the first _readDelta so the seek happens exactly once per fresh
            // cursor. That keeps the alignment fallback (back to offset 0) from re-seeking to the tail on
            // the next pass and looping. Reset by the truncation rewind, since a rewritten file gets a fresh
            // first sight.
            bool seekDecided{ false };
            // Agentmaster (FIRST-SIGHT TAIL SEEK): the cursor sits at an ARBITRARY byte (the tail-seek
            // point), mid-line. The next read must drop the leading fragment up to the first complete line
            // (FirstCompleteLineStart) before parsing. Cleared once aligned, or on the fallback to offset 0,
            // which is a line start by definition.
            bool alignSeek{ false };
            // Tail facts that disambiguate "blocked on the user" / "interrupted" from "working" —
            // the missed-Stop backstop can't read either from stop_reason alone (HookEvents.h notes).
            std::wstring pendingInteractiveTool; // an UNANSWERED interactive tool_use (AskUserQuestion) is the latest assistant block; "" once answered / moved on
            bool interrupted{ false }; // the latest user line is a turn-abort marker (Esc) — treat as a turn-ender
            // Agentmaster (presence-driven blocked-on-user, SessionScanner.h ShouldSynthesize*FromPresence):
            // have we OBSERVED claude's heartbeat report "waiting" (blocked on the user) for this
            // session, and not yet acted on its release? Armed by a "waiting" read, disarmed the moment
            // the heartbeat leaves it (either by the resume synth -> Running, or silently when it goes
            // to "idle", which is recon-stop's -> Waiting). This is what makes the release a genuine
            // waiting->working EDGE rather than a "busy while NeedsApproval" LEVEL — see the predicate
            // for why that distinction is load-bearing. Purely in-memory, like the rest of ScanState.
            bool presenceWasWaiting{ false };
            // Agentmaster: did THIS pass consume the tool_result that answered a pending interactive
            // tool_use (AskUserQuestion)? Per-pass scratch — reset before each _readDelta, read by the
            // recon-resume gate as the "the user just answered" signal. See ShouldSynthesizeResumed.
            bool answeredInteractive{ false };
            // Agentmaster (ScanSessionIsDormant): has the CURRENT dormancy episode already logged a held
            // Running promotion? It turns the per-pass hold into one [recon-dormant] line per episode, so a
            // parked tab whose dying claude left a burst of residue reads as one line, not one per pass.
            // Re-armed by the first pass that finds the session started again (a new park is a new episode).
            bool dormantHoldLogged{ false };
            // Agentmaster (DELIVERY.md §12 — the box-not-visible WARNING's once-per-episode dedup):
            // the SessionInfo::pendingBoxStateUnixMs stamp this scanner already warned for. The R5
            // escalation used to pause the autorunner (mode → Off), which self-deduped; the §12
            // defang keeps the mode, so ShouldWarnOnBoxNotVisible stays true every pass while the
            // NoBox episode stands — this stamp keeps the [send-verify] warning to one line per
            // episode (a recovered-then-re-lost box mints a new stamp and warns afresh).
            int64_t boxNotVisibleWarnedStamp{ 0 };
            // Agentmaster (the TURN-START RACE — NewerPromptNotYetInTranscript): the prompt stamp
            // (TurnAccounting::lastPromptUnixMs) this scanner already logged a held turn end for. The hold
            // repeats every pass until the prompt line lands (~0.2-0.5 s, at most
            // kScanPromptLandingGraceMs), so this keeps the [recon-stop-hold] evidence to ONE line per
            // prompt. 0 == nothing logged yet.
            int64_t promptHoldLoggedFor{ 0 };
            // The latest consumed turn event was the synthetic API-error message (isApiErrorMessage),
            // i.e. the transcript tail is CURRENTLY an UNRECOVERED API error. Set by an apiError
            // assistant line; CLEARED by any later turn event (a new user prompt, a non-error assistant
            // line, or a tool_result) — so it tracks "is the conversation's NEWEST event an error?".
            // This is the BYTE-POSITIONAL signal; it is REFINED by the active-leaf check below
            // (activeLeafUuid == errorEpochLeaf) so a double-ESC REWIND past the error — which leaves the
            // error physically last in the file (no later turn event clears this) yet repoints the active
            // leaf OFF it — is recognized as recovered (ApiErrorIsActiveLeaf / ShouldReleaseErrorOnLeafMove).
            // Drives ShouldSynthesizeError, and its clearing is the "come out of Error on first change"
            // edge (a fresh prompt then re-derives Running via the push hook or ShouldSynthesizeRunning).
            bool lastWasApiError{ false };
            // The MESSAGE + HTTP status code of the error line that set lastWasApiError (status 0 when
            // none). BOTH captured DIRECTLY from the error event the instant it is consumed, and carried
            // into the recon-error synth as HookMessage.errorMessage/errorStatus -> SessionInfo. The
            // message is kept separately (NOT read back from lastAssistantText) precisely because that
            // mirror skips an empty-text line — so a (hypothetical) empty-text error could otherwise
            // surface a STALE prior assistant message under the error styling. Read only while
            // lastWasApiError is true (set by that same event), so never stale when used.
            std::wstring lastApiErrorMessage;
            int lastApiErrorStatus{ 0 };
            // The active-branch head, tracked from {"type":"last-prompt","leafUuid":…} markers (Claude
            // names the last user PROMPT — the re-prompt anchor — and rewrites it as the leaf moves). The
            // running value (updated by every LeafMarker event); "" until the first marker is seen.
            std::wstring activeLeafUuid;
            // The active leaf captured AT THE MOMENT the API error was consumed (= activeLeafUuid then).
            // The error is "the last message" while the leaf STAYS here OR advances onto the error's OWN
            // descendant chain (errorBranchUuids below); a later last-prompt naming a leaf that is NEITHER
            // is a REWIND past the error -> the error is off the active branch -> leave Error. "" when no
            // marker had been seen yet (then activeLeafUuid is "" too, so the positional lastWasApiError
            // governs — older/subagent transcripts have no markers).
            std::wstring errorEpochLeaf;
            // The API-error line's OWN uuid + its forward DESCENDANT frontier — the bookkeeping lines Claude
            // appends AFTER the error (a system/turn_duration child, then an away_summary), each parented to
            // the prior. Seeded with the error's uuid when the error is consumed; extended by any later Node
            // (bookkeeping) line whose parentUuid is already in the set (_readDelta). The post-error
            // last-prompt marker points at the turn_duration CHILD, so the active leaf lands IN this set —
            // ApiErrorIsActiveLeaf then reads it as the error still being the tail, NOT a rewind (the fix for
            // an API error that fired no Error state because its terminal stop_reason read as a clean
            // turn-complete). CLEARED together with lastWasApiError when a turn event supersedes the error.
            std::wstring errorUuid;
            std::unordered_set<std::wstring> errorBranchUuids;
            // Agentmaster (CUT-OFF TURNS — HOOKS.md "Cut-off turns"; ClassifyCutOffTurn): the turn that is
            // still IN FLIGHT at the tail, as the kind of line it stopped on; None once the tail is closed
            // (a terminal stop_reason, an API error, a user interrupt) or before any turn line was read.
            // Folded in _readDelta from every turn line. Claude Code's resume placeholder does NOT close
            // it (that line is what a resuming claude writes for a turn that got no answer), and a
            // tool result that arrives after the user's interrupt marker does not reopen it.
            OpenTurnKind openTurn{ OpenTurnKind::None };
            // The resume placeholder line arrived while `openTurn` was open: a later claude process loaded
            // this conversation and found that turn unanswered. Cleared by the next real turn line (a
            // human prompt or an assistant line), and by a pass that finds the session Running.
            bool resumeMarkerOnOpenTurn{ false };
            // The tail carries Claude Code's interruptedByShutdown flag (TranscriptEvent::shutdownInterrupt)
            // on its interrupt marker or on the tool results before it. Cleared like the flag above.
            bool shutdownInterrupt{ false };
            // The scanner saw `openTurn` open while the session was DORMANT (ScanSessionIsDormant), and no
            // real turn line replaced it since. It is what keeps the cut-off verdict standing across the
            // moment the tab starts: `dormant` turns false there, but a turn that had no process a second
            // ago did not get one by the tab starting. Without it the verdict would drop for the seconds
            // until the resumed claude writes its placeholder (or for good, when it writes none — it
            // writes none after an unfinished ASSISTANT line), and the stale-Error release would then
            // hand the session back as Waiting. Cleared like the two flags above.
            bool openTurnSeenDormant{ false };
            int64_t contextTokens{ 0 }; // newest assistant usage tokens (≈ context occupancy); mirrored QUIETLY to SessionInfo for the board card's context-% adornment
            // The newest REAL assistant line's message.model ("<synthetic>" API-error lines skipped) —
            // mirrored change-gated onto SessionInfo.currentModel via the NOTIFYING Update (a model
            // change is rare — first reply / a `/model` switch — and the card/overlay/tooltip should
            // repaint when it lands, unlike the per-line contextTokens churn above).
            std::wstring currentModel;
        };

        void _worker() noexcept;
        int64_t _scanOnce(); // one coalesced pass; returns the next sleep ms (<0 == sleep until woken)
        void _reconcileSession(const SessionInfo& s);
        // Reads (at most one window, or one grown window for a single 1-4 MiB line) of the transcript
        // past st.offset and folds the complete lines into the tail facts. On a fresh cursor it first
        // seeks to the tail window (FirstSightReadOffset) and aligns to a complete line, falling back to
        // offset 0 when the window lies inside one line.
        //   st   — the session's cursor + tail facts. offset/primed/readReachedEnd/seekDecided/alignSeek
        //          are advanced here, and readReachedEnd is set on EVERY exit path: true only when this
        //          read covered the stat'd size, false on a capped chunk, an I/O failure, a short grown
        //          re-read, or the alignment fallback.
        //   s    — the pass snapshot of the session: its id keys every registry mirror (recap / last
        //          assistant text / context / model / the pull echo / the interrupt hold), and its
        //          workingDir resolves the CommandWatch's relative Write/Edit paths.
        //   size — the transcript size from THIS pass's stat (the read never goes past it).
        // Returns true when ≥1 turn event (user prompt / assistant line) was consumed this call —
        // the trigger for the missed-UserPromptSubmit repair in _reconcileSession. Claude Code's resume
        // placeholder line (TranscriptEvent::resumePlaceholder) is NOT a turn event: it is written at
        // load, not by a turn, so it must neither expire a dismissed error nor look like progress.
        // Throws only what an allocation can (std::bad_alloc from the byte/wide buffers or a registry
        // mirror). The parse contains its own failures (ParseTranscriptDelta), and anything that
        // escapes is caught and logged by _worker's top-level net. The cursor and readReachedEnd are
        // stamped BEFORE the fold (the cursor always was), so a throw mid-fold costs at most that
        // chunk's unfolded facts; the next append's lines re-establish them.
        bool _readDelta(ScanState& st, const SessionInfo& s, int64_t size);
        void _maybeSweepLiveness(int64_t nowMs, bool anyLive);
        void _maybeDecayWaiting(const SessionInfo& s, int64_t nowMs); // WaitingForInput older than the decay window -> Idle

        std::shared_ptr<SessionRegistry> _registry;
        std::thread _thread;
        std::mutex _mtx; // guards _woken + _probes (NOT _scan: worker-confined)
        std::condition_variable _cv;
        std::atomic<bool> _running{ false };
        bool _woken{ false };

        std::unordered_map<std::wstring, ScanState> _scan; // sessionId -> tail cursor (worker thread only)
        std::vector<std::pair<LivenessToken, LivenessProbe>> _probes;
        uint64_t _nextProbeId{ 1 };
        int64_t _lastSweepMs{ 0 };

        // Keeps the worker TICKING with nothing live (so each window's observer probe + liveness
        // sweep keep running). Set once via ArmDiscovery at engine init; the transcript-ENUMERATION
        // it used to also drive is retired (O7) — the Fleet Observer's PEB correlation subsumes it.
        std::atomic<bool> _discoverArmed{ false };

        // COMMANDS.md: the slash-command binding/await engine (set once before Start; may be null).
        std::shared_ptr<CommandWatch> _commandWatch;

        // HOOKS.md "Transcript watcher": the ReadDirectoryChangesW change feed over <claude config>\projects.
        // Created by Start() BEFORE the worker thread (the thread creation is the happens-before for the
        // worker's unsynchronized reads of the pointer), stopped + destroyed by Stop() AFTER the worker has
        // joined. Null when it could not start — every use then falls back to the classic polling.
        std::unique_ptr<TranscriptWatcher> _watcher;

        // Waiting-for-you -> Idle timeout in minutes (see SetWaitingDecayMinutes). The default
        // mirrors AppSettings::waitingForYouTimeoutMinutes (3 days) so the behavior holds even before the seed lands.
        std::atomic<uint32_t> _waitingDecayMinutes{ 4320 };
    };
}
