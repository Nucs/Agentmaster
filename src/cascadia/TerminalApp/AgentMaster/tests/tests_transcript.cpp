// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster M5 engine test harness (7 partial files)
// Standalone engine test harness (NOT in the msbuild) -- run-m5-tests.bat compiles every TU
// unity-style without the WinRT PCH and links the engine .cpp. The 38 tests + 2 benches were
// split out of the former 6006-line m5_tests.cpp into themed TUs that share m5_tests.h.
//
// Partial files in this group (★ marks THIS file):
//   m5_tests.cpp              - the RUNNER: wmain (calls every entry point, in order) + the g_checks/g_failures defs
//   m5_tests.h                - shared header: the CHECK macro, the extern counters, the fixtures (MakeSession/Msg/UPS/NowMsTest), and all 40 test entry-point declarations
//   tests_state.cpp           - state machine / ordered-state / wire / registry / fanout / fork-echo / typed-capture / ObserveClaude / supersede
//   tests_spawn_sched.cpp     - spawn builders / profile bootstrap / bridge round-trip / scheduler / enter-retry / build-prompt / scheduler integration
//   tests_persistence.cpp     - persistence / manager layout / window record / app settings / tab naming + color
// ★ tests_transcript.cpp      - transcript scan + reconcilers / ProcessInspect tree+parse / transcript resolve / Codex / store / lineage / search / live / bring-to-front
//   tests_summary_anchor.cpp  - summary table-trim + user-msg noise / PromptAnchor (+ edge/corpus/benches) / pending-input
// ======================================================================================
//
// Agentmaster - M5 standalone test harness: transcript tests. Shared CHECK/fixtures/decls
// live in m5_tests.h; the runner (m5_tests.cpp) calls each entry point. See run-m5-tests.bat.
#include "m5_tests.h"

void TestTranscriptScan()
{
    std::wprintf(L"Transcript reconciler (interval scan):\n");

    // assistant line: content array with a text block + stop_reason -> one Assistant event
    {
        const std::wstring line = LR"j({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"All done."}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1, "assistant line -> 1 event");
        CHECK(!r.events.empty() && r.events[0].kind == TranscriptEvent::Kind::Assistant, "assistant kind");
        CHECK(!r.events.empty() && r.events[0].text == L"All done.", "assistant text collected");
        CHECK(!r.events.empty() && r.events[0].stopReason == L"end_turn", "assistant stop_reason captured");
        CHECK(r.consumed == line.size(), "consumed the full complete line");
        CHECK(!r.events.empty() && !r.events[0].apiError, "normal assistant line is NOT an apiError");
    }
    // Agentmaster: the synthetic API-error turn-ender — top-level isApiErrorMessage:true, model
    // "<synthetic>", a terminal stop_reason. The parser flags ev.apiError so the scanner can produce
    // SessionState::Error instead of reading the terminal stop as a clean turn-complete. (Mirrors the
    // real c66ec7c8 "Server is temporarily limiting requests" rate-limit shape.)
    {
        const std::wstring line = LR"j({"type":"assistant","isApiErrorMessage":true,"apiErrorStatus":429,"message":{"model":"<synthetic>","stop_reason":"stop_sequence","content":[{"type":"text","text":"API Error: Server is temporarily limiting requests (not your usage limit) · Rate limited"}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].kind == TranscriptEvent::Kind::Assistant, "apiError line -> 1 assistant event");
        CHECK(!r.events.empty() && r.events[0].apiError, "isApiErrorMessage:true -> ev.apiError");
        CHECK(!r.events.empty() && r.events[0].apiErrorStatus == 429, "apiErrorStatus captured (the HTTP code)");
        CHECK(!r.events.empty() && r.events[0].text.find(L"Rate limited") != std::wstring::npos, "apiError text captured (the reason)");
    }
    // A client-side error (no HTTP status, e.g. "Prompt is too long") -> apiError true, status 0.
    {
        const std::wstring line = LR"j({"type":"assistant","isApiErrorMessage":true,"message":{"model":"<synthetic>","stop_reason":"stop_sequence","content":[{"type":"text","text":"Prompt is too long"}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(!r.events.empty() && r.events[0].apiError && r.events[0].apiErrorStatus == 0, "client-side apiError -> status 0 (no HTTP code)");
    }
    // Agentmaster (active-leaf tracking): a {"type":"last-prompt","leafUuid":…} marker -> a LeafMarker
    // event carrying the leafUuid (the active branch head). NOT a turn event — it never affects the
    // missed-Stop / run-repair logic; it lets the scanner tell that an API error rewound off the leaf.
    {
        const std::wstring line = LR"j({"type":"last-prompt","lastPrompt":"audit the diff","leafUuid":"cdb0b74e-d0ae-47b6-a060-fa389e4675e7"})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].kind == TranscriptEvent::Kind::LeafMarker, "last-prompt -> 1 LeafMarker event");
        CHECK(!r.events.empty() && r.events[0].text == L"cdb0b74e-d0ae-47b6-a060-fa389e4675e7", "LeafMarker carries the leafUuid in text");
    }
    // A last-prompt with NO leafUuid (the trust-dialog prompt) carries no leaf -> emit nothing (so it
    // never clobbers the tracked active leaf to empty).
    {
        const std::wstring line = LR"j({"type":"last-prompt","lastPrompt":"Accessing workspace: trust?","sessionId":"x"})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.empty(), "last-prompt without leafUuid -> no LeafMarker event (never clobbers the leaf)");
    }
    // Agentmaster (API-error descendant lineage — the reported "not detected in error state" bug): a REAL
    // post-error sequence from the corpus — the synthetic error (uuid E) is followed by its bookkeeping
    // CHILD (a system/turn_duration line, parent E) and a last-prompt marker naming that CHILD. The parser
    // must surface the error's OWN uuid (frontier seed), a Node carrying the child's lineage (uuid+parent),
    // and the LeafMarker — so the scanner can fold the forward leaf advance into the error's descendant
    // frontier instead of misreading it as a double-ESC rewind (which suppressed Error entirely).
    {
        const std::wstring seq =
            LR"j({"type":"assistant","uuid":"E","isApiErrorMessage":true,"message":{"model":"<synthetic>","stop_reason":"stop_sequence","content":[{"type":"text","text":"API Error: Overloaded"}]}})j" L"\n"
            LR"j({"type":"system","subtype":"turn_duration","uuid":"C","parentUuid":"E"})j" L"\n"
            LR"j({"type":"last-prompt","leafUuid":"C"})j" L"\n";
        const auto r = ParseTranscriptDelta(seq);
        CHECK(r.events.size() == 3, "post-error sequence -> 3 events (apiError assistant + Node child + LeafMarker)");
        CHECK(r.events.size() == 3 && r.events[0].kind == TranscriptEvent::Kind::Assistant && r.events[0].apiError && r.events[0].uuid == L"E",
              "ev0: apiError assistant carries its OWN uuid (frontier seed)");
        CHECK(r.events.size() == 3 && r.events[1].kind == TranscriptEvent::Kind::Node && r.events[1].uuid == L"C" && r.events[1].parentUuid == L"E",
              "ev1: the turn_duration child -> a Node carrying uuid + parentUuid (the descendant chain link)");
        CHECK(r.events.size() == 3 && r.events[2].kind == TranscriptEvent::Kind::LeafMarker && r.events[2].text == L"C",
              "ev2: the last-prompt marker names the CHILD C (the forward advance, not a rewind)");
    }
    // A mode/permission-mode config line carries NO uuid/parentUuid -> no Node (never pollutes the chain).
    {
        const auto r = ParseTranscriptDelta(LR"j({"type":"mode","mode":"default"})j" L"\n");
        CHECK(r.events.empty(), "a config line without uuid/parentUuid -> no Node event");
    }
    // assistant tool_use turn: stop_reason tool_use, no text (turn NOT complete -> no synth Stop)
    {
        const std::wstring line = LR"j({"type":"assistant","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"Bash"}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].stopReason == L"tool_use", "tool_use stop_reason");
        CHECK(r.events.size() == 1 && r.events[0].text.empty(), "tool_use line has no text");
    }
    // user line: plain string content -> a UserPrompt
    {
        const std::wstring line = LR"j({"type":"user","message":{"role":"user","content":"hello there"}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].kind == TranscriptEvent::Kind::UserPrompt, "user string -> prompt");
        CHECK(r.events.size() == 1 && r.events[0].text == L"hello there", "user prompt text");
    }
    // user line: tool_result array -> NOT a human prompt (no false positive); it is a ToolResult
    // marker (a tool completed -> answers a pending interactive tool_use), never a UserPrompt.
    {
        const std::wstring line = LR"j({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"x","content":"ok"}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].kind == TranscriptEvent::Kind::ToolResult, "tool_result user line -> ToolResult marker, not a prompt");
    }
    // user line: isMeta -> skipped
    {
        const std::wstring line = LR"j({"type":"user","isMeta":true,"message":{"content":"<command-reminder>"}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.empty(), "meta user line -> skipped");
    }
    // Agentmaster (the /model false-Running fix): a LOCAL slash command writes NON-meta user lines —
    // the "<command-name>…" echo + its "<local-command-stdout>…" result (the exact live de4fcb12
    // shape below) — and starts NO API turn. They must NOT come back as UserPrompt turn events:
    // emitting them made recon-run light an Idle/Waiting session Running off a mere `/model`, and
    // their tail-fact wipes also released NeedsApproval / Error. Skipped like isMeta lines.
    {
        const std::wstring line = LR"j({"type":"user","message":{"role":"user","content":"<command-name>/model</command-name>\n            <command-message>model</command-message>\n            <command-args></command-args>"}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.empty(), "a /model <command-name> echo (non-meta user line) -> NO turn event");
    }
    {
        const std::wstring line = LR"j({"type":"user","message":{"role":"user","content":"<local-command-stdout>Set model to Opus 4.8 and saved as your default</local-command-stdout>"}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.empty(), "a /model <local-command-stdout> result line -> NO turn event");
    }
    // ...the whole IsNoiseUserPrompt set rides the same skip — a teammate WAKE wrapper included
    // (its turn is REAL, but state rides the real UserPromptSubmit hook / the wake turn's own
    // assistant lines, never the wrapper echo)...
    {
        const std::wstring line = LR"j({"type":"user","message":{"content":"Another Claude session sent a message:\n<teammate-message teammate_id=\"P3-docs\">\n{\"type\":\"idle_notification\"}\n</teammate-message>"}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.empty(), "a teammate-wake wrapper user line -> NO turn event (push owns the wake)");
    }
    // ...EXCEPT the interrupt marker: it is in the noise list too, but it is a real turn-ENDER the
    // reconciler must keep seeing (recon-stop keys on it), so it still flows as a UserPrompt event.
    {
        const std::wstring line = LR"j({"type":"user","message":{"content":"[Request interrupted by user]"}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].kind == TranscriptEvent::Kind::UserPrompt && IsUserInterruptMarker(r.events[0].text),
              "the interrupt marker is noise-listed but STILL emitted (a real turn-ender)");
    }
    // ...and a REAL prompt merely MENTIONING a marker mid-text is untouched (prefix-anchored filter).
    {
        const std::wstring line = LR"j({"type":"user","message":{"content":"explain <command-name> semantics to me"}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].kind == TranscriptEvent::Kind::UserPrompt,
              "a real prompt mentioning a marker mid-text is still a UserPrompt");
    }
    // The isCompactSummary bridge ("This session is being continued…" — a /compact's synthetic,
    // NON-meta user line): skipped like isMeta, mirroring every other transcript reader. Emitting
    // it read as a turn START and back-filled the WHOLE summary into the Typed record (91 rows ≈
    // 1.36 MB of machine text found persisted in the prod registry, 2026-07-16 sweep).
    {
        const std::wstring line = LR"j({"type":"user","isCompactSummary":true,"message":{"content":"This session is being continued from a previous conversation that ran out of context. The summary…"}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.empty(), "the isCompactSummary /compact bridge -> NO turn event (skipped like isMeta)");
    }
    // The background-BASH completion wake (<bash-notification> — the shell twin of
    // <task-notification>; a NON-meta string user line, 14 corpus-wide): noise-listed now, so the
    // parser drops it like the other wake wrappers (its real wake turn lights via hook/assistant).
    {
        const std::wstring line = LR"j({"type":"user","message":{"content":"<bash-notification>\n<shell-id>b92b2f3</shell-id>\n<output-file>C:\\tmp\\out.txt</output-file>\n</bash-notification>"}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.empty(), "a <bash-notification> background-shell wake -> NO turn event");
    }
    // partial trailing line: only the complete line is parsed; consumed stops at the last newline
    {
        const std::wstring chunk = LR"j({"type":"user","message":{"content":"first"}})j" L"\n" LR"j({"type":"user","message":{"content":"par)j";
        const auto r = ParseTranscriptDelta(chunk);
        CHECK(r.events.size() == 1 && r.events[0].text == L"first", "partial tail: only the complete line parsed");
        CHECK(r.consumed == chunk.find(L'\n') + 1, "partial tail: consumed stops at the last newline");
    }
    // CRLF + leading blank line tolerated
    {
        const std::wstring chunk = std::wstring{ L"\r\n" } + LR"j({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok?"}]}})j" + L"\r\n";
        const auto r = ParseTranscriptDelta(chunk);
        CHECK(r.events.size() == 1 && r.events[0].text == L"ok?", "CRLF + blank line tolerated");
    }
    // garbage / non-JSON lines are skipped, not fatal
    {
        const auto r = ParseTranscriptDelta(L"not json at all\n{bad\n");
        CHECK(r.events.empty(), "garbage transcript lines -> no events (no throw)");
    }
    // ORDER contract: events come back in transcript order. The scanner's missed-Stop fix relies
    // on processing an assistant(end_turn) THEN a following user(prompt) in order, so the new
    // human turn clears the stale end_turn before it could trigger a premature synthesized Stop.
    {
        const std::wstring chunk =
            std::wstring{ LR"j({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"done"}]}})j" } + L"\n" +
            LR"j({"type":"user","message":{"content":"next please"}})j" + L"\n";
        const auto r = ParseTranscriptDelta(chunk);
        CHECK(r.events.size() == 2, "mixed delta -> 2 events");
        CHECK(r.events.size() == 2 && r.events[0].kind == TranscriptEvent::Kind::Assistant && r.events[0].stopReason == L"end_turn", "order: assistant end_turn first");
        CHECK(r.events.size() == 2 && r.events[1].kind == TranscriptEvent::Kind::UserPrompt && r.events[1].text == L"next please", "order: user prompt second (clears stale end_turn)");
    }

    // away_summary (Claude Code's idle RECAP): a system line is captured into .recap (normalized —
    // the "(disable recaps in /config)" hint stripped), is NOT a turn event, and never strands the
    // state machine. The LAST recap in a chunk wins; a recap-less chunk leaves .recap empty so the
    // scanner can never clear a good recap with a blank.
    {
        const std::wstring line = LR"j({"type":"system","subtype":"away_summary","content":"We fixed the build. Next: deploy. (disable recaps in /config)"})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.empty(), "recap: away_summary is NOT a turn event");
        CHECK(r.recap == L"We fixed the build. Next: deploy.", "recap: away_summary captured + disable hint stripped");
    }
    {
        const std::wstring chunk =
            std::wstring{ LR"j({"type":"system","subtype":"away_summary","content":"first recap"})j" } + L"\n" +
            LR"j({"type":"system","subtype":"turn_duration","content":"ignored"})j" + L"\n" +
            LR"j({"type":"system","subtype":"away_summary","content":"second recap"})j" + L"\n";
        const auto r = ParseTranscriptDelta(chunk);
        CHECK(r.recap == L"second recap", "recap: the LAST away_summary in a chunk wins; a non-away system subtype is ignored");
    }
    {
        const std::wstring line = LR"j({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"hi"}]}})j" L"\n";
        CHECK(ParseTranscriptDelta(line).recap.empty(), "recap: a recap-less chunk yields an empty .recap (never clears a stored recap)");
    }

    // NoteExternalPrompt: dedups against an existing Sent (the hook already recorded the message),
    // but records a genuinely missed one, tagged Typed/Sent.
    {
        SessionRegistry reg;
        reg.Upsert(MakeSession(L"s1"));
        reg.OnHookEvent(UPS(L"s1", L"typed by human")); // hook path records a Typed (Sent) entry
        const auto before = reg.Get(L"s1");
        const size_t n0 = before ? before->queue.size() : 0;
        reg.NoteExternalPrompt(L"s1", L"typed by human"); // reconciler sees the same line -> skip
        const auto after = reg.Get(L"s1");
        CHECK(after && after->queue.size() == n0, "NoteExternalPrompt dedups vs an existing Sent");
        reg.NoteExternalPrompt(L"s1", L"a dropped-hook message"); // genuinely new -> recorded
        const auto after2 = reg.Get(L"s1");
        CHECK(after2 && after2->queue.size() == n0 + 1, "NoteExternalPrompt records a missed message");
        bool taggedTyped = false;
        if (after2)
        {
            for (const auto& p : after2->queue)
            {
                if (p.text == L"a dropped-hook message")
                {
                    taggedTyped = (p.origin == PromptOrigin::Typed && p.status == PromptStatus::Sent);
                }
            }
        }
        CHECK(taggedTyped, "reconciled prompt tagged Typed/Sent");
        reg.NoteExternalPrompt(L"s1", L""); // empty -> no-op
        const auto after3 = reg.Get(L"s1");
        CHECK(after3 && after3->queue.size() == n0 + 1, "NoteExternalPrompt ignores empty text");
    }

    // Missed/folded-UserPromptSubmit repair — the PURE gate (ShouldSynthesizeRunning), the Running
    // mirror of the missed-Stop synthesis: fires only off a freshly-appended turn event consumed by
    // an already-PRIMED cursor (the initial history replay never counts) whose tail says a turn is
    // in progress, and only out of the two states a missed prompt strands a session in.
    {
        CHECK(ShouldSynthesizeRunning(SessionState::WaitingForInput, true, true, L"", 500), "run-repair: fresh user line + Waiting -> synthesize");
        CHECK(ShouldSynthesizeRunning(SessionState::Idle, true, true, L"tool_use", 500), "run-repair: assistant mid-turn line + Idle -> synthesize");
        CHECK(ShouldSynthesizeRunning(SessionState::Idle, true, true, L"", -200), "run-repair: future mtime (clock skew) counts as fresh");
        CHECK(!ShouldSynthesizeRunning(SessionState::Running, true, true, L"", 500), "run-repair: already Running -> no-op");
        CHECK(!ShouldSynthesizeRunning(SessionState::NeedsApproval, true, true, L"", 500), "run-repair: NeedsApproval never cleared by a transcript line");
        // Agentmaster (API-error recovery — the PULL half of "come out of Error on first change"): a
        // fresh in-progress turn event after an API error IS the user retrying, so Error recovers to
        // Running. But a TERMINAL tail (the error line's own stop_sequence) must NOT self-recover, an
        // unprimed history replay must not, and a quiet pass (no new event) must keep it Error.
        CHECK(ShouldSynthesizeRunning(SessionState::Error, true, true, L"", 500), "run-repair: Error + fresh in-progress turn event -> recovers to Running");
        CHECK(!ShouldSynthesizeRunning(SessionState::Error, true, true, L"end_turn", 500), "run-repair: Error + terminal tail -> NOT Running (the error's own stop_reason must not self-recover)");
        CHECK(!ShouldSynthesizeRunning(SessionState::Error, false, true, L"", 500), "run-repair: Error + no new event this pass -> stays Error");
        CHECK(!ShouldSynthesizeRunning(SessionState::Error, true, false, L"", 500), "run-repair: Error + unprimed (history replay) -> no spurious recovery");
        CHECK(!ShouldSynthesizeRunning(SessionState::Done, true, true, L"", 500), "run-repair: Done never revived");
        CHECK(!ShouldSynthesizeRunning(SessionState::WaitingForInput, true, true, L"end_turn", 500), "run-repair: end_turn tail is missed-Stop territory, not Running");
        CHECK(!ShouldSynthesizeRunning(SessionState::WaitingForInput, false, true, L"", 500), "run-repair: no new turn event this pass -> no synthesis");
        CHECK(!ShouldSynthesizeRunning(SessionState::Idle, true, true, L"", kScanRunRepairFreshMs + 1), "run-repair: stale write (late scan) -> no synthesis");
        // THE window-restore bug: a session closed MID-TURN and resumed leaves a transcript whose
        // history ends "turn in progress" with a FRESH mtime; the first scanner pass replays it
        // from offset 0 (cursor not yet primed) — that replay must NOT light the idle, just-resumed
        // claude Running (it then STUCK blue: recon-stop needs a terminal-stop tail to clear it).
        CHECK(!ShouldSynthesizeRunning(SessionState::Idle, true, false, L"", 500), "run-repair: unprimed cursor (history replay) -> no synthesis even when FRESH");
        CHECK(!ShouldSynthesizeRunning(SessionState::Idle, true, false, L"tool_use", 500), "run-repair: unprimed replay of a mid-turn tail (killed mid-turn) -> no synthesis");
        CHECK(!ShouldSynthesizeRunning(SessionState::WaitingForInput, true, false, L"", -200), "run-repair: unprimed beats even a future mtime");
        // #3 (recon gates were end_turn-ONLY): every TERMINAL stop_reason ends the turn, not just
        // end_turn — a stop_sequence/max_tokens/refusal-ended turn previously read as "in
        // progress" here (wrongly re-lighting Running off its own tail) and never qualified for
        // the missed-Stop synthesis (stuck Running forever). An UNKNOWN reason stays in-flight —
        // the pre-existing default (everything != end_turn), e.g. pause_turn genuinely continues.
        CHECK(IsTerminalStopReason(L"end_turn") && IsTerminalStopReason(L"stop_sequence") && IsTerminalStopReason(L"max_tokens") && IsTerminalStopReason(L"refusal"), "stop-reason: all four terminal reasons recognized");
        CHECK(!IsTerminalStopReason(L"tool_use") && !IsTerminalStopReason(L"") && !IsTerminalStopReason(L"pause_turn"), "stop-reason: mid-turn / empty / unknown read as in-flight");
        CHECK(!ShouldSynthesizeRunning(SessionState::WaitingForInput, true, true, L"stop_sequence", 500), "run-repair: stop_sequence tail ended the turn -> not Running");
        CHECK(!ShouldSynthesizeRunning(SessionState::Idle, true, true, L"max_tokens", 500), "run-repair: max_tokens tail ended the turn -> not Running");
        CHECK(!ShouldSynthesizeRunning(SessionState::Idle, true, true, L"refusal", 500), "run-repair: refusal tail ended the turn -> not Running");
        CHECK(ShouldSynthesizeRunning(SessionState::Idle, true, true, L"pause_turn", 500), "run-repair: an unknown stop_reason stays in-flight (old default)");
    }
    // Agentmaster (API-error reconciliation — ShouldSynthesizeError): the tail is an unrecovered API
    // error (lastWasApiError) and the transcript has settled -> Error. Idempotent (never re-fires from
    // Error), never from Done, never without the error tail, and gated on the same quiescence as the
    // missed-Stop (a fast retry clears the tail first). Fires defensively from Waiting/Idle too (the
    // real Stop hook for the errored turn, or an earlier pass, may already have moved it there).
    {
        CHECK(ShouldSynthesizeError(SessionState::Running, true, kScanStopQuiescenceMs), "api-error: Running + error tail + quiet -> Error");
        CHECK(ShouldSynthesizeError(SessionState::WaitingForInput, true, kScanStopQuiescenceMs), "api-error: Waiting (push Stop landed) + error tail -> Error");
        CHECK(ShouldSynthesizeError(SessionState::Idle, true, kScanStopQuiescenceMs), "api-error: Idle + error tail -> Error (defensive)");
        CHECK(ShouldSynthesizeError(SessionState::NeedsApproval, true, kScanStopQuiescenceMs), "api-error: NeedsApproval + error tail -> Error");
        CHECK(!ShouldSynthesizeError(SessionState::Running, true, kScanStopQuiescenceMs - 1), "api-error: not quiet long enough -> wait (a fast retry clears the tail first)");
        CHECK(!ShouldSynthesizeError(SessionState::Running, false, kScanStopQuiescenceMs), "api-error: not the active leaf -> no Error");
        CHECK(!ShouldSynthesizeError(SessionState::Error, true, kScanStopQuiescenceMs), "api-error: already Error -> idempotent (never re-fires)");
        CHECK(!ShouldSynthesizeError(SessionState::Done, true, kScanStopQuiescenceMs), "api-error: a cleanly-ended (Done) session never flips to Error");
    }
    // Agentmaster (active-leaf distinction — ApiErrorIsActiveLeaf): the error is "the last message" while it
    // is positionally newest (lastWasApiError) AND the active leaf is the error, its UNMOVED anchor
    // (== errorEpochLeaf), or on its forward bookkeeping DESCENDANT chain (errorBranchUuids). A double-ESC
    // REWIND repoints the leaf to a uuid that is NONE of these — off the error branch — with NO turn event,
    // so the error is off the active branch even though it stays physically last. (Equality-with-epoch alone
    // was the ORIGINAL test; it missed the forward advance — the post-error last-prompt names the error's
    // turn_duration CHILD, not the pre-error anchor — so a real error never entered Error: the reported bug.)
    {
        const std::unordered_set<std::wstring> branch{ L"E", L"C" }; // {error uuid, its turn_duration child}
        const std::unordered_set<std::wstring> none{ L"E" }; // error seeded, no descendant marker yet
        // Normal error tail: leaf unchanged since the error -> the active leaf.
        CHECK(ApiErrorIsActiveLeaf(true, L"p", L"p", none), "leaf: error is the active leaf (leaf unmoved since the error)");
        // THE FIX — post-error bookkeeping: the last-prompt marker advances the leaf onto the error's OWN
        // turn_duration CHILD (a DESCENDANT, in the frontier), which is NOT a rewind -> still the active leaf.
        CHECK(ApiErrorIsActiveLeaf(true, L"C", L"p", branch),
              "leaf: forward advance onto the error's bookkeeping child (descendant) -> STILL the active leaf");
        // Rewind past the error: a later last-prompt repointed the leaf to an EARLIER prompt (off-branch).
        CHECK(!ApiErrorIsActiveLeaf(true, L"p0", L"p", branch), "leaf: rewind moved the leaf off the error branch -> not the active leaf");
        // No turn-positional error at all -> never the active leaf (a later turn event cleared it).
        CHECK(!ApiErrorIsActiveLeaf(false, L"p", L"p", none), "leaf: positional flag cleared (a turn event) -> not the active leaf");
        // No last-prompt marker ever seen (older/subagent transcript): leaf "" -> positional governs.
        CHECK(ApiErrorIsActiveLeaf(true, L"", L"", none), "leaf: no marker seen -> falls back to positional (empty leaf)");
        // The full ShouldSynthesizeError gate keys on the leaf-refined bool: a rewound error never enters
        // Error even when positionally newest + quiet; an unmoved OR descendant-advanced one DOES.
        CHECK(!ShouldSynthesizeError(SessionState::Running, ApiErrorIsActiveLeaf(true, L"p0", L"p", branch), kScanStopQuiescenceMs),
              "leaf: a rewound error (positionally newest but off-branch) does NOT enter Error");
        CHECK(ShouldSynthesizeError(SessionState::Running, ApiErrorIsActiveLeaf(true, L"C", L"p", branch), kScanStopQuiescenceMs),
              "leaf: a post-error bookkeeping advance (descendant) DOES enter Error (the reported bug, fixed)");
        CHECK(ShouldSynthesizeError(SessionState::Running, ApiErrorIsActiveLeaf(true, L"p", L"p", none), kScanStopQuiescenceMs),
              "leaf: an unmoved-leaf error DOES enter Error");
    }
    // Agentmaster (Error RELEASE on a leaf move — ShouldReleaseErrorOnLeafMove): a session stuck in Error
    // leaves when the leaf rewinds OFF the error branch WITHOUT a turn event (the pure rewind-and-sit).
    // Distinct from recon-run / push (which own the user-RETRIED case, lastWasApiError already cleared).
    {
        const std::unordered_set<std::wstring> branch{ L"E", L"C" };
        // The rewind case: in Error, error still positionally last, leaf moved OFF the branch + quiet -> release.
        CHECK(ShouldReleaseErrorOnLeafMove(SessionState::Error, true, L"p0", L"p", branch, kScanStopQuiescenceMs),
              "release: Error + rewind (leaf off the error branch) + quiet -> release to Waiting");
        // Not quiet yet (the rewind just wrote the marker) -> wait for the settle window.
        CHECK(!ShouldReleaseErrorOnLeafMove(SessionState::Error, true, L"p0", L"p", branch, kScanStopQuiescenceMs - 1),
              "release: not quiet long enough -> wait");
        // Leaf has NOT moved (error still the active leaf) -> stay Error.
        CHECK(!ShouldReleaseErrorOnLeafMove(SessionState::Error, true, L"p", L"p", branch, kScanStopQuiescenceMs),
              "release: error still the active leaf (unmoved) -> stay Error");
        // Leaf advanced onto the error's bookkeeping DESCENDANT -> NOT a rewind -> stay Error (the fix's
        // release-side guard: post-error bookkeeping must not bounce the session out of Error).
        CHECK(!ShouldReleaseErrorOnLeafMove(SessionState::Error, true, L"C", L"p", branch, kScanStopQuiescenceMs),
              "release: forward advance onto a descendant child is NOT a rewind -> stay Error");
        // Positional flag cleared (a turn event) -> recon-run / push owns recovery, not this path.
        CHECK(!ShouldReleaseErrorOnLeafMove(SessionState::Error, false, L"p0", L"p", branch, kScanStopQuiescenceMs),
              "release: positional cleared (turn event) -> recon-run/push owns it, not the leaf release");
        // Only from Error: a non-Error state has nothing to release here.
        CHECK(!ShouldReleaseErrorOnLeafMove(SessionState::Running, true, L"p0", L"p", branch, kScanStopQuiescenceMs),
              "release: only from Error (a Running session is not stuck)");
        CHECK(!ShouldReleaseErrorOnLeafMove(SessionState::WaitingForInput, true, L"p0", L"p", branch, kScanStopQuiescenceMs),
              "release: already left Error (Waiting) -> no-op (no re-fire)");
    }
    // The synthesized event's effect through the ONE state machine: UserPromptSubmit-shaped, ts
    // stamped (refreshes the decay anchor), EMPTY promptText (no Auto-Testing side effects — the
    // prompt back-fill stays NoteExternalPrompt's job).
    {
        SessionRegistry reg;
        reg.Upsert(MakeSession(L"run1", SessionState::WaitingForInput));
        HookMessage synth = Msg(L"run1", HookEvent::UserPromptSubmit);
        synth.ts = 777;
        reg.OnHookEvent(synth);
        const auto got = reg.Get(L"run1");
        CHECK(got && got->state == SessionState::Running, "synthesized UserPromptSubmit -> Running (one state machine)");
        CHECK(got && got->lastActivityUnixMs == 777, "synthesized ts stamps lastActivityUnixMs (decay anchor refreshed)");
        CHECK(got && got->queue.empty(), "empty promptText -> no Auto-Testing entry recorded");
    }
    // Agentmaster (API-error synth through the registry): the scanner's [recon-error] event (a
    // Notification carrying apiError) lands SessionState::Error, and the session then COMES OUT of Error
    // on the first new turn event (a UserPromptSubmit -> Running) — the full produce + recover cycle.
    {
        SessionRegistry reg;
        reg.Upsert(MakeSession(L"err1", SessionState::Running));
        HookMessage synthErr = Msg(L"err1", HookEvent::Notification);
        synthErr.apiError = true;
        synthErr.errorMessage = L"API Error: Server is temporarily limiting requests · Rate limited";
        synthErr.errorStatus = 429;
        synthErr.ts = 1000;
        reg.OnHookEvent(synthErr);
        const auto errored = reg.Get(L"err1");
        CHECK(errored && errored->state == SessionState::Error, "recon-error synth -> Error (through the one state machine)");
        // The reason is PRESERVED on the record for the Triage-Board Error card (message + HTTP code).
        CHECK(errored && errored->errorMessage.find(L"Rate limited") != std::wstring::npos && errored->errorStatus == 429,
              "Error preserves errorMessage + errorStatus for the card");
        // First change: the user retries. UserPromptSubmit -> Running (come out of Error on first change).
        HookMessage retry = Msg(L"err1", HookEvent::UserPromptSubmit);
        retry.ts = 2000;
        reg.OnHookEvent(retry);
        const auto recovered = reg.Get(L"err1");
        CHECK(recovered && recovered->state == SessionState::Running, "Error leaves on the first new turn event (UserPromptSubmit -> Running)");
        // Recovery CLEARS the preserved reason, so a recovered card never shows a stale error.
        CHECK(recovered && recovered->errorMessage.empty() && recovered->errorStatus == 0, "recovery clears errorMessage + errorStatus");
    }
    // Agentmaster (Error triage dismissal — the "Move to Idle/Done" offered on an Error card): the manual
    // demote's registry mutation (the SAME mutator both UI surfaces apply: Error -> Idle + the
    // errorDismissed ack the scanner's recon-error gate suppresses off + the preserved reason dropped, per
    // the "Empty/0 outside Error" invariant), and the ack's EXPIRY on a fresh Error entry — OnHookEvent's
    // apiError branch clears it, so a later, undismissed error is never mis-suppressed by a stale ack.
    {
        SessionRegistry reg;
        reg.Upsert(MakeSession(L"err2", SessionState::Running));
        HookMessage synthErr = Msg(L"err2", HookEvent::Notification);
        synthErr.apiError = true;
        synthErr.errorMessage = L"API Error: Overloaded";
        synthErr.errorStatus = 529;
        synthErr.ts = 1000;
        reg.OnHookEvent(synthErr);
        // The UI triage move's mutator (board/tree menu + tab menu): re-check under the lock, demote, ack.
        reg.Update(L"err2", [](SessionInfo& s) {
            if (s.state == SessionState::Error)
            {
                s.state = SessionState::Idle;
                s.manualUnread = false;
                s.errorDismissed = true;
                s.errorMessage.clear();
                s.errorStatus = 0;
            }
        });
        const auto dismissed = reg.Get(L"err2");
        CHECK(dismissed && dismissed->state == SessionState::Idle, "error dismissal: Error -> Idle (the manual Move to Idle/Done)");
        CHECK(dismissed && dismissed->errorDismissed, "error dismissal: the ack is recorded (recon-error suppresses off it)");
        CHECK(dismissed && dismissed->errorMessage.empty() && dismissed->errorStatus == 0, "error dismissal: the preserved reason drops (Empty/0 outside Error)");
        // A FRESH Error entry consumes the stale ack — the new error is a new fact the user hasn't acked.
        HookMessage nextErr = Msg(L"err2", HookEvent::Notification);
        nextErr.apiError = true;
        nextErr.errorMessage = L"API Error: Server is temporarily limiting requests · Rate limited";
        nextErr.errorStatus = 429;
        nextErr.ts = 2000;
        reg.OnHookEvent(nextErr);
        const auto reErrored = reg.Get(L"err2");
        CHECK(reErrored && reErrored->state == SessionState::Error, "a fresh apiError entry re-lands Error after a dismissal");
        CHECK(reErrored && !reErrored->errorDismissed, "a fresh Error entry clears the stale dismissal ack");
        CHECK(reErrored && reErrored->errorStatus == 429, "the fresh entry re-preserves the new failure reason");
    }
    // UpdateQuiet mutates the record (and, by contract, fires no observer — exercised here for the mutation)
    {
        SessionRegistry reg;
        int observed = 0;
        reg.AddObserver([&](const SessionInfo&, HookEvent) { ++observed; });
        reg.Upsert(MakeSession(L"s2")); // Upsert notifies -> observed == 1
        const int afterUpsert = observed;
        reg.UpdateQuiet(L"s2", [](SessionInfo& s) { s.lastAssistantText = L"peek"; });
        const auto s = reg.Get(L"s2");
        CHECK(s && s->lastAssistantText == L"peek", "UpdateQuiet mutates the record");
        CHECK(observed == afterUpsert, "UpdateQuiet fires NO observer (no persist/UI churn)");
    }
}

// Agentmaster — the CURRENT-MODEL adornment (the board card / per-tab overlay / tab tooltip model):
// ShortModelName + SessionDisplayModel (pure, SessionModels.h), the delta parser's assistant
// message.model capture (the SessionScanner's managed-session source), and the one-pass tail-facts
// extractor the Fleet Observer reads an EXTERNAL session's recap + current model from
// (TailFactsFromTranscriptChunk — RecapFromTranscriptChunk is now a view over it).
void TestCurrentModel()
{
    std::wprintf(L"Current-model adornment (ShortModelName / delta capture / tail facts):\n");

    // --- ShortModelName: every Anthropic id shape in the wild shortens; anything else passes through ---
    {
        CHECK(ShortModelName(L"claude-fable-5") == L"fable-5", "short: claude-fable-5 -> fable-5 (new date-less id)");
        CHECK(ShortModelName(L"claude-opus-4-8") == L"opus-4.8", "short: claude-opus-4-8 -> opus-4.8");
        CHECK(ShortModelName(L"claude-sonnet-5") == L"sonnet-5", "short: claude-sonnet-5 -> sonnet-5");
        CHECK(ShortModelName(L"claude-opus-4-6-20260105") == L"opus-4.6", "short: dated id -> opus-4.6 (the 8-digit date never counts as a version)");
        CHECK(ShortModelName(L"claude-haiku-4-5-20251001") == L"haiku-4.5", "short: claude-haiku-4-5-20251001 -> haiku-4.5");
        CHECK(ShortModelName(L"claude-opus-4-20250514") == L"opus-4", "short: a single version number keeps no dot");
        CHECK(ShortModelName(L"claude-3-7-sonnet-20250219") == L"sonnet-3.7", "short: OLD family-last id -> sonnet-3.7");
        CHECK(ShortModelName(L"claude-3-5-haiku-20241022") == L"haiku-3.5", "short: old id -> haiku-3.5");
        CHECK(ShortModelName(L"claude-3-opus-20240229") == L"opus-3", "short: old id -> opus-3");
        CHECK(ShortModelName(L"claude-instant-1.2") == L"instant-1.2", "short: a dotted version token is kept whole");
        CHECK(ShortModelName(L"claude-3-5-sonnet-latest") == L"sonnet-3.5", "short: the -latest suffix is stripped");
        CHECK(ShortModelName(L"us.anthropic.claude-sonnet-4-5-20250929-v1:0") == L"sonnet-4.5", "short: Bedrock prefix + -v1:0 suffix stripped");
        CHECK(ShortModelName(L"claude-3-5-sonnet-v2@20241022") == L"sonnet-3.5", "short: Vertex @date suffix stripped (the v2 marker drops)");
        // bare aliases users pass to --model (already short) come back unchanged
        CHECK(ShortModelName(L"opus") == L"opus", "short: a bare alias passes through");
        CHECK(ShortModelName(L"sonnet-5") == L"sonnet-5", "short: an alias with a version is already short");
        CHECK(ShortModelName(L"fable") == L"fable", "short: the fable alias passes through");
        CHECK(ShortModelName(L"sonnet-5[1m]") == L"sonnet-5[1m]", "short: a [1m] context marker is preserved");
        CHECK(ShortModelName(L"claude-sonnet-4-5[1m]") == L"sonnet-4.5[1m]", "short: full id + [1m] -> short + [1m]");
        // NON-Anthropic ids are NEVER mangled (a managed Codex's model rides the same display path)
        CHECK(ShortModelName(L"gpt-5.1-codex") == L"gpt-5.1-codex", "short: a Codex model passes through verbatim");
        CHECK(ShortModelName(L"o4-mini") == L"o4-mini", "short: an unknown family passes through verbatim");
        CHECK(ShortModelName(L"<synthetic>") == L"<synthetic>", "short: the API-error pseudo-model passes through (producers skip it upstream)");
        CHECK(ShortModelName(L"").empty(), "short: empty -> empty");
        CHECK(ShortModelName(L"  claude-fable-5  ") == L"fable-5", "short: surrounding whitespace is trimmed");
        CHECK(ShortModelName(L"claude") == L"claude", "short: a bare 'claude' has nothing to shorten");
        CHECK(ShortModelName(L"claude-2") == L"claude-2", "short: a familyless claude-2 keeps its shape");
    }

    // --- SessionDisplayModel: the transcript truth wins; the launch request is the fallback ---
    {
        SessionInfo s = MakeSession(L"m1");
        CHECK(SessionDisplayModel(s).empty(), "display: neither known -> empty (never-prompted bare launch)");
        s.model = L"opus"; // the --model launch request (the observer's cmdline read)
        CHECK(SessionDisplayModel(s) == L"opus", "display: the launch request alone -> shown");
        s.currentModel = L"claude-fable-5"; // the transcript truth (e.g. after a /model switch)
        CHECK(SessionDisplayModel(s) == L"claude-fable-5", "display: the CURRENT model wins over the launch request");
    }

    // --- The CONFIGURABLE family list (AppSettings::modelFamilies -> ParseModelFamilies ->
    // ShortModelName's bare-alias gate; a full "claude-…" id never consults it) ---
    {
        const auto f = ParseModelFamilies(L"Opus, sonnet;  ZEPHYR\nfable\tfable");
        CHECK(f.size() == 4 && f[0] == L"opus" && f[1] == L"sonnet" && f[2] == L"zephyr" && f[3] == L"fable",
              "families: split on comma/semicolon/whitespace, lowercased, deduped, order kept");
        CHECK(ParseModelFamilies(L"").empty(), "families: empty csv -> empty vector (display falls back to built-ins)");
        CHECK(ParseModelFamilies(L" ,;  \n").empty(), "families: separator-only csv -> empty vector");
        CHECK(DefaultModelFamilies().size() == 6 && DefaultModelFamilies()[0] == L"opus",
              "families: the built-ins parse from kDefaultModelFamilies (opus/sonnet/haiku/fable/mythos/instant)");
        // a custom list REPLACES the built-ins for BARE aliases — how a NEW family ships without code
        const auto zeph = ParseModelFamilies(L"zephyr");
        CHECK(ShortModelName(L"Zephyr-3-1", zeph) == L"zephyr-3.1", "families: a configured NEW family shortens a bare alias");
        CHECK(ShortModelName(L"fable-5", zeph) == L"fable-5", "families: an unlisted bare alias passes through (identity here)");
        CHECK(ShortModelName(L"FABLE-5-20260101", zeph) == L"FABLE-5-20260101", "families: an unlisted alias is never normalized (case/date kept verbatim)");
        CHECK(ShortModelName(L"claude-zephyr-3-1") == L"zephyr-3.1", "families: a claude-prefixed id shortens on ANY family — the list is bare-alias-only");
        CHECK(ShortModelName(L"claude-fable-5", zeph) == L"fable-5", "families: a claude-prefixed id ignores the custom list too");
        CHECK(ShortModelName(L"fable-5", {}) == L"fable-5", "families: an empty list -> the built-ins govern (the cleared-box self-heal)");
        // AppSettings persistence: the csv round-trips; an ABSENT key seeds the shipped default
        // (presence-gated like launchModels); a PRESENT empty value is kept verbatim.
        AppSettings as;
        as.modelFamilies = L"opus, zephyr";
        CHECK(AppSettingsFromJson(ToJson(as)).modelFamilies == L"opus, zephyr", "families: modelFamilies round-trips settings.json");
        CHECK(AppSettingsFromJson(json::Value::MkObj()).modelFamilies == std::wstring{ kDefaultModelFamilies },
              "families: an ABSENT key seeds the shipped default list (the cog box shows it, ready to extend)");
        {
            // A fresh object (json::Value::Set APPENDS — re-setting a key on ToJson's output would
            // leave the original entry first, and Find/StrAt read the FIRST match).
            auto j = json::Value::MkObj();
            j.Set(L"modelFamilies", json::Value::MkStr(L""));
            CHECK(AppSettingsFromJson(j).modelFamilies.empty(), "families: a PRESENT empty value is kept (display then uses the built-ins)");
        }
    }

    // --- ParseTranscriptDelta: the assistant line's message.model is captured onto the event ---
    {
        const std::wstring line = LR"j({"type":"assistant","message":{"model":"claude-fable-5","stop_reason":"end_turn","content":[{"type":"text","text":"hi"}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].model == L"claude-fable-5", "delta: assistant message.model captured");
    }
    {
        const std::wstring line = LR"j({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"hi"}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].model.empty(), "delta: a model-less assistant line -> empty (never invents one)");
    }
    {
        // The synthetic API-error line carries "<synthetic>" — the parser extracts it as-is (a plain
        // extractor); the scanner FOLD skips it (the apiError flag + the literal), so it can never
        // become SessionInfo.currentModel.
        const std::wstring line = LR"j({"type":"assistant","isApiErrorMessage":true,"message":{"model":"<synthetic>","stop_reason":"stop_sequence","content":[{"type":"text","text":"API Error: Overloaded"}]}})j" L"\n";
        const auto r = ParseTranscriptDelta(line);
        CHECK(r.events.size() == 1 && r.events[0].model == L"<synthetic>" && r.events[0].apiError, "delta: the error line's pseudo-model rides WITH the apiError flag (the fold's skip signal)");
    }

    // --- TailFactsFromTranscriptChunk: ONE pass -> recap + current model (the observer's external read) ---
    {
        const std::wstring chunk =
            std::wstring{ LR"j({"type":"assistant","message":{"model":"claude-opus-4-8","content":[{"type":"text","text":"a"}]}})j" } + L"\n" +
            LR"j({"type":"system","subtype":"away_summary","content":"Shipped. (disable recaps in /config)"})j" + L"\n" +
            LR"j({"type":"assistant","message":{"model":"claude-fable-5","content":[{"type":"text","text":"b"}]}})j" + L"\n";
        const auto tf = TailFactsFromTranscriptChunk(chunk);
        CHECK(tf.model == L"claude-fable-5", "tail: the LAST real assistant line's model wins (a /model switch supersedes)");
        CHECK(tf.recap == L"Shipped.", "tail: the recap rides the same pass (normalized)");
    }
    {
        const std::wstring chunk =
            std::wstring{ LR"j({"type":"assistant","message":{"model":"claude-fable-5","content":[{"type":"text","text":"real"}]}})j" } + L"\n" +
            LR"j({"type":"assistant","isApiErrorMessage":true,"message":{"model":"<synthetic>","stop_reason":"stop_sequence","content":[{"type":"text","text":"API Error"}]}})j" + L"\n";
        CHECK(TailFactsFromTranscriptChunk(chunk).model == L"claude-fable-5", "tail: a trailing API-error line's <synthetic> never shadows the real model");
    }
    {
        const std::wstring chunk =
            std::wstring{ LR"j({"type":"assistant","message":{"model":"claude-fable-5","content":[{"type":"text","text":"parent"}]}})j" } + L"\n" +
            LR"j({"type":"assistant","isSidechain":true,"message":{"model":"claude-haiku-4-5-20251001","content":[{"type":"text","text":"subagent"}]}})j" + L"\n";
        CHECK(TailFactsFromTranscriptChunk(chunk).model == L"claude-fable-5", "tail: an inlined subagent (isSidechain) line's model never shadows the session's");
    }
    {
        const std::wstring chunk = LR"j({"type":"user","message":{"content":"just me"}})j" L"\n";
        const auto tf = TailFactsFromTranscriptChunk(chunk);
        CHECK(tf.model.empty() && tf.recap.empty(), "tail: an assistant-less chunk yields empty facts (so 'empty never clears' holds at the caller)");
    }
    {
        // A partial LEADING line (a tail read starting mid-line) is skipped; the complete line after it lands.
        const std::wstring chunk = LR"j(odel":"claude-x"}})j" L"\n" LR"j({"type":"assistant","message":{"model":"claude-sonnet-5","content":[{"type":"text","text":"ok"}]}})j" L"\n";
        CHECK(TailFactsFromTranscriptChunk(chunk).model == L"claude-sonnet-5", "tail: a partial leading line is skipped; the complete assistant line is read");
    }
    // RecapFromTranscriptChunk stays the recap-only VIEW over the same pass (one implementation).
    {
        const std::wstring chunk = LR"j({"type":"system","subtype":"away_summary","content":"r1"})j" L"\n";
        CHECK(RecapFromTranscriptChunk(chunk) == TailFactsFromTranscriptChunk(chunk).recap, "tail: RecapFromTranscriptChunk == TailFacts.recap (delegation, no drift)");
    }
}

// Agentmaster — the stuck-state reconcilers (proved against 4 live Desktop sessions):
//   #1 an unanswered AskUserQuestion left the session BLOCKED on the user but showing Running
//      forever (a "tool_use" tail is non-terminal, so the missed-Stop backstop never fired);
//   #2 an INTERRUPTED turn (Esc) showed Running forever (no clean Stop hook, and the interrupt
//      marker cleared the stop_reason, disarming the backstop);
//   and the original report: a NeedsApproval whose post-approval Stop hook was DROPPED stayed
//   NeedsApproval (the missed-Stop backstop was Running-ONLY). All three now reconcile.
void TestBlockedAndInterruptedStates()
{
    std::wprintf(L"Blocked-on-user / interrupted / needs-approval-exit reconcilers:\n");

    // --- pure decision helpers ---
    CHECK(IsUserInterruptMarker(L"[Request interrupted by user for tool use]"), "interrupt: tool-use variant recognized");
    CHECK(IsUserInterruptMarker(L"[Request interrupted by user]"), "interrupt: bare variant recognized");
    CHECK(!IsUserInterruptMarker(L"please don't interrupt me"), "interrupt: ordinary prompt is not a marker");
    CHECK(IsInteractiveTool(L"AskUserQuestion"), "interactive: AskUserQuestion blocks on the user");
    CHECK(!IsInteractiveTool(L"Bash") && !IsInteractiveTool(L""), "interactive: Bash / none do not block");

    // recon-stop: fires from Running OR NeedsApproval, on a terminal stop_reason OR an interrupt,
    // only once quiescent.
    CHECK(ShouldSynthesizeStop(SessionState::Running, L"end_turn", false, 5000), "stop: Running + terminal tail + quiet -> stop");
    CHECK(ShouldSynthesizeStop(SessionState::Running, L"", true, 5000), "stop: Running + interrupt + quiet -> stop (#2 fix)");
    CHECK(ShouldSynthesizeStop(SessionState::NeedsApproval, L"end_turn", false, 5000), "stop: NeedsApproval + terminal tail -> stop (original fix)");
    CHECK(!ShouldSynthesizeStop(SessionState::Running, L"tool_use", false, 5000), "stop: a pending tool_use tail is NOT the turn's end");
    CHECK(!ShouldSynthesizeStop(SessionState::Running, L"end_turn", false, 1000), "stop: not quiescent yet -> hold");
    CHECK(!ShouldSynthesizeStop(SessionState::Idle, L"end_turn", false, 5000), "stop: an Idle session has no turn to end");
    CHECK(!ShouldSynthesizeStop(SessionState::WaitingForInput, L"end_turn", false, 5000), "stop: already settled -> no-op");

    // recon-block: an unanswered interactive tool_use, only from Running, only once quiescent.
    CHECK(ShouldSynthesizeBlockedOnUser(SessionState::Running, L"AskUserQuestion", false, 5000), "block: Running + unanswered question + quiet -> NeedsApproval (#1 fix)");
    CHECK(!ShouldSynthesizeBlockedOnUser(SessionState::Running, L"", false, 5000), "block: no pending interactive tool -> no-op");
    CHECK(!ShouldSynthesizeBlockedOnUser(SessionState::Running, L"Bash", false, 5000), "block: a pending Bash is WORKING, not blocked");
    CHECK(!ShouldSynthesizeBlockedOnUser(SessionState::Running, L"AskUserQuestion", true, 5000), "block: an interrupt takes precedence (-> stop)");
    CHECK(!ShouldSynthesizeBlockedOnUser(SessionState::NeedsApproval, L"AskUserQuestion", false, 5000), "block: idempotent — never re-fires while already NeedsApproval");
    CHECK(!ShouldSynthesizeBlockedOnUser(SessionState::Running, L"AskUserQuestion", false, 1000), "block: not quiescent yet -> hold");

    // recon-resume: the NeedsApproval -> Running edge that was MISSING — a session answered MID-turn
    // (the question/approval resolved + the agent working again) used to show "needs you" (orange)
    // until end-of-turn. Fires only from NeedsApproval, off a fresh primed turn event, with the
    // pending interactive tool cleared and a non-terminal/non-interrupted tail (mutually exclusive
    // with recon-stop). Signature: (state, consumedTurnEvent, primedBeforePass, pendingTool, lastStop, interrupted, sinceWriteMs).
    CHECK(ShouldSynthesizeResumed(SessionState::NeedsApproval, true, true, L"", L"tool_use", false, 500), "resume: answered + working again (pending tool) -> Running");
    CHECK(ShouldSynthesizeResumed(SessionState::NeedsApproval, true, true, L"", L"", false, 500), "resume: answered + plain assistant text (no stop_reason) -> Running");
    CHECK(!ShouldSynthesizeResumed(SessionState::NeedsApproval, true, true, L"AskUserQuestion", L"tool_use", false, 500), "resume: a NEW unanswered question still pending -> stay NeedsApproval");
    CHECK(!ShouldSynthesizeResumed(SessionState::NeedsApproval, false, true, L"", L"tool_use", false, 500), "resume: no new turn event this pass -> no synthesis");
    CHECK(!ShouldSynthesizeResumed(SessionState::NeedsApproval, true, false, L"", L"tool_use", false, 500), "resume: unprimed cursor (history replay) -> no synthesis");
    CHECK(!ShouldSynthesizeResumed(SessionState::NeedsApproval, true, true, L"", L"end_turn", false, 500), "resume: terminal tail = turn ended -> recon-stop's job (Waiting), not Running");
    CHECK(!ShouldSynthesizeResumed(SessionState::NeedsApproval, true, true, L"", L"", true, 500), "resume: an interrupt ended the turn -> recon-stop (Waiting), not a resume");
    CHECK(!ShouldSynthesizeResumed(SessionState::NeedsApproval, true, true, L"", L"tool_use", false, kScanRunRepairFreshMs + 1), "resume: stale write (late scan) -> no synthesis");
    CHECK(!ShouldSynthesizeResumed(SessionState::Running, true, true, L"", L"tool_use", false, 500), "resume: already Running -> no-op");
    CHECK(!ShouldSynthesizeResumed(SessionState::Idle, true, true, L"", L"tool_use", false, 500), "resume: Idle is recon-run's domain, not resume");
    CHECK(!ShouldSynthesizeResumed(SessionState::WaitingForInput, true, true, L"", L"tool_use", false, 500), "resume: Waiting is recon-run's domain, not resume");

    // --- Subagent/fork activity: presence "busy" + external-work Running promotion (the recon-subagent gate) ---
    CHECK(PresenceIsBusy(L"busy"), "presence: 'busy' == working");
    CHECK(!PresenceIsBusy(L"idle"), "presence: 'idle' is not working");
    CHECK(!PresenceIsBusy(L"waiting"), "presence: 'waiting' (for the user) is not working");
    CHECK(!PresenceIsBusy(L"shell"), "presence: 'shell' is not working");
    CHECK(!PresenceIsBusy(L""), "presence: no heartbeat is not working");
    // subagentActive arm — a subagent transcript is actively growing (the parent's tail is the pending Task tool_use, non-terminal):
    CHECK(ShouldSynthesizeRunningFromExternalWork(SessionState::Idle, true, false, L"", false), "ext-work: Idle + subagent writing (no parent stop_reason) -> Running");
    CHECK(ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, true, false, L"tool_use", false), "ext-work: subagent writing + in-flight (tool_use) tail -> Running");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, true, false, L"end_turn", false), "ext-work: turn ENDED (terminal tail) — a subagent's final write lands us before end_turn, so 'fresh subagent' here is the just-finished turn, NOT new work: do NOT bounce Waiting->Running");
    // presenceBusy arm — gated on a NON-terminal tail (no post-Stop flicker):
    CHECK(ShouldSynthesizeRunningFromExternalWork(SessionState::Idle, false, true, L"", false), "ext-work: Idle + presence busy + in-flight tail -> Running (e.g. a freshly /fork'd conversation)");
    CHECK(ShouldSynthesizeRunningFromExternalWork(SessionState::Idle, false, true, L"tool_use", false), "ext-work: presence busy + mid-turn tail -> Running");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, false, true, L"end_turn", false), "ext-work: stale 'busy' right after a real Stop (terminal tail) does NOT bounce Waiting back to Running");
    // interrupt guard (the Running<->Waiting oscillation fix) — an INTERRUPTED turn is recon-stop's job
    // (-> Waiting); after an Esc the dying subagents keep flushing side files + the heartbeat lingers
    // "busy", so promoting here would flip-flop with ShouldSynthesizeStop every tick (the field freeze that
    // flooded hooks.log to ~291 MB). The latch self-clears on a real resume (parser, fresh-append), so this
    // only suppresses while the tail IS still the interrupt marker:
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, true, false, L"", true), "ext-work: INTERRUPTED + subagent still flushing -> do NOT promote Waiting->Running (recon-stop owns the interrupt; else oscillation)");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::Idle, false, true, L"", true), "ext-work: INTERRUPTED + lingering 'busy' heartbeat -> no promotion (the turn was aborted, not resumed)");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, true, true, L"tool_use", true), "ext-work: INTERRUPTED wins even over a non-terminal tool_use tail with both activity signals set");
    CHECK(ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, true, false, L"", false), "ext-work: NON-interrupted subagent work still promotes (the latch self-clears on a real resume)");
    // state gate — only Idle/Waiting are repairable:
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::Running, true, true, L"tool_use", false), "ext-work: already Running -> no-op");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::NeedsApproval, true, true, L"", false), "ext-work: NeedsApproval ('needs you') never cleared by activity");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::Error, true, true, L"", false), "ext-work: Error never cleared by inference");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::Done, true, true, L"", false), "ext-work: Done never revived");
    // neither signal -> unchanged (a genuinely idle session):
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::Idle, false, false, L"tool_use", false), "ext-work: no subagent + not busy -> no synthesis (parent-only path owns it)");

    // --- work that OUTLIVES the turn (teammates / background agents / live shells): "a shell or agent
    //     or teammate still running means Running, not idle/done/waiting-for-you". The 6th arg is the
    //     caller's kScanExternalWorkGraceMs proof that the external activity postdates the parent's
    //     last write by more than the grace margin — ONLY that proof may promote past a terminal /
    //     interrupted tail; margin-less residue (the cases above) keeps the classic settled behavior.
    CHECK(ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, true, false, L"end_turn", false, true), "outlive: teammate/agent side-files written LONG after end_turn promote Waiting -> Running");
    CHECK(ShouldSynthesizeRunningFromExternalWork(SessionState::Idle, false, true, L"end_turn", false, true), "outlive: presence 'shell'/'busy' persisting long past turn-end promotes Idle -> Running (a live shell job)");
    CHECK(ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, true, false, L"", true, true), "outlive: an INTERRUPTED lead whose background team kept working past the abort still promotes (Esc kills the turn, not the teammates)");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, true, true, L"end_turn", false, false), "outlive: terminal tail WITHOUT the outlive proof is turn-tail residue -> stay settled (byte-identical classic behavior)");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::NeedsApproval, true, true, L"end_turn", false, true), "outlive: NeedsApproval ('needs you') is never cleared, even by outliving work");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::Running, true, true, L"end_turn", false, true), "outlive: already Running -> no-op");
    CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::Done, true, true, L"end_turn", false, true), "outlive: Done (claude exited) never revived — in-process teammates die with the process");
    // ... and the recon-stop HOLD mirror (ShouldSynthesizeStop's 5th arg): while the SAME expression is
    // true, a Running session is NOT demoted to WaitingForInput on its terminal tail — mutual exclusion
    // by construction, so promotion/demotion can never oscillate. Scoped to Running: the NeedsApproval
    // release (answered -> Waiting) still fires, and the promotion re-lights Running from Waiting.
    CHECK(!ShouldSynthesizeStop(SessionState::Running, L"end_turn", false, 5000, true), "outlive-hold: Running + terminal tail + quiet BUT external work outlives the turn -> HOLD Running (teammates/agent/shell)");
    CHECK(!ShouldSynthesizeStop(SessionState::Running, L"", true, 5000, true), "outlive-hold: interrupted tail also held while background work outlives the abort");
    CHECK(ShouldSynthesizeStop(SessionState::NeedsApproval, L"end_turn", false, 5000, true), "outlive-hold: NeedsApproval release is NOT held (answered -> Waiting; the promotion re-lights Running next pass)");
    CHECK(ShouldSynthesizeStop(SessionState::Running, L"end_turn", false, 5000, false), "outlive-hold: no external work -> the classic missed-Stop fires unchanged");
    // PresenceIsWorking — the scanner's activity signal: a turn in flight OR a live shell job.
    CHECK(PresenceIsWorking(L"busy"), "presence-working: 'busy' (a turn in flight)");
    CHECK(PresenceIsWorking(L"shell"), "presence-working: 'shell' (a live shell job claude owns) counts as working");
    CHECK(!PresenceIsWorking(L"idle"), "presence-working: 'idle' is at rest");
    CHECK(!PresenceIsWorking(L"waiting"), "presence-working: 'waiting' (needs-you) is not working");
    CHECK(!PresenceIsWorking(L""), "presence-working: no heartbeat is not working");
    static_assert(kScanExternalWorkGraceMs > kScanSubagentFreshMs, "outlive margin must exceed the dying-subagent flush window");

    // Toast HOLD gates — the NOTIFICATION mirror of the outlive pair. A completion toast fires on
    // the Running -> X push edge BEFORE the outlived-turn promotion can veto the transition (its
    // presence arm needs the transcript quiet past kScanExternalWorkGraceMs), and a shown toast
    // can't be recalled — proven live on 513d1366: "waiting for you" toasted at the Stop edge, the
    // presence=shell promotion re-lit Running 20s later. So the hosting window HOLDS Idle/Waiting
    // toasts while the external-work signal is live, DROPS them on a Running re-light, and FIRES
    // when the signal clears / a hard needs-you state lands / the cap backstop elapses.
    CHECK(ShouldHoldCompletionToast(SessionState::WaitingForInput, true, false), "toast-hold: Waiting + working presence (busy linger or a live shell job) holds");
    CHECK(ShouldHoldCompletionToast(SessionState::Idle, false, true), "toast-hold: Idle + fresh side files (a background agent/teammate) holds");
    CHECK(!ShouldHoldCompletionToast(SessionState::WaitingForInput, false, false), "toast-hold: no signal -> fire immediately (the classic path, unchanged)");
    CHECK(!ShouldHoldCompletionToast(SessionState::NeedsApproval, true, true), "toast-hold: NeedsApproval never holds (external work can't answer a question)");
    CHECK(!ShouldHoldCompletionToast(SessionState::Error, true, true), "toast-hold: Error never holds");
    CHECK(!ShouldHoldCompletionToast(SessionState::Done, true, true), "toast-hold: Done (claude exited) never holds");
    CHECK(DecideHeldToast(SessionState::Running, true, true, 5000) == HeldToastVerdict::Drop, "held-toast: re-lit Running -> DROP (the promotion confirmed the completion spurious)");
    CHECK(DecideHeldToast(SessionState::WaitingForInput, false, true, 5000) == HeldToastVerdict::Drop, "held-toast: archived mid-hold -> DROP (no deferred toast off a previous life)");
    CHECK(DecideHeldToast(SessionState::WaitingForInput, true, false, 5000) == HeldToastVerdict::Fire, "held-toast: signal cleared while still at rest -> FIRE (the busy-linger case, one sweep tick late)");
    CHECK(DecideHeldToast(SessionState::NeedsApproval, true, true, 5000) == HeldToastVerdict::Fire, "held-toast: moved to NeedsApproval mid-hold -> FIRE now (genuinely needs you, work or not)");
    CHECK(DecideHeldToast(SessionState::WaitingForInput, true, true, 5000) == HeldToastVerdict::Keep, "held-toast: signal still live inside the cap -> KEEP holding");
    CHECK(DecideHeldToast(SessionState::WaitingForInput, true, true, kNotifyExternalHoldCapMs) == HeldToastVerdict::Fire, "held-toast: the cap backstop fires even with the signal still present");
    CHECK(ShouldSuppressDuplicateToast(100000, 90000), "toast-dedupe: a second toast within the 20s window is suppressed (the W->R->W flap)");
    CHECK(!ShouldSuppressDuplicateToast(100000, 100000 - kNotifyDuplicateToastMs), "toast-dedupe: exactly at the window boundary fires");
    CHECK(!ShouldSuppressDuplicateToast(100000, 0), "toast-dedupe: never-shown fires");
    static_assert(kNotifyExternalHoldCapMs > kScanExternalWorkGraceMs + 2 * kScanSweepMs, "the hold cap must outlast the outlived-turn promotion latency (grace + sweep ticks), else the backstop fires the spurious toast right before the promotion drops it");

    // --- presence-IDLE release: claude's OWN heartbeat says "idle" while we are stuck Running on a
    //     NON-terminal tail (a trailing user prompt that produced no assistant output + a dropped/absent
    //     Stop). The IDLE mirror of PresenceIsBusy; it covers the exact gap ShouldSynthesizeStop cannot
    //     (no terminal stop_reason, no interrupt). PresenceIsAtRest is deliberately "idle"-only.
    CHECK(PresenceIsAtRest(L"idle"), "presence-rest: 'idle' == at rest");
    CHECK(!PresenceIsAtRest(L"busy"), "presence-rest: 'busy' is working, not at rest");
    CHECK(!PresenceIsAtRest(L"waiting"), "presence-rest: 'waiting' (needs-you) left to recon-block, not released here");
    CHECK(!PresenceIsAtRest(L"shell"), "presence-rest: 'shell' is not a claude turn-rest signal");
    CHECK(!PresenceIsAtRest(L""), "presence-rest: no heartbeat is not 'at rest'");
    // the no-op / stuck-Running case: Running + 'idle' heartbeat + cleared (non-terminal) tail, quiet for
    // the LONG floor -> release to WaitingForInput. Signature: (state, presence, lastStop, interrupted, quietForMs).
    CHECK(ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"idle", L"", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: Running + idle + cleared tail + quiet for the LONG floor -> stop (the no-op / stuck-Running case)");
    // Agentmaster (idle<->running flap fix): a Running cleared tail quiet for only the SHORT base window is
    // NOT released — that shape is indistinguishable from a turn paused behind a "No response from API ·
    // Retrying" backoff / a slow first token, and releasing at 5s oscillated Running<->WaitingForInput
    // against recon-run every scan (the reported "card bg" flap + "jumps to idle while the API retries").
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"idle", L"", false, kScanPresenceIdleQuiescenceMs), "presence-idle (flap fix): Running + idle + cleared tail quiet only the SHORT 5s window -> HOLD (a transient API-retry / stream pause, not a finished turn)");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"idle", L"", false, kScanPresenceIdleRunningQuiescenceMs - 1), "presence-idle: Running just under the long floor -> hold");
    CHECK(ShouldSynthesizeStopFromPresenceIdle(SessionState::NeedsApproval, L"idle", L"tool_use", false, kScanPresenceIdleQuiescenceMs), "presence-idle: a NeedsApproval stranded by a dropped post-answer Stop is released at the SHORT base floor (no API-retry ambiguity)");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::NeedsApproval, L"idle", L"tool_use", false, kScanPresenceIdleQuiescenceMs - 1), "presence-idle: NeedsApproval not quiet long enough -> hold");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"busy", L"", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: heartbeat 'busy' -> never release");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"", L"", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: no heartbeat -> no signal, no release");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"waiting", L"", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: 'waiting' is not released here (recon-block owns needs-you)");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"idle", L"end_turn", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: a TERMINAL tail is plain recon-stop's job (mutually exclusive)");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"idle", L"", true, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: an interrupt is plain recon-stop's job (mutually exclusive)");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Idle, L"idle", L"", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: an Idle session has no turn to end");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::WaitingForInput, L"idle", L"", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: already settled -> no-op");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Done, L"idle", L"", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: Done is never re-ended/revived");
    // Agentmaster (idle<->running flap fix): a RUNNING session whose tail is a PENDING tool_use is
    // mid-turn — claude is running a tool (a long Bash/build) or waiting/retrying the next API call
    // ("No response from API · Retrying in …"), during which it is not generating so its heartbeat
    // reads "idle" and the transcript sits quiet. Releasing it would flap Running<->Waiting against
    // recon-run every scan (the "card bg" report). It must NOT release regardless of how long quiet;
    // only the cleared-tail no-op turn (above), and only past the long floor, releases for Running.
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"idle", L"tool_use", false, kScanPresenceIdleRunningQuiescenceMs), "presence-idle: Running + idle + PENDING tool_use tail (mid-tool / API-retry backoff) -> NOT released (would flap against recon-run)");
    CHECK(!ShouldSynthesizeStopFromPresenceIdle(SessionState::Running, L"idle", L"tool_use", false, kScanPresenceIdleRunningQuiescenceMs * 100), "presence-idle: a Running pending tool_use stays held no matter how long quiet (a long Bash/build or multi-minute API retry is still the same turn)");

    // --- Waiting-for-you "unread" decay gate (ShouldDecayWaitingToIdle) ---
    // A WaitingForInput card demotes to Idle ONLY once the timeout has elapsed AND it has been READ
    // (readUnixMs >= lastActivity). It waits the FULL timeout regardless of reading, and past the
    // timeout it keeps waiting while still unread. A manual Mark Unread (or a busy heartbeat) never
    // time-decays; 0 minutes == never. Signature: (state, lastActivityMs, readUnixMs, manualUnread,
    // presenceBusy, minutes, nowMs).
    {
        using S = SessionState;
        constexpr uint32_t kMin = 60; // 1h timeout
        constexpr int64_t kTimeout = 60LL * 60000; // 3,600,000 ms
        constexpr int64_t last = 1'000'000; // last activity anchor
        constexpr int64_t pastNow = last + kTimeout; // exactly at the timeout edge
        constexpr int64_t withinNow = last + kTimeout - 1; // 1ms inside the window
        // within the timeout: ALWAYS waits, even when already read
        CHECK(!ShouldDecayWaitingToIdle(S::WaitingForInput, last, last, false, false, kMin, withinNow), "decay-waiting: within the timeout -> waits the full window (even when read)");
        // past the timeout + read -> decays
        CHECK(ShouldDecayWaitingToIdle(S::WaitingForInput, last, last, false, false, kMin, pastNow), "decay-waiting: past timeout + read (readUnixMs == lastActivity) -> Idle");
        CHECK(ShouldDecayWaitingToIdle(S::WaitingForInput, last, last + 5, false, false, kMin, pastNow + 1000), "decay-waiting: past timeout + read AFTER the activity -> Idle");
        // past the timeout but UNREAD -> keeps waiting until read
        CHECK(!ShouldDecayWaitingToIdle(S::WaitingForInput, last, last - 1, false, false, kMin, pastNow), "decay-waiting: past timeout but UNREAD (readUnixMs < lastActivity) -> keep waiting until read");
        CHECK(!ShouldDecayWaitingToIdle(S::WaitingForInput, last, 0, false, false, kMin, pastNow + 999999), "decay-waiting: never read (readUnixMs 0) -> never decays however long");
        // manual Mark Unread is sticky — never time-decays even past timeout + read
        CHECK(!ShouldDecayWaitingToIdle(S::WaitingForInput, last, last, true, false, kMin, pastNow), "decay-waiting: manualUnread sticky -> never time-decays");
        // a busy heartbeat (a long Task/Agent subagent) holds it out of Idle
        CHECK(!ShouldDecayWaitingToIdle(S::WaitingForInput, last, last, false, true, kMin, pastNow), "decay-waiting: presence 'busy' (subagent) -> never raced to Idle");
        // 0 minutes == never; wrong state / no anchor are no-ops
        CHECK(!ShouldDecayWaitingToIdle(S::WaitingForInput, last, last, false, false, 0, pastNow), "decay-waiting: 0 minutes == never decay");
        CHECK(!ShouldDecayWaitingToIdle(S::Running, last, last, false, false, kMin, pastNow), "decay-waiting: not WaitingForInput -> no-op");
        CHECK(!ShouldDecayWaitingToIdle(S::WaitingForInput, 0, last, false, false, kMin, pastNow), "decay-waiting: no activity anchor (lastActivity 0) -> no-op");
    }

    // --- ParseTranscriptDelta now surfaces the interactive tool name + a ToolResult marker ---
    {
        const auto p = ParseTranscriptDelta(
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"text\",\"text\":\"hold on\"},{\"type\":\"tool_use\",\"name\":\"AskUserQuestion\",\"input\":{}}]}}\n");
        CHECK(p.events.size() == 1 && p.events[0].kind == TranscriptEvent::Kind::Assistant, "parse: assistant tool_use line");
        CHECK(p.events[0].toolName == L"AskUserQuestion", "parse: interactive tool name surfaced");
        CHECK(p.events[0].text == L"hold on", "parse: text alongside the tool_use still collected");
    }
    {
        const auto p = ParseTranscriptDelta(
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"Bash\",\"input\":{}}]}}\n");
        CHECK(p.events.size() == 1 && p.events[0].toolName.empty(), "parse: a NON-interactive tool_use sets no toolName");
    }
    {
        const auto p = ParseTranscriptDelta(
            L"{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"content\":\"ok\"}]}}\n");
        CHECK(p.events.size() == 1 && p.events[0].kind == TranscriptEvent::Kind::ToolResult, "parse: tool_result -> ToolResult marker (not a typed prompt)");
    }

    // --- end-to-end: derive the tail (mirroring the scanner) from the REAL transcript tails and
    //     drive the real SessionRegistry through one reconcile pass on a quiescent transcript. ---
    auto deriveTail = [](std::wstring_view chunk) {
        std::wstring lastStop, pendingTool;
        bool interrupted = false;
        for (const auto& ev : ParseTranscriptDelta(chunk).events)
        {
            if (ev.kind == TranscriptEvent::Kind::Assistant) { lastStop = ev.stopReason; interrupted = false; pendingTool = ev.toolName; }
            else if (ev.kind == TranscriptEvent::Kind::ToolResult) { pendingTool.clear(); }
            else { lastStop.clear(); pendingTool.clear(); interrupted = IsUserInterruptMarker(ev.text); }
        }
        return std::make_tuple(lastStop, pendingTool, interrupted);
    };
    auto reconcileQuiescent = [&](SessionRegistry& reg, const std::wstring& id, std::wstring_view chunk) {
        const auto [lastStop, pendingTool, interrupted] = deriveTail(chunk);
        const auto st = reg.Get(id)->state;
        if (ShouldSynthesizeStop(st, lastStop, interrupted, 3000))
        {
            HookMessage stop = Msg(id, HookEvent::Stop); stop.ts = 9000; stop.quiescentStop = true;
            reg.OnHookEvent(stop);
        }
        else if (ShouldSynthesizeBlockedOnUser(st, pendingTool, interrupted, 3000))
        {
            HookMessage n = Msg(id, HookEvent::Notification); n.permissionRequest = true; n.ts = 9000;
            reg.OnHookEvent(n);
        }
    };

    { // #1 oldest (4f2ea3bc): unanswered AskUserQuestion -> the "needs you" column, NOT Running
        SessionRegistry reg; reg.Upsert(MakeSession(L"e1", SessionState::Running));
        reconcileQuiescent(reg, L"e1",
            L"{\"type\":\"user\",\"message\":{\"content\":\"q\"}}\n"
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"AskUserQuestion\",\"input\":{}}]}}\n");
        CHECK(reg.Get(L"e1")->state == SessionState::NeedsApproval, "e2e #1: unanswered AskUserQuestion -> NeedsApproval (was: stuck Running)");
    }
    { // #2 mid (3de852d6): question rejected then user interrupt -> turn over -> Waiting
        SessionRegistry reg; reg.Upsert(MakeSession(L"e2", SessionState::Running));
        reconcileQuiescent(reg, L"e2",
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"AskUserQuestion\",\"input\":{}}]}}\n"
            L"{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"content\":\"rejected\"}]}}\n"
            L"{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"[Request interrupted by user for tool use]\"}]}}\n");
        CHECK(reg.Get(L"e2")->state == SessionState::WaitingForInput, "e2e #2: interrupted turn -> WaitingForInput (was: stuck Running)");
    }
    { // original: NeedsApproval whose post-approval Stop was dropped -> a terminal tail releases it
        SessionRegistry reg; reg.Upsert(MakeSession(L"e3", SessionState::Running));
        reg.OnHookEvent([] { HookMessage m = Msg(L"e3", HookEvent::Notification); m.permissionRequest = true; return m; }());
        CHECK(reg.Get(L"e3")->state == SessionState::NeedsApproval, "e2e orig: permission Notification -> NeedsApproval");
        reconcileQuiescent(reg, L"e3",
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"All done.\"}]}}\n");
        CHECK(reg.Get(L"e3")->state == SessionState::WaitingForInput, "e2e orig: dropped post-approval Stop -> terminal tail releases NeedsApproval -> Waiting");
    }
    { // CONTROL: a genuinely-working session (pending NON-interactive Bash) must STAY Running
        SessionRegistry reg; reg.Upsert(MakeSession(L"e4", SessionState::Running));
        reconcileQuiescent(reg, L"e4",
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"Bash\",\"input\":{}}]}}\n");
        CHECK(reg.Get(L"e4")->state == SessionState::Running, "e2e control: pending Bash (working) stays Running — no false positive");
    }

    // --- recon-resume e2e: NeedsApproval -> Running when the user answers + the agent works again
    //     (a FRESH live append, the recon-run-family path — mirrors _reconcileSession). ---
    auto reconcileResume = [&](SessionRegistry& reg, const std::wstring& id, std::wstring_view chunk) {
        const auto [lastStop, pendingTool, interrupted] = deriveTail(chunk);
        bool consumedTurnEvent = false; // an assistant / human line (a bare tool_result never counts — mirrors _readDelta)
        for (const auto& ev : ParseTranscriptDelta(chunk).events)
        {
            if (ev.kind != TranscriptEvent::Kind::ToolResult) { consumedTurnEvent = true; }
        }
        if (ShouldSynthesizeResumed(reg.Get(id)->state, consumedTurnEvent, /*primed*/ true, pendingTool, lastStop, interrupted, /*fresh*/ 500))
        {
            HookMessage r = Msg(id, HookEvent::PostToolUse); r.ts = 9000;
            reg.OnHookEvent(r);
        }
    };

    { // THE bug: AskUserQuestion answered, agent KEEPS WORKING -> Running (was: stuck NeedsApproval/orange until end-of-turn)
        SessionRegistry reg; reg.Upsert(MakeSession(L"r1", SessionState::Running));
        reconcileQuiescent(reg, L"r1", // unanswered question goes quiescent -> NeedsApproval
            L"{\"type\":\"user\",\"message\":{\"content\":\"q\"}}\n"
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"AskUserQuestion\",\"input\":{}}]}}\n");
        CHECK(reg.Get(L"r1")->state == SessionState::NeedsApproval, "e2e resume #1a: unanswered AskUserQuestion -> NeedsApproval");
        reconcileResume(reg, L"r1", // the answer (tool_result) + the agent resumes working (fresh assistant line, turn not over)
            L"{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"content\":\"Option A\"}]}}\n"
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"text\",\"text\":\"Great, doing it.\"}]}}\n");
        CHECK(reg.Get(L"r1")->state == SessionState::Running, "e2e resume #1b: answered + working again -> Running (THE FIX: was stuck NeedsApproval)");
    }
    { // "not limited to AskUserQuestion": a real permission approval, then the agent works again -> Running
        SessionRegistry reg; reg.Upsert(MakeSession(L"r2", SessionState::Running));
        reg.OnHookEvent([] { HookMessage m = Msg(L"r2", HookEvent::Notification); m.permissionRequest = true; return m; }());
        CHECK(reg.Get(L"r2")->state == SessionState::NeedsApproval, "e2e resume #2a: permission Notification -> NeedsApproval");
        reconcileResume(reg, L"r2", // approved: the tool ran (a fresh assistant line, mid-turn)
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"Bash\",\"input\":{}}]}}\n");
        CHECK(reg.Get(L"r2")->state == SessionState::Running, "e2e resume #2b: approved + working again -> Running (NOT limited to AskUserQuestion)");
    }
    { // CONTROL: a NEW question in the resume window keeps it blocked (must NOT flip to Running)
        SessionRegistry reg; reg.Upsert(MakeSession(L"r3", SessionState::Running));
        reconcileQuiescent(reg, L"r3",
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"AskUserQuestion\",\"input\":{}}]}}\n");
        reconcileResume(reg, L"r3", // answered the first, but the agent immediately asks ANOTHER
            L"{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"content\":\"A\"}]}}\n"
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"name\":\"AskUserQuestion\",\"input\":{}}]}}\n");
        CHECK(reg.Get(L"r3")->state == SessionState::NeedsApproval, "e2e resume control: a NEW pending question stays NeedsApproval");
    }
    { // CONTROL: an answer that ENDS the turn is recon-stop's job (-> Waiting), never a Running blip
        SessionRegistry reg; reg.Upsert(MakeSession(L"r4", SessionState::Running));
        reg.OnHookEvent([] { HookMessage m = Msg(L"r4", HookEvent::Notification); m.permissionRequest = true; return m; }());
        reconcileResume(reg, L"r4", // a terminal tail: resume declines (mutually exclusive with recon-stop)
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"All done.\"}]}}\n");
        CHECK(reg.Get(L"r4")->state == SessionState::NeedsApproval, "e2e resume control: terminal tail does NOT resume (recon-stop territory)");
        reconcileQuiescent(reg, L"r4", // and the quiescent recon-stop then releases it the right way
            L"{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"All done.\"}]}}\n");
        CHECK(reg.Get(L"r4")->state == SessionState::WaitingForInput, "e2e resume control: end-of-turn -> recon-stop -> WaitingForInput");
    }
}

// ===== Fleet Observer O1 (ProcessInspect primitives; doc/agentmaster/OBSERVER.md §6, §8b) =====

// A canned snapshot modelling one WindowsTerminal hosting three tabs: pwsh->claude (tab A),
// cmd->cmd-shim->claude (tab B, 2 levels deep), pwsh->git (tab C, no claude). 201 (claude) also
// has a node child; a claude-rooted search must match the ROOT itself (descendant-or-self —
// the Manager-launched shape: claude IS the ConPTY root, no shell in between).
static std::vector<ProcEntry> CannedSnapshot()
{
    return {
        { 10, 1, L"explorer.exe" },
        { 100, 10, L"WindowsTerminal.exe" },
        { 200, 100, L"pwsh.exe" }, // tab A shell
        { 201, 200, L"claude.exe" }, // claude under tab A (direct)
        { 202, 201, L"node.exe" }, // a tool spawned by claude A
        { 300, 100, L"cmd.exe" }, // tab B shell
        { 301, 300, L"cmd.exe" }, // the .cmd shim
        { 302, 301, L"claude.exe" }, // claude under tab B (2 levels deep)
        { 400, 100, L"pwsh.exe" }, // tab C shell (no claude)
        { 401, 400, L"git.exe" },
    };
}

void TestProcessInspectTree()
{
    std::wprintf(L"ProcessInspect tree helpers (over a canned snapshot):\n");
    const auto snap = CannedSnapshot();

    CHECK(ImageNameEq(L"Claude.exe", L"claude.exe"), "ImageNameEq case-insensitive");
    CHECK(!ImageNameEq(L"claude.exe", L"claude"), "ImageNameEq length-strict");
    CHECK(!ImageNameEq(L"claude.exe", L"codex.exe"), "ImageNameEq distinct names");

    CHECK(FindDescendantByImage(snap, 200, L"claude.exe") == 201, "claude direct child of pwsh");
    CHECK(FindDescendantByImage(snap, 300, L"claude.exe") == 302, "claude 2 levels under cmd shim");
    CHECK(FindDescendantByImage(snap, 400, L"claude.exe") == 0, "no claude under a git tab");
    CHECK(FindDescendantByImage(snap, 999, L"claude.exe") == 0, "unknown root -> 0");
    CHECK(FindDescendantByImage(snap, 100, L"claude.exe") == 201, "BFS finds the shallowest claude (tab A)");
    CHECK(FindDescendantByImage(snap, 201, L"claude.exe") == 201, "descendant-or-self: the root itself matches (a Manager-launched claude IS the ConPTY root)");
    CHECK(FindDescendantByImage(snap, 201, L"node.exe") == 202, "descendant of a claude found");
    CHECK(FindDescendantByImage(snap, 201, L"pwsh.exe") == 0, "never matches upward (the root's parent shell is out of scope)");

    const auto kids = ChildrenOf(snap, 100);
    CHECK(kids.size() == 3, "ChildrenOf(WT) count");
    CHECK(kids.size() == 3 && kids[0] == 200 && kids[1] == 300 && kids[2] == 400, "ChildrenOf preserves snapshot order");
    CHECK(ChildrenOf(snap, 200).size() == 1 && ChildrenOf(snap, 200)[0] == 201, "ChildrenOf(pwsh A) == {claude}");
    CHECK(ChildrenOf(snap, 401).empty(), "ChildrenOf of a leaf is empty");

    // --- O6 busy heuristics: IsShellImage / HasActiveChild (claude) / HasNonShellChild (shell) ---
    CHECK(IsShellImage(L"pwsh.exe") && IsShellImage(L"PowerShell.exe") && IsShellImage(L"cmd.exe"), "IsShellImage: shells");
    CHECK(!IsShellImage(L"claude.exe") && !IsShellImage(L"codex.exe") && !IsShellImage(L"git.exe") && !IsShellImage(L""), "IsShellImage: non-shells");

    CHECK(HasActiveChild(snap, 201), "claude A is busy: has a tool child (node)");
    CHECK(!HasActiveChild(snap, 302), "claude B is idle: no children");
    CHECK(!HasActiveChild(snap, 401), "a leaf process has no active child");
    CHECK(!HasActiveChild(snap, 999), "unknown pid -> no active child");

    CHECK(HasNonShellChild(snap, 400), "pwsh C is busy: running git (a non-shell command)");
    CHECK(!HasNonShellChild(snap, 300), "cmd B's only direct child is the cmd shim (a shell) -> not busy");
    CHECK(HasNonShellChild(snap, 200), "pwsh A has a non-shell child (claude)");

    // console infrastructure (conhost / OpenConsole) is NOT "a command in progress"
    const std::vector<ProcEntry> infra = {
        { 500, 1, L"pwsh.exe" },
        { 501, 500, L"conhost.exe" },
    };
    CHECK(!HasActiveChild(infra, 500), "HasActiveChild ignores a conhost child");
    CHECK(!HasNonShellChild(infra, 500), "HasNonShellChild ignores a conhost child");
    const std::vector<ProcEntry> infra2 = {
        { 600, 1, L"pwsh.exe" },
        { 601, 600, L"OpenConsole.exe" },
        { 602, 600, L"rg.exe" },
    };
    CHECK(HasActiveChild(infra2, 600), "HasActiveChild sees a real (rg) child past OpenConsole");
    CHECK(HasNonShellChild(infra2, 600), "HasNonShellChild sees rg (a non-shell command)");

    // --- CommandChildrenOf: the shell's real command children (the out-of-band cwd source) ---
    // A shell's NATIVE children inherit its live cwd at spawn, so they recover a pwsh tab's cwd
    // (pwsh freezes its own process cwd). Console infra (conhost / OpenConsole) is excluded — it's
    // OS plumbing, not a command, and runs in C:\WINDOWS, which would poison the reading.
    CHECK(CommandChildrenOf(snap, 200).size() == 1 && CommandChildrenOf(snap, 200)[0] == 201, "CommandChildrenOf(pwsh A) == {claude} (a native child)");
    CHECK(CommandChildrenOf(snap, 400).size() == 1 && CommandChildrenOf(snap, 400)[0] == 401, "CommandChildrenOf(pwsh C) == {git}");
    CHECK(CommandChildrenOf(snap, 401).empty(), "CommandChildrenOf of a leaf is empty");
    CHECK(CommandChildrenOf(snap, 999).empty(), "CommandChildrenOf of an unknown pid is empty");
    CHECK(CommandChildrenOf(infra, 500).empty(), "CommandChildrenOf excludes a conhost-only child (would read C:\\WINDOWS)");
    {
        const auto cc = CommandChildrenOf(infra2, 600);
        CHECK(cc.size() == 1 && cc[0] == 602, "CommandChildrenOf skips OpenConsole, keeps the rg command child");
    }

    // --- FindTerminalHostPid: walk a claude up to its hosting WindowsTerminal.exe (the host-label core) ---
    // The label that follows (real WT vs Agentmaster vs Agentmaster Dev) keys on this host process's
    // package family; here we just verify the ancestor walk finds the right terminal (or none).
    CHECK(FindTerminalHostPid(snap, 201) == 100, "claude A -> its hosting WindowsTerminal (via pwsh)");
    CHECK(FindTerminalHostPid(snap, 302) == 100, "claude B -> the hosting WindowsTerminal (past cmd + shim)");
    CHECK(FindTerminalHostPid(snap, 100) == 0, "WindowsTerminal itself has no WT ancestor (skips self)");
    CHECK(FindTerminalHostPid(snap, 999) == 0, "unknown pid -> no host");
    {
        // An orphan: a claude whose parent terminal already exited (not in the snapshot) -> 0, so the
        // label falls back to the AM_SESSION stamp ("Agentmaster") rather than a live host.
        const std::vector<ProcEntry> orphan = { { 700, 690 /*gone*/, L"claude.exe" } };
        CHECK(FindTerminalHostPid(orphan, 700) == 0, "orphaned claude (dead host) -> no terminal host pid");
    }
}

void TestProcessInspectParse()
{
    std::wprintf(L"ProcessInspect cmdline/env parse + classify:\n");
    using Env = std::unordered_map<std::wstring, std::wstring>;

    // --- ExtractCmdlineArg: "--flag value", "--flag=value", quoting, absent/dangling ---
    CHECK(ExtractCmdlineArg(L"claude --model opus --effort high", L"--model") == std::optional<std::wstring>(L"opus"), "extract --model value");
    CHECK(ExtractCmdlineArg(L"claude --model=sonnet", L"--model") == std::optional<std::wstring>(L"sonnet"), "extract --model=value");
    CHECK(ExtractCmdlineArg(L"claude --settings \"C:/a b/s.json\" --x", L"--settings") == std::optional<std::wstring>(L"C:/a b/s.json"), "extract quoted value with a space");
    CHECK(!ExtractCmdlineArg(L"claude --resume", L"--resume").has_value(), "dangling flag -> nullopt");
    CHECK(!ExtractCmdlineArg(L"claude --model opus", L"--effort").has_value(), "absent flag -> nullopt");

    // --- ParseClaudeFacts: a full fresh launch line ---
    {
        ClaudeProcessFacts f;
        Env env{ { L"WT_SESSION", L"wt-1" }, { L"AM_SESSION", L"am-1" } };
        ParseClaudeFacts(L"claude --dangerously-skip-permissions --model opus --effort high --permission-mode plan --settings \"C:/x/s.json\" --session-id abc-123", env, f);
        CHECK(f.model == L"opus", "facts model from --model");
        CHECK(f.effort == L"high", "facts effort from --effort");
        CHECK(f.permissionMode == L"plan", "facts permission-mode");
        CHECK(f.sessionIdArg == L"abc-123", "facts --session-id");
        CHECK(f.resumeTarget.empty(), "fresh launch has no resume target");
        CHECK(f.wtSession == L"wt-1" && f.amSession == L"am-1", "facts WT_SESSION + AM_SESSION from env");
        CHECK(!f.background, "interactive launch is not background");
    }

    // --- resume line + env-derived model/effort fallback ---
    {
        ClaudeProcessFacts f;
        Env env{ { L"CLAUDE_CODE_MODEL", L"haiku" }, { L"CLAUDE_CODE_EFFORT_LEVEL", L"low" } };
        ParseClaudeFacts(L"claude --resume conv-xyz --settings \"C:/x/s.json\"", env, f);
        CHECK(f.resumeTarget == L"conv-xyz", "facts --resume target");
        CHECK(f.model == L"haiku", "facts model falls back to CLAUDE_CODE_MODEL");
        CHECK(f.effort == L"low", "facts effort falls back to CLAUDE_CODE_EFFORT_LEVEL");
    }
    {
        ClaudeProcessFacts f;
        Env env{ { L"ANTHROPIC_MODEL", L"claude-x" } };
        ParseClaudeFacts(L"claude", env, f);
        CHECK(f.model == L"claude-x", "facts model falls back to ANTHROPIC_MODEL");
    }

    // --- background detection (env kind / CLAUDE_BG_* / daemon-run cmdline) ---
    {
        ClaudeProcessFacts f;
        Env env{ { L"CLAUDE_CODE_SESSION_KIND", L"bg" }, { L"CLAUDE_CODE_SESSION_NAME", L"nightly" } };
        ParseClaudeFacts(L"claude", env, f);
        CHECK(f.background, "background via CLAUDE_CODE_SESSION_KIND=bg");
        CHECK(f.sessionName == L"nightly", "background session name");
    }
    {
        ClaudeProcessFacts f;
        Env env{ { L"CLAUDE_BG_HOST", L"x" } };
        ParseClaudeFacts(L"claude", env, f);
        CHECK(f.background, "background via any CLAUDE_BG_* env");
    }
    {
        ClaudeProcessFacts f;
        Env env{};
        ParseClaudeFacts(L"claude daemon run --bg-pty-host", env, f);
        CHECK(f.background, "background via daemon-run cmdline");
    }

    // --- EnvLookup is case-insensitive (Windows env names ignore case) ---
    {
        Env env{ { L"wt_session", L"low-key" } };
        CHECK(EnvLookup(env, L"WT_SESSION") == L"low-key", "EnvLookup case-insensitive key match");
        CHECK(EnvLookup(env, L"missing").empty(), "EnvLookup miss -> empty");
    }

    // --- ClassifyRunningApp truth table (OBSERVER.md §7) ---
    CHECK(ClassifyRunningApp(L"am-1", L"wt-1", L"am-1") == RunningApp::Agentmaster, "classify ours -> Agentmaster");
    CHECK(ClassifyRunningApp(L"am-1", L"", L"am-1") == RunningApp::Agentmaster, "classify ours even without WT_SESSION");
    CHECK(ClassifyRunningApp(L"", L"wt-1", L"am-1") == RunningApp::WindowsTerminal, "classify bare WT_SESSION -> WindowsTerminal");
    CHECK(ClassifyRunningApp(L"", L"", L"am-1") == RunningApp::Other, "classify neither -> Other");
    CHECK(ClassifyRunningApp(L"am-2", L"wt-1", L"am-1") == RunningApp::Other, "classify a FOREIGN AM_SESSION -> Other (never ours)");
    CHECK(ClassifyRunningApp(L"", L"wt-1", L"") == RunningApp::WindowsTerminal, "classify with our stamp unminted: empty am + WT -> WindowsTerminal");
    CHECK(ClassifyRunningApp(L"", L"", L"") == RunningApp::Other, "classify all-empty -> Other (no false Agentmaster)");

    // §19-Q1: AM_SESSION may be "<guid>:<windowId>" (a Launched session) — classify on the GUID prefix.
    CHECK(ClassifyRunningApp(L"am-1:win-9", L"wt-1", L"am-1") == RunningApp::Agentmaster, "classify ours with :<windowId> suffix -> Agentmaster (prefix match)");
    CHECK(ClassifyRunningApp(L"am-2:win-9", L"wt-1", L"am-1") == RunningApp::Other, "classify a FOREIGN am with :<windowId> -> Other");
    CHECK(WindowIdFromAmSession(L"am-1:win-9") == L"win-9", "extract windowId from <guid>:<windowId>");
    CHECK(WindowIdFromAmSession(L"am-1").empty(), "bare <guid> has no windowId");
    CHECK(WindowIdFromAmSession(L"").empty(), "empty AM_SESSION -> no windowId");

    // --- IsClaudeDesktopGuiApp: tell the Claude Code CLI (console) from the Claude desktop app (GUI) ---
    // The desktop Electron app (and its renderer/gpu/utility children) share the leaf name Claude.exe
    // and run with cwd C:\WINDOWS\system32; only the PE subsystem separates them. Excluding them keeps
    // the External census free of bogus "system32 sessions".
    {
        ClaudeProcessFacts cli;
        cli.subsystem = 3; // IMAGE_SUBSYSTEM_WINDOWS_CUI
        CHECK(!IsClaudeDesktopGuiApp(cli), "console-subsystem claude is the CLI -> not the desktop app");
        ClaudeProcessFacts desktop;
        desktop.subsystem = 2; // IMAGE_SUBSYSTEM_WINDOWS_GUI
        desktop.cwd = L"C:\\WINDOWS\\system32";
        CHECK(IsClaudeDesktopGuiApp(desktop), "GUI-subsystem Claude.exe is the desktop app -> excluded");
        ClaudeProcessFacts unknown; // 0 == undeterminable (denied/elevated/WOW64): never hide a real session
        CHECK(!IsClaudeDesktopGuiApp(unknown), "undeterminable subsystem -> treated as CLI (not hidden)");
    }
}

static FILETIME UnixMsToFileTime(int64_t ms)
{
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<uint64_t>(ms) * 10000ull + 116444736000000000ull;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

static void MakeJsonl(const std::wstring& path, const std::string& content, int64_t mtimeMs, int64_t ctimeMs)
{
    const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return;
    }
    if (!content.empty())
    {
        DWORD w = 0;
        ::WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &w, nullptr);
    }
    const FILETIME c = UnixMsToFileTime(ctimeMs);
    const FILETIME m = UnixMsToFileTime(mtimeMs);
    ::SetFileTime(h, &c, nullptr, &m);
    ::CloseHandle(h);
}

void TestTranscriptResolve()
{
    std::wprintf(L"ProcessInspect transcript resolution (encode + newest/tie-break):\n");

    // --- EncodeCwdToProjectDir: every non-[A-Za-z0-9] -> '-', no case folding ---
    CHECK(EncodeCwdToProjectDir(L"C:\\Users\\ELI") == L"C--Users-ELI", "encode plain path");
    CHECK(EncodeCwdToProjectDir(L"C:\\Users\\ELI\\.claude") == L"C--Users-ELI--claude", "encode dotted segment (\\. -> --)");
    CHECK(EncodeCwdToProjectDir(L"C:\\Program Files (x86)\\X") == L"C--Program-Files--x86--X", "encode spaces + parens");
    CHECK(EncodeCwdToProjectDir(L"K:/source/NumSharp") == L"K--source-NumSharp", "encode forward slashes, keep case");

    // --- ExtractCwdFromTranscriptHead: pull the cwd off the user/assistant lines ---
    {
        const std::wstring head =
            LR"j({"type":"mode","mode":"x"})j" L"\n"
            LR"j({"type":"user","cwd":"C:\\Users\\ELI","message":{"content":"hi"}})j" L"\n";
        CHECK(ExtractCwdFromTranscriptHead(head) == L"C:\\Users\\ELI", "extract cwd (backslash-unescaped) from head");
        CHECK(ExtractCwdFromTranscriptHead(LR"j({"type":"mode"})j").empty(), "no cwd in head -> empty");
    }

    // --- PickNewestTranscript (PURE): newest mtime; tie-break by ctime ~ start ---
    {
        std::vector<TranscriptCandidate> c{
            { L"old", 1000, 1000 },
            { L"new", 9000, 9000 },
        };
        CHECK(PickNewestTranscript(c, 0) == L"new", "newest mtime wins (no start hint)");
        CHECK(PickNewestTranscript({}, 0).empty(), "no candidates -> empty");
    }
    {
        // Two written within the tie window of each other; the one created closest to the claude's
        // start time wins (the same-cwd, two-claudes disambiguation).
        std::vector<TranscriptCandidate> c{
            { L"a", 5000, 100 },
            { L"b", 5000, 4000 },
        };
        CHECK(PickNewestTranscript(c, 4200) == L"b", "tie-break: ctime closest to start");
        CHECK(PickNewestTranscript(c, 50) == L"a", "tie-break: other start picks the other");
    }
    {
        // IDENTITY is by creation time, not activity: a claude resolves to the transcript CREATED
        // near its start (its own), even when ANOTHER transcript in the cwd is more recently written.
        std::vector<TranscriptCandidate> c{
            { L"mine", 5200, 5000 }, // created at my start (5000), lightly written
            { L"other", 9000, 100 }, // created long before me (100), very active (mtime 9000)
        };
        CHECK(PickNewestTranscript(c, 5000) == L"mine", "resolve to the transcript created near MY start, not the busiest other");
    }
    {
        // Never-prompted: only transcripts predating this claude's start exist -> resolve to "" (no
        // collapse onto a stale / another claude's transcript). The same-cwd, 0-message case (§11d).
        std::vector<TranscriptCandidate> c{
            { L"old1", 1000, 100 },
            { L"old2", 2000, 500 },
        };
        CHECK(PickNewestTranscript(c, 8000).empty(), "no transcript created at/after start -> empty (never-prompted; no stale collapse)");
    }

    // --- ResolveSessionIdIn over a temp projects root (real glob) ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring{ tmp } + L"am_obs_test_" + std::to_wstring(::GetCurrentProcessId());
        const std::wstring projRoot = root + L"\\projects";
        const std::wstring cwd = L"C:\\AmObsTest\\proj";
        const std::wstring dir = projRoot + L"\\" + EncodeCwdToProjectDir(cwd);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ dir }, ec);
        MakeJsonl(dir + L"\\older-id.jsonl", "{}", 1000, 1000);
        MakeJsonl(dir + L"\\newer-id.jsonl", "{}", 9000, 9000);

        CHECK(ResolveSessionIdIn(projRoot, cwd, 0) == L"newer-id", "ResolveSessionIdIn picks the newest transcript in the encoded dir (no start hint)");
        CHECK(ResolveSessionIdIn(projRoot, L"C:\\Nope\\missing", 0).empty(), "ResolveSessionIdIn empty when the encoded dir has no transcripts");
        CHECK(ResolveSessionIdIn(L"", cwd, 0).empty(), "ResolveSessionIdIn empty for an empty projects root");
        // start-aware (the same-cwd disambiguation over a real glob): resolve to the transcript
        // created nearest the claude's start; "" when none is at/after it.
        CHECK(ResolveSessionIdIn(projRoot, cwd, 1000) == L"older-id", "start near older's ctime -> older-id");
        CHECK(ResolveSessionIdIn(projRoot, cwd, 9000) == L"newer-id", "start near newer's ctime -> newer-id");
        CHECK(ResolveSessionIdIn(projRoot, cwd, 50000).empty(), "start after all transcripts -> empty (never-prompted)");

        std::filesystem::remove_all(std::filesystem::path{ root }, ec);
    }

    // --- SessionStore: the generalized DURABLE per-session key/value store (titles + future data) ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring store = std::wstring{ tmp } + L"am_sstore_" + std::to_wstring(::GetCurrentProcessId());
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path{ store }, ec); // a clean slate

        const std::wstring a = L"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const std::wstring b = L"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";

        // absent -> empty; set -> get (durability across "windows" is just re-reading the same dir).
        CHECK(GetSessionStoreFieldIn(store, a, L"title").empty(), "store: a missing field reads empty");
        CHECK(SetSessionStoreFieldIn(store, a, L"title", L"My Renamed Session"), "store: set title ok");
        CHECK(GetSessionStoreFieldIn(store, a, L"title") == L"My Renamed Session", "store: get returns the set title");

        // generalized: a second arbitrary key on the same session coexists with the first.
        CHECK(SetSessionStoreFieldIn(store, a, L"note", L"hello"), "store: set a second arbitrary field");
        const auto recA = LoadSessionStoreIn(store, a);
        CHECK(recA.size() == 2 && recA.at(L"title") == L"My Renamed Session" && recA.at(L"note") == L"hello", "store: record holds both keys");

        // dedup: setting the same value again still reports success, value unchanged.
        CHECK(SetSessionStoreFieldIn(store, a, L"title", L"My Renamed Session"), "store: redundant set is a no-op success");
        CHECK(GetSessionStoreFieldIn(store, a, L"title") == L"My Renamed Session", "store: value stable after a redundant set");

        // an empty value REMOVES a key; removing the last key deletes the file (record empty).
        CHECK(SetSessionStoreFieldIn(store, a, L"note", L""), "store: empty value removes the key");
        CHECK(GetSessionStoreFieldIn(store, a, L"note").empty(), "store: a removed key reads empty");
        CHECK(LoadSessionStoreIn(store, a).size() == 1, "store: only the title remains");

        // a second session is independent (per-session files; O(1) by id).
        CHECK(SetSessionStoreFieldIn(store, b, L"title", L"Other Session"), "store: set title on a second session");

        // bulk: every session that has the field, in ONE scan (sparse — only titled sessions).
        const auto all = LoadAllSessionStoreFieldIn(store, L"title");
        CHECK(all.size() == 2 && all.at(a) == L"My Renamed Session" && all.at(b) == L"Other Session", "store: LoadAll gathers both titles");
        CHECK(LoadAllSessionStoreFieldIn(store, L"note").empty(), "store: LoadAll for a field no session has is empty");

        // a malformed session id never escapes the store dir; an empty id is inert.
        CHECK(!SetSessionStoreFieldIn(store, L"..\\evil", L"title", L"x"), "store: a path-bearing id is rejected");
        CHECK(GetSessionStoreFieldIn(store, L"", L"title").empty(), "store: an empty id reads empty");

        // --- the FAVORITE key (FAVORITES.md): the durable star, the same store + mechanism. ---
        CHECK(std::wstring{ kSessionStoreFavoriteKey } == L"favorite", "store: the favorite key is \"favorite\"");
        // not favorited until set; a star coexists with a title on the same session.
        CHECK(GetSessionStoreFieldIn(store, a, kSessionStoreFavoriteKey).empty(), "store: a session is not favorite by default");
        CHECK(SetSessionStoreFieldIn(store, a, kSessionStoreFavoriteKey, L"1"), "store: favorite a");
        const auto recFav = LoadSessionStoreIn(store, a);
        CHECK(recFav.size() == 2 && recFav.at(L"title") == L"My Renamed Session" && recFav.at(kSessionStoreFavoriteKey) == L"1", "store: favorite coexists with the title");
        // LoadAll(favorite) gathers ONLY favorited sessions (b has a title but no star).
        const auto favs = LoadAllSessionStoreFieldIn(store, kSessionStoreFavoriteKey);
        CHECK(favs.size() == 1 && favs.count(a) == 1 && favs.count(b) == 0, "store: LoadAll favorites returns only the starred session");
        // un-favorite removes the key (back to title-only) but the second-session star is independent.
        CHECK(SetSessionStoreFieldIn(store, b, kSessionStoreFavoriteKey, L"1"), "store: favorite b too");
        CHECK(SetSessionStoreFieldIn(store, a, kSessionStoreFavoriteKey, L""), "store: un-favorite a (empty removes the key)");
        CHECK(GetSessionStoreFieldIn(store, a, kSessionStoreFavoriteKey).empty(), "store: a is no longer favorite");
        const auto favs2 = LoadAllSessionStoreFieldIn(store, kSessionStoreFavoriteKey);
        CHECK(favs2.size() == 1 && favs2.count(b) == 1 && favs2.count(a) == 0, "store: only b remains favorited");
        CHECK(LoadSessionStoreIn(store, a).size() == 1, "store: un-favorite left the title intact");

        std::filesystem::remove_all(std::filesystem::path{ store }, ec);
    }

    // --- AnalyzeSessionTranscript: a real user prompt that BEGINS WITH A NEWLINE is kept ----------
    // Regression (the empty-summary bug): a pasted prompt (e.g. a terminal-screen capture) frequently
    // starts with a leading "\n". The summary noise filter ported session-end.js's startsWith('\n')
    // skip rule, which a full-corpus scan (2765 transcripts) proved a 100% false positive — every
    // message it dropped was real content, so a session whose only human input was such a paste showed
    // an EMPTY summary panel. The leading whitespace is now trimmed first and that rule is gone, so the
    // message is KEPT (and its leading newline stripped from the stored text). A leading-newline-THEN-
    // noise message (e.g. "\nCaveat:") is still dropped — the surviving startsWith prefixes see past
    // the trimmed whitespace — and a whitespace-only message still drops out entirely.
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring base = std::wstring{ tmp } + L"am_nlmsg_" + std::to_wstring(::GetCurrentProcessId());

        const std::wstring pNl = base + L"_nl.jsonl";
        MakeJsonl(pNl,
                  // (1) a REAL prompt that begins with "\n" -> KEPT (leading newline trimmed off)
                  R"j({"type":"user","userType":"external","message":{"content":"\nrecap: pick up the non-cast task"},"timestamp":"2026-01-24T20:00:00.000Z"})j" "\n"
                  // (2) leading "\n" THEN a Caveat: noise prefix -> still DROPPED (prefix seen post-trim)
                  R"j({"type":"user","userType":"external","message":{"content":"\nCaveat: generated while running a local command"},"timestamp":"2026-01-24T20:01:00.000Z"})j" "\n"
                  // (3) only whitespace/newlines -> DROPPED (trims to empty)
                  R"j({"type":"user","userType":"external","message":{"content":"\n   \n"},"timestamp":"2026-01-24T20:02:00.000Z"})j" "\n",
                  1000, 1000);
        const auto a = AnalyzeSessionTranscript(pNl, 0);
        CHECK(a.userMsgs.size() == 1, "AnalyzeSessionTranscript: a leading-newline real prompt is kept; the Caveat-after-newline + whitespace-only messages are dropped");
        CHECK(!a.userMsgs.empty() && a.userMsgs[0] == L"recap: pick up the non-cast task", "AnalyzeSessionTranscript: the kept message has its leading newline trimmed from the stored text");
        CHECK(a.lastUserTs == L"2026-01-24T20:00:00.000Z", "AnalyzeSessionTranscript: the kept leading-newline prompt advances last-user time (was empty when it was wrongly filtered)");

        std::error_code ecNl;
        std::filesystem::remove(std::filesystem::path{ pNl }, ecNl);
    }

    // --- AnalyzeSessionTranscript: answering an AskUserQuestion advances "last user msg" ----------
    // Regression: the user's answer to AskUserQuestion is a tool_result block (not a typed text
    // prompt), so it once never moved lastUserTs and "last user msg" stayed pinned to the older typed
    // prompt. The answer is now correlated back to the interactive tool_use id and DOES advance the
    // timestamp, while its synthetic "User has answered…" text stays OUT of the Messages list. A
    // NON-interactive tool_result (a Bash result) must NOT advance it.
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring base = std::wstring{ tmp } + L"am_lastuser_" + std::to_wstring(::GetCurrentProcessId());

        // Case A: typed prompt (18:00) -> AskUserQuestion (18:06) -> the user's answer (18:10).
        const std::wstring pAsk = base + L"_ask.jsonl";
        MakeJsonl(pAsk,
                  R"j({"type":"user","userType":"external","message":{"content":"do the thing"},"timestamp":"2026-01-24T18:00:00.000Z"})j" "\n"
                  R"j({"type":"assistant","message":{"content":[{"type":"tool_use","id":"toolu_ASK1","name":"AskUserQuestion","input":{"questions":[]}}]},"timestamp":"2026-01-24T18:06:41.211Z"})j" "\n"
                  R"j({"type":"user","userType":"external","message":{"content":[{"type":"tool_result","content":"User has answered your questions.","tool_use_id":"toolu_ASK1"}]},"timestamp":"2026-01-24T18:10:27.815Z"})j" "\n",
                  1000, 1000);
        const auto a = AnalyzeSessionTranscript(pAsk, 0);
        CHECK(a.lastUserTs == L"2026-01-24T18:10:27.815Z", "AnalyzeSessionTranscript: answering AskUserQuestion advances last-user time to the ANSWER (not the older typed prompt)");
        CHECK(a.userMsgs.size() == 1 && a.userMsgs[0] == L"do the thing", "AnalyzeSessionTranscript: the synthetic answer text stays OUT of the Messages list (only the typed prompt)");

        // Case B (control): typed prompt (19:00) -> Bash tool_use -> a Bash tool_result (19:05). A
        // non-interactive tool result must NOT advance last-user time — it stays at the typed prompt.
        const std::wstring pBash = base + L"_bash.jsonl";
        MakeJsonl(pBash,
                  R"j({"type":"user","userType":"external","message":{"content":"run a build"},"timestamp":"2026-01-24T19:00:00.000Z"})j" "\n"
                  R"j({"type":"assistant","message":{"content":[{"type":"tool_use","id":"toolu_BASH1","name":"Bash","input":{"command":"echo hi"}}]},"timestamp":"2026-01-24T19:01:00.000Z"})j" "\n"
                  R"j({"type":"user","userType":"external","message":{"content":[{"type":"tool_result","content":"hi","tool_use_id":"toolu_BASH1"}]},"timestamp":"2026-01-24T19:05:00.000Z"})j" "\n",
                  1000, 1000);
        const auto b = AnalyzeSessionTranscript(pBash, 0);
        CHECK(b.lastUserTs == L"2026-01-24T19:00:00.000Z", "AnalyzeSessionTranscript: a non-interactive (Bash) tool_result does NOT advance last-user time");

        std::error_code ec2;
        std::filesystem::remove(std::filesystem::path{ pAsk }, ec2);
        std::filesystem::remove(std::filesystem::path{ pBash }, ec2);
    }

    // --- Revert-aware DISPLAY: a double-ESC rewind orphans a branch; summary/title must skip it -----
    // Claude stores a conversation as a TREE (each message line carries uuid + parentUuid; a root's
    // parentUuid is null). A double-ESC REWIND (or /rewind) repoints the trailing `leafUuid` marker to
    // an EARLIER node, ORPHANING the abandoned branch — whose lines STAY in the .jsonl, INTERLEAVED
    // with the live ones (in-flight tool results land AFTER the new branch starts, so the discarded set
    // is NOT a contiguous prefix). DISPLAY surfaces (summary panel + title/prompt list) must show only
    // the chain from the current leaf to root; SEARCH/index keeps every line (a reverted message stays
    // findable). Mirrors the real session 1adaa37c, where a typed "test" + a follow-up were rewound away.
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring base = std::wstring{ tmp } + L"am_revert_" + std::to_wstring(::GetCurrentProcessId());

        // File order: a discarded "test" branch, then the LIVE branch, with a discarded follow-up
        // ("u_orphan") INTERLEAVED *after* the live root — so a naive file-order parser keeps it.
        const std::string revert =
            R"j({"type":"user","userType":"external","uuid":"u_test","parentUuid":null,"message":{"content":"test"},"timestamp":"2026-06-26T10:00:00.000Z"})j" "\n"
            R"j({"type":"assistant","uuid":"u_a1","parentUuid":"u_test","message":{"content":[{"type":"text","text":"hi"}]},"timestamp":"2026-06-26T10:00:01.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"u_live1","parentUuid":null,"message":{"content":"What if my window lags"},"timestamp":"2026-06-26T10:05:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"u_orphan","parentUuid":"u_a1","message":{"content":"ORPHANED follow-up"},"timestamp":"2026-06-26T10:05:30.000Z"})j" "\n"
            R"j({"type":"assistant","uuid":"u_a2","parentUuid":"u_live1","message":{"content":[{"type":"text","text":"here is how"}]},"timestamp":"2026-06-26T10:06:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"u_live2","parentUuid":"u_a2","message":{"content":"audit b and then a"},"timestamp":"2026-06-26T10:10:00.000Z"})j" "\n"
            R"j({"type":"last-prompt","lastPrompt":"audit b and then a","leafUuid":"u_live2"})j" "\n";

        // (1) the pure resolver: active == the leaf->root chain; both discarded roots AND the
        // interleaved orphan are excluded.
        const auto active = ActiveBranchUuids(std::wstring{ revert.begin(), revert.end() });
        CHECK(active.size() == 3 && active.count(L"u_live1") && active.count(L"u_a2") && active.count(L"u_live2"),
              "ActiveBranchUuids: exactly the 3 live leaf->root nodes are included");
        CHECK(active.count(L"u_test") == 0 && active.count(L"u_a1") == 0 && active.count(L"u_orphan") == 0,
              "ActiveBranchUuids: the rewound-away branch (incl. the INTERLEAVED orphan after the live root) is excluded");

        // (1b) the per-message property: ClassifyTranscriptLines stamps onActiveBranch ("IsActiveLeaf")
        // on every message; ClassifyTranscriptLine alone captures uuid + defaults onActiveBranch true.
        {
            const auto facts = ClassifyTranscriptLines(std::wstring{ revert.begin(), revert.end() }, 4096, 0);
            auto branchOf = [&facts](const wchar_t* id) -> int {
                for (const auto& f : facts)
                {
                    if (f.uuid == id)
                    {
                        return f.onActiveBranch ? 1 : 0;
                    }
                }
                return -1; // not found
            };
            CHECK(branchOf(L"u_live1") == 1 && branchOf(L"u_a2") == 1 && branchOf(L"u_live2") == 1,
                  "ClassifyTranscriptLines: live nodes carry onActiveBranch=true");
            CHECK(branchOf(L"u_test") == 0 && branchOf(L"u_a1") == 0 && branchOf(L"u_orphan") == 0,
                  "ClassifyTranscriptLines: rewound-away nodes (incl. the interleaved orphan) carry onActiveBranch=false");

            const auto one = ClassifyTranscriptLine(LR"j({"type":"user","uuid":"solo","parentUuid":null,"message":{"content":"hi"}})j", 100, 0);
            CHECK(one.uuid == L"solo" && one.onActiveBranch,
                  "ClassifyTranscriptLine: captures uuid; onActiveBranch defaults true (a single line has no tree context)");

            // markActiveBranch=false (a partial / HEAD read) => nothing is marked inactive (keep-all).
            const auto noMark = ClassifyTranscriptLines(std::wstring{ revert.begin(), revert.end() }, 4096, 0, false);
            bool anyInactive = false;
            for (const auto& f : noMark)
            {
                if (!f.onActiveBranch)
                {
                    anyInactive = true;
                }
            }
            CHECK(!anyInactive, "ClassifyTranscriptLines: markActiveBranch=false leaves every message active (partial/head read)");
        }

        // (2) the summary panel (AnalyzeSessionTranscript, full read): only the live typed prompts; the
        // discarded "test" + the interleaved "ORPHANED follow-up" are gone, and first-activity is the
        // LIVE root's time (the discarded earlier turn never sets it).
        const std::wstring pRevert = base + L"_summary.jsonl";
        MakeJsonl(pRevert, revert, 2000, 1000);
        const auto a = AnalyzeSessionTranscript(pRevert, 0);
        CHECK(a.userMsgs.size() == 2 && a.userMsgs[0] == L"What if my window lags" && a.userMsgs[1] == L"audit b and then a",
              "AnalyzeSessionTranscript: only the LIVE branch's typed prompts (discarded 'test' + interleaved orphan excluded)");
        CHECK(a.firstTs == L"2026-06-26T10:05:00.000Z",
              "AnalyzeSessionTranscript: first activity is the live root's time, not the rewound-away earlier turn");

        // (2b) the "Transcript" COPY (ReadConversationText, full read): the copied conversation is the
        // LIVE branch only — the discarded "test" turn + its reply + the interleaved orphan are excluded.
        const std::wstring convo = ReadConversationText(pRevert, false, 0);
        CHECK(convo.find(L"What if my window lags") != std::wstring::npos &&
                  convo.find(L"here is how") != std::wstring::npos &&
                  convo.find(L"audit b and then a") != std::wstring::npos,
              "ReadConversationText: the live User+Assistant turns are present in the copied transcript");
        CHECK(convo.find(L"test") == std::wstring::npos && convo.find(L"ORPHANED") == std::wstring::npos,
              "ReadConversationText: the rewound-away 'test' branch + the interleaved orphan are excluded");

        // (3) the title + prompt list (ReadTranscriptInfoIn, full read): title is the live first
        // prompt, not the discarded "test"; the prompt list excludes the orphan.
        const std::wstring projectsDir = base + L"_proj";
        const std::wstring cwd = L"K:\\some\\where";
        const std::wstring sub = projectsDir + L"\\" + EncodeCwdToProjectDir(cwd);
        std::error_code ecMk;
        std::filesystem::create_directories(std::filesystem::path{ sub }, ecMk);
        const std::wstring sid = L"11111111-2222-3333-4444-555555555555";
        MakeJsonl(sub + L"\\" + sid + L".jsonl", revert, 2000, 1000);
        const auto ti = ReadTranscriptInfoIn(projectsDir, cwd, sid, 0, 1000);
        CHECK(TranscriptDisplayTitle(ti) == L"What if my window lags",
              "ReadTranscriptInfoIn: the title is the LIVE first prompt, not the rewound-away 'test'");
        CHECK(ti.userPrompts.size() == 2 && ti.userPrompts[0] == L"What if my window lags" && ti.userPrompts[1] == L"audit b and then a",
              "ReadTranscriptInfoIn: the prompt list is the live branch only (no 'test', no interleaved orphan)");

        // (4) backward-compat: a transcript with NO leaf marker => empty set => EVERY line kept (the
        // legacy all-messages behavior); and a leaf naming an ABSENT node likewise degrades to keep-all
        // (never orphan the whole file off a bad pointer).
        const std::string noMarker =
            R"j({"type":"user","userType":"external","uuid":"x1","parentUuid":null,"message":{"content":"alpha"},"timestamp":"2026-06-26T11:00:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"x2","parentUuid":"x1","message":{"content":"beta"},"timestamp":"2026-06-26T11:01:00.000Z"})j" "\n";
        CHECK(ActiveBranchUuids(std::wstring{ noMarker.begin(), noMarker.end() }).empty(),
              "ActiveBranchUuids: no leafUuid marker => empty (caller keeps every line)");
        const std::string badLeaf = noMarker + R"j({"type":"last-prompt","leafUuid":"NONEXISTENT"})j" "\n";
        CHECK(ActiveBranchUuids(std::wstring{ badLeaf.begin(), badLeaf.end() }).empty(),
              "ActiveBranchUuids: a leaf naming an absent node => empty (never orphan the whole file)");
        const std::wstring pNoMarker = base + L"_nomarker.jsonl";
        MakeJsonl(pNoMarker, noMarker, 2000, 1000);
        const auto an = AnalyzeSessionTranscript(pNoMarker, 0);
        CHECK(an.userMsgs.size() == 2, "AnalyzeSessionTranscript: with no leaf marker, every message is kept (legacy all-messages behavior)");

        std::error_code ecR;
        std::filesystem::remove(std::filesystem::path{ pRevert }, ecR);
        std::filesystem::remove(std::filesystem::path{ pNoMarker }, ecR);
        std::filesystem::remove_all(std::filesystem::path{ projectsDir }, ecR);
    }

    // --- Conversation lineage: in-file /compact splits into previous + current segments -----------
    // A /compact writes a system/compact_boundary (parentUuid:null => a NEW root, so the active leaf
    // chain STOPS there) + an isCompactSummary "continued from a previous conversation" bridge. The
    // pre-compaction turns stay in the file but OFF the active chain. The current Messages list is the
    // post-compaction segment (leaf-filtered, the summary bridge excluded); the pre-compaction segment
    // surfaces as a numbered "previous session". Mirrors the real session 3751c455.
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring base = std::wstring{ tmp } + L"am_compact_" + std::to_wstring(::GetCurrentProcessId());
        const std::string compacted =
            R"j({"type":"user","userType":"external","uuid":"o1","parentUuid":null,"message":{"content":"old prompt one"},"timestamp":"2026-06-26T09:00:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"o2","parentUuid":"o1","message":{"content":"old prompt two"},"timestamp":"2026-06-26T09:05:00.000Z"})j" "\n"
            R"j({"type":"system","subtype":"compact_boundary","uuid":"B","parentUuid":null,"logicalParentUuid":"o2","compactMetadata":{"trigger":"manual","preTokens":409797,"postTokens":5012},"timestamp":"2026-06-26T09:06:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","isCompactSummary":true,"uuid":"S","parentUuid":"B","message":{"content":"This session is being continued from a previous conversation..."},"timestamp":"2026-06-26T09:06:01.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"n1","parentUuid":"S","message":{"content":"new prompt one"},"timestamp":"2026-06-26T09:10:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"n2","parentUuid":"n1","message":{"content":"new prompt two"},"timestamp":"2026-06-26T09:15:00.000Z"})j" "\n"
            R"j({"type":"last-prompt","lastPrompt":"new prompt two","leafUuid":"n2"})j" "\n";

        // (1) the pure segment collector: 2 segments; seg0 = pre-compaction (labeled), seg1 = current.
        const auto segs = CollectConversationSegments(std::wstring{ compacted.begin(), compacted.end() });
        CHECK(segs.size() == 2, "CollectConversationSegments: a /compact boundary splits into 2 segments");
        CHECK(segs[0].userMsgs.size() == 2 && segs[0].userMsgs[0] == L"old prompt one" && segs[0].userMsgs[1] == L"old prompt two",
              "CollectConversationSegments: segment 0 = the pre-compaction prompts");
        CHECK(segs[0].label.find(L"compacted") != std::wstring::npos && segs[0].label.find(L"manual") != std::wstring::npos && segs[0].label.find(L"409k") != std::wstring::npos,
              "CollectConversationSegments: segment 0's label carries trigger + token counts");
        CHECK(segs[1].userMsgs.size() == 2 && segs[1].userMsgs[0] == L"new prompt one",
              "CollectConversationSegments: segment 1 = the current (post-compaction) prompts");

        // (2) AnalyzeSessionTranscript: current Messages = post-compaction only (leaf-filtered, summary
        // bridge excluded); the pre-compaction segment surfaces as exactly one previous session.
        const std::wstring pComp = base + L"_c.jsonl";
        MakeJsonl(pComp, compacted, 2000, 1000);
        const auto a = AnalyzeSessionTranscript(pComp, 0);
        CHECK(a.compacted, "AnalyzeSessionTranscript: a /compact session is flagged compacted");
        CHECK(a.userMsgs.size() == 2 && a.userMsgs[0] == L"new prompt one" && a.userMsgs[1] == L"new prompt two",
              "AnalyzeSessionTranscript: current Messages = post-compaction only (leaf-filtered; isCompactSummary bridge excluded)");
        bool leak = false;
        for (const auto& m : a.userMsgs)
        {
            if (m.find(L"continued from a previous") != std::wstring::npos)
            {
                leak = true;
            }
        }
        CHECK(!leak, "AnalyzeSessionTranscript: the isCompactSummary bridge does NOT leak into the Messages list");
        CHECK(a.previousSegments.size() == 1 && a.previousSegments[0].userMsgs.size() == 2 && a.previousSegments[0].userMsgs[0] == L"old prompt one",
              "AnalyzeSessionTranscript: the pre-compaction segment surfaces as one numbered previous session");

        // (3) a non-compacted session: one segment, not flagged, no previous.
        const std::string plain =
            R"j({"type":"user","userType":"external","uuid":"p1","parentUuid":null,"message":{"content":"hello there"},"timestamp":"2026-06-26T09:00:00.000Z"})j" "\n"
            R"j({"type":"last-prompt","leafUuid":"p1"})j" "\n";
        CHECK(CollectConversationSegments(std::wstring{ plain.begin(), plain.end() }).size() == 1, "CollectConversationSegments: a non-compacted session is one segment");
        const std::wstring pPlain = base + L"_p.jsonl";
        MakeJsonl(pPlain, plain, 2000, 1000);
        const auto ap = AnalyzeSessionTranscript(pPlain, 0);
        CHECK(!ap.compacted && ap.previousSegments.empty(), "AnalyzeSessionTranscript: a non-compacted session has no previous segments");

        std::error_code ec;
        std::filesystem::remove(std::filesystem::path{ pComp }, ec);
        std::filesystem::remove(std::filesystem::path{ pPlain }, ec);
    }

    // --- NormalizeRecapText: the one-true recap normalizer (shared by every recap reader) ----------
    CHECK(NormalizeRecapText(L"Did X. Next: Y. (disable recaps in /config)") == L"Did X. Next: Y.", "NormalizeRecapText: trailing disable hint + the space before it are stripped");
    CHECK(NormalizeRecapText(L"  spaced recap \n") == L"spaced recap", "NormalizeRecapText: surrounding whitespace/newlines trimmed");
    CHECK(NormalizeRecapText(L"no hint here") == L"no hint here", "NormalizeRecapText: a recap without the hint is unchanged");
    CHECK(NormalizeRecapText(L"(disable recaps in /config)") == L"", "NormalizeRecapText: a hint-only body normalizes to empty");
    CHECK(NormalizeRecapText(L"") == L"", "NormalizeRecapText: empty stays empty");

    // --- RecapFromTranscriptChunk: the PURE idle-recap extractor the observer's TAIL reader uses ------
    // The Fleet Observer reads an EXTERNAL session's recap from the transcript TAIL (the SAME region the
    // SessionScanner pulls a managed session's recap from — an external has no scanner cursor); this pure
    // helper does the per-chunk extraction (ReadTranscriptRecapTail = ReadFileTail + this). Tested with no
    // file IO. Mirrors ParseTranscriptDelta.recap / AnalyzeSessionTranscript semantics (shared
    // NormalizeRecapText, "last wins", "empty never clears"), PLUS the tail-specific tolerance of a
    // PARTIAL leading line (a tail read can begin mid-line).
    {
        const std::wstring one = LR"j({"type":"system","subtype":"away_summary","content":"Shipped the fix. Next: deploy. (disable recaps in /config)"})j" L"\n";
        CHECK(RecapFromTranscriptChunk(one) == L"Shipped the fix. Next: deploy.", "RecapFromTranscriptChunk: single away_summary captured + disable hint stripped");

        const std::wstring multi =
            std::wstring{ LR"j({"type":"system","subtype":"away_summary","content":"older recap"})j" } + L"\n" +
            LR"j({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"hi"}]}})j" + L"\n" +
            LR"j({"type":"system","subtype":"away_summary","content":"newer recap"})j" + L"\n";
        CHECK(RecapFromTranscriptChunk(multi) == L"newer recap", "RecapFromTranscriptChunk: the LAST away_summary in the chunk wins (newer supersedes)");

        CHECK(RecapFromTranscriptChunk(LR"j({"type":"assistant","message":{"stop_reason":"end_turn"}})j" L"\n").empty(),
              "RecapFromTranscriptChunk: a recap-less chunk yields \"\" (so an empty tail never clears a stored recap)");

        // A TAIL read can begin MID-LINE: the leading partial JSON fails json::Parse and is skipped, but
        // a COMPLETE away_summary after the first newline is still captured.
        const std::wstring partialLead =
            std::wstring{ LR"j(xt","text":"...a truncated assistant line from before the tail window..."}]}})j" } + L"\n" +
            LR"j({"type":"system","subtype":"away_summary","content":"recap after a partial leading line"})j" + L"\n";
        CHECK(RecapFromTranscriptChunk(partialLead) == L"recap after a partial leading line",
              "RecapFromTranscriptChunk: a partial leading line (tail starting mid-line) is skipped; a complete recap after it is captured");

        CHECK(RecapFromTranscriptChunk(L"").empty(), "RecapFromTranscriptChunk: empty chunk -> empty");
    }

    // --- LastActivityMsFromTranscriptChunk: line-derived last-activity (ignores untimestamped state) --
    // The Fleet Observer feeds SessionInfo.convLastActivityUnixMs from THIS, not the file mtime: a
    // `claude --resume` + a /model / permission-mode / shell-cwd change APPEND UNTIMESTAMPED trailer
    // lines (last-prompt/mode/permission-mode) that bump the file mtime WITHOUT being conversation
    // activity — so a restored tab focused after a restart would otherwise read "active just now" (it
    // only resumed; measured live: 7–32 h gaps between the last real line and the mtime). This is the
    // PURE extractor (no file IO): the NEWEST timestamp among non-meta/compact/sidechain user/assistant
    // lines. Mirrors TranscriptStore::QuickRowFacts (the Sessions browser's line-derived last-activity).
    {
        const std::wstring userEarly = LR"j({"type":"user","userType":"external","message":{"content":"hi"},"timestamp":"2026-02-01T10:00:00.000Z"})j" L"\n";
        const std::wstring asstLate = LR"j({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"done"}]},"timestamp":"2026-02-01T10:05:00.000Z"})j" L"\n";
        // The untimestamped trailer/state lines `claude --resume` and mode/permission changes append:
        const std::wstring trailers =
            LR"j({"type":"last-prompt","sessionId":"x"})j" L"\n"
            LR"j({"type":"mode","mode":"default","sessionId":"x"})j" L"\n"
            LR"j({"type":"permission-mode","permissionMode":"bypassPermissions","sessionId":"x"})j" L"\n";

        const int64_t both = LastActivityMsFromTranscriptChunk(userEarly + asstLate);
        const int64_t early = LastActivityMsFromTranscriptChunk(userEarly);
        const int64_t late = LastActivityMsFromTranscriptChunk(asstLate);
        CHECK(both > 0 && early > 0 && late > 0, "LastActivityMsFromTranscriptChunk: timestamped user/assistant lines yield a positive ms");
        CHECK(both == late, "LastActivityMsFromTranscriptChunk: the NEWEST conversation timestamp wins (assistant @10:05 > user @10:00)");
        CHECK(late > early, "LastActivityMsFromTranscriptChunk: a later ISO timestamp parses to a greater ms (sanity on the parse)");

        // THE BUG: the untimestamped resume/mode/permission trailer lines must NOT change the answer (the
        // file mtime would jump to resume-time; the line-derived value must stay at the last REAL line).
        CHECK(LastActivityMsFromTranscriptChunk(userEarly + asstLate + trailers) == both,
              "LastActivityMsFromTranscriptChunk: untimestamped resume/mode/permission trailer lines are IGNORED (the fix)");
        CHECK(LastActivityMsFromTranscriptChunk(trailers) == 0,
              "LastActivityMsFromTranscriptChunk: a chunk of only untimestamped state lines yields 0 (-> caller falls back to mtime)");

        // A fork copies its parent's tail VERBATIM (old stamps); a newer real line still wins (max, not last-seen).
        CHECK(LastActivityMsFromTranscriptChunk(asstLate + userEarly) == late,
              "LastActivityMsFromTranscriptChunk: newest wins even when an OLDER stamp appears last (fork-copied tail)");

        // The away_summary RECAP (type "system") is written WHILE idle (~5 min after the last real line):
        // it is NOT conversation activity, so a timestamped system line is excluded.
        const std::wstring recapSys = LR"j({"type":"system","subtype":"away_summary","content":"recap","timestamp":"2026-02-01T10:30:00.000Z"})j" L"\n";
        CHECK(LastActivityMsFromTranscriptChunk(userEarly + recapSys) == early,
              "LastActivityMsFromTranscriptChunk: a timestamped away_summary system line does NOT count as activity");

        // Meta / sidechain (subagent) lines are excluded too (mirrors ReadTranscriptInfo / ParseTranscriptDelta).
        const std::wstring metaLine = LR"j({"type":"user","isMeta":true,"message":{"content":"<command>"},"timestamp":"2026-02-01T11:00:00.000Z"})j" L"\n";
        const std::wstring sideLine = LR"j({"type":"assistant","isSidechain":true,"message":{"content":[{"type":"text","text":"sub"}]},"timestamp":"2026-02-01T11:00:00.000Z"})j" L"\n";
        CHECK(LastActivityMsFromTranscriptChunk(userEarly + metaLine) == early, "LastActivityMsFromTranscriptChunk: isMeta lines are skipped");
        CHECK(LastActivityMsFromTranscriptChunk(userEarly + sideLine) == early, "LastActivityMsFromTranscriptChunk: isSidechain (subagent) lines are skipped");

        // A TAIL read can begin MID-LINE: the partial leading JSON fails to parse and is skipped; a
        // complete conversation line after it is still captured.
        const std::wstring partialLead = std::wstring{ LR"j(...","text":"a truncated line"}]},"timestamp":"2020-01-01T00:00:00.000Z"})j" } + L"\n" + asstLate;
        CHECK(LastActivityMsFromTranscriptChunk(partialLead) == late,
              "LastActivityMsFromTranscriptChunk: a partial leading line (tail starting mid-line) is skipped; a complete line after it is captured");

        CHECK(LastActivityMsFromTranscriptChunk(L"") == 0, "LastActivityMsFromTranscriptChunk: empty chunk -> 0");
    }

    // --- AnalyzeSessionTranscript: the idle RECAP (away_summary) is captured into .awaySummary -------
    // The summary panel / Sessions detail / copyable Summary read SessionSummary.awaySummary. The LAST
    // away_summary wins (a session that went idle, came back, and went idle again has a fresher recap),
    // and the "(disable recaps in /config)" UI hint is stripped. A recap is NOT a user message — it must
    // never leak into the numbered Messages list.
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring pRecap = std::wstring{ tmp } + L"am_recap_" + std::to_wstring(::GetCurrentProcessId()) + L".jsonl";
        MakeJsonl(pRecap,
                  R"j({"type":"user","userType":"external","message":{"content":"start the work"},"timestamp":"2026-02-01T10:00:00.000Z"})j" "\n"
                  R"j({"type":"system","subtype":"away_summary","content":"Old recap. (disable recaps in /config)","timestamp":"2026-02-01T10:30:00.000Z"})j" "\n"
                  R"j({"type":"user","userType":"external","message":{"content":"keep going"},"timestamp":"2026-02-01T11:00:00.000Z"})j" "\n"
                  R"j({"type":"system","subtype":"away_summary","content":"We did the work; next is to ship it. (disable recaps in /config)","timestamp":"2026-02-01T11:30:00.000Z"})j" "\n",
                  1000, 1000);
        const auto a = AnalyzeSessionTranscript(pRecap, 0);
        CHECK(a.awaySummary == L"We did the work; next is to ship it.", "AnalyzeSessionTranscript: the LATEST away_summary wins + the disable hint is stripped");
        CHECK(a.userMsgs.size() == 2, "AnalyzeSessionTranscript: the two recap lines stay OUT of the Messages list (only the 2 typed prompts)");

        // The shared text box renders a "Recap:" section above the Messages, with the label INLINE with
        // the prose (one line, no wasted break) — "Recap: <text>", not "Recap:\n<text>".
        const auto box = RenderSessionSummaryBox(a, L"sid-recap", L"K:\\x", pRecap, L"claude --resume sid-recap", L"", L"", L"", /*full*/ true);
        CHECK(box.find(L"Recap: We did the work; next is to ship it.") != std::wstring::npos, "RenderSessionSummaryBox: the recap label is INLINE with the prose (one line, no break)");
        CHECK(box.find(L"Recap:") < box.find(L"start the work"), "RenderSessionSummaryBox: the Recap section sits ABOVE the numbered user messages");

        std::error_code ecR;
        std::filesystem::remove(std::filesystem::path{ pRecap }, ecR);
    }

    // --- the recap is shown in FULL — never length-capped (unlike the numbered messages, which cap at
    // 240 chars in this one-line box). A recap far longer than that cap renders whole, no "...".
    {
        SessionSummary big;
        big.found = true;
        big.userMsgs.push_back(std::wstring(500, L'M')); // a long MESSAGE: still capped at 240 (control)
        big.awaySummary = std::wstring(800, L'R') + L" RECAP_TAIL_MARKER"; // a long RECAP: shown whole
        const auto box = RenderSessionSummaryBox(big, L"id", L"K:\\x", L"t.jsonl", L"claude --resume id", L"", L"", L"", /*full*/ false);
        // Isolate the recap line: from "Recap: " up to the section separator before the Messages.
        const size_t rp = box.find(L"Recap: ");
        const size_t sepAfter = rp == std::wstring::npos ? std::wstring::npos : box.find(kSummarySepMark, rp);
        const std::wstring recapLine = rp == std::wstring::npos ? L"" : box.substr(rp, sepAfter == std::wstring::npos ? std::wstring::npos : sepAfter - rp);
        CHECK(recapLine.find(L"RECAP_TAIL_MARKER") != std::wstring::npos, "RenderSessionSummaryBox: a long recap (>240) is rendered in FULL (its tail survives)");
        CHECK(!recapLine.empty() && recapLine.find(L"...") == std::wstring::npos, "RenderSessionSummaryBox: the recap line carries NO '...' truncation");
        CHECK(box.find(L"MMM...") != std::wstring::npos || box.find(L"...") != std::wstring::npos, "RenderSessionSummaryBox: a long numbered MESSAGE is still capped (control — the cap applies to messages, not the recap)");
    }
}

// "YYYY\\MM\\DD" in LOCAL time for a unix-ms instant — mirrors ProcessInspect's CodexDayDirLocal
// (same Win32 conversion, so the date dir the test CREATES matches the one the resolver GLOBS).
static std::wstring TestCodexDayDir(int64_t unixMs)
{
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<uint64_t>(unixMs) * 10000ull + 116444736000000000ull;
    FILETIME ut;
    ut.dwLowDateTime = u.LowPart;
    ut.dwHighDateTime = u.HighPart;
    FILETIME lt{};
    ::FileTimeToLocalFileTime(&ut, &lt);
    SYSTEMTIME st{};
    ::FileTimeToSystemTime(&lt, &st);
    wchar_t buf[16]{};
    ::swprintf(buf, 16, L"%04u\\%02u\\%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

// Agentmaster Phase C1: Codex (OpenAI Codex CLI) observe-only enrichment. Covers the PURE
// primitives (ParseCodexFacts / CodexRolloutUuid / ParseCodexRolloutText) + the file-read
// (ReadCodexRolloutInfo) + the date-sharded resolution (ResolveCodexSessionIn) over a temp home.
void TestCodexObserve()
{
    std::wprintf(L"Codex (Phase C1) facts/uuid/rollout parse + resolution:\n");
    using Env = std::unordered_map<std::wstring, std::wstring>;

    // --- ParseCodexFacts: flags (--model/-m, --sandbox/-s, --ask-for-approval/-a), env, resume ---
    {
        CodexProcessFacts f;
        Env env{ { L"WT_SESSION", L"wt-9" }, { L"AM_SESSION", L"am-9" }, { L"CODEX_HOME", L"D:/cx" } };
        ParseCodexFacts(L"codex --model gpt-5.5 --sandbox danger-full-access --ask-for-approval never", env, f);
        CHECK(f.model == L"gpt-5.5", "codex facts model from --model");
        CHECK(f.sandbox == L"danger-full-access", "codex facts sandbox from --sandbox");
        CHECK(f.approvalMode == L"never", "codex facts approval from --ask-for-approval");
        CHECK(f.wtSession == L"wt-9" && f.amSession == L"am-9", "codex facts WT_SESSION + AM_SESSION");
        CHECK(f.codexHome == L"D:/cx", "codex facts CODEX_HOME from env");
        CHECK(f.resumeTarget.empty(), "fresh codex launch has no resume target");
    }
    {
        CodexProcessFacts f;
        ParseCodexFacts(L"codex -m o3 -s read-only -a on-request", {}, f);
        CHECK(f.model == L"o3", "codex facts model from -m");
        CHECK(f.sandbox == L"read-only", "codex facts sandbox from -s");
        CHECK(f.approvalMode == L"on-request", "codex facts approval from -a");
    }
    {
        CodexProcessFacts f;
        ParseCodexFacts(L"codex", {}, f); // bare: config.toml carries everything -> nothing on the cmdline
        CHECK(f.model.empty() && f.sandbox.empty() && f.approvalMode.empty() && f.resumeTarget.empty(), "bare codex cmdline -> empty parsed facts");
    }
    {
        CodexProcessFacts f;
        ParseCodexFacts(L"codex resume 019d8aa3-10a5-7273-ae65-a62cba4b63de", {}, f);
        CHECK(f.resumeTarget == L"019d8aa3-10a5-7273-ae65-a62cba4b63de", "codex `resume <guid>` -> authoritative resumeTarget");
        CodexProcessFacts g;
        ParseCodexFacts(L"codex resume --last", {}, g);
        CHECK(g.resumeTarget.empty(), "codex `resume --last` -> no explicit id (cwd discovery)");
    }

    // --- CodexRolloutUuid: the trailing 36-char UUIDv7 of a rollout stem ---
    CHECK(CodexRolloutUuid(L"rollout-2026-04-14T09-17-15-019d8aa3-10a5-7273-ae65-a62cba4b63de") == L"019d8aa3-10a5-7273-ae65-a62cba4b63de", "uuid from a real rollout stem");
    CHECK(CodexRolloutUuid(L"rollout-2026-04-14T09-17-15-not-a-guid-here-xxxx-xxxxxxxxxxxx").empty(), "non-guid tail -> empty");
    CHECK(CodexRolloutUuid(L"short").empty(), "too-short stem -> empty");

    // --- ParseCodexRolloutText: model/effort/sandbox/approval (turn_context), title + prompts
    //     (event_msg/user_message), and the AGENTS.md response_item user message is IGNORED ---
    {
        const std::string roll =
            R"({"type":"session_meta","payload":{"id":"019d8aa3-10a5-7273-ae65-a62cba4b63de","cwd":"K:/AmCodexTest/proj"}})" "\n"
            R"({"type":"turn_context","payload":{"model":"gpt-5.5","approval_policy":"never","sandbox_policy":{"type":"danger-full-access"},"collaboration_mode":{"settings":{"reasoning_effort":"xhigh"}}}})" "\n"
            R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"# AGENTS.md instructions for K:/AmCodexTest/proj"}]}})" "\n"
            R"({"type":"event_msg","payload":{"type":"user_message","message":"First real question\nsecond line"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"agent_message","message":"an answer"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"user_message","message":"Second question"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"task_complete","turn_id":"t","last_agent_message":"done"}})" "\n";
        const std::wstring wide(roll.begin(), roll.end()); // ASCII content -> safe narrow->wide
        CodexRolloutInfo info;
        ParseCodexRolloutText(wide, false, 100, info);
        CHECK(info.cwd == L"K:/AmCodexTest/proj", "rollout cwd from session_meta");
        CHECK(info.model == L"gpt-5.5", "rollout model from turn_context");
        CHECK(info.effort == L"xhigh", "rollout effort from collaboration_mode.settings.reasoning_effort");
        CHECK(info.sandbox == L"danger-full-access", "rollout sandbox from sandbox_policy.type");
        CHECK(info.approvalMode == L"never", "rollout approval from approval_policy");
        CHECK(info.title == L"First real question", "title = first user_message, first line");
        CHECK(info.userPrompts.size() == 2, "two human prompts (AGENTS.md response_item ignored)");
        CHECK(info.userPrompts.size() == 2 && info.userPrompts[0] == L"First real question\nsecond line", "first prompt full text");
        CHECK(info.userPrompts.size() == 2 && info.userPrompts[1] == L"Second question", "second prompt");
    }

    // --- ReadCodexRolloutInfo over a temp FILE (path-direct; validates read + timing wiring) ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring{ tmp } + L"am_codex_file_" + std::to_wstring(::GetCurrentProcessId());
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ root }, ec);
        const std::wstring path = root + L"\\rollout-2025-06-15T10-00-00-019d8aa3-10a5-7273-ae65-a62cba4b63de.jsonl";
        const std::string roll =
            R"({"type":"session_meta","payload":{"cwd":"K:/AmCodexTest/proj"}})" "\n"
            R"({"type":"turn_context","payload":{"model":"gpt-5.5"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"user_message","message":"hello codex"}})" "\n";
        MakeJsonl(path, roll, 7000, 3000);
        const auto info = ReadCodexRolloutInfo(path, 0, 100);
        CHECK(info.found, "ReadCodexRolloutInfo found the file");
        CHECK(info.createdUnixMs == 3000 && info.lastActivityUnixMs == 7000, "rollout ctime/mtime");
        CHECK(info.title == L"hello codex" && info.model == L"gpt-5.5", "rollout title + model from file");
        CHECK(!ReadCodexRolloutInfo(root + L"\\nope.jsonl", 0, 100).found, "missing rollout -> not found");
        std::filesystem::remove_all(std::filesystem::path{ root }, ec);
    }

    // --- ResolveCodexSessionIn over a temp CODEX_HOME (date-sharded glob + cwd-confirm + pick) ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring home = std::wstring{ tmp } + L"am_codex_home_" + std::to_wstring(::GetCurrentProcessId());
        const int64_t startMs = 1750000000000LL; // a fixed instant; the day dir is derived locally from it
        const std::wstring sdir = home + L"\\sessions\\" + TestCodexDayDir(startMs);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ sdir }, ec);
        const std::string metaMine = R"({"type":"session_meta","payload":{"cwd":"K:/AmCodexTest/proj"}})" "\n";
        const std::string metaOther = R"({"type":"session_meta","payload":{"cwd":"K:/Other/dir"}})" "\n";
        const std::wstring uuidMine = L"019d8aa3-10a5-7273-ae65-a62cba4b63de";
        const std::wstring uuidOld = L"019d0000-0000-7000-8000-000000000000";
        const std::wstring uuidOther = L"019dffff-ffff-7fff-bfff-ffffffffffff";
        MakeJsonl(sdir + L"\\rollout-x-" + uuidMine + L".jsonl", metaMine, startMs, startMs); // ctime≈start, my cwd
        MakeJsonl(sdir + L"\\rollout-x-" + uuidOld + L".jsonl", metaMine, startMs - 3600000, startMs - 3600000); // older, my cwd
        MakeJsonl(sdir + L"\\rollout-x-" + uuidOther + L".jsonl", metaOther, startMs + 5000, startMs + 5000); // diff cwd

        const auto sess = ResolveCodexSessionIn(home, L"K:/AmCodexTest/proj", startMs);
        CHECK(sess.sessionId == uuidMine, "resolve: rollout created at my start, in my cwd (ctime≈start identity)");
        CHECK(sess.rolloutPath.find(uuidMine) != std::wstring::npos, "resolve returns the rollout path");
        CHECK(sess.createdUnixMs == startMs, "resolve carries the rollout ctime");

        // resume fallback: start far past every rollout's ctime -> newest-mtime IN my cwd (mine).
        const auto sessR = ResolveCodexSessionIn(home, L"K:/AmCodexTest/proj", startMs + 100000000LL);
        CHECK(sessR.sessionId == uuidMine, "resume fallback: newest-mtime rollout in cwd when none matches start");

        CHECK(ResolveCodexSessionIn(home, L"K:/Nope/x", startMs).sessionId.empty(), "no rollout in cwd -> empty session");
        CHECK(ResolveCodexSessionIn(L"", L"K:/AmCodexTest/proj", startMs).sessionId.empty(), "empty home -> empty session");

        // ResolveCodexRolloutPathIn: find a rollout by its known uuid (the explicit-resume path).
        const auto byId = ResolveCodexRolloutPathIn(home, uuidOther);
        CHECK(byId.find(uuidOther) != std::wstring::npos, "ResolveCodexRolloutPathIn finds a rollout by uuid");
        CHECK(ResolveCodexRolloutPathIn(home, L"019dded0-0000-7000-8000-000000000000").empty(), "ResolveCodexRolloutPathIn: unknown uuid -> empty");

        std::filesystem::remove_all(std::filesystem::path{ home }, ec);
    }

    // ===== Phase C2: rollout-tail turn state (Running / Waiting / Idle) =====================
    std::wprintf(L"Codex (Phase C2) turn-state from rollout tail:\n");

    // --- ClassifyCodexLine: the pure per-line turn-boundary verdict ---
    {
        auto cl = [](const std::string& s) { return ClassifyCodexLine(std::wstring(s.begin(), s.end())); };
        CHECK(cl(R"({"type":"event_msg","payload":{"type":"task_started","turn_id":"t1"}})").isBoundary &&
                  cl(R"({"type":"event_msg","payload":{"type":"task_started"}})").state == CodexState::Running,
              "task_started -> Running boundary");
        {
            const auto b = cl(R"({"type":"event_msg","payload":{"type":"task_complete","last_agent_message":"all done"}})");
            CHECK(b.isBoundary && b.state == CodexState::Waiting, "task_complete -> Waiting boundary");
            CHECK(b.lastAgentMessage == L"all done", "task_complete carries last_agent_message");
        }
        CHECK(cl(R"({"type":"event_msg","payload":{"type":"turn_aborted","reason":"interrupted"}})").state == CodexState::Waiting, "turn_aborted -> Waiting");
        CHECK(cl(R"({"type":"event_msg","payload":{"type":"thread_rolled_back","num_turns":1}})").state == CodexState::Waiting, "thread_rolled_back -> Waiting");
        CHECK(!cl(R"({"type":"event_msg","payload":{"type":"user_message","message":"hi"}})").isBoundary, "user_message is not a turn boundary");
        CHECK(!cl(R"({"type":"event_msg","payload":{"type":"token_count"}})").isBoundary, "token_count is not a boundary");
        CHECK(!cl(R"({"type":"response_item","payload":{"type":"function_call"}})").isBoundary, "response_item is not a boundary");
        CHECK(!cl("not json at all").isBoundary, "non-JSON line -> no boundary");
        CHECK(!cl("").isBoundary, "empty line -> no boundary");
    }

    // --- ReadCodexStateDelta: forward byte-cursor + first-sight tail-seek + partial-line safety ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring{ tmp } + L"am_codex_state_" + std::to_wstring(::GetCurrentProcessId());
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ root }, ec);
        const std::wstring path = root + L"\\rollout-state.jsonl";
        const auto writeFile = [](const std::wstring& p, const std::string& b) {
            const HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return;
            }
            DWORD wr = 0;
            ::WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &wr, nullptr);
            ::CloseHandle(h);
        };

        const std::string turn1 =
            R"({"type":"session_meta","payload":{"cwd":"K:/x"}})" "\n"
            R"({"type":"turn_context","payload":{"model":"gpt-5.5"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"task_started","turn_id":"t1"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"agent_message","message":"working"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"task_complete","turn_id":"t1","last_agent_message":"done 1"}})" "\n";
        writeFile(path, turn1);
        int64_t off = 0;
        std::wstring lastMsg;
        CHECK(ReadCodexStateDelta(path, off, CodexState::Unknown, &lastMsg) == CodexState::Waiting, "first read: tail ends on task_complete -> Waiting");
        CHECK(lastMsg == L"done 1", "delta surfaces last_agent_message");
        CHECK(off == static_cast<int64_t>(turn1.size()), "cursor advanced to EOF (whole file consumed)");

        const int64_t offAtEof = off;
        CHECK(ReadCodexStateDelta(path, off, CodexState::Waiting) == CodexState::Waiting && off == offAtEof, "no new bytes -> sticky state, cursor steady");

        // A new turn OPENS (append task_started, no complete) -> Running, reading only the delta.
        const std::string turn2open = turn1 +
            R"({"type":"event_msg","payload":{"type":"task_started","turn_id":"t2"}})" "\n"
            R"({"type":"event_msg","payload":{"type":"agent_message","message":"thinking"}})" "\n";
        writeFile(path, turn2open);
        CHECK(ReadCodexStateDelta(path, off, CodexState::Waiting) == CodexState::Running, "appended task_started (open turn) -> Running");

        const std::string turn2done = turn2open +
            R"({"type":"event_msg","payload":{"type":"task_complete","turn_id":"t2","last_agent_message":"done 2"}})" "\n";
        writeFile(path, turn2done);
        CHECK(ReadCodexStateDelta(path, off, CodexState::Running) == CodexState::Waiting, "appended task_complete -> Waiting");

        // First-sight (offset 0) tail-seek: a fresh reader derives state from the file end.
        int64_t offRest = 0;
        CHECK(ReadCodexStateDelta(path, offRest, CodexState::Unknown) == CodexState::Waiting, "fresh reader, at-rest file -> Waiting");
        writeFile(path, turn2open); // a file whose last boundary is an OPEN turn
        int64_t offMid = 0;
        CHECK(ReadCodexStateDelta(path, offMid, CodexState::Unknown) == CodexState::Running, "fresh reader, mid-turn file -> Running");

        // A partial trailing line (append in flight, no newline) is NOT consumed — the last COMPLETE
        // boundary wins, and the cursor stops before the partial so the next read re-sees it whole.
        writeFile(path, turn2done + R"({"type":"event_msg","payload":{"type":"task_started")"); // truncated, no newline
        int64_t offPartial = 0;
        CHECK(ReadCodexStateDelta(path, offPartial, CodexState::Unknown) == CodexState::Waiting, "partial trailing line ignored -> last complete boundary (Waiting) wins");

        std::filesystem::remove_all(std::filesystem::path{ root }, ec);
    }
}

void TestProcessInspectLive()
{
    std::wprintf(L"ProcessInspect live PEB self-read (our own process):\n");
    const uint32_t self = ::GetCurrentProcessId();

    const auto snap = SnapshotProcesses();
    CHECK(!snap.empty(), "SnapshotProcesses returns a non-empty census");
    bool foundSelf = false;
    std::wstring selfImage;
    for (const auto& e : snap)
    {
        if (e.pid == self)
        {
            foundSelf = true;
            selfImage = e.image;
        }
    }
    CHECK(foundSelf, "snapshot contains our own pid");
    CHECK(foundSelf && ImageNameEq(selfImage, L"m5_tests.exe"), "snapshot image leaf for self is m5_tests.exe");

    const auto cwd = ReadProcessCwd(self);
    CHECK(!cwd.empty(), "ReadProcessCwd(self) non-empty");
    wchar_t cur[MAX_PATH]{};
    const DWORD n = ::GetCurrentDirectoryW(MAX_PATH, cur);
    std::wstring curw(cur, n);
    while (!curw.empty() && (curw.back() == L'\\' || curw.back() == L'/'))
    {
        curw.pop_back();
    }
    CHECK(NormDirKey(cwd) == NormDirKey(curw), "ReadProcessCwd(self) matches GetCurrentDirectory");

    const auto cl = ReadProcessCommandLine(self);
    CHECK(!cl.empty(), "ReadProcessCommandLine(self) non-empty");
    CHECK(cl.find(L"m5_tests") != std::wstring::npos, "command line carries our exe name");

    const auto env = ReadProcessEnv(self);
    CHECK(!env.empty(), "ReadProcessEnv(self) non-empty");
    CHECK(!EnvLookup(env, L"SystemRoot").empty(), "live env carries SystemRoot");
    CHECK(!EnvLookup(env, L"systemroot").empty(), "EnvLookup case-insensitive against a live env");

    CHECK(ProcessStartUnixMs(self) > 0, "ProcessStartUnixMs(self) > 0");
    CHECK(ProcessAlive(self), "ProcessAlive(self) true");
    CHECK(!ProcessAlive(0), "ProcessAlive(0) false");

    // Full facts read for self (exercises the OS read + parse path end-to-end). We can't assert
    // claude-specific fields, but the PEB-derived ones must be populated.
    const auto facts = ReadClaudeFacts(self);
    CHECK(facts.pid == self, "ReadClaudeFacts carries the pid");
    CHECK(!facts.cwd.empty() && !facts.commandline.empty(), "ReadClaudeFacts filled cwd + commandline from the PEB");
    CHECK(facts.startUnixMs > 0, "ReadClaudeFacts filled start time");
}

void TestBringToFrontHeuristics()
{
    std::wprintf(L"Bring Window To Front heuristics (PathLeaf + tab scorers, PURE):\n");

    // --- PathLeaf: last segment, separator-agnostic, trailing-slash tolerant ---
    CHECK(PathLeaf(L"K:\\source\\Agentmaster") == L"Agentmaster", "PathLeaf backslash path");
    CHECK(PathLeaf(L"K:\\source\\Agentmaster\\") == L"Agentmaster", "PathLeaf trailing backslash");
    CHECK(PathLeaf(L"K:/source/api/") == L"api", "PathLeaf forward slashes");
    CHECK(PathLeaf(L"api") == L"api", "PathLeaf bare leaf");
    CHECK(PathLeaf(L"").empty(), "PathLeaf empty");

    // --- ScoreClaudeTabName tiers: exact (100) > claude word (90) > glyph (80) > containment (70)
    //     > title head (60) > cwd leaf (40) ---
    CHECK(ScoreClaudeTabName(L"npyiter perf?", L"npyiter perf?", L"") == 100, "tab score: exact title match");
    CHECK(ScoreClaudeTabName(L"  npyiter perf? ", L"npyiter PERF?", L"") == 100, "tab score: exact is trimmed + case-insensitive");
    CHECK(ScoreClaudeTabName(L"Claude Code", L"", L"") == 90, "tab score: the word claude");
    CHECK(ScoreClaudeTabName(L"my CLAUDE session", L"", L"") == 90, "tab score: claude case-insensitive");
    CHECK(ScoreClaudeTabName(L"\x2733 Fixing the build", L"", L"") == 80, "tab score: leading OSC status glyph");
    CHECK(ScoreClaudeTabName(L"resume flow", L"the resume flow", L"") == 70, "tab score: name contained in hint");
    CHECK(ScoreClaudeTabName(L"[1] the resume flow (wt)", L"the resume flow", L"") == 70, "tab score: hint contained in name");
    CHECK(ScoreClaudeTabName(L"fixing the build error", L"Fixing the build error in CI", L"") == 70, "tab score: name is a prefix of the hint (containment)");
    CHECK(ScoreClaudeTabName(L"\x2734 fixing the build error CI-side", L"Fixing the build error in CI", L"") == 60, "tab score: title head past a foreign prefix");
    CHECK(ScoreClaudeTabName(L"short", L"shor", L"") == 0, "tab score: tiny hints never match");
    CHECK(ScoreClaudeTabName(L"ELI: Agentmaster", L"", L"Agentmaster") == 40, "tab score: cwd leaf");
    CHECK(ScoreClaudeTabName(L"PowerShell", L"a long enough hint", L"src") == 0, "tab score: no signal");
    CHECK(ScoreClaudeTabName(L"", L"whatever hint", L"dir") == 0, "tab score: empty name");
    // priority: a claude-word name keeps 90 even when weaker tiers also match
    CHECK(ScoreClaudeTabName(L"claude \x2014 Agentmaster", L"claude \x2014 Agentmaster and more", L"Agentmaster") == 90, "tab score: strongest signal wins");

    // --- ScoreTabNameTokens: hand-renamed tab labels vs the conversation corpus (whole words) ---
    const auto corpus = TokenizeTextLower(L"Fix the --resume path so the archived session restores; also the npyiter migration plan.");
    CHECK(ScoreTabNameTokens(L"am resume", corpus) == 50, "token overlap: every >=3-char token present ('am' dropped as noise)");
    CHECK(ScoreTabNameTokens(L"npyiter migration", corpus) == 50, "token overlap: two tokens, both present");
    CHECK(ScoreTabNameTokens(L"am runner", corpus) == 0, "token overlap: one missing token kills the match");
    CHECK(ScoreTabNameTokens(L"npyiter perf", corpus) == 0, "token overlap: all-or-nothing");
    CHECK(ScoreTabNameTokens(L"resumes", corpus) == 0, "token overlap: longer-than-corpus-word is a miss ('resumes' !~ 'resume')");
    CHECK(ScoreTabNameTokens(L"am gh act", TokenizeTextLower(L"do we have gh actions publishes?")) == 50, "token overlap: tab token as a PREFIX of a corpus word (act ~ actions)");
    CHECK(ScoreTabNameTokens(L"gh am", corpus) == 0, "token overlap: only short tokens -> no signal");
    CHECK(ScoreTabNameTokens(L"am resume", {}) == 0, "token overlap: empty corpus");
    CHECK(TokenizeTextLower(L"").empty(), "tokenize: empty text");
    CHECK(TokenizeTextLower(L"a bb ccc").size() == 1, "tokenize: drops tokens under 3 chars");

    // --- PickClaudeTab: the full per-window pick (subset disqualifier + head/full corpus tiers +
    //     unique guard), over the real-world shape that motivated it: a WT window of hand-renamed
    //     claude tabs. ---
    {
        const std::vector<std::wstring> tabs{
            L"npyiter test all x all", L"npyiter perf?", L"npyiter multithread",
            L"npyiter migrate", L"am resume", L"npyiter pr"
        };
        int score = 0;
        bool unique = false;
        // The resume conversation: its tab's one meaningful token is unique among the tabs.
        auto corpus = TokenizeTextLower(L"please fix the --resume path so archived sessions restore");
        int idx = PickClaudeTab(tabs, {}, L"", corpus, corpus, score, unique);
        CHECK(idx == 4 && score == 50 && unique, "pick: token-unique renamed tab wins");
        // The perf conversation mentions npyiter but never 'perf' — the generic-prefix tab
        // ("npyiter pr" == {npyiter}, a strict subset of its siblings) must NOT win by tokens.
        corpus = TokenizeTextLower(L"Can NpyIter make matmul or dot or argsort faster?");
        idx = PickClaudeTab(tabs, {}, L"", corpus, corpus, score, unique);
        CHECK(score == 0, "pick: generic-prefix tab disqualified (strict subset of a sibling)");
        // An exact title hint wins outright.
        idx = PickClaudeTab(tabs, { L"npyiter migrate" }, L"", {}, {}, score, unique);
        CHECK(idx == 3 && score == 100 && unique, "pick: exact title hint wins uniquely");
        // Tied scorers are reported non-unique (the caller must not select).
        const std::vector<std::wstring> twins{ L"PowerShell", L"PowerShell", L"claude one", L"claude two" };
        idx = PickClaudeTab(twins, {}, L"", {}, {}, score, unique);
        CHECK(idx == 2 && score == 90 && !unique, "pick: tied claude tabs -> not unique");
        // Equal token sets stay eligible and tie out (vs the strict-subset disqualifier).
        const std::vector<std::wstring> same{ L"npyiter perf", L"npyiter perf?" };
        corpus = TokenizeTextLower(L"npyiter perf work");
        idx = PickClaudeTab(same, {}, L"", corpus, corpus, score, unique);
        CHECK(score == 50 && !unique, "pick: equal token sets tie (not subset-disqualified)");
        // Head corpus (title + FIRST prompt) outranks the full corpus: a tab named for the
        // conversation's PURPOSE ("am gh act" ~ "gh actions" in the first prompt) beats a tab
        // matching an incidental later-prompt word ("am changes" ~ "changes" said much later).
        const std::vector<std::wstring> mixed{ L"am gh act", L"am changes" };
        const auto head = TokenizeTextLower(L"Do we have publishes in the gh actions?");
        const auto full = TokenizeTextLower(L"Do we have publishes in the gh actions? later: apply the changes to the workflow");
        idx = PickClaudeTab(mixed, {}, L"", head, full, score, unique);
        CHECK(idx == 0 && score == 50 && unique, "pick: head-corpus match (50) outranks full-corpus match (45)");
    }

    // --- ReadTranscriptInfoIn: custom-title lines (the LAST one wins) ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring{ tmp } + L"am_bringfront_test_" + std::to_wstring(::GetCurrentProcessId());
        const std::wstring projRoot = root + L"\\projects";
        const std::wstring cwd = L"C:\\AmBringFront\\proj";
        const std::wstring dir = projRoot + L"\\" + EncodeCwdToProjectDir(cwd);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ dir }, ec);
        const std::string content =
            "{\"type\":\"custom-title\",\"customTitle\":\"old label\",\"sessionId\":\"ct\"}\n"
            "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"hello world prompt\"},\"gitBranch\":\"main\"}\n"
            "{\"type\":\"custom-title\",\"customTitle\":\"am resume\",\"sessionId\":\"ct\"}\n";
        MakeJsonl(dir + L"\\ct.jsonl", content, 5000, 1000);
        const auto ti = ReadTranscriptInfoIn(projRoot, cwd, L"ct", 0, 10);
        CHECK(ti.found, "custom-title: transcript read");
        CHECK(ti.customTitle == L"am resume", "custom-title: the LAST custom-title line wins");
        CHECK(ti.title == L"hello world prompt", "custom-title does not displace the first-prompt title field");
        CHECK(ti.gitBranch == L"main", "custom-title lines don't break gitBranch capture");
        std::filesystem::remove_all(std::filesystem::path{ root }, ec);
    }
}

void TestTranscriptStore()
{
    std::wprintf(L"TranscriptStore (enumerate + classify + stats + quick facts):\n");

    // --- IsSessionIdStem: the uuid filename filter (excludes legacy agent-* files) ---
    CHECK(IsSessionIdStem(L"3f98f88e-b122-41b5-9048-4b93a39fe5fa"), "uuid stem accepted");
    CHECK(!IsSessionIdStem(L"agent-a05bb6b"), "legacy agent-* stem rejected");
    CHECK(!IsSessionIdStem(L"3f98f88e-b122-41b5-9048-4b93a39fe5f"), "35 chars rejected");
    CHECK(!IsSessionIdStem(L"3f98f88e-b122-41b5-9048_4b93a39fe5fa"), "wrong separator rejected");

    // --- ParseTranscriptTimestamp: ISO-Z (the entire recent corpus) + epoch strata ---
    CHECK(ParseTranscriptTimestamp(L"1970-01-01T00:00:00.000Z") == 0, "epoch-zero ISO -> 0 (degenerate, treated as unset)");
    CHECK(ParseTranscriptTimestamp(L"2026-06-10T00:00:00Z") == 1781049600000LL, "ISO without millis");
    CHECK(ParseTranscriptTimestamp(L"2026-06-10T00:00:00.250Z") == 1781049600250LL, "ISO with millis");
    CHECK(ParseTranscriptTimestamp(L"2026-06-10T00:00:00.2Z") == 1781049600200LL, "ISO with 1-digit fraction scales to ms");
    CHECK(ParseTranscriptTimestamp(L"1781049600000") == 1781049600000LL, "pure-digit epoch ms");
    CHECK(ParseTranscriptTimestamp(L"1781049600") == 1781049600000LL, "pure-digit epoch seconds -> ms");
    CHECK(ParseTranscriptTimestamp(L"garbage") == 0, "garbage -> 0");
    CHECK(ParseTranscriptTimestamp(L"") == 0, "empty -> 0");

    // --- IsNoiseUserPrompt: the §6a control-marker rule set ---
    CHECK(IsNoiseUserPrompt(L"<command-name>/clear</command-name>"), "command-name echo is noise");
    CHECK(IsNoiseUserPrompt(L"  <command-message>x</command-message>"), "command-message (leading ws) is noise");
    CHECK(IsNoiseUserPrompt(L"<local-command-stdout>x"), "local-command wrapper is noise");
    CHECK(IsNoiseUserPrompt(L"<bash-input>ls</bash-input>"), "bash-input echo is noise");
    CHECK(IsNoiseUserPrompt(L"<task-notification>done</task-notification>"), "task-notification is noise");
    CHECK(IsNoiseUserPrompt(L"<bash-notification>\n<shell-id>b92b2f3</shell-id>"), "bash-notification (background-shell wake) is noise");
    CHECK(IsNoiseUserPrompt(L"<system-reminder>r</system-reminder>"), "system-reminder is noise");
    CHECK(IsNoiseUserPrompt(L"Caveat: the messages below were generated"), "Caveat preamble is noise");
    CHECK(IsNoiseUserPrompt(L"[Request interrupted by user]"), "interrupt marker is noise");
    CHECK(IsNoiseUserPrompt(L"[Request interrupted by user for tool use]"), "tool interrupt marker is noise");
    CHECK(!IsNoiseUserPrompt(L"fix the build please"), "a real prompt is NOT noise");
    CHECK(!IsNoiseUserPrompt(L"explain <command-name> semantics"), "marker NOT at start is not noise");
    // Agent/teammate wrappers across ALL delivery strata (corpus-audited: 486 bare <teammate-message>
    // deliveries in the older strata + 21 preambled in the newer; <agent-message from=...> and
    // <task-notification> both fingerprinted as UserPromptSubmit texts in the live registries):
    CHECK(IsNoiseUserPrompt(L"<teammate-message teammate_id=\"ingestion\" color=\"green\">\n{\"type\":\"idle_notification\"}"), "BARE teammate-message delivery (older strata, no preamble) is noise");
    CHECK(IsNoiseUserPrompt(L"<agent-message from=\"finder-reuse\">\n[{\"file\": \"x.h\"}]"), "agent-message wrapper (a background Agent reporting back) is noise");
    CHECK(IsNoiseUserPrompt(L"Another Claude session sent a message:\n<agent-message from=\"finder-conventions\">\n[{\"file\": \"y.cpp\"}]"), "PREAMBLED agent-message delivery (the TRANSCRIPT-row shape of the same wire delivery; corpus 2/2 preambled) is noise");
    CHECK(!IsNoiseUserPrompt(L"the <agent-message from=...> wrapper should be filtered"), "agent-message mentioned mid-text is a real prompt");

    // --- Teammate (multi-agent) protocol vs real teammate content (session b2da261d repro) ---
    // Claude Code wraps a peer session's message as "Another Claude session sent a message:" + a
    // <teammate-message> block. A PROTOCOL envelope ({"type":"idle_notification",...}) is machine
    // signaling; a real REPORT (prose payload) is content. The two filters treat them differently:
    //   - IsNoiseUserPrompt (titles / prompt-lists): NEITHER is a user prompt => BOTH are noise (the
    //     preamble prefix fires on the newer strata; the bare "<teammate-message" prefix carries the
    //     OLDER strata, whose deliveries have no preamble — 486 such in the corpus audit).
    //   - SeIsCommandNoise (summary digest): drops ONLY the protocol envelope, KEEPS the real report.
    const std::wstring kTeammateIdle =
        L"Another Claude session sent a message:\n"
        L"<teammate-message teammate_id=\"audit-structure\" color=\"blue\">\n"
        L"{\"type\":\"idle_notification\",\"from\":\"audit-structure\",\"timestamp\":\"2026-06-27T11:13:29.741Z\",\"idleReason\":\"available\"}\n"
        L"</teammate-message>\n\nThis came from another Claude session - not typed by your user.";
    const std::wstring kTeammateReport =
        L"Another Claude session sent a message:\n"
        L"<teammate-message teammate_id=\"audit-cla\" color=\"green\" summary=\"Full CLA audit report delivered\">\n"
        L"# CLA Audit - Agentmaster\n\nThe draft is structurally sound for dual-licensing.\n"
        L"</teammate-message>";
    CHECK(SeIsCommandNoise(kTeammateIdle), "summary: a teammate idle_notification PROTOCOL envelope is noise");
    CHECK(!SeIsCommandNoise(kTeammateReport), "summary: a real teammate REPORT (prose payload) is KEPT");
    CHECK(!SeIsCommandNoise(L"here is json: {\"type\":\"x\"} thoughts?"), "summary: a plain prompt mentioning a JSON type (no teammate wrapper) is NOT noise");
    CHECK(IsNoiseUserPrompt(kTeammateIdle), "prompt-list: the teammate preamble (idle_notification) is noise");
    CHECK(IsNoiseUserPrompt(kTeammateReport), "prompt-list: the teammate preamble (report) is noise too - not a user prompt");

    // --- PickDisplayTitle precedence ---
    CHECK(PickDisplayTitle(L"c", L"a", L"s", L"f") == L"c", "customTitle wins");
    CHECK(PickDisplayTitle(L"", L"a", L"s", L"f") == L"a", "aiTitle second");
    CHECK(PickDisplayTitle(L"", L"", L"s", L"f") == L"s", "legacy summary third");
    CHECK(PickDisplayTitle(L"", L"", L"", L"f") == L"f", "first prompt last");

    // --- ClassifyTranscriptLine: canned lines from the real schema ---
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"user","timestamp":"2026-06-10T00:00:00Z","cwd":"K:\\x","gitBranch":"main","message":{"role":"user","content":"hello there"}})", 100, 100);
        CHECK(f.kind == TranscriptLineKind::UserPrompt && !f.meta, "user string content -> REAL UserPrompt");
        CHECK(f.userText == L"hello there", "userText extracted");
        CHECK(f.timestampMs == 1781049600000LL, "timestamp parsed");
        CHECK(f.cwd == L"K:\\x" && f.gitBranch == L"main", "cwd + gitBranch extracted");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"user","message":{"content":"[Request interrupted by user]"}})", 100, 100);
        CHECK(f.kind == TranscriptLineKind::UserPrompt && f.meta && f.userText.empty(), "interrupt marker -> meta, no userText");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"user","isCompactSummary":true,"message":{"content":"This session is being continued"}})", 100, 100);
        CHECK(f.meta, "isCompactSummary -> meta");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"42 passed"}]},"toolUseResult":{"stdout":"ok!","stderr":""}})", 100, 100);
        CHECK(f.kind == TranscriptLineKind::UserToolResult, "tool_result blocks -> UserToolResult");
        CHECK(f.agentText.find(L"42 passed") != std::wstring::npos, "tool_result content in agentText");
        CHECK(f.agentText.find(L"ok!") != std::wstring::npos, "toolUseResult stdout in agentText");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"assistant","timestamp":"2026-06-10T00:00:01Z","message":{"role":"assistant","content":[{"type":"thinking","thinking":"hmm secret"},{"type":"text","text":"done."},{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"git log"}},{"type":"tool_use","id":"t2","name":"Read","input":{"file_path":"a.cpp"}}]}})", 0, 200);
        CHECK(f.kind == TranscriptLineKind::Assistant, "assistant line classified");
        CHECK(f.toolUses == 2, "two tool_use blocks counted");
        CHECK(f.agentText.find(L"done.") != std::wstring::npos && f.agentText.find(L"hmm secret") != std::wstring::npos, "text + thinking in agentText");
        CHECK(f.agentText.find(L"Bash") != std::wstring::npos && f.agentText.find(L"git log") != std::wstring::npos, "tool name + input in agentText");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"assistant","isSidechain":true,"timestamp":"2026-06-10T00:00:02Z","message":{"content":[{"type":"text","text":"sub"}]}})", 0, 50);
        CHECK(f.sidechain, "isSidechain flagged");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"custom-title","customTitle":"am resume"})");
        CHECK(f.kind == TranscriptLineKind::CustomTitle && f.title == L"am resume", "custom-title payload");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"ai-title","aiTitle":"Fixing the build"})");
        CHECK(f.kind == TranscriptLineKind::AiTitle && f.title == L"Fixing the build", "ai-title payload");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"summary","summary":"Legacy summary","leafUuid":"x"})");
        CHECK(f.kind == TranscriptLineKind::Summary && f.title == L"Legacy summary", "legacy summary payload");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"user","forkedFrom":{"sessionId":"f100e8de-b6c5-4fb8-bbe3-9f98eb63ab11","messageUuid":"m1"},"message":{"content":"copied"}})", 50, 0);
        CHECK(f.forkedFromId == L"f100e8de-b6c5-4fb8-bbe3-9f98eb63ab11", "forkedFrom.sessionId extracted");
    }
    {
        const auto f = ClassifyTranscriptLine(LR"({"type":"system","subtype":"away_summary","timestamp":"2026-06-10T01:00:00Z","content":"While you were away..."})", 0, 100);
        CHECK(f.kind == TranscriptLineKind::System && f.agentText.find(L"away") != std::wstring::npos, "system content in agentText");
    }
    {
        const auto f = ClassifyTranscriptLine(L"not json at all");
        CHECK(f.kind == TranscriptLineKind::Other, "non-JSON line tolerated as Other");
    }
    {
        // Caps respected: a 100-char budget truncates, 0 budget extracts nothing.
        const std::wstring big(500, L'x');
        const auto f = ClassifyTranscriptLine(LR"({"type":"user","message":{"content":")" + big + LR"("}})", 100, 0);
        CHECK(f.userText.size() <= 100, "maxUserTextChars cap respected");
        const auto f0 = ClassifyTranscriptLine(LR"({"type":"user","message":{"content":"hi"}})", 0, 0);
        CHECK(f0.userText.empty(), "0 budget -> no text extraction");
    }

    // --- Enumerate + Scan + Stats + QuickFacts over a temp projects root ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring{ tmp } + L"am_store_test_" + std::to_wstring(::GetCurrentProcessId());
        const std::wstring projRoot = root + L"\\projects";
        const std::wstring dirA = projRoot + L"\\K--source-AmStoreA";
        const std::wstring dirB = projRoot + L"\\K--source-AmStoreB";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ dirA + L"\\11111111-1111-1111-1111-111111111111" }, ec); // a <sid>/ subdir (subagents home) — must NOT enumerate
        std::filesystem::create_directories(std::filesystem::path{ dirB }, ec);

        const std::string sessA = // a realistic mini-transcript
            "{\"type\":\"permission-mode\",\"permissionMode\":\"bypassPermissions\"}\n"
            "{\"type\":\"user\",\"timestamp\":\"2026-06-01T10:00:00Z\",\"cwd\":\"K:\\\\source\\\\AmStoreA\",\"gitBranch\":\"dev\",\"message\":{\"content\":\"Caveat: injected preamble\"}}\n"
            "{\"type\":\"user\",\"timestamp\":\"2026-06-01T10:00:05Z\",\"cwd\":\"K:\\\\source\\\\AmStoreA\",\"message\":{\"content\":\"build the thing\"}}\n"
            "{\"type\":\"assistant\",\"timestamp\":\"2026-06-01T10:00:09Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"on it\"},{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"Bash\",\"input\":{\"command\":\"build\"}}],\"usage\":{\"input_tokens\":50,\"cache_creation_input_tokens\":10,\"cache_read_input_tokens\":500,\"output_tokens\":10}}}\n"
            "{\"type\":\"user\",\"timestamp\":\"2026-06-01T10:00:20Z\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\",\"content\":\"ok\"}]}}\n"
            "{\"type\":\"assistant\",\"timestamp\":\"2026-06-01T10:00:30Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"done\"}],\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":100,\"cache_creation_input_tokens\":20,\"cache_read_input_tokens\":2000,\"output_tokens\":30}}}\n"
            "{\"type\":\"system\",\"subtype\":\"away_summary\",\"timestamp\":\"2026-06-01T10:30:00Z\",\"content\":\"away note\"}\n"
            "{\"type\":\"custom-title\",\"customTitle\":\"store test A\"}\n"
            "{\"type\":\"permission-mode\",\"permissionMode\":\"bypassPermissions\"}\n";
        const std::wstring sidA = L"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        MakeJsonl(dirA + L"\\" + sidA + L".jsonl", sessA, 5000, 1000);
        MakeJsonl(dirA + L"\\agent-a05bb6b.jsonl", "{}", 6000, 1000); // legacy subagent file — must NOT enumerate
        const std::wstring sidB = L"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        MakeJsonl(dirB + L"\\" + sidB + L".jsonl", "{\"type\":\"mode\",\"mode\":\"normal\"}\n", 9000, 8000); // never-prompted prefix-only

        const auto all = EnumerateTranscriptsIn(projRoot, 0);
        CHECK(all.size() == 2, "enumerate: exactly the 2 uuid transcripts (agent-*/subdirs excluded)");
        CHECK(all.size() == 2 && all[0].sessionId == sidB && all[1].sessionId == sidA, "enumerate: newest-mtime first");
        CHECK(all.size() == 2 && all[1].projectDirLeaf == L"K--source-AmStoreA" && all[1].sizeBytes > 0, "enumerate: leaf + size filled");
        const auto windowed = EnumerateTranscriptsIn(projRoot, 7000);
        CHECK(windowed.size() == 1 && windowed[0].sessionId == sidB, "enumerate: sinceUnixMs window filters by mtime");
        CHECK(EnumerateTranscriptsIn(projRoot + L"\\missing", 0).empty(), "enumerate: missing root -> empty");

        // Stats: one accumulate over session A.
        TranscriptStats st;
        CHECK(AccumulateTranscriptStats(dirA + L"\\" + sidA + L".jsonl", st), "stats: accumulate ok");
        CHECK(st.userPrompts == 1, "stats: ONE real prompt (Caveat + tool_result filtered)");
        CHECK(st.firstUserPrompt == L"build the thing", "stats: first REAL prompt captured");
        CHECK(st.assistantLines == 2 && st.toolUses == 1, "stats: assistant lines + tool uses counted");
        CHECK(st.contextTokens == 2150, "stats: contextTokens = NEWEST assistant usage (100+20+2000+30), not the first (570)");
        CHECK(st.customTitle == L"store test A", "stats: custom title captured");
        CHECK(st.firstTimestampMs == 1780308000000LL, "stats: first timestamp (2026-06-01T10:00:00Z)");
        CHECK(st.lastTimestampMs == 1780308030000LL, "stats: last activity is the LAST MESSAGE (10:00:30), NOT the away_summary");
        CHECK(st.cwd == L"K:\\source\\AmStoreA" && st.gitBranch == L"dev", "stats: cwd + branch from the lines");
        CHECK(st.parsedBytes == static_cast<int64_t>(sessA.size()), "stats: cursor consumed the whole file");
        CHECK(PickDisplayTitle(st.customTitle, st.aiTitle, st.summary, st.firstUserPrompt) == L"store test A", "stats: display title = custom title");

        // Incremental: append a new turn; re-accumulate picks up ONLY the suffix.
        {
            const std::string more =
                "{\"type\":\"user\",\"timestamp\":\"2026-06-01T11:00:00Z\",\"message\":{\"content\":\"and another thing\"}}\n";
            const HANDLE h = ::CreateFileW((dirA + L"\\" + sidA + L".jsonl").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD w = 0;
            ::WriteFile(h, more.data(), static_cast<DWORD>(more.size()), &w, nullptr);
            ::CloseHandle(h);
            CHECK(AccumulateTranscriptStats(dirA + L"\\" + sidA + L".jsonl", st), "stats: incremental accumulate ok");
            CHECK(st.userPrompts == 2, "stats: appended prompt counted once");
            CHECK(st.lastTimestampMs == 1780311600000LL, "stats: last activity advanced to the appended turn (11:00:00)");
            CHECK(st.firstUserPrompt == L"build the thing", "stats: first prompt unchanged across resume");
        }
        // Replaced (shrunk) file: stats self-reset and rebuild.
        {
            MakeJsonl(dirA + L"\\" + sidA + L".jsonl", "{\"type\":\"user\",\"timestamp\":\"2026-06-02T00:00:00Z\",\"message\":{\"content\":\"fresh\"}}\n", 9500, 9500);
            CHECK(AccumulateTranscriptStats(dirA + L"\\" + sidA + L".jsonl", st), "stats: shrink-rebuild ok");
            CHECK(st.userPrompts == 1 && st.firstUserPrompt == L"fresh", "stats: shrink resets and rebuilds from 0");
            CHECK(st.contextTokens == 0, "stats: shrink-rebuild clears contextTokens (replaced file has no assistant usage)");
        }
        TranscriptStats gone;
        CHECK(!AccumulateTranscriptStats(dirA + L"\\missing.jsonl", gone) && !gone.found, "stats: vanished file -> found=false");

        // QuickFacts: created from the first timestamped line; last activity skips the
        // trailing away_summary + state lines; fork detection flips created to file birth.
        MakeJsonl(dirA + L"\\" + sidA + L".jsonl", sessA, 5000, 1000); // restore the full fixture
        {
            const auto q = ReadTranscriptQuickFacts(dirA + L"\\" + sidA + L".jsonl", 1000);
            CHECK(q.found, "quick: read ok");
            CHECK(q.createdMs == 1780308000000LL, "quick: created = first line timestamp (not file birth)");
            CHECK(q.lastActivityMs == 1780308030000LL, "quick: last activity = last MESSAGE ts (away_summary + custom-title + permission-mode tail skipped)");
            CHECK(!q.fork && q.cwd == L"K:\\source\\AmStoreA", "quick: not a fork; cwd from head");
        }
        {
            const std::string fork =
                "{\"type\":\"user\",\"timestamp\":\"2026-06-01T10:00:05Z\",\"forkedFrom\":{\"sessionId\":\"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa\",\"messageUuid\":\"m1\"},\"message\":{\"content\":\"build the thing\"}}\n";
            const std::wstring sidF = L"cccccccc-cccc-4ccc-8ccc-cccccccccccc";
            MakeJsonl(dirA + L"\\" + sidF + L".jsonl", fork, 9990, 9990);
            const auto q = ReadTranscriptQuickFacts(dirA + L"\\" + sidF + L".jsonl", 9990);
            CHECK(q.fork && q.forkedFromId == sidA, "quick: fork detected with parent id");
            CHECK(q.createdMs == 9990, "quick: a fork's created = file birth (copied line timestamps lie)");
        }
        {
            const auto q = ReadTranscriptQuickFacts(dirB + L"\\" + sidB + L".jsonl", 8000);
            CHECK(q.found && q.createdMs == 0 && q.lastActivityMs == 0, "quick: prefix-only (never-prompted) file -> zeros (caller falls back to birth/mtime)");
        }

        std::filesystem::remove_all(std::filesystem::path{ root }, ec);
    }

    // --- live smoke over the REAL corpus (guarded: skips silently when absent) ---
    {
        const auto recent = EnumerateTranscripts(NowMsTest() - 92LL * 24 * 3600 * 1000);
        if (!recent.empty())
        {
            bool allUuid = true;
            for (const auto& r : recent)
            {
                if (!IsSessionIdStem(r.sessionId))
                {
                    allUuid = false;
                }
            }
            CHECK(allUuid, "live: every enumerated stem is a session uuid");
            const auto q = ReadTranscriptQuickFacts(recent[0].path, recent[0].birthMs);
            CHECK(q.found, "live: quick facts read the newest real transcript");
            CHECK(q.lastActivityMs == 0 || q.lastActivityMs <= recent[0].mtimeMs + 60000, "live: last-activity <= mtime (+clock slack) — mtime is the superset");
            std::wprintf(L"  [info] live corpus: %zu transcripts in 92d window; newest lastActivity-vs-mtime gap %lld s\n",
                         recent.size(), q.lastActivityMs ? (recent[0].mtimeMs - q.lastActivityMs) / 1000 : -1);
        }
    }
}

// Cross-file conversation lineage on disk: CollectConversationLineage walks a session's predecessors via
// the SOLID plan-restart parent link ONLY — the explicit "read the full transcript at: <parent>.jsonl"
// reference the child transcript itself carries. The former timing-based /clear predecessor was REMOVED
// (no solid on-disk signal for a /clear successor — /clear leaves no link, /compact is in-place), so a
// /clear successor now has NO traceable lineage. Stages a throwaway CLAUDE_CONFIG_DIR so the resolver
// (ResolveClaudeTranscriptPath) finds the fixtures. [Agentmaster]
void TestConversationLineage()
{
    std::wprintf(L"ConversationLineage (plan-restart parent link ONLY — /clear timing trace removed):\n");
    const auto narrow = [](const std::wstring& w) { std::string s; s.reserve(w.size()); for (wchar_t c : w) { s.push_back(static_cast<char>(c)); } return s; }; // ASCII ids only (explicit cast => no C4244)

    wchar_t tmp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tmp);
    const std::wstring cfg = std::wstring{ tmp } + L"am_lineage_cfg_" + std::to_wstring(::GetCurrentProcessId());
    const std::wstring projects = cfg + L"\\projects";

    // Point Claude's transcript root at our temp dir for the duration of this test, restore after.
    wchar_t prevBuf[2048]{};
    const DWORD prevN = ::GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", prevBuf, 2048);
    const std::wstring prevCfg{ prevBuf, prevN };
    ::SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", cfg.c_str());

    // --- (1) a /clear successor has NO traceable lineage. B started shortly after A in the SAME dir —
    // exactly what the old timing heuristic chained — but there is no solid on-disk link, so B must now
    // stand ALONE (the false-positive that merged unrelated conversations is gone). ---
    {
        const std::wstring cwd = L"K:\\am_lin\\clearcase";
        const std::wstring dir = projects + L"\\" + EncodeCwdToProjectDir(cwd);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ dir }, ec);
        const std::wstring idA = L"aaaaaaaa-1111-4aaa-8aaa-aaaaaaaaaaaa";
        const std::wstring idB = L"bbbbbbbb-2222-4bbb-8bbb-bbbbbbbbbbbb";
        const std::string aJson =
            R"j({"type":"user","userType":"external","uuid":"a1","parentUuid":null,"cwd":"K:\\am_lin\\clearcase","message":{"content":"alpha one"},"timestamp":"2026-06-26T09:00:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"a2","parentUuid":"a1","cwd":"K:\\am_lin\\clearcase","message":{"content":"alpha two"},"timestamp":"2026-06-26T09:05:00.000Z"})j" "\n";
        const std::string bJson =
            R"j({"type":"user","userType":"external","uuid":"b1","parentUuid":null,"cwd":"K:\\am_lin\\clearcase","message":{"content":"beta one"},"timestamp":"2026-06-26T09:06:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"b2","parentUuid":"b1","cwd":"K:\\am_lin\\clearcase","message":{"content":"beta two"},"timestamp":"2026-06-26T09:10:00.000Z"})j" "\n";
        MakeJsonl(dir + L"\\" + idA + L".jsonl", aJson, 100000, 90000);
        MakeJsonl(dir + L"\\" + idB + L".jsonl", bJson, 200000, 190000);

        CHECK(CollectConversationLineage(idB, cwd, 16).empty(), "lineage/disk: a /clear successor has NO lineage (timing trace removed — no false merge)");
        CHECK(CollectConversationLineage(idA, cwd, 16).empty(), "lineage/disk: the origin session A has no previous session");
    }

    // --- (2) plan-restart parent in a DIFFERENT dir — the SOLID link is followed; the parent's OWN
    // (timing) /clear predecessor Q is NOT. C (child dir) --plan--> P (parent dir); Q sits in P's dir as a
    // would-be /clear predecessor of P. The plan hop C->P is an explicit cross-file reference (resolved by
    // id, cwd-independent) and surfaces; Q has no solid link to P, so it must stay INVISIBLE. ---
    {
        const std::wstring childCwd = L"K:\\am_lin\\planchild";
        const std::wstring parentCwd = L"K:\\am_lin\\planparent";
        const std::wstring childDir = projects + L"\\" + EncodeCwdToProjectDir(childCwd);
        const std::wstring parentDir = projects + L"\\" + EncodeCwdToProjectDir(parentCwd);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ childDir }, ec);
        std::filesystem::create_directories(std::filesystem::path{ parentDir }, ec);
        const std::wstring idQ = L"eeeeeeee-5555-4eee-8eee-eeeeeeeeeeee"; // P's /clear predecessor (parent dir)
        const std::wstring idP = L"cccccccc-3333-4ccc-8ccc-cccccccccccc"; // plan parent (parent dir)
        const std::wstring idC = L"dddddddd-4444-4ddd-8ddd-dddddddddddd"; // plan child (child dir)
        const std::string qJson =
            R"j({"type":"user","userType":"external","uuid":"q1","parentUuid":null,"cwd":"K:\\am_lin\\planparent","message":{"content":"earlier groundwork"},"timestamp":"2026-06-26T09:50:00.000Z"})j" "\n"
            R"j({"type":"user","userType":"external","uuid":"q2","parentUuid":"q1","cwd":"K:\\am_lin\\planparent","message":{"content":"more groundwork"},"timestamp":"2026-06-26T09:58:00.000Z"})j" "\n";
        const std::string pJson =
            R"j({"type":"user","userType":"external","uuid":"p1","parentUuid":null,"cwd":"K:\\am_lin\\planparent","message":{"content":"plan the feature"},"timestamp":"2026-06-26T10:00:00.000Z"})j" "\n";
        // The breadcrumb only needs a token whose basename is "<idP>.jsonl" — the parent is then resolved
        // by GLOB on that id (forward slashes keep the embedded path valid JSON), so the link spans dirs.
        const std::string cJson =
            std::string(R"j({"type":"user","userType":"external","uuid":"c1","parentUuid":null,"cwd":"K:\\am_lin\\planchild","message":{"content":"read the full transcript at: /plans/x/)j") +
            narrow(idP) + R"j(.jsonl and continue"},"timestamp":"2026-06-26T10:10:00.000Z"})j" "\n";
        MakeJsonl(parentDir + L"\\" + idQ + L".jsonl", qJson, 100000, 80000);
        MakeJsonl(parentDir + L"\\" + idP + L".jsonl", pJson, 110000, 90000);
        MakeJsonl(childDir + L"\\" + idC + L".jsonl", cJson, 200000, 190000);

        const auto lin = CollectConversationLineage(idC, childCwd, 16);
        CHECK(lin.size() == 1, "lineage/disk: ONLY the solid plan parent P surfaces (Q's /clear timing link is gone)");
        CHECK(lin.size() == 1 && lin[0].userMsgs.size() == 1 && lin[0].userMsgs[0] == L"plan the feature",
              "lineage/disk: the plan PARENT P is the sole previous session; its would-be /clear predecessor Q stays invisible");
    }

    ::SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", prevCfg.empty() ? nullptr : prevCfg.c_str());
    std::error_code ecCleanup;
    std::filesystem::remove_all(std::filesystem::path{ cfg }, ecCleanup);
}

void TestSessionSearch()
{
    std::wprintf(L"SessionSearch (regex/match/snippet + fast phase + history + presence + index):\n");

    // --- BuildSearchRegex: literal escape + fuzzy lazy-gap join ---
    CHECK(BuildSearchRegex(L"a.b", false) == LR"(a\.b)", "regex: literal dot escaped");
    CHECK(BuildSearchRegex(L"abc", true) == LR"(a.*?b.*?c)", "regex: fuzzy lazy-gap join");
    CHECK(BuildSearchRegex(L"a c", true) == LR"(a.*?c)", "regex: fuzzy drops whitespace");
    CHECK(BuildSearchRegex(L"x(y)", false) == LR"(x\(y\))", "regex: parens escaped");

    // --- MatchesQueryText (pre-folded inputs) ---
    CHECK(MatchesQueryText(L"the quick brown fox", L"quick", false), "match: substring");
    CHECK(!MatchesQueryText(L"the quick brown fox", L"quirk", false), "match: non-substring misses");
    CHECK(MatchesQueryText(L"the quick brown fox", L"qbf", true), "match: fuzzy subsequence");
    CHECK(!MatchesQueryText(L"the quick brown fox", L"fbq", true), "match: fuzzy order matters");
    CHECK(MatchesQueryText(L"anything", L"", true), "match: empty query matches");

    // --- MatchesPathQuery: '/' and '\' equivalent for directory haystacks (pre-folded) ---
    CHECK(MatchesPathQuery(L"c:\\users\\src\\foo", L"src/foo", false), "pathmatch: forward-slash query finds a backslash dir");
    CHECK(MatchesPathQuery(L"c:/users/src/foo", L"src\\foo", false), "pathmatch: backslash query finds a forward-slash dir");
    CHECK(MatchesPathQuery(L"c:\\users\\src\\foo", L"src\\foo", false), "pathmatch: backslash query, backslash dir (unchanged)");
    CHECK(MatchesPathQuery(L"c:/users/src/foo", L"src/foo", false), "pathmatch: forward query, forward dir (unchanged)");
    CHECK(!MatchesPathQuery(L"c:\\users\\src\\foo", L"src/bar", false), "pathmatch: a non-substring still misses");
    CHECK(MatchesPathQuery(L"c:\\users\\src\\foo", L"users/src/foo", true), "pathmatch: fuzzy honors slash equivalence");
    // A separator-free needle behaves EXACTLY like MatchesQueryText (the allocation-free fast path).
    CHECK(MatchesPathQuery(L"c:\\users\\src\\foo", L"src", false) == MatchesQueryText(L"c:\\users\\src\\foo", L"src", false), "pathmatch: separator-free needle == MatchesQueryText");
    CHECK(MatchesPathQuery(L"the quick brown fox", L"", false), "pathmatch: empty query matches");

    // --- MakeSnippet ---
    {
        const std::wstring text = L"prefix prefix prefix prefix prefix prefix THE-NEEDLE suffix\nsecond line";
        const auto s = MakeSnippet(text, L"the-needle", false, 60);
        CHECK(s.find(L"THE-NEEDLE") != std::wstring::npos, "snippet: contains the match");
        CHECK(s.find(L'\n') == std::wstring::npos, "snippet: single-line collapsed");
        CHECK(!s.empty() && s.front() == L'…', "snippet: left-context ellipsis when clipped");
    }

    // --- IsGuidToken / ParseSessionQuery: the term grammar ("quoted" exact + whole-guid id terms) ---
    {
        CHECK(IsGuidToken(L"dddddddd-dddd-4ddd-8ddd-dddddddddddd"), "guid: bare uuid accepted");
        CHECK(IsGuidToken(L"{DDDDDDDD-DDDD-4DDD-8DDD-DDDDDDDDDDDD}"), "guid: braced uppercase accepted");
        CHECK(!IsGuidToken(L"dddddddd-dddd-4ddd-8ddd-ddddddddddd"), "guid: 35 chars rejected");
        CHECK(!IsGuidToken(L"ddddddddxdddd-4ddd-8ddd-dddddddddddd"), "guid: misplaced dash rejected");
        CHECK(!IsGuidToken(L"gddddddd-dddd-4ddd-8ddd-dddddddddddd"), "guid: non-hex rejected");
        CHECK(!IsGuidToken(L""), "guid: empty rejected");

        auto t = ParseSessionQuery(L"  foo   bar ");
        CHECK(t.size() == 2 && t[0].text == L"foo" && t[1].text == L"bar" && !t[0].exact && !t[0].isGuid, "parse: whitespace-split plain terms");
        t = ParseSessionQuery(L"\"foo bar\" baz");
        CHECK(t.size() == 2 && t[0].text == L"foo bar" && t[0].exact && t[1].text == L"baz" && !t[1].exact, "parse: quoted phrase is ONE exact term");
        t = ParseSessionQuery(L"\"unterminated tail");
        CHECK(t.size() == 1 && t[0].text == L"unterminated tail" && t[0].exact, "parse: unterminated quote runs to the end");
        CHECK(ParseSessionQuery(L"\"\"").empty(), "parse: empty quotes drop (no terms)");
        CHECK(ParseSessionQuery(L"   ").empty(), "parse: whitespace-only -> no terms");
        t = ParseSessionQuery(L"{dddddddd-dddd-4ddd-8ddd-dddddddddddd}");
        CHECK(t.size() == 1 && t[0].isGuid && t[0].text == L"dddddddd-dddd-4ddd-8ddd-dddddddddddd", "parse: braced guid term, braces stripped");
        t = ParseSessionQuery(L"\"dddddddd-dddd-4ddd-8ddd-dddddddddddd\"");
        CHECK(t.size() == 1 && !t[0].isGuid && t[0].exact, "parse: QUOTED guid is a pure text term");
        t = ParseSessionQuery(L"DDDDDDDD-DDDD-4DDD-8DDD-DDDDDDDDDDDD");
        CHECK(t.size() == 1 && t[0].isGuid && t[0].textLower == L"dddddddd-dddd-4ddd-8ddd-dddddddddddd", "parse: guid term pre-folded for the id compare");

        const SearchTerm plain{ L"a", L"a", false, false };
        const SearchTerm exact{ L"a", L"a", true, false };
        const SearchTerm guid{ L"a", L"a", false, true };
        CHECK(TermIsFuzzy(plain, true) && !TermIsFuzzy(exact, true) && !TermIsFuzzy(guid, true) && !TermIsFuzzy(plain, false), "terms: (F) applies to plain terms only");
    }

    // --- SearchIndexFast: toggle semantics over canned entries ---
    {
        std::vector<SessionIndexEntry> entries(3);
        entries[0].sessionId = L"s-title";
        entries[0].stats.customTitle = L"Fix the BUILD pipeline";
        entries[0].stats.cwd = L"K:\\source\\alpha";
        entries[1].sessionId = L"s-dir";
        entries[1].stats.firstUserPrompt = L"unrelated";
        entries[1].stats.cwd = L"K:\\source\\BravoProj";
        entries[1].stats.pathsAccessed = { L"K:\\source\\BravoProj\\src\\widget.cpp" };
        entries[2].sessionId = L"s-file";
        entries[2].stats.cwd = L"K:\\elsewhere";
        entries[2].stats.pathsAccessed = { L"C:\\temp\\notes\\Findings.md" };

        SessionQuery q;
        q.text = L"build";
        auto r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-title", "fast: title matches (case-insensitive), both scopes off");

        q.text = L"bravoproj";
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-dir", "fast: cwd matches in the baseline");

        q.text = L"findings.md";
        r = SearchIndexFast(entries, q);
        CHECK(r.empty(), "fast: an accessed FILE does not match without the file scope");
        q.scopeFiles = true;
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-file", "fast: file scope matches the path LEAF");

        q = {};
        q.text = L"temp\\notes";
        q.scopeDirs = true;
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-file", "fast: dir scope matches the path's DIRECTORY part");
        q.scopeDirs = false;
        q.scopeFiles = true;
        r = SearchIndexFast(entries, q);
        CHECK(r.empty(), "fast: dir text does not match the file scope's leaf");

        q = {};
        q.text = L"";
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 3, "fast: empty text lists everything (window-only)");

        q.text = L"fbp";
        q.fuzzy = true;
        r = SearchIndexFast(entries, q);
        CHECK(!r.empty() && r[0] == L"s-title", "fast: fuzzy subsequence over the title");
    }

    // --- SearchIndexFast: directory matching is slash-insensitive (cwd + 📁), both directions ---
    {
        std::vector<SessionIndexEntry> entries(2);
        entries[0].sessionId = L"s-win"; // backslash-stored cwd (the Windows norm)
        entries[0].stats.cwd = L"K:\\source\\BravoProj";
        entries[0].stats.pathsAccessed = { L"K:\\source\\BravoProj\\src\\widget.cpp" };
        entries[1].sessionId = L"s-posix"; // forward-slash-stored cwd (a transcript can carry either)
        entries[1].stats.cwd = L"/home/user/AlphaProj";
        entries[1].stats.pathsAccessed = { L"/home/user/AlphaProj/lib/core.ts" };

        SessionQuery q;
        q.text = L"source/bravoproj"; // forward-slash query against the backslash cwd
        auto r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-win", "fast: forward-slash cwd query matches the backslash-stored cwd");

        q.text = L"home\\user\\alphaproj"; // backslash query against the forward-slash cwd
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-posix", "fast: backslash cwd query matches the forward-slash-stored cwd");

        q = {};
        q.scopeDirs = true;
        q.text = L"bravoproj/src"; // forward-slash 📁 query against the backslash path's dir part
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-win", "fast: 📁 dir scope is slash-insensitive (forward query, backslash path)");

        q = {};
        q.scopeDirs = true;
        q.text = L"alphaproj\\lib"; // backslash 📁 query against the forward-slash path's dir part
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-posix", "fast: 📁 dir scope is slash-insensitive (backslash query, forward path)");

        q = {};
        q.text = L"source/zeta"; // a genuinely-absent path term still misses (no false positives)
        r = SearchIndexFast(entries, q);
        CHECK(r.empty(), "fast: slash-insensitivity does not loosen a non-substring miss");
    }

    // --- SearchIndexFast: the 🏷 title scope gates title matching; liveTitle overlay is searchable ---
    {
        std::vector<SessionIndexEntry> entries(2);
        entries[0].sessionId = L"s-titled";
        entries[0].stats.customTitle = L"Refactor the SCHEDULER";
        entries[0].stats.cwd = L"K:\\source\\gamma";
        entries[1].sessionId = L"s-live";
        entries[1].stats.firstUserPrompt = L"do a thing";
        entries[1].stats.cwd = L"K:\\source\\delta";
        entries[1].liveTitle = L"My Renamed Tab"; // an OPEN session's real tab title (UI overlay)

        SessionQuery q; // scopeTitle defaults ON
        q.text = L"scheduler";
        auto r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-titled", "fast: title matches with scopeTitle ON (default)");
        q.scopeTitle = false;
        r = SearchIndexFast(entries, q);
        CHECK(r.empty(), "fast: scopeTitle OFF excludes a title-only match");

        q = {}; // DMI restores scopeTitle = true
        q.text = L"gamma"; // the cwd — must match even with titles off (cwd is the always-on baseline)
        q.scopeTitle = false;
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-titled", "fast: cwd stays matched with scopeTitle OFF");

        q = {};
        q.text = L"renamed tab"; // present only in the liveTitle overlay
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == L"s-live", "fast: liveTitle (open tab name) is searchable under scopeTitle");
        q.scopeTitle = false;
        r = SearchIndexFast(entries, q);
        CHECK(r.empty(), "fast: liveTitle is gated by scopeTitle too");
    }

    // --- SearchIndexFast: guid -> session-identity terms, quoted-exact, AND semantics ---
    {
        const std::wstring gidA = L"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const std::wstring gidB = L"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        std::vector<SessionIndexEntry> entries(3);
        entries[0].sessionId = gidA;
        entries[0].stats.customTitle = L"Fix the BUILD pipeline";
        entries[0].stats.cwd = L"K:\\source\\alpha";
        entries[1].sessionId = gidB;
        entries[1].stats.customTitle = L"fork child";
        entries[1].stats.forkedFromId = gidA;
        entries[1].stats.cwd = L"K:\\source\\alpha";
        entries[2].sessionId = L"cccccccc-cccc-4ccc-8ccc-cccccccccccc";
        entries[2].stats.customTitle = L"unrelated";
        entries[2].stats.cwd = L"K:\\elsewhere";

        SessionQuery q;
        q.text = gidA;
        auto r = SearchIndexFast(entries, q);
        CHECK(r.size() == 2 && r[0] == gidA && r[1] == gidB, "fast: whole-guid query matches the session id AND its fork");

        q.text = L"{AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA}";
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 2 && r[0] == gidA, "fast: guid id match is case-insensitive, braces tolerated");

        q.text = gidA + L" build";
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == gidA, "fast: guid + word AND — the fork lacks the word");

        q.text = L"build alpha";
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == gidA, "fast: AND terms may hit DIFFERENT fields (title + cwd)");

        q.text = L"\"the build\"";
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == gidA, "fast: quoted phrase exact-matches across a space");
        q.text = L"\"build the\"";
        r = SearchIndexFast(entries, q);
        CHECK(r.empty(), "fast: quoted phrase is order-exact (no token shuffle)");

        q = {};
        q.fuzzy = true;
        q.text = L"fxbld";
        r = SearchIndexFast(entries, q);
        CHECK(r.size() == 1 && r[0] == gidA, "fast: plain term fuzzy-matches under (F)");
        q.text = L"\"fxbld\"";
        r = SearchIndexFast(entries, q);
        CHECK(r.empty(), "fast: quotes suppress (F) for that term");
    }

    // --- temp-root fixtures: history accelerator + presence + slow phase + index sidecar ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring{ tmp } + L"am_search_test_" + std::to_wstring(::GetCurrentProcessId());
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path{ root }, ec);

        // history.jsonl: two sids, one ancient line without a sessionId.
        const std::wstring hist = root + L"\\history.jsonl";
        MakeJsonl(hist,
                  "{\"display\":\"please fix the flaky test\",\"project\":\"K:\\\\a\",\"sessionId\":\"sid-1\",\"timestamp\":1}\n"
                  "{\"display\":\"deploy the build\",\"project\":\"K:\\\\a\",\"sessionId\":\"sid-2\",\"timestamp\":2}\n"
                  "{\"display\":\"fix it again (flaky)\",\"project\":\"K:\\\\a\",\"sessionId\":\"sid-1\",\"timestamp\":3}\n"
                  "{\"display\":\"ancient flaky line, no sid\",\"timestamp\":4}\n",
                  1000, 1000);
        SessionQuery q;
        q.text = L"FLAKY";
        q.scopeUser = true;
        const auto hh = SearchHistoryPrompts(hist, q, 3);
        CHECK(hh.size() == 1 && hh.count(L"sid-1") == 1, "history: per-sid aggregation, case-insensitive, no-sid lines skipped");
        CHECK(hh.count(L"sid-1") && hh.at(L"sid-1").hitCount == 2 && hh.at(L"sid-1").snippets.size() == 2, "history: hit count + snippets");

        // history2.jsonl: guid-sid lines — a guid term scopes hits to that session; "" = exact.
        const std::wstring gid1 = L"11111111-1111-4111-8111-111111111111";
        const std::wstring gid2 = L"22222222-2222-4222-8222-222222222222";
        const std::wstring hist2 = root + L"\\history2.jsonl";
        MakeJsonl(hist2,
                  "{\"display\":\"refactor the parser module\",\"sessionId\":\"11111111-1111-4111-8111-111111111111\",\"timestamp\":1}\n"
                  "{\"display\":\"refactor the lexer\",\"sessionId\":\"22222222-2222-4222-8222-222222222222\",\"timestamp\":2}\n",
                  1000, 1000);
        SessionQuery hq;
        hq.scopeUser = true;
        hq.text = gid1 + L" refactor";
        auto hg = SearchHistoryPrompts(hist2, hq, 3);
        CHECK(hg.size() == 1 && hg.count(gid1) == 1 && hg.at(gid1).hitCount == 1, "history: a guid term scopes hits to that session");
        hq.text = gid1;
        hg = SearchHistoryPrompts(hist2, hq, 3);
        CHECK(hg.empty(), "history: guid-only query -> no content hits (identity is the fast phase's job)");
        hq.text = L"\"the parser\"";
        hg = SearchHistoryPrompts(hist2, hq, 3);
        CHECK(hg.size() == 1 && hg.count(gid1) == 1, "history: quoted phrase exact-matches");
        hq.text = L"\"parser the\"";
        hg = SearchHistoryPrompts(hist2, hq, 3);
        CHECK(hg.empty(), "history: quoted phrase is order-exact");
        hq.text = L"refactor " + gid2;
        hg = SearchHistoryPrompts(hist2, hq, 3);
        CHECK(hg.size() == 1 && hg.count(gid2) == 1, "history: guid term position-independent (word + guid)");

        // presence: one row per file; bad/empty files tolerated.
        const std::wstring presDir = root + L"\\sessions";
        std::filesystem::create_directories(std::filesystem::path{ presDir }, ec);
        MakeJsonl(presDir + L"\\123.json", "{\"pid\":123,\"sessionId\":\"sid-1\",\"cwd\":\"K:\\\\a\",\"status\":\"busy\",\"version\":\"2.1.170\",\"startedAt\":111,\"updatedAt\":222}", 1, 1);
        MakeJsonl(presDir + L"\\999.json", "not json", 1, 1);
        const auto pres = ReadSessionPresenceIn(presDir);
        CHECK(pres.size() == 1 && pres[0].pid == 123 && pres[0].sessionId == L"sid-1" && pres[0].status == L"busy" && pres[0].updatedAtMs == 222, "presence: parsed row; garbage tolerated");

        // slow phase over a transcript (in-process fallback semantics; rg, when present, only
        // pre-filters files — same results either way since this file DOES match).
        const std::wstring projDir = root + L"\\projects\\K--a";
        std::filesystem::create_directories(std::filesystem::path{ projDir }, ec);
        const std::wstring sidT = L"dddddddd-dddd-4ddd-8ddd-dddddddddddd";
        MakeJsonl(projDir + L"\\" + sidT + L".jsonl",
                  "{\"type\":\"user\",\"timestamp\":\"2026-06-01T10:00:00Z\",\"message\":{\"content\":\"the magic word is xyzzy\"}}\n"
                  "{\"type\":\"assistant\",\"timestamp\":\"2026-06-01T10:00:05Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"plugh and xyzzy acknowledged\"}]}}\n",
                  2000, 2000);
        TranscriptRef ref;
        ref.sessionId = sidT;
        ref.path = projDir + L"\\" + sidT + L".jsonl";
        ref.sizeBytes = 0;
        ref.mtimeMs = 2000;
        ref.birthMs = 2000;

        SessionQuery sq;
        sq.text = L"xyzzy";
        sq.scopeUser = true;
        auto hits = SearchTranscriptsSlow({ ref }, sq, 4, nullptr);
        CHECK(hits.size() == 1 && hits[0].hitCount == 1, "slow: user scope hits ONLY the prompt line");
        sq.scopeAgent = true;
        hits = SearchTranscriptsSlow({ ref }, sq, 4, nullptr);
        CHECK(hits.size() == 1 && hits[0].hitCount == 2, "slow: both scopes hit prompt + assistant");
        sq.scopeUser = false;
        sq.text = L"plugh";
        hits = SearchTranscriptsSlow({ ref }, sq, 4, nullptr);
        CHECK(hits.size() == 1 && hits[0].hitCount == 1 && !hits[0].snippets.empty(), "slow: agent-only scope with snippet");
        sq.scopeAgent = false;
        hits = SearchTranscriptsSlow({ ref }, sq, 4, nullptr);
        CHECK(hits.empty(), "slow: no message scope -> no content search");
        const auto cancelledImmediately = []() { return true; };
        sq.scopeAgent = true;
        hits = SearchTranscriptsSlow({ ref }, sq, 4, cancelledImmediately);
        CHECK(hits.empty(), "slow: cancellation respected");

        // slow phase, term grammar: guid = session-identity scope; "" = exact; per-MESSAGE AND.
        SessionQuery gq;
        gq.scopeUser = true;
        gq.text = sidT + L" xyzzy";
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.size() == 1 && hits[0].hitCount == 1, "slow: guid+word — the file's own id satisfies the guid term");
        gq.text = L"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee xyzzy";
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.empty(), "slow: a DIFFERENT guid excludes the session (id mismatch, not in text)");
        gq.text = sidT;
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.empty(), "slow: guid-only query -> no content scan (identity is the fast phase's job)");
        gq.text = L"\"magic word\"";
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.size() == 1 && hits[0].hitCount == 1, "slow: quoted phrase exact-matches the prompt");
        gq.fuzzy = true;
        gq.text = L"\"mgc wrd\"";
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.empty(), "slow: quotes suppress (F) inside the phrase");
        gq.text = L"mgc wrd";
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.size() == 1 && hits[0].hitCount == 1, "slow: the same terms unquoted DO fuzzy-match");
        gq.fuzzy = false;
        gq.scopeAgent = true;
        gq.text = L"magic plugh";
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.empty(), "slow: AND is per-MESSAGE — terms split across two messages don't hit");
        gq.text = L"plugh xyzzy";
        gq.scopeUser = false;
        hits = SearchTranscriptsSlow({ ref }, gq, 4, nullptr);
        CHECK(hits.size() == 1 && hits[0].hitCount == 1, "slow: both terms in ONE assistant message hit");

        // index sidecar: refresh -> hit -> incremental refresh.
        const std::wstring idxDir = root + L"\\sessions-index";
        std::filesystem::create_directories(std::filesystem::path{ idxDir }, ec);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        ::GetFileAttributesExW(ref.path.c_str(), GetFileExInfoStandard, &fad);
        ref.sizeBytes = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;

        auto e1 = LoadOrRefreshSessionIndexIn(idxDir, ref);
        CHECK(e1.valid && e1.stats.userPrompts == 1 && e1.stats.assistantLines == 1, "index: first refresh builds stats");
        CHECK(std::filesystem::exists(std::filesystem::path{ idxDir + L"\\" + sidT + L".json" }), "index: sidecar written");
        auto e2 = LoadOrRefreshSessionIndexIn(idxDir, ref);
        CHECK(e2.valid && e2.stats.userPrompts == 1 && e2.stats.parsedBytes == e1.stats.parsedBytes, "index: (size,mtime) hit loads the sidecar only");
        {
            const std::string more = "{\"type\":\"user\",\"timestamp\":\"2026-06-01T11:00:00Z\",\"message\":{\"content\":\"second prompt\"}}\n";
            const HANDLE h = ::CreateFileW(ref.path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD w = 0;
            ::WriteFile(h, more.data(), static_cast<DWORD>(more.size()), &w, nullptr);
            ::CloseHandle(h);
            ::GetFileAttributesExW(ref.path.c_str(), GetFileExInfoStandard, &fad);
            ref.sizeBytes = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            ref.mtimeMs = 3000; // changed key
            auto e3 = LoadOrRefreshSessionIndexIn(idxDir, ref);
            CHECK(e3.valid && e3.stats.userPrompts == 2, "index: stale key resumes the accumulate incrementally");
        }

        // contextTokens: parsed from message.usage (newest assistant wins), round-trips the sidecar,
        // and an OLD sidecar (written before the ctxTokens key existed) is one-time-backfilled on load.
        {
            const std::wstring sidC = L"ffffffff-ffff-4fff-8fff-ffffffffffff";
            const std::string sessC =
                "{\"type\":\"user\",\"timestamp\":\"2026-06-01T10:00:00Z\",\"message\":{\"content\":\"go\"}}\n"
                "{\"type\":\"assistant\",\"timestamp\":\"2026-06-01T10:00:05Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"a\"}],\"usage\":{\"input_tokens\":50,\"cache_creation_input_tokens\":10,\"cache_read_input_tokens\":500,\"output_tokens\":10}}}\n"
                "{\"type\":\"assistant\",\"timestamp\":\"2026-06-01T10:00:20Z\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"b\"}],\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":100,\"cache_creation_input_tokens\":20,\"cache_read_input_tokens\":2000,\"output_tokens\":30}}}\n";
            const std::wstring pathC = projDir + L"\\" + sidC + L".jsonl";
            MakeJsonl(pathC, sessC, 5000, 5000);
            TranscriptRef rc;
            rc.sessionId = sidC;
            rc.path = pathC;
            ::GetFileAttributesExW(pathC.c_str(), GetFileExInfoStandard, &fad);
            rc.sizeBytes = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            rc.mtimeMs = 5000;
            rc.birthMs = 5000;
            auto ecx1 = LoadOrRefreshSessionIndexIn(idxDir, rc);
            CHECK(ecx1.valid && ecx1.stats.contextTokens == 2150, "index: contextTokens = newest assistant usage (2150), persisted to the sidecar");
            auto ecx2 = LoadOrRefreshSessionIndexIn(idxDir, rc);
            CHECK(ecx2.valid && ecx2.stats.contextTokens == 2150, "index: contextTokens round-trips a (size,mtime) cache hit");

            // Overwrite the sidecar with an OLD-format one (no ctxTokens key) but a matching
            // (size,mtime) cache key + assistantLines>0 -> the next load BACKFILLS from the transcript
            // instead of trusting the hit with 0 context.
            auto o = json::Value::MkObj();
            o.Set(L"sid", json::Value::MkStr(sidC));
            o.Set(L"size", json::Value::MkNum(static_cast<double>(rc.sizeBytes)));
            o.Set(L"mtime", json::Value::MkNum(5000.0));
            o.Set(L"parsedBytes", json::Value::MkNum(static_cast<double>(sessC.size())));
            o.Set(L"assistantLines", json::Value::MkNum(2));
            const std::wstring dumped = json::Dump(o); // ASCII content -> narrows to valid UTF-8
            MakeJsonl(idxDir + L"\\" + sidC + L".json", std::string(dumped.begin(), dumped.end()), 5000, 5000);
            auto ecx3 = LoadOrRefreshSessionIndexIn(idxDir, rc);
            CHECK(ecx3.valid && ecx3.stats.contextTokens == 2150, "index: an old (no-ctxTokens) sidecar is backfilled from the transcript on load");
        }

        // pathsAccessed flow into the index + the file/dir scopes end-to-end.
        {
            const std::wstring sidP = L"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee";
            MakeJsonl(projDir + L"\\" + sidP + L".jsonl",
                      "{\"type\":\"assistant\",\"timestamp\":\"2026-06-01T10:00:00Z\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"t\",\"name\":\"Read\",\"input\":{\"file_path\":\"K:\\\\proj\\\\deep\\\\Widget.xaml\"}}]}}\n",
                      4000, 4000);
            TranscriptRef rp;
            rp.sessionId = sidP;
            rp.path = projDir + L"\\" + sidP + L".jsonl";
            ::GetFileAttributesExW(rp.path.c_str(), GetFileExInfoStandard, &fad);
            rp.sizeBytes = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            rp.mtimeMs = 4000;
            const auto ep = LoadOrRefreshSessionIndexIn(idxDir, rp);
            CHECK(ep.valid && ep.stats.pathsAccessed.size() == 1 && ep.stats.pathsAccessed[0] == L"K:\\proj\\deep\\Widget.xaml", "index: tool-touched path captured");
            SessionQuery fq;
            fq.text = L"widget.xaml";
            fq.scopeFiles = true;
            const auto fr = SearchIndexFast({ ep }, fq);
            CHECK(fr.size() == 1, "index->fast: file scope finds the accessed file");
        }

        std::filesystem::remove_all(std::filesystem::path{ root }, ec);
    }

    // --- rg integration smoke (only when ripgrep is on PATH) ---
    if (!ResolveRipgrep().empty())
    {
        std::wprintf(L"  [info] rg resolved: %ls\n", ResolveRipgrep().c_str());
        CHECK(ResolveRipgrep().find(L"rg") != std::wstring::npos, "rg: resolved path names rg");
    }
    else
    {
        std::wprintf(L"  [info] rg not on PATH — in-process fallback paths exercised above\n");
    }
}

// --- Bookmark tags (SessionStore "tags" key + the pure tag primitives) -----------------------
// The tab strip's bookmark badges + the tab menu's Tag panel ride these: NormalizeTagName /
// FoldTagName canonicalize a user-typed name, Encode/DecodeTagList JSON-pack the list into the ONE
// store value, Add/Remove/Get/SetSessionTagsIn are the durable CRUD (empty list => key removed =>
// sparse store), CollectGlobalTags derives the GLOBAL universe (fold-merged, max-activity-stamped,
// sorted desc — the panel's ordering), and ClampMaxTags is the settings cap band (default 20,
// ceiling 40) shared by the Persistence load and the cog Save.
void TestSessionTags()
{
    std::wprintf(L"\n[TestSessionTags]\n");

    // --- NormalizeTagName: trim + control-strip + length cap ---
    CHECK(NormalizeTagName(L"  bug  ") == L"bug", "tags: normalize trims surrounding spaces");
    CHECK(NormalizeTagName(L"a\tb\nc") == L"abc", "tags: normalize strips control chars (tab/newline)");
    CHECK(NormalizeTagName(L"   ").empty(), "tags: whitespace-only normalizes to empty");
    CHECK(NormalizeTagName(L"").empty(), "tags: empty stays empty");
    CHECK(NormalizeTagName(L"perf hot-path") == L"perf hot-path", "tags: internal spaces/punctuation kept as typed");
    {
        const std::wstring longName(100, L'x');
        CHECK(NormalizeTagName(longName).size() == kMaxTagNameLength, "tags: over-long name capped at kMaxTagNameLength");
    }

    // --- FoldTagName: the case-insensitive identity ---
    CHECK(FoldTagName(L"Bug") == FoldTagName(L"bUG"), "tags: fold is case-insensitive");
    CHECK(FoldTagName(L"Bug") != FoldTagName(L"Bugs"), "tags: fold keeps distinct names distinct");

    // --- Encode/Decode: JSON round-trip, tolerant decode, CI dedupe preserving order ---
    CHECK(EncodeTagList({}).empty(), "tags: an empty list encodes to \"\" (key removed; sparse store)");
    {
        const auto rt = DecodeTagList(EncodeTagList({ L"bug", L"perf", L"UI polish" }));
        CHECK(rt.size() == 3 && rt[0] == L"bug" && rt[1] == L"perf" && rt[2] == L"UI polish", "tags: encode->decode round-trips order + content");
    }
    CHECK(DecodeTagList(L"not json").empty(), "tags: malformed value decodes to no tags (never throws)");
    CHECK(DecodeTagList(L"{\"a\":1}").empty(), "tags: a non-array JSON value decodes to no tags");
    {
        const auto d = DecodeTagList(L"[\"bug\", 7, \"BUG\", \"  \", \"perf\"]");
        CHECK(d.size() == 2 && d[0] == L"bug" && d[1] == L"perf", "tags: decode skips non-strings/blank + CI-dedupes, order preserved");
    }

    // --- the store CRUD (explicit temp dir, like the SessionStore tests) ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring store = std::wstring{ tmp } + L"am_tags_" + std::to_wstring(::GetCurrentProcessId());
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path{ store }, ec);

        const std::wstring a = L"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        const std::wstring b = L"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";

        CHECK(GetSessionTagsIn(store, a).empty(), "tags: an untagged session reads no tags");
        CHECK(AddSessionTagIn(store, a, L"bug"), "tags: add the first tag");
        CHECK(AddSessionTagIn(store, a, L"  perf  "), "tags: add a second (normalized) tag");
        CHECK(!AddSessionTagIn(store, a, L"BUG"), "tags: a case-variant duplicate add is a no-op (false)");
        {
            const auto t = GetSessionTagsIn(store, a);
            CHECK(t.size() == 2 && t[0] == L"bug" && t[1] == L"perf", "tags: stored in add order, normalized");
        }
        CHECK(AddSessionTagIn(store, b, L"Bug"), "tags: a second session may carry the same tag (its own casing)");

        // the tags key coexists with a title on the same record (the generalized store).
        CHECK(SetSessionStoreFieldIn(store, a, L"title", L"T"), "tags: a title coexists on the same record");
        CHECK(GetSessionTagsIn(store, a).size() == 2, "tags: title write left the tags intact");

        {
            const auto all = LoadAllSessionTagsIn(store);
            CHECK(all.size() == 2 && all.at(a).size() == 2 && all.at(b).size() == 1, "tags: LoadAll gathers both sessions' tags in one scan");
        }

        CHECK(RemoveSessionTagIn(store, a, L"BUG"), "tags: remove is case-insensitive");
        CHECK(!RemoveSessionTagIn(store, a, L"bug"), "tags: removing an absent tag is a no-op (false)");
        {
            const auto t = GetSessionTagsIn(store, a);
            CHECK(t.size() == 1 && t[0] == L"perf", "tags: the other tag survives a remove");
        }
        // removing b's last tag removes the KEY (and the record file — b had nothing else).
        CHECK(RemoveSessionTagIn(store, b, L"bug"), "tags: remove b's only tag");
        CHECK(GetSessionStoreFieldIn(store, b, kSessionStoreTagsKey).empty(), "tags: an emptied list removed the key");
        {
            const auto all = LoadAllSessionTagsIn(store);
            CHECK(all.size() == 1 && all.count(a) == 1, "tags: LoadAll no longer lists the untagged session");
        }
        // SetSessionTagsIn canonicalizes: dupes folded, blanks dropped.
        CHECK(SetSessionTagsIn(store, a, { L"One", L"one", L" ", L"two" }), "tags: bulk set ok");
        {
            const auto t = GetSessionTagsIn(store, a);
            CHECK(t.size() == 2 && t[0] == L"One" && t[1] == L"two", "tags: bulk set stored canonical (CI-deduped, blanks dropped)");
        }

        std::filesystem::remove_all(std::filesystem::path{ store }, ec);
    }

    // --- tag COLORS: the profile-level tag-colors.json (the tag editor's color picker) ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring dir = std::wstring{ tmp } + L"am_tagcolors_" + std::to_wstring(::GetCurrentProcessId());
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path{ dir }, ec);

        CHECK(GetTagColorIn(dir, L"bug").empty(), "tag colors: unset tag reads \"\" (hash fallback)");
        CHECK(LoadAllTagColorsIn(dir).empty(), "tag colors: no file -> empty map");
        CHECK(SetTagColorIn(dir, L"Bug", L"#FF112233"), "tag colors: store a #AARRGGBB");
        CHECK(GetTagColorIn(dir, L"bug") == L"#FF112233", "tag colors: keyed by the FOLDED name (case-insensitive identity)");
        CHECK(GetTagColorIn(dir, L"  BUG  ") == L"#FF112233", "tag colors: lookup normalizes first (trim + fold)");
        CHECK(!SetTagColorIn(dir, L"Bug", L"red"), "tag colors: a non-hex value is refused");
        CHECK(!SetTagColorIn(dir, L"Bug", L"#GG112233"), "tag colors: bad hex digits refused");
        CHECK(!SetTagColorIn(dir, L"Bug", L"#123"), "tag colors: wrong length refused");
        CHECK(GetTagColorIn(dir, L"bug") == L"#FF112233", "tag colors: the stored value survives refused writes");
        CHECK(SetTagColorIn(dir, L"Bug", L"#FF112233"), "tag colors: an unchanged re-set is a dedup'd success");
        CHECK(SetTagColorIn(dir, L"perf", L"#4FC3F7"), "tag colors: a legacy #RRGGBB shape is accepted too");
        {
            const auto all = LoadAllTagColorsIn(dir);
            CHECK(all.size() == 2 && all.at(L"bug") == L"#FF112233" && all.at(L"perf") == L"#4FC3F7", "tag colors: LoadAll gathers both, folded-keyed");
        }
        CHECK(SetTagColorIn(dir, L"BUG", L""), "tag colors: \"\" removes the entry (back to the hash)");
        CHECK(GetTagColorIn(dir, L"bug").empty(), "tag colors: removed entry reads \"\"");
        CHECK(SetTagColorIn(dir, L"bug", L""), "tag colors: removing an absent entry is a no-op success");
        CHECK(LoadAllTagColorsIn(dir).size() == 1, "tag colors: the other entry survives a remove");
        CHECK(!SetTagColorIn(dir, L"   ", L"#FF112233"), "tag colors: a blank tag name is refused");
        CHECK(!SetTagColorIn(L"", L"bug", L"#FF112233"), "tag colors: an empty state dir is refused");
        CHECK(GetTagColorIn(L"", L"bug").empty(), "tag colors: an empty state dir reads \"\"");

        std::filesystem::remove_all(std::filesystem::path{ dir }, ec);
    }

    // --- the KNOWN-TAG registry (tags.json): a tag survives 0 carriers until explicitly removed ---
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring dir = std::wstring{ tmp } + L"am_tagreg_" + std::to_wstring(::GetCurrentProcessId());
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path{ dir }, ec);

        CHECK(LoadKnownTagsIn(dir).empty(), "tag registry: no file -> empty");
        CHECK(RegisterKnownTagIn(dir, L"Bug"), "tag registry: register a tag");
        CHECK(RegisterKnownTagIn(dir, L"  bug "), "tag registry: a CI/normalized re-register is a no-op success");
        {
            const auto k = LoadKnownTagsIn(dir);
            CHECK(k.size() == 1 && k[0] == L"Bug", "tag registry: registered once, creation casing kept");
        }
        CHECK(RegisterKnownTagIn(dir, L"perf"), "tag registry: register a second tag");
        CHECK(!RegisterKnownTagIn(dir, L"   "), "tag registry: a blank name is refused");
        CHECK(UnregisterKnownTagIn(dir, L"BUG"), "tag registry: unregister is case-insensitive");
        {
            const auto k = LoadKnownTagsIn(dir);
            CHECK(k.size() == 1 && k[0] == L"perf", "tag registry: the other tag survives an unregister");
        }
        CHECK(UnregisterKnownTagIn(dir, L"bug"), "tag registry: unregistering an absent tag is a no-op success");
        CHECK(UnregisterKnownTagIn(dir, L"perf"), "tag registry: empty the registry");
        CHECK(LoadKnownTagsIn(dir).empty(), "tag registry: an emptied registry reads empty (a valid [] file)");
        CHECK(!RegisterKnownTagIn(L"", L"bug"), "tag registry: an empty state dir is refused");
        CHECK(!UnregisterKnownTagIn(L"", L"bug"), "tag registry: an empty state dir is refused (unregister)");

        std::filesystem::remove_all(std::filesystem::path{ dir }, ec);
    }

    // --- CollectGlobalTags + the registry: a 0-carrier known tag stays listed (count 0, sorts last) ---
    {
        std::unordered_map<std::wstring, std::vector<std::wstring>> tbs{
            { L"s1", { L"bug" } },
        };
        std::unordered_map<std::wstring, int64_t> act{
            { L"s1", 1000 },
        };
        const std::vector<std::wstring> known{ L"Idea", L"BUG" }; // Idea uncarried; BUG carried (casing differs)
        const auto u = CollectGlobalTags(tbs, act, known);
        CHECK(u.size() == 2, "tag registry: an uncarried known tag adds ONE row; a carried known tag never duplicates");
        CHECK(u[0].name == L"bug" && u[0].sessionCount == 1 && u[0].lastActivityUnixMs == 1000, "tag registry: a carried tag keeps its carrier casing/count/activity");
        CHECK(u[1].name == L"Idea" && u[1].sessionCount == 0 && u[1].lastActivityUnixMs == 0, "tag registry: the uncarried known tag lists at count 0 / activity 0 (sorts last), registry casing");
        // The 2-arg overload is unchanged: no registry -> the uncarried tag doesn't exist.
        CHECK(CollectGlobalTags(tbs, act).size() == 1, "tag registry: the 2-arg overload stays purely session-derived");
    }

    // --- CollectGlobalTags: fold-merge + max activity + count + ordering + display casing ---
    {
        std::unordered_map<std::wstring, std::vector<std::wstring>> tagsBySession{
            { L"s1", { L"bug", L"perf" } },
            { L"s2", { L"BUG" } }, // the same tag as s1's "bug", different casing — s2 is more active
            { L"s3", { L"idea" } }, // s3 has NO activity entry -> 0
        };
        std::unordered_map<std::wstring, int64_t> activity{
            { L"s1", 1000 },
            { L"s2", 5000 },
        };
        const auto u = CollectGlobalTags(tagsBySession, activity);
        CHECK(u.size() == 3, "tags: universe fold-merges case variants (3 distinct tags)");
        CHECK(u[0].lastActivityUnixMs == 5000 && FoldTagName(u[0].name) == L"bug", "tags: sorted by max activity desc (bug via s2 first)");
        CHECK(u[0].name == L"BUG", "tags: display casing = the highest-activity carrier's");
        CHECK(u[0].sessionCount == 2, "tags: sessionCount counts every carrier across casings");
        CHECK(u[1].name == L"perf" && u[1].lastActivityUnixMs == 1000, "tags: perf second (activity 1000)");
        CHECK(u[2].name == L"idea" && u[2].lastActivityUnixMs == 0, "tags: a session absent from the activity map stamps 0 (sorts last)");
    }
    {
        // determinism on a full tie: activity equal -> folded-name ASC.
        std::unordered_map<std::wstring, std::vector<std::wstring>> tbs{
            { L"s1", { L"zeta", L"alpha" } },
        };
        const auto u = CollectGlobalTags(tbs, {});
        CHECK(u.size() == 2 && u[0].name == L"alpha" && u[1].name == L"zeta", "tags: activity tie breaks by folded name asc");
    }

    // --- ClampMaxTags + the settings round-trip ---
    CHECK(ClampMaxTags(0) == 20, "tags: cap 0/absent -> the 20 default");
    CHECK(ClampMaxTags(1) == 1, "tags: cap floor is 1");
    CHECK(ClampMaxTags(20) == 20, "tags: the default passes through");
    CHECK(ClampMaxTags(40) == 40, "tags: the ceiling passes through");
    CHECK(ClampMaxTags(41) == 40, "tags: over-ceiling clamps to 40");
    {
        AppSettings s;
        CHECK(s.maxTags == 20, "tags: AppSettings default is 20");
        s.maxTags = 33;
        const auto back = AppSettingsFromJson(ToJson(s));
        CHECK(back.maxTags == 33, "tags: maxTags round-trips through settings.json");
        auto o = json::Value::MkObj();
        o.Set(L"maxTags", json::Value::MkNum(999));
        CHECK(AppSettingsFromJson(o).maxTags == 40, "tags: a hand-edited over-ceiling value self-heals to 40 on load");
        auto z = json::Value::MkObj();
        z.Set(L"maxTags", json::Value::MkNum(0));
        CHECK(AppSettingsFromJson(z).maxTags == 20, "tags: a stored 0 self-heals to the 20 default");
    }

    // --- ClampTooltipTagsOpacity + the settings round-trip (the tooltip tag-chip opacity slider) ---
    CHECK(ClampTooltipTagsOpacity(0.0) == 0.9, "tags: opacity 0/absent -> the 0.9 (90%) default");
    CHECK(ClampTooltipTagsOpacity(-1.0) == 0.9, "tags: a negative opacity -> the 0.9 default");
    CHECK(ClampTooltipTagsOpacity(0.05) == 0.1, "tags: opacity floor is 0.1 (10%)");
    CHECK(ClampTooltipTagsOpacity(0.5) == 0.5, "tags: an in-band opacity passes through");
    CHECK(ClampTooltipTagsOpacity(1.0) == 1.0, "tags: the 1.0 ceiling passes through");
    CHECK(ClampTooltipTagsOpacity(2.0) == 1.0, "tags: over-ceiling opacity clamps to 1.0");
    {
        AppSettings s;
        CHECK(s.tooltipTagsOpacity > 0.899 && s.tooltipTagsOpacity < 0.901, "tags: AppSettings tooltipTagsOpacity default is 0.9");
        s.tooltipTagsOpacity = 0.4;
        const auto back = AppSettingsFromJson(ToJson(s));
        CHECK(back.tooltipTagsOpacity > 0.399 && back.tooltipTagsOpacity < 0.401, "tags: tooltipTagsOpacity round-trips through settings.json");
        auto hi = json::Value::MkObj();
        hi.Set(L"tooltipTagsOpacity", json::Value::MkNum(5.0));
        CHECK(AppSettingsFromJson(hi).tooltipTagsOpacity == 1.0, "tags: a hand-edited over-ceiling opacity self-heals to 1.0 on load");
        auto lo = json::Value::MkObj();
        lo.Set(L"tooltipTagsOpacity", json::Value::MkNum(0.0));
        CHECK(AppSettingsFromJson(lo).tooltipTagsOpacity == 0.9, "tags: a stored 0 opacity self-heals to the 0.9 default");
    }
}

// --- StripSummaryTableRules: COLLAPSE a one-line message's embedded tables (drop rules + de-frame) ---
// When a summary message is flattened to one line (wrap-off panel / the always-one-line Sessions
// detail), an embedded table is pure noise. The collapser (1) DROPS box-drawing "├──┼──┤" / markdown
// "|---|---|" rule rows, and (2) DE-FRAMES data rows: "│ Name │ Age │" -> "Name · Age" (strip the
// │/| bars + padding, rejoin cells with " · " = U+00B7, drop empty cells). Box-drawing chars + the dot
// are written as \x escapes / a built separator so the test is source-encoding independent.

// --- ExtractPathsFromText (tab color modes): absolute-path mining from shell command strings ---
// Quoted paths consume freely to the closing quote (spaces anywhere, leaf included — the shell
// itself demands the quotes for such a command to work, so the quotes are the ground truth).
// Unquoted paths accept ONE interior space only in a NON-LEAF segment (a later separator confirms
// it), so prose after a path is never swallowed. Prose punctuation terminates; unbalanced closers
// and sentence dots trim; drive/UNC roots alone carry no signal; matches dedupe case-insensitively.
void TestExtractPathsFromText()
{
    std::wprintf(L"[extract paths from text]\n");
    const auto one = [](std::wstring_view text) -> std::wstring {
        const auto v = ExtractPathsFromText(text, 8);
        return v.size() == 1 ? v[0] : (v.empty() ? std::wstring{ L"<none>" } : std::wstring{ L"<many:" } + std::to_wstring(v.size()) + L">");
    };

    // Plain absolute path in a command.
    CHECK(one(L"cat K:\\repo\\src\\main.cs") == L"K:\\repo\\src\\main.cs", "plain drive path extracted");
    // Prose after the path is NOT swallowed (the space rule: no later separator => stop at the space).
    CHECK(one(L"cat K:\\repo\\src\\x.txt and then commit") == L"K:\\repo\\src\\x.txt", "prose after a path not swallowed");
    // ONE interior space in a FOLDER name — confirmed by the later separator.
    CHECK(one(L"dir C:\\Program Files\\App\\x.exe") == L"C:\\Program Files\\App\\x.exe", "one space inside a folder name (unquoted) kept");
    // An unquoted leaf-with-space truncates AT the space — the parent (what the inference votes with) stays exact.
    CHECK(one(L"type K:\\repo\\my file.txt") == L"K:\\repo\\my", "unquoted leaf space truncates (parent exact)");
    // Quoted: spaces free-form, leaf included.
    CHECK(one(L"cd \"K:\\My Projects\\some repo\"") == L"K:\\My Projects\\some repo", "quoted path with spaces (incl. leaf) kept whole");
    // Quoted "(x86)" — two spaces in one folder name + a balanced paren pair survive.
    CHECK(one(L"run \"C:\\Program Files (x86)\\App\\tool.exe\"") == L"C:\\Program Files (x86)\\App\\tool.exe", "quoted Program Files (x86) kept whole");
    // A later colon is a line ref / prose, never part of the path.
    CHECK(one(L"see K:\\a\\f.cs:123") == L"K:\\a\\f.cs", "file.cs:123 line ref stops at the colon");
    // Prose punctuation terminates; sentence dot + unbalanced closer trim.
    CHECK(one(L"K:\\a\\b, then more") == L"K:\\a\\b", "comma terminates");
    CHECK(one(L"(in K:\\repo\\src)") == L"K:\\repo\\src", "unbalanced trailing ) trimmed");
    CHECK(one(L"open K:\\repo\\src.") == L"K:\\repo\\src", "sentence dot trimmed");
    // Shell operators terminate.
    CHECK(one(L"cd K:\\a\\b&&echo hi") == L"K:\\a\\b", "&& terminates");
    // A glob tail stops at the wildcard; the dir prefix (trailing sep trimmed) is still the signal.
    CHECK(one(L"del K:\\repo\\*.tmp") == L"K:\\repo", "wildcard stops; dir prefix kept");
    // UNC: a child segment is required below the share root.
    CHECK(one(L"copy \\\\nas\\share\\proj\\a.bin") == L"\\\\nas\\share\\proj\\a.bin", "UNC path extracted");
    CHECK(ExtractPathsFromText(L"ping \\\\nas\\share", 8).empty(), "a bare UNC share root carries no signal");
    // Forward slashes tolerated; the original spelling is returned.
    CHECK(one(L"gcc K:/source/x/file.h") == L"K:/source/x/file.h", "forward-slash spelling preserved");
    // Word boundary: a drive spec mid-token never matches.
    CHECK(ExtractPathsFromText(L"ABCK:\\x\\y", 8).empty(), "no match mid-word (ABCK:)");
    // Bare roots carry no signal.
    CHECK(ExtractPathsFromText(L"K:\\ alone", 8).empty(), "a bare drive root is not a path");
    // Case/separator-insensitive dedupe; first spelling wins; the cap is honored.
    {
        const auto v = ExtractPathsFromText(L"K:\\a\\b K:/a/b k:\\A\\B", 8);
        CHECK(v.size() == 1 && v[0] == L"K:\\a\\b", "case/slash variants dedupe to the first spelling");
        const auto capped = ExtractPathsFromText(L"K:\\a\\1 K:\\a\\2 K:\\a\\3", 2);
        CHECK(capped.size() == 2, "maxPaths cap honored");
    }
    // Two paths in one command line.
    {
        const auto v = ExtractPathsFromText(L"copy K:\\src\\a.txt C:\\dst\\b.txt", 8);
        CHECK(v.size() == 2 && v[0] == L"K:\\src\\a.txt" && v[1] == L"C:\\dst\\b.txt", "two paths in one command both extracted");
    }

    // The ClassifyTranscriptLine wiring: a Bash tool_use `command` feeds toolPaths. The canned line
    // is built via json::Value + Dump so the JSON escaping is exact, never hand-rolled.
    {
        auto input = json::Value::MkObj();
        input.Set(L"command", json::Value::MkStr(L"cd \"K:\\My Projects\\repo\" && type K:\\repo\\src\\main.cs"));
        auto tu = json::Value::MkObj();
        tu.Set(L"type", json::Value::MkStr(L"tool_use"));
        tu.Set(L"name", json::Value::MkStr(L"Bash"));
        tu.Set(L"input", std::move(input));
        auto contentArr = json::Value::MkArr();
        contentArr.Push(std::move(tu));
        auto msg = json::Value::MkObj();
        msg.Set(L"content", std::move(contentArr));
        auto root = json::Value::MkObj();
        root.Set(L"type", json::Value::MkStr(L"assistant"));
        root.Set(L"message", std::move(msg));
        const auto facts = ClassifyTranscriptLine(json::Dump(root));
        bool foundQuoted = false;
        bool foundPlain = false;
        for (const auto& p : facts.toolPaths)
        {
            foundQuoted = foundQuoted || p == L"K:\\My Projects\\repo";
            foundPlain = foundPlain || p == L"K:\\repo\\src\\main.cs";
        }
        CHECK(foundQuoted && foundPlain, "Bash command paths mined into toolPaths (quoted + plain)");
    }
}

// --- InferWorkingDirectory (tab color modes): the pure inferred-workdir picker ---
// RANKED BY OCCURRENCE: every tool-touched path votes for its ancestor DIRECTORY chain (leaf
// excluded) and belongs to one disjoint WORK CLUSTER (its git root via the injected resolver,
// else its top-level dir); clusters are ranked by vote count — the STRICT top wins (plurality;
// a tied top => the fallback cwd). A git cluster answers its repo root AS-IS; a non-git cluster
// answers by LOCAL-MAJORITY DESCENT (a child takes the pick from its base only while it holds
// >50% of the base's own paths — the base-vs-path priority). Keeps a stray one-off read from
// dragging the pick to the drive root the way a plain longest-common-prefix would. Roots never
// win; no usable paths => the fallback (launch cwd).
void TestInferWorkingDirectory()
{
    std::wprintf(L"[infer working directory]\n");
    const std::wstring cwd = L"K:\\fallback";

    // Empty / unusable inputs -> the fallback.
    CHECK(InferWorkingDirectory({}, cwd) == cwd, "no paths -> fallback cwd");
    CHECK(InferWorkingDirectory({ L"" }, cwd) == cwd, "empty path -> fallback cwd");
    CHECK(InferWorkingDirectory({ L"src\\a\\f.cs", L"src\\b\\g.cs" }, cwd) == cwd, "relative paths never vote -> fallback");
    CHECK(InferWorkingDirectory({ L"K:\\rootfile.txt" }, cwd) == cwd, "a drive-root file has no candidate dir -> fallback");

    // One file: its parent chain is 100% — the deepest ancestor (its immediate parent) wins.
    CHECK(InferWorkingDirectory({ L"K:\\repo\\src\\f.cs" }, cwd) == L"K:\\repo\\src", "single file -> its parent dir");

    // All files under one subtree: the deepest FULLY-shared dir wins (not the shallower repo root).
    {
        const std::vector<std::wstring> paths = {
            L"K:\\repo\\src\\app\\a.cs",
            L"K:\\repo\\src\\app\\b.cs",
            L"K:\\repo\\src\\app\\sub\\c.cs",
        };
        CHECK(InferWorkingDirectory(paths, cwd) == L"K:\\repo\\src\\app", "deepest majority dir wins (sub only holds 1/3)");
    }

    // Stray-path robustness: a minority of out-of-repo reads must NOT drag the pick toward the
    // root (the longest-common-prefix failure mode) — the repo subtree keeps its strict majority.
    {
        const std::vector<std::wstring> paths = {
            L"K:\\repo\\src\\a.cs",
            L"K:\\repo\\src\\b.cs",
            L"K:\\repo\\src\\c.cs",
            L"C:\\Users\\me\\.claude\\CLAUDE.md", // the classic stray read
            L"C:\\Windows\\Temp\\t.tmp",
        };
        CHECK(InferWorkingDirectory(paths, cwd) == L"K:\\repo\\src", "stray minority reads don't drag the pick off the majority subtree");
    }

    // Majority ranking across sibling subtrees: neither sibling has >50%, their shared parent does.
    {
        const std::vector<std::wstring> paths = {
            L"K:\\repo\\src\\area1\\a.cs",
            L"K:\\repo\\src\\area1\\b.cs",
            L"K:\\repo\\src\\area2\\c.cs",
            L"K:\\repo\\src\\area2\\d.cs",
            L"K:\\repo\\docs\\readme.md",
        };
        CHECK(InferWorkingDirectory(paths, cwd) == L"K:\\repo\\src", "siblings below majority -> their shared parent (4/5) wins");
    }

    // Dominant deep subtree: a dir with a strict majority beats its own (also-majority) ancestors
    // by depth — the "most reoccurring occurrence" picks the deepest dominant dir.
    {
        const std::vector<std::wstring> paths = {
            L"K:\\repo\\src\\hot\\a.cs",
            L"K:\\repo\\src\\hot\\b.cs",
            L"K:\\repo\\src\\hot\\c.cs",
            L"K:\\repo\\src\\d.cs",
            L"K:\\repo\\e.cs",
        };
        CHECK(InferWorkingDirectory(paths, cwd) == L"K:\\repo\\src\\hot", "a deep 3/5-majority dir beats its shallower ancestors");
    }

    // Exactly half is NOT a majority (strict >50%): 2 of 4 under each of two drives -> fallback.
    {
        const std::vector<std::wstring> paths = {
            L"K:\\one\\a.cs",
            L"K:\\one\\b.cs",
            L"C:\\two\\c.cs",
            L"C:\\two\\d.cs",
        };
        CHECK(InferWorkingDirectory(paths, cwd) == cwd, "a 50/50 drive split has no strict majority -> fallback");
    }

    // Separator + case insensitivity (Rule #8): variants collapse onto one key; the returned
    // spelling is the FIRST seen (its case preserved, separators normalized to backslash).
    {
        const std::vector<std::wstring> paths = {
            L"K:/Repo/Src/a.cs",
            L"k:\\repo\\src\\b.cs",
            L"K:\\REPO\\SRC\\c.cs",
        };
        CHECK(InferWorkingDirectory(paths, cwd) == L"K:\\Repo\\Src", "case/slash variants collapse; first-seen case kept, separators normalized");
    }

    // Deduped input contract: pathsAccessed is a per-file SET, so one hot file can't stuff the
    // ballot — but the same file listed once among siblings still counts once per file.
    {
        const std::vector<std::wstring> paths = {
            L"K:\\repo\\hot\\same.cs", // one distinct file
            L"K:\\repo\\cold\\a.cs",
            L"K:\\repo\\cold\\b.cs",
            L"K:\\repo\\cold\\c.cs",
        };
        CHECK(InferWorkingDirectory(paths, cwd) == L"K:\\repo\\cold", "per-file votes: the 3-file dir outweighs the 1-file dir");
    }

    // UNC: candidates start below the \\server\share root; the share root itself never wins.
    {
        const std::vector<std::wstring> paths = {
            L"\\\\nas\\share\\proj\\a.cs",
            L"\\\\nas\\share\\proj\\b.cs",
        };
        CHECK(InferWorkingDirectory(paths, cwd) == L"\\\\nas\\share\\proj", "UNC: deepest majority below the share root");
        CHECK(InferWorkingDirectory({ L"\\\\nas\\share" }, cwd) == cwd, "UNC share root alone has no candidate -> fallback");
    }

    // A searched DIRECTORY input (Grep/Glob path) votes one level shallower (its own parent) —
    // acceptable by design; here it reinforces the same subtree.
    {
        const std::vector<std::wstring> paths = {
            L"K:\\repo\\src\\a.cs",
            L"K:\\repo\\src\\b.cs",
            L"K:\\repo\\src", // a dir input: votes K:\repo (parent chain), not itself
        };
        CHECK(InferWorkingDirectory(paths, cwd) == L"K:\\repo\\src", "a dir input can't demote the file majority");
    }

    // Trailing separators are tolerated (a dir input with a trailing slash).
    CHECK(InferWorkingDirectory({ L"K:\\repo\\src\\f.cs", L"K:\\repo\\src\\g.cs\\" }, cwd) == L"K:\\repo\\src", "trailing separator tolerated");

    // --- the git-root arm ("Use .git folder to infer", AppSettings::inferGitRoot) -------------
    // A FAKE resolver (pure — no filesystem): dir -> its enclosing git root. Layout: K:\repoA
    // (a checkout), K:\repoA\.claude\worktrees\wt (a NESTED worktree — nearest root wins),
    // K:\repoB (a second checkout); everything else is outside any repo.
    {
        const auto low = [](std::wstring s) {
            for (auto& c : s)
            {
                c = (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : (c == L'/' ? L'\\' : c);
            }
            return s;
        };
        const auto fakeGit = [low](const std::wstring& d) -> std::wstring {
            const std::wstring k = low(d);
            if (k.rfind(L"k:\\repoa\\.claude\\worktrees\\wt", 0) == 0)
            {
                return L"K:\\repoA\\.claude\\worktrees\\wt";
            }
            if (k.rfind(L"k:\\repoa", 0) == 0)
            {
                return L"K:\\repoA";
            }
            if (k.rfind(L"k:\\repob", 0) == 0)
            {
                return L"K:\\repoB";
            }
            return {};
        };

        // The reported "switching modes suddenly recolors" case: work CONCENTRATED in a repo
        // subfolder infers the REPO (== the usual launch cwd), never the subfolder — while the
        // classic arm (no resolver) picks the deep majority dir.
        const std::vector<std::wstring> inRepo = {
            L"K:\\repoA\\src\\cascadia\\a.cpp",
            L"K:\\repoA\\src\\cascadia\\b.cpp",
            L"K:\\repoA\\doc\\c.md",
        };
        CHECK(InferWorkingDirectory(inRepo, cwd, fakeGit) == L"K:\\repoA", "git arm: a repo-majority snaps to the repo ROOT, never deeper");
        CHECK(InferWorkingDirectory(inRepo, cwd) == L"K:\\repoA\\src\\cascadia", "no resolver: the classic deepest-majority pick is unchanged");
        CHECK(InferWorkingDirectory(inRepo, cwd, {}) == L"K:\\repoA\\src\\cascadia", "an EMPTY resolver == the classic two-arg behavior");

        // Nested worktree: per-path NEAREST root — worktree paths tally the worktree, outer-repo
        // paths the checkout; the worktree's 3/4 strict majority keys the worktree root.
        const std::vector<std::wstring> wt = {
            L"K:\\repoA\\.claude\\worktrees\\wt\\src\\a.cpp",
            L"K:\\repoA\\.claude\\worktrees\\wt\\src\\b.cpp",
            L"K:\\repoA\\.claude\\worktrees\\wt\\doc\\c.md",
            L"K:\\repoA\\src\\d.cpp",
        };
        CHECK(InferWorkingDirectory(wt, cwd, fakeGit) == L"K:\\repoA\\.claude\\worktrees\\wt", "git arm: a nested worktree majority keys the WORKTREE, not the outer checkout");

        // Two repos split 2/2: a TIED top rank has no single "most reoccurring" cluster -> fallback.
        const std::vector<std::wstring> split = {
            L"K:\\repoA\\src\\a.cpp",
            L"K:\\repoA\\src\\b.cpp",
            L"K:\\repoB\\src\\c.cpp",
            L"K:\\repoB\\src\\d.cpp",
        };
        CHECK(InferWorkingDirectory(split, cwd, fakeGit) == cwd, "git arm: a 50/50 repo split is a tied top rank -> fallback");

        // Plurality — the RANKING headline: 3 vs 2 across two repos needs no majority; the
        // most-occurrences repo wins (the old strict-majority rule fell back to the cwd here).
        const std::vector<std::wstring> plurality = {
            L"K:\\repoA\\src\\a.cpp",
            L"K:\\repoA\\src\\b.cpp",
            L"K:\\repoA\\doc\\c.md",
            L"K:\\repoB\\src\\d.cpp",
            L"K:\\repoB\\src\\e.cpp",
        };
        CHECK(InferWorkingDirectory(plurality, cwd, fakeGit) == L"K:\\repoA", "ranking: a 3-vs-2 repo plurality picks the most-occurrences repo (no majority bar)");

        // Out-of-repo votes DILUTE the git tally but a 3/5 repo majority still snaps to the root.
        const std::vector<std::wstring> diluted = {
            L"K:\\repoA\\src\\a.cpp",
            L"K:\\repoA\\src\\b.cpp",
            L"K:\\repoA\\tools\\c.ps1",
            L"C:\\Users\\me\\.claude\\CLAUDE.md",
            L"C:\\Windows\\Temp\\t.tmp",
        };
        CHECK(InferWorkingDirectory(diluted, cwd, fakeGit) == L"K:\\repoA", "git arm: 3/5 in one repo still snaps to its root");

        // Work mostly OUTSIDE any repo: a repo cluster is ranked like any other, so the 1-vote
        // repo loses to the 3-vote notes cluster, whose descent lands its concentrated subdir.
        const std::vector<std::wstring> notes = {
            L"C:\\notes\\x\\a.md",
            L"C:\\notes\\x\\b.md",
            L"C:\\notes\\x\\c.md",
            L"K:\\repoA\\src\\f.cs",
        };
        CHECK(InferWorkingDirectory(notes, cwd, fakeGit) == L"C:\\notes\\x", "ranking: a 1-vote repo can't outrank the 3-vote notes cluster");

        // A resolver yielding a BARE ROOT is ignored (roots never win — same bar as the ancestor
        // arm): the ancestor pick proceeds untouched.
        const auto rootGit = [](const std::wstring&) -> std::wstring { return L"K:\\"; };
        CHECK(InferWorkingDirectory({ L"K:\\junk\\x\\a.txt", L"K:\\junk\\x\\b.txt" }, cwd, rootGit) == L"K:\\junk\\x", "git arm: a bare drive-root resolver result is rejected");
        const auto shareGit = [](const std::wstring&) -> std::wstring { return L"\\\\nas\\share"; };
        CHECK(InferWorkingDirectory({ L"\\\\nas\\share\\proj\\a.cs", L"\\\\nas\\share\\proj\\b.cs" }, cwd, shareGit) == L"\\\\nas\\share\\proj", "git arm: a bare UNC-share resolver result is rejected");

        // Resolver spelling is normalized (separators -> backslash, trailing sep stripped); a
        // below-share UNC root is a valid winner.
        const auto slashGit = [](const std::wstring&) -> std::wstring { return L"K:/repoA/"; };
        CHECK(InferWorkingDirectory({ L"K:\\repoA\\src\\a.cpp" }, cwd, slashGit) == L"K:\\repoA", "git arm: resolver spelling separator-normalized + trailing sep stripped");
        const auto uncGit = [](const std::wstring&) -> std::wstring { return L"\\\\nas\\share\\proj"; };
        CHECK(InferWorkingDirectory({ L"\\\\nas\\share\\proj\\deep\\a.cs", L"\\\\nas\\share\\proj\\deep\\b.cs" }, cwd, uncGit) == L"\\\\nas\\share\\proj", "git arm: a below-share UNC git root wins");
    }

    // --- occurrence RANKING: clusters by count + the base-vs-child local-majority descent ------
    {
        // Non-git plurality between top-level clusters, then descent within the winner: docs(3)
        // outranks tools(2); inside docs, p holds 2 of its 3 paths (a local majority) -> docs\p.
        {
            const std::vector<std::wstring> paths = {
                L"K:\\docs\\p\\a.md",
                L"K:\\docs\\p\\b.md",
                L"K:\\docs\\q\\c.md",
                L"K:\\tools\\t1.ps1",
                L"K:\\tools\\t2.ps1",
            };
            CHECK(InferWorkingDirectory(paths, cwd) == L"K:\\docs\\p", "ranking: the 3-vote cluster outranks the 2-vote one; descent lands its majority subdir");
        }
        // Base-vs-child by occurrence: hot holds 7 of the repo's OWN 12 paths (a local majority)
        // -> hot takes the pick from its base, even at only 7/20 of the whole corpus (the old
        // global-majority rule stopped at the repo).
        {
            std::vector<std::wstring> paths;
            for (int i = 0; i < 7; ++i)
            {
                paths.push_back(L"K:\\repo\\hot\\f" + std::to_wstring(i) + L".cs");
            }
            for (int i = 0; i < 5; ++i)
            {
                paths.push_back(L"K:\\repo\\m" + std::to_wstring(i) + L".cs");
            }
            for (int i = 0; i < 8; ++i)
            {
                paths.push_back(L"C:\\o\\g" + std::to_wstring(i) + L".txt");
            }
            CHECK(InferWorkingDirectory(paths, cwd) == L"K:\\repo\\hot", "ranking: a child with the majority of its BASE's paths takes the pick (7 of repo's 12)");
        }
        // ...and when NO child dominates its base (a 2/2/1 sibling split), the BASE keeps it.
        {
            const std::vector<std::wstring> paths = {
                L"K:\\repo\\a\\1.cs",
                L"K:\\repo\\a\\2.cs",
                L"K:\\repo\\b\\3.cs",
                L"K:\\repo\\b\\4.cs",
                L"K:\\repo\\c\\5.cs",
            };
            CHECK(InferWorkingDirectory(paths, cwd) == L"K:\\repo", "ranking: no child holds a majority of the base's paths -> the base keeps the pick");
        }
        // A 1/1/1 three-way top tie has no "most reoccurring" cluster -> fallback.
        {
            const std::vector<std::wstring> paths = {
                L"K:\\x\\a.txt",
                L"C:\\y\\b.txt",
                L"D:\\z\\c.txt",
            };
            CHECK(InferWorkingDirectory(paths, cwd) == cwd, "ranking: a three-way 1/1/1 top tie -> fallback");
        }
    }

    // --- ExcludePathsUnderRoots (the temp-scratch vote filter) ---------------------------------
    // The live NumSharp regression: a session whose ONLY captured path was one scratchpad write
    // under %TEMP% inferred the SCRATCHPAD dir (a 1/1 "majority"; no git root above temp). Temp
    // paths must never vote — filtered out, the corpus empties and the honest cwd fallback wins.
    {
        const std::vector<std::wstring> tempRoots = {
            L"C:\\Users\\ELI\\AppData\\Local\\Temp",
            L"C:\\Windows\\Temp",
        };

        // Segment-boundary containment: the root itself + descendants go; a SIBLING sharing the
        // prefix chars ("C:\Temperature") stays.
        {
            std::vector<std::wstring> paths = {
                L"C:\\Temp\\x\\y.txt",
                L"C:\\Temp",
                L"C:\\Temperature\\probe.log",
                L"K:\\repo\\src\\f.cs",
            };
            ExcludePathsUnderRoots(paths, { L"C:\\Temp" });
            CHECK(paths.size() == 2 && paths[0] == L"C:\\Temperature\\probe.log" && paths[1] == L"K:\\repo\\src\\f.cs",
                  "ExcludePathsUnderRoots: root + descendants dropped; prefix-sibling dir survives");
        }
        // Case / separator / trailing-sep variants fold together (Rule #8).
        {
            std::vector<std::wstring> paths = {
                L"C:\\Users\\ELI\\AppData\\Local\\Temp\\claude\\K--source-NumSharp\\1ffb3335\\scratchpad\\pr-body.md",
                L"c:/users/eli/appdata/local/temp/other.tmp",
                L"K:\\source\\NumSharp\\src\\a.cs",
            };
            ExcludePathsUnderRoots(paths, { L"c:/USERS/eli/AppData/Local/Temp/" });
            CHECK(paths.size() == 1 && paths[0] == L"K:\\source\\NumSharp\\src\\a.cs",
                  "ExcludePathsUnderRoots: case/slash/trailing-sep variants all fold onto the root");
        }
        // Empty roots / empty paths are no-ops.
        {
            std::vector<std::wstring> paths = { L"C:\\Temp\\a.txt" };
            ExcludePathsUnderRoots(paths, {});
            CHECK(paths.size() == 1, "ExcludePathsUnderRoots: empty roots -> no-op");
            std::vector<std::wstring> none;
            ExcludePathsUnderRoots(none, tempRoots);
            CHECK(none.empty(), "ExcludePathsUnderRoots: empty paths -> no-op");
        }

        // THE regression, end-to-end (pure): the lone scratchpad write filters away -> empty
        // corpus -> InferWorkingDirectory falls back to the launch cwd (K:\source\NumSharp).
        {
            const std::wstring nsCwd = L"K:\\source\\NumSharp";
            std::vector<std::wstring> paths = {
                L"C:\\Users\\ELI\\AppData\\Local\\Temp\\claude\\K--source-NumSharp\\1ffb3335\\scratchpad\\pr-body.md",
            };
            CHECK(InferWorkingDirectory(paths, nsCwd) != nsCwd, "regression precondition: unfiltered, the lone temp path WOULD hijack the inference");
            ExcludePathsUnderRoots(paths, tempRoots);
            CHECK(paths.empty(), "regression: the lone scratchpad path is filtered away");
            CHECK(InferWorkingDirectory(paths, nsCwd) == nsCwd, "regression: empty corpus -> the honest cwd fallback (NumSharp)");
        }
        // Temp-FLOOD vs a repo minority: unfiltered, the deep scratchpad chain out-votes the repo;
        // filtered, the repo's 2/2 snaps to its git root.
        {
            const auto repoGit = [](const std::wstring& d) -> std::wstring {
                std::wstring k = d;
                for (auto& ch : k)
                {
                    ch = (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : (ch == L'/' ? L'\\' : ch);
                }
                return k.rfind(L"k:\\source\\numsharp", 0) == 0 ? L"K:\\source\\NumSharp" : std::wstring{};
            };
            std::vector<std::wstring> paths = {
                L"C:\\Users\\ELI\\AppData\\Local\\Temp\\claude\\enc\\sid\\scratchpad\\a.bat",
                L"C:\\Users\\ELI\\AppData\\Local\\Temp\\claude\\enc\\sid\\scratchpad\\b.py",
                L"C:\\Users\\ELI\\AppData\\Local\\Temp\\claude\\enc\\sid\\scratchpad\\c.txt",
                L"K:\\source\\NumSharp\\src\\nd\\x.cs",
                L"K:\\source\\NumSharp\\test\\y.cs",
            };
            ExcludePathsUnderRoots(paths, tempRoots);
            CHECK(paths.size() == 2, "temp flood: the 3 scratchpad votes are gone, the 2 repo votes remain");
            CHECK(InferWorkingDirectory(paths, cwd, repoGit) == L"K:\\source\\NumSharp", "temp flood: the remaining repo majority snaps to the repo root");
        }

        // CollectMachineTempRoots (Win32 smoke): at least the user temp; every root absolute; the
        // first one exists on disk (it's this process's own temp dir).
        {
            const auto roots = CollectMachineTempRoots();
            CHECK(!roots.empty(), "CollectMachineTempRoots: yields at least the user temp dir");
            bool absOk = true;
            for (const auto& r : roots)
            {
                absOk = absOk && r.size() >= 3 && r[1] == L':';
            }
            CHECK(absOk, "CollectMachineTempRoots: every root is drive-absolute");
            if (!roots.empty())
            {
                const DWORD attrs = ::GetFileAttributesW(roots.front().c_str());
                CHECK(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY), "CollectMachineTempRoots: the user temp root exists");
            }
        }
    }

    // --- FindGitRootForDir (the REAL resolver; filesystem smoke) -------------------------------
    {
        CHECK(FindGitRootForDir(L"relative\\x").empty(), "FindGitRootForDir: relative input -> none");
        CHECK(FindGitRootForDir(L"K:\\").empty(), "FindGitRootForDir: a bare drive root -> none (never probed)");
        CHECK(FindGitRootForDir(L"\\\\server\\share").empty(), "FindGitRootForDir: a bare UNC share -> none");
        // The harness runs from the repo's tests dir (run-m5-tests.bat cd's there), so the walk-up
        // from the CURRENT dir must land on a dir that actually holds a .git entry (dir or file —
        // this covers the worktree-file form when run from a worktree checkout). Tolerant when run
        // from outside any repo: only the contract "non-empty => holds .git" is asserted.
        wchar_t cd[MAX_PATH]{};
        if (::GetCurrentDirectoryW(MAX_PATH, cd) > 0)
        {
            const std::wstring root = FindGitRootForDir(cd);
            if (!root.empty())
            {
                CHECK(::GetFileAttributesW((root + L"\\.git").c_str()) != INVALID_FILE_ATTRIBUTES, "FindGitRootForDir: returned dir holds a .git entry");
            }
            else
            {
                CHECK(true, "FindGitRootForDir: cwd outside any repo (tolerated)");
            }
        }
    }
}

// Agentmaster (analyze footprint): ScrubLargeBase64Payloads + the AnalyzeSessionTranscript cache —
// the two halves of the v0.6.x resume crash-loop fix. The scrubber elides pasted-image base64 blobs
// from the raw UTF-8 BEFORE widening (an image-heavy transcript cost 5-8x its size in transient
// allocations for bytes no summary field reads); the cache collapses the resume-time burst (overlay
// panel + Manager Summary pane + Sessions detail/prefetch all analyzing ONE file concurrently) into
// one analyze + copies, keyed (path, maxBytes) and validated by (size, mtime).
void TestAnalyzeFootprint()
{
    std::wprintf(L"Analyze footprint (base64 scrub + analyze cache):\n");

    // --- ScrubLargeBase64Payloads (pure) ------------------------------------------------------
    {
        const std::string run(5000, 'A'); // an unbroken base64 run (image "data" payload shape)

        // A whole-string blob is elided; the placeholder carries the length; siblings + JSON survive.
        const std::string line = "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/png\",\"data\":\"" + run + "\"},\"n\":7}";
        const std::string scrubbed = ScrubLargeBase64Payloads(line);
        CHECK(scrubbed.find(run) == std::string::npos, "scrub: a 5000-char whole-string base64 run is elided");
        CHECK(scrubbed.find("<base64 5000 chars elided>") != std::string::npos, "scrub: the placeholder carries the elided length");
        CHECK(scrubbed.find("image/png") != std::string::npos, "scrub: sibling fields are untouched");
        {
            const std::wstring wide(scrubbed.begin(), scrubbed.end()); // pure-ASCII content
            const auto parsed = json::Parse(wide);
            CHECK(parsed && parsed->type == json::Value::Type::Obj, "scrub: the scrubbed line still parses as JSON");
            const auto* src = parsed ? parsed->Find(L"source") : nullptr;
            CHECK(src && src->StrAt(L"media_type") == L"image/png", "scrub: parsed sibling value intact");
        }

        // '=' padding is part of the blob (placeholder counts it); the closing quote is required.
        const std::string padded = "{\"data\":\"" + run + "==\"}";
        const std::string scrubbedPad = ScrubLargeBase64Payloads(padded);
        CHECK(scrubbedPad.find("<base64 5002 chars elided>") != std::string::npos, "scrub: '=' padding is elided with the run");

        // Sub-threshold runs, prose-broken strings, and bare (unquoted) digit runs are untouched.
        const std::string shortRun = "{\"data\":\"" + std::string(100, 'B') + "\"}";
        CHECK(ScrubLargeBase64Payloads(shortRun) == shortRun, "scrub: a short (100-char) run is untouched");
        const std::string prose = "{\"text\":\"" + std::string(3000, 'C') + " " + std::string(3000, 'D') + "\"}";
        CHECK(ScrubLargeBase64Payloads(prose) == prose, "scrub: a space-broken long string is untouched (not a whole-string run)");
        const std::string bigNum = "{\"n\":" + std::string(5000, '7') + "}";
        CHECK(ScrubLargeBase64Payloads(bigNum) == bigNum, "scrub: a bare 5000-digit number (not quote-delimited) is untouched");

        // A blob cut by EOF (truncated head read mid-image, no closing quote) is left alone.
        const std::string cut = "{\"data\":\"" + run;
        CHECK(ScrubLargeBase64Payloads(cut) == cut, "scrub: a truncated blob (no closing quote) is untouched");

        // Two blobs on one line are BOTH elided; the bytes between them survive verbatim.
        const std::string two = "{\"a\":\"" + run + "\",\"keep\":\"middle\",\"b\":\"" + std::string(4200, 'Z') + "\"}";
        const std::string scrubbedTwo = ScrubLargeBase64Payloads(two);
        CHECK(scrubbedTwo.find("<base64 5000 chars elided>") != std::string::npos && scrubbedTwo.find("<base64 4200 chars elided>") != std::string::npos, "scrub: two blobs in one line are both elided");
        CHECK(scrubbedTwo.find("\"keep\":\"middle\"") != std::string::npos, "scrub: the bytes between two blobs survive verbatim");

        // No candidates at all => the input comes back byte-identical (the zero-copy common case).
        const std::string plain = "{\"type\":\"user\",\"message\":{\"content\":\"hello world\"}}";
        CHECK(ScrubLargeBase64Payloads(plain) == plain, "scrub: a no-image line is byte-identical");
    }

    // --- end-to-end: the analyzer over an image-paste transcript --------------------------------
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring p = std::wstring{ tmp } + L"am_scrub_" + std::to_wstring(::GetCurrentProcessId()) + L".jsonl";
        const std::string img(6000, 'Q');
        MakeJsonl(p,
                  std::string("{\"type\":\"user\",\"userType\":\"external\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"look at the shot\"},{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/png\",\"data\":\"") + img + "\"}}]},\"timestamp\":\"2026-01-24T20:00:00.000Z\"}\n",
                  1111, 1111);
        const auto a = AnalyzeSessionTranscript(p, 0);
        CHECK(a.found, "scrub e2e: image-paste transcript analyzes");
        CHECK(a.userMsgs.size() == 1 && a.userMsgs[0] == L"look at the shot", "scrub e2e: the prompt's text block survives the image elide");
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path{ p }, ec);
    }

    // --- the analyze cache: hit / invalidation / key / copy semantics ---------------------------
    {
        wchar_t tmp[MAX_PATH]{};
        ::GetTempPathW(MAX_PATH, tmp);
        const std::wstring p = std::wstring{ tmp } + L"am_acache_" + std::to_wstring(::GetCurrentProcessId()) + L".jsonl";
        // Two payloads of IDENTICAL byte length (so size can't tell them apart — only mtime can).
        const std::string lineA = "{\"type\":\"user\",\"userType\":\"external\",\"message\":{\"content\":\"cache-aaa\"},\"timestamp\":\"2026-01-24T20:00:00.000Z\"}\n";
        const std::string lineB = "{\"type\":\"user\",\"userType\":\"external\",\"message\":{\"content\":\"cache-bbb\"},\"timestamp\":\"2026-01-24T20:00:00.000Z\"}\n";
        CHECK(lineA.size() == lineB.size(), "cache fixture: payloads are size-identical by construction");

        MakeJsonl(p, lineA, 111111, 111111);
        const auto a1 = AnalyzeSessionTranscript(p, 0);
        CHECK(a1.userMsgs.size() == 1 && a1.userMsgs[0] == L"cache-aaa", "cache: first analyze reads the file");

        // Rewrite with DIFFERENT content but IDENTICAL (size, mtime): the cache must serve the OLD
        // result — proof the hit path answers without re-reading. (A real transcript only APPENDS,
        // so identical (size, mtime) with changed bytes cannot happen outside this fixture.)
        MakeJsonl(p, lineB, 111111, 111111);
        const auto a2 = AnalyzeSessionTranscript(p, 0);
        CHECK(a2.userMsgs.size() == 1 && a2.userMsgs[0] == L"cache-aaa", "cache: identical (size,mtime) is served from the cache (no re-read)");

        // Advance mtime: the entry is invalidated and a fresh analyze sees the new content.
        MakeJsonl(p, lineB, 222222, 111111);
        const auto a3 = AnalyzeSessionTranscript(p, 0);
        CHECK(a3.userMsgs.size() == 1 && a3.userMsgs[0] == L"cache-bbb", "cache: an mtime change invalidates -> fresh analyze");

        // A different maxBytes is a DIFFERENT key (a tail-capped read never collides with whole-file).
        const auto a4 = AnalyzeSessionTranscript(p, 1u << 20);
        CHECK(a4.userMsgs.size() == 1 && a4.userMsgs[0] == L"cache-bbb", "cache: a capped-read key analyzes correctly (no cross-key hit)");

        // Callers get a COPY: mutating a returned result must not poison the cached entry.
        auto a5 = AnalyzeSessionTranscript(p, 0);
        a5.userMsgs.clear();
        const auto a6 = AnalyzeSessionTranscript(p, 0);
        CHECK(a6.userMsgs.size() == 1 && a6.userMsgs[0] == L"cache-bbb", "cache: callers mutate a COPY, never the cached result");

        std::error_code ec;
        std::filesystem::remove(std::filesystem::path{ p }, ec);
    }

    // --- containment smoke: the wrapped helpers stay total on garbage/absent inputs -------------
    {
        CHECK(FindPlanFileInTranscript(L"").empty(), "contained: FindPlanFileInTranscript('') -> empty");
        CHECK(FindPlanFileInTranscript(L"Z:\\no\\such\\file.jsonl").empty(), "contained: FindPlanFileInTranscript(absent) -> empty");
        CHECK(CollectConversationLineage(L"no-such-session-id", L"C:\\nowhere", 4).empty(), "contained: CollectConversationLineage(absent) -> empty");
    }
}

// Local fixtures for TestTeammateLiveCorpus: read up to the LAST `cap` bytes of a file (the
// engine's own tail reader is TU-internal) and widen UTF-8 for the json parser.
static std::string TeamReadTailBytes(const std::wstring& path, int64_t cap)
{
    std::string bytes;
    const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return bytes;
    }
    LARGE_INTEGER sz{};
    if (::GetFileSizeEx(h, &sz) && sz.QuadPart > 0)
    {
        const int64_t take = sz.QuadPart < cap ? sz.QuadPart : cap;
        LARGE_INTEGER off{};
        off.QuadPart = sz.QuadPart - take;
        if (::SetFilePointerEx(h, off, nullptr, FILE_BEGIN))
        {
            bytes.resize(static_cast<size_t>(take));
            DWORD rd = 0;
            if (!::ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &rd, nullptr))
            {
                rd = 0;
            }
            bytes.resize(rd);
        }
    }
    ::CloseHandle(h);
    return bytes;
}

static std::wstring TeamWidenUtf8(const std::string& s)
{
    if (s.empty())
    {
        return {};
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Agentmaster (teammates / background work — LIVE-CORPUS validation): replay the outlived-turn
// promotion + hold (the minimal mechanism), the "cache hint" API-activity split, and the
// teammate-wrapper noise gate against REAL on-disk sessions in the live ~/.claude. The outlive
// block SWEEPS the corpus for the at-rest teammate/background-agent signature (side files that
// postdate their lead transcript's last write — a lead woken/resumed AFTER its team finished has
// an mtime PAST its side files and honestly does not match; measured on this corpus: 7 such
// sessions, deltas from minutes to DAYS). The noise block replays the 2026-07-07 fuzz-team repro:
// lead bd0d5b2f received "Another Claude session sent a message:" <teammate-message> wrapper
// deliveries, each firing a REAL UserPromptSubmit (the push-path noise-gate motivation). Every
// block gates on its fixture existing in the expected shape on this machine and [info]-skips
// otherwise (skip, never fail; the pure-fixture unit tests cover the logic machine-independently).
void TestTeammateLiveCorpus()
{
    std::wprintf(L"Teammate/background-work live corpus (real ~/.claude sessions):\n");
    const std::wstring proj = ClaudeProjectsDir();

    // --- (1) the minimal mechanism against the REAL corpus: sweep every session directory for
    //         subagent/tool-result side files that OUTLIVE their lead transcript's last write —
    //         the at-rest teammate/background-agent signature — and replay the scanner's exact
    //         expressions on the strongest find. (A lead woken/resumed AFTER its team finished has
    //         an mtime PAST its side files and honestly does not match.) -------------------------
    {
        std::wstring bestParent;
        int64_t bestDelta = 0, bestParentMs = 0, bestSubMs = 0;
        std::error_code ec;
        for (std::filesystem::directory_iterator projIt{ std::filesystem::path{ proj }, ec }, projEnd; !ec && projIt != projEnd; projIt.increment(ec))
        {
            std::error_code ed;
            if (!projIt->is_directory(ed))
            {
                continue;
            }
            std::error_code es;
            for (std::filesystem::directory_iterator sesIt{ projIt->path(), es }, sesEnd; !es && sesIt != sesEnd; sesIt.increment(es))
            {
                std::error_code ei;
                if (!sesIt->is_directory(ei))
                {
                    continue;
                }
                const std::wstring parent = sesIt->path().wstring() + L".jsonl";
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (!::GetFileAttributesExW(parent.c_str(), GetFileExInfoStandard, &fad))
                {
                    continue; // a side dir without a matching transcript
                }
                ULARGE_INTEGER u{};
                u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                const int64_t parentWriteMs = static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
                const int64_t subMs = SubagentActivityUnixMs(parent); // 0 == the session dir has no side files
                if (subMs > 0 && subMs - parentWriteMs > bestDelta)
                {
                    bestDelta = subMs - parentWriteMs;
                    bestParent = parent;
                    bestParentMs = parentWriteMs;
                    bestSubMs = subMs;
                }
            }
        }
        if (bestDelta > kScanExternalWorkGraceMs)
        {
            // Replay the scanner's exact expressions at a simulated instant DURING the run — one
            // second after the last side-file write, when that external work was "fresh".
            const int64_t simNow = bestSubMs + 1000;
            const bool subagentActive = (simNow - bestSubMs) <= kScanSubagentFreshMs;
            const bool outlived = subagentActive && bestSubMs > bestParentMs + kScanExternalWorkGraceMs;
            CHECK(subagentActive && outlived, "live-team: REAL side files outlive their lead's last write by > grace");
            CHECK(ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, subagentActive, false, L"end_turn", false, outlived),
                  "live-team: the terminal-tail promotion fires on the real session's data (Waiting -> Running while the background work runs)");
            CHECK(!ShouldSynthesizeStop(SessionState::Running, L"end_turn", false, 5000, outlived),
                  "live-team: recon-stop HELD on the same expression (mutual exclusion — never oscillates)");
            CHECK(!ShouldSynthesizeRunningFromExternalWork(SessionState::WaitingForInput, subagentActive, false, L"end_turn", false, false),
                  "live-team: without the outlive proof the same freshness does NOT promote (turn-tail residue rule intact)");
            std::wprintf(L"  [info] outlive sweep: %s outlives its lead by %lld s\n",
                         std::filesystem::path{ bestParent }.filename().wstring().c_str(),
                         static_cast<long long>(bestDelta / 1000));

            // --- (2) the cache-hint split on the SAME found session: the folded display value
            //         (which the side files ride) must NOT keep the hint warm past the lead's own
            //         last conversation line — the exact measured teammate false positive --------
            const int64_t apiMs = LastActivityMsFromTranscriptChunk(TeamWidenUtf8(TeamReadTailBytes(bestParent, 4ll << 20)));
            const int64_t foldedMs = apiMs > bestSubMs ? apiMs : bestSubMs; // == ReadTranscriptLastActivityTailIn's fold
            if (apiMs > 0 && foldedMs - apiMs > 5 * 60000)
            {
                SessionInfo si;
                si.id = L"live-team-cache";
                si.live = true;
                si.kind = AgentKind::Claude;
                si.convLastActivityUnixMs = foldedMs; // what the folded (display) value carried
                si.convApiActivityUnixMs = apiMs; // the lead's own last conversation line
                CHECK(!ServerCacheStillWarm(si, 5, foldedMs + 60000),
                      "live-team: cache hint COLD while only the background work was writing — the folded value alone would have lit it (the lead's own cache had expired)");
                std::wprintf(L"  [info] cache split: folded-vs-parent-line delta %lld s\n",
                             static_cast<long long>((foldedMs - apiMs) / 1000));
            }
            else
            {
                std::wprintf(L"  [info] found session's folded/api gap under the cache window -> cache block skipped\n");
            }
        }
        else
        {
            std::wprintf(L"  [info] no at-rest session with side files outliving its lead by > grace -> outlive block skipped\n");
        }
    }

    // --- (3) the noise gate against the REAL wrapper deliveries (lead bd0d5b2f): every on-disk
    //         "Another Claude session sent a message:" main-chain user line is filtered from the
    //         Typed record, while the session's REAL human prompts are not ------------------------
    {
        const std::wstring lead = proj + L"\\K--source-NumSharp\\bd0d5b2f-1ff6-4a04-94c2-8eca1a7167e4.jsonl";
        std::string bytes;
        {
            const HANDLE h = ::CreateFileW(lead.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                LARGE_INTEGER sz{};
                if (::GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (64ll << 20))
                {
                    bytes.resize(static_cast<size_t>(sz.QuadPart));
                    DWORD rd = 0;
                    if (!::ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &rd, nullptr))
                    {
                        bytes.clear();
                    }
                    bytes.resize(rd);
                }
                ::CloseHandle(h);
            }
        }
        if (!bytes.empty())
        {
            const auto widen = [](const std::string& s) -> std::wstring {
                if (s.empty())
                {
                    return {};
                }
                const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
                std::wstring w(static_cast<size_t>(n), L'\0');
                ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
                return w;
            };
            size_t wrappers = 0, realPrompts = 0;
            bool allWrappersNoise = true;
            std::wstring firstWrapper;
            size_t start = 0;
            while (start < bytes.size())
            {
                size_t end = bytes.find('\n', start);
                if (end == std::string::npos)
                {
                    end = bytes.size();
                }
                const std::string line = bytes.substr(start, end - start);
                start = end + 1;
                // Cheap prefilter (the JSON parse below is authoritative): a main-chain user line
                // whose content is a plain STRING (a typed prompt or an injected wrapper).
                if (line.find("\"type\":\"user\"") == std::string::npos || line.find("tool_use_id") != std::string::npos)
                {
                    continue;
                }
                const auto parsed = json::Parse(widen(line));
                if (!parsed || parsed->type != json::Value::Type::Obj)
                {
                    continue;
                }
                if (parsed->StrAt(L"type") != L"user" || parsed->BoolAt(L"isSidechain") || parsed->BoolAt(L"isMeta"))
                {
                    continue;
                }
                const json::Value* msg = parsed->Find(L"message");
                if (!msg || msg->type != json::Value::Type::Obj)
                {
                    continue;
                }
                const json::Value* content = msg->Find(L"content");
                if (!content || content->type != json::Value::Type::Str)
                {
                    continue; // array-form content (tool results etc.) — not a typed/injected prompt
                }
                const std::wstring text = content->AsStr();
                if (text.rfind(L"Another Claude session sent a message:", 0) == 0)
                {
                    ++wrappers;
                    allWrappersNoise = allWrappersNoise && IsNoiseUserPrompt(text);
                    if (firstWrapper.empty())
                    {
                        firstWrapper = text;
                    }
                }
                else if (!text.empty() && !IsNoiseUserPrompt(text))
                {
                    ++realPrompts; // a genuine human prompt must never be classified noise
                }
            }
            if (wrappers > 0)
            {
                CHECK(allWrappersNoise, "live-noise: EVERY real on-disk teammate wrapper is classified noise (IsNoiseUserPrompt)");
                CHECK(realPrompts > 0, "live-noise: the session's REAL human prompts survive the filter (at least one non-noise prompt)");
                // End-to-end through the push path: feed the ACTUAL on-disk wrapper into a live
                // registry — state runs, the Typed record does not.
                SessionRegistry reg;
                reg.Upsert(MakeSession(L"live-noise-1"));
                HookMessage wake;
                wake.event = HookEvent::UserPromptSubmit;
                wake.sessionId = L"live-noise-1";
                wake.promptText = firstWrapper;
                wake.ts = NowMsTest();
                reg.OnHookEvent(wake);
                const auto g = reg.Get(L"live-noise-1");
                CHECK(g && g->queue.empty(), "live-noise: the ACTUAL wrapper text is filtered from the Typed record end-to-end");
                CHECK(g && g->state == SessionState::Running, "live-noise: ...while the wake turn still drives state (a REAL turn)");
                std::wprintf(L"  [info] bd0d5b2f: %zu teammate wrapper deliveries, %zu real prompts\n", wrappers, realPrompts);
            }
            else
            {
                std::wprintf(L"  [info] bd0d5b2f present but no wrapper lines found -> noise block skipped\n");
            }
        }
        else
        {
            std::wprintf(L"  [info] teammate fixture bd0d5b2f not on this machine -> skipped\n");
        }
    }

    // --- (4) the presence heartbeats LIVE: PresenceIsWorking classifies whatever statuses the real
    //         ~/.claude/sessions currently holds ("shell" == a live shell job, measured on sessions
    //         carrying hours-old cmd/bash children) --------------------------------------------
    {
        const auto rows = ReadSessionPresence();
        if (!rows.empty())
        {
            size_t busyN = 0, shellN = 0, restN = 0, otherN = 0;
            bool allClassified = true;
            for (const auto& r : rows)
            {
                if (r.status == L"busy")
                {
                    ++busyN;
                }
                else if (r.status == L"shell")
                {
                    ++shellN;
                }
                else if (r.status == L"idle" || r.status == L"waiting")
                {
                    ++restN;
                }
                else
                {
                    ++otherN;
                }
                allClassified = allClassified &&
                                (PresenceIsWorking(r.status) == (r.status == L"busy" || r.status == L"shell")) &&
                                (PresenceIsAtRest(r.status) == (r.status == L"idle"));
            }
            CHECK(allClassified, "live-presence: PresenceIsWorking/PresenceIsAtRest classify every REAL heartbeat status (busy/shell work; idle rests; waiting neither)");
            std::wprintf(L"  [info] live presence: %zu heartbeats (busy=%zu shell=%zu idle/waiting=%zu other=%zu)\n",
                         rows.size(), busyN, shellN, restN, otherN);
        }
        else
        {
            std::wprintf(L"  [info] no live presence heartbeats -> skipped\n");
        }
    }
}
