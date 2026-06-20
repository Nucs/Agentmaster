# Summary-panel JUMP — scroll a session's terminal to where a prompt was sent

> Status: **core complete, tested (1005-check engine harness) + benchmarked + optimized; full chain
> lib-compiles green (TerminalControlLib + TerminalAppLib).** Runtime verification (the visible jump in
> a live session) needs a deploy — gated on the user's build/deploy permission. See §6.

## 1. What this is

The per-tab **summary panel** (`AgentTabOverlay`, see [`TAB_OVERLAY.md`](TAB_OVERLAY.md)) numbers a
session's user prompts (` 1. …`, ` 2. …`). Each numbered prompt now carries a small **▸ jump button**
that scrolls the session's **terminal view** to where that prompt is rendered, **centering** it.

The hard part is that the two sides have **no shared coordinate system**:

- The summary text comes from the **transcript** (`.jsonl`) — what was *said*, with no buffer position.
- "Scroll the view" moves the rendered **ConPTY text buffer** — and Claude Code's Ink TUI repaints,
  scrolls, and reflows constantly, so any resolved position **ages**; and the **same prompt may be sent
  (and rendered) more than once**.

So a jump is really a **fuzzy, re-validated, duplicate-aware search** of the live buffer for the prompt
text, then a centered scroll onto the match. This doc is the design + the performance work.

## 2. Architecture (one pure brain, a thin per-layer adapter)

```
AgentTabOverlay (summary panel UI)                     TerminalApp
  └─ ▸ jump button on each " N. <prompt>" line
       └─ _onJumpToPrompt(prompts, index)              ──► TerminalPage::_JumpToPromptInSession
                                                              (resolve the tab's TermControl by sessionId)
                                                                 └─► TermControl::JumpToConversationPrompt   TerminalControl
                                                                        ├─ ControlCore::ResolveConversationPromptRow
                                                                        │     ├─ linearize a RECENT window of the buffer
                                                                        │     ├─ Agentmaster::ResolvePromptAnchors  ◄── PromptAnchor.h (PURE, header-only)
                                                                        │     └─ map char offset → absolute buffer row
                                                                        └─ center via ScrollBar().Value(row − viewH/2)
```

- **`AgentMaster/PromptAnchor.h`** — the **pure, header-only** resolver (no WinRT / TextBuffer / ICU).
  Header-only (like `ProfileBootstrap.h`) so it is shared by the overlay path AND `ControlCore` (a
  separate `Microsoft.Terminal.Control` DLL) with no cross-project source/link. Unit-tested +
  benchmarked standalone in `AgentMaster/tests/m5_tests.cpp` (`TestPromptAnchor` / `BenchPromptAnchor`).
- **`ControlCore::ResolveConversationPromptRow`** (`ControlCore.{idl,h,cpp}`) — **read-only**. Under the
  terminal read-lock it linearizes a recent window of the buffer into a haystack (rows concatenated; a
  **hard** line-break adds one `\n`, a **soft wrap** adds nothing — so a wrapped prompt stays continuous,
  matching `TextBuffer::SearchText`'s own haystack) + a parallel offset→row index, runs the pure resolver
  over the **whole** prompt list, and maps the resolved char offset back to an absolute buffer row.
- **`TermControl::JumpToConversationPrompt`** (`TermControl.{idl,h,cpp}`) — calls the core, then **centers**
  by driving the scrollbar (`ScrollBar().Value(row − ViewHeight()/2)`, the same UI-thread-safe path as
  `ScrollViewport`; the scrollbar clamps near-top/near-bottom targets). Returns the row, or `-1`.
- **`TerminalPage::_JumpToPromptInSession`** (`TerminalPage.AgentObserver.cpp`) — resolves the session's
  tab → `TerminalPaneContent` → `TermControl` **at click time** (robust to a pane restart), passes the
  prompts, returns the row. Wired into the overlay in `_AttachClaudeOverlay` via `SetJumpHandler`.
- **Overlay UI** (`AgentTabOverlay.cpp`) — `_SetSummaryContent` renders each ` N. <prompt>` line as a
  2-column Grid (▸ button + wrapping text); `_LoadSummaryAsync` stashes the raw prompts (`_summaryUserMsgs`,
  aligned with the numbering) so the button resolves the right prompt; a hit plays the confirmation chime.
  Message-start detection is **sequence-number-guarded** (the renderer numbers prompts strictly 1..N, so a
  line is a real message only when its number is the next expected one) — so in **wrap-ON** mode a
  multi-line prompt's continuation line that merely looks like `2. foo` is not mis-detected as a numbered
  message and mis-mapped to the wrong prompt.

## 3. The resolver (PromptAnchor.h)

- **Needle** = the prompt's first non-trivial line, whitespace-collapsed + ASCII-lowercased, ≤ `maxNeedle`
  (64) chars. Matching is **whitespace-tolerant** (a wrap / re-indent doesn't defeat it) and
  case-insensitive, and skips a render prefix like `> ` naturally (substring search).
- **Partial — "match as much as possible"**: after a needle hits, the match is extended char-by-char
  against the whole normalized prompt; `quality` = matched fraction. A truncated/reflowed render still
  resolves at `quality < 1` (flagged `partial`). If the full needle is absent it **backs off** to shorter
  prefixes (down to `minNeedle` = 8) before giving up.
- **Duplicates** — all prompts resolve together in ONE pass with an **order-preserving greedy assignment**
  (prompt order == buffer order): the i-th send of a repeated text maps to the i-th surviving on-screen
  occurrence. A send scrolled off the top is simply gone; the rest still line up. With no in-order
  candidate it falls back to the **last (most-recent) global occurrence**, flagged `outOfOrder` (lower
  confidence) rather than guessing silently.
- **Re-validation tiers** (the answer to "refresh on click + interval without hurting performance"):
  1. **Epoch gate** (caller, O(1)) — buffer mutation-id unchanged ⇒ a cached row is still valid ⇒ no work.
  2. **`ValidatePromptAnchor`** (O(needle)) — the cheap per-click re-check at the cached offset.
  3. **`ResolvePromptAnchors`** (O(haystack)) — the full re-find, only on a validate miss / gated idle
     refresh, never per render frame.

  > The shipped path resolves **on click** only (so steady state is **0** by construction). The epoch
  > cache (tier 1/2) is a documented optional optimization for rapid repeated clicks on a quiet buffer —
  > not needed for correctness and omitted from v1 to keep the unverifiable surface small.

## 4. Performance — benchmark + optimization

Measured by `BenchPromptAnchor` (in the engine harness, this machine: i9-13900K). `resolve(present)` =
all prompts on screen; `resolve(all-miss)` = every prompt scrolled off (the worst case — each is a full
absence scan); `validate-one` = the cheap per-click recheck.

| haystack | prompts | resolve (present) | resolve (all-miss) | validate-one |
|---|---|---|---|---|
| 0.18 MB (~1k rows) | 20 | **0.94 ms** | 1.20 ms | 2.6 µs |
| 1.84 MB (~10k rows) | 50 | **9.1 ms** | 16.1 ms | 3.5 µs |
| 9.19 MB (~50k rows) | 200 | 67 ms | 477 ms | 3.5 µs |

**Production-effective ceiling ≈ the 1.84 MB row (~9 ms / ~16 ms).** `ControlCore` caps the linearized
haystack to `kAnchorRecentWindowChars` (1.2M wchars ≈ 2.4 MB ≈ a typical full WT scrollback) before
resolving, so even a 50k-row buffer resolves over ~the medium row, not the uncapped 9 MB figure. A prompt
older than the window has scrolled out of practical reach anyway; the greedy cursor handles the gap.

**Real-world impact is far below even that**, because the resolve runs **only on a click** (a user action,
parity with the existing Ctrl+Shift+F search which also scans under lock on the UI thread):
- **Steady state (idle or actively rendering): 0** — nothing resolves unless you click a jump button.
- **Per click: ~9 ms** under the read-lock at a realistic scrollback; **0** if the epoch cache is later added and the buffer is quiet.

**Optimizations applied** (each measured):
1. **Index-written normalization** — the dominant cost (it touches the whole haystack) writes the
   normalized string + offset map **by index** into pre-sized buffers instead of `push_back`. Cut the
   common *present* case **~37%** (109 → 69 ms at 9 MB; 18.6 → 9.1 ms at the realistic ~2 MB).
2. **True-absence membership pre-check** — if even the shortest needle prefix is nowhere, no longer needle
   can be, so a scrolled-off prompt short-circuits in **one** scan instead of the full backoff + global
   fallback (~8× on a genuine miss).
3. **Recent-window cap** (`kAnchorRecentWindowChars`) — bounds the haystack so cost is independent of
   total scrollback depth.

Not done (deliberately): a single-pass multi-pattern matcher (Aho-Corasick) for the all-miss case — the
window cap already bounds it to ~16 ms, and the extra build cost would regress the common *present* path.

## 4a. Icon eligibility (dimming dead jumps)

Many summary prompts won't resolve at a given moment (scrolled off the recent window, never rendered, or
mixed-bidi). Those jump buttons are **dimmed** (opacity 0.2) while resolvable ones stay normal (0.75), so
the working icons are obvious. The state is computed by a **single batch resolve** —
`ControlCore::ResolveConversationPromptRows` → a row (or -1) per prompt in one linearize+resolve — surfaced
to the overlay through `TermControl::ResolveConversationPromptRows` and `TerminalPage::_JumpEligibilityInSession`.

Refresh is gated to stay cheap (the user's "don't hurt performance"):
- **On (re)build** of the panel — which only happens when the transcript grew (the existing mtime gate), so
  it tracks buffer changes during active sessions for free.
- **On a 5 s tick** — reusing `_summaryTimer`, which is **stopped while the panel is hidden**, so idle/hidden
  cost is 0; a visible panel pays at most one bounded resolve per 5 s.
- **After a click** — a jump makes the other icons re-check immediately (the buffer/viewport just moved).

(A mutation-id epoch gate could make an idle *visible* panel free too — noted as a future optimization.)

## 5. Edge behavior

- **Not on screen** → `-1`, no scroll (no chime). The transcript still has the prompt; a "open transcript
  here" fallback is a future nicety. **The icon is also DIMMED** (opacity 0.2 vs 0.75) so a prompt that
  currently won't jump is visually distinct — see *icon eligibility* below.
- **RTL (Hebrew / Arabic)** — a terminal renders an RTL line in **visual order (character-reversed)** while
  the transcript stores it **logical**, so an RTL prompt appears reversed in the buffer and a logical-order
  search misses. The resolver detects an RTL prompt (`ContainsRtl`) and **also tries the character-reversal**
  of the needle (forward is always tried first, so LTR matching is never perturbed; the matched *row* is the
  same, so centering works). This fixes Hebrew/Arabic prompts not jumping. Mixed bidi (RTL + Latin/digits) is
  not a pure reversal and may still miss — a full BiDi (ICU `ubidi`) reorder is a future refinement.
- **Alt screen buffer** — if a session ran in the alt buffer (no scrollback), the resolve finds nothing in
  history → `-1`. Claude Code renders inline (real scrollback), so this is a guard, not the norm.
- **Codex** — the summary's prompts list is wired for **Claude** (`AnalyzeSessionTranscript.userMsgs`);
  Codex rollouts get no jump buttons in v1 (the resolver itself is agent-agnostic — only the prompt-source
  wiring differs).

## 6. Verification status

- ✅ **Resolver**: 1005-check engine harness passes (`TestPromptAnchor` covers normalize, needle, in-order,
  whitespace tolerance, `> ` prefix, duplicates, partial, not-found, out-of-order, validate); benchmarked.
- ✅ **Full chain compiles**: `TerminalControlLib` (IDL + ControlCore + TermControl) and `TerminalAppLib`
  (overlay + page) both build green.
- ⏳ **Runtime**: the visible jump in a live session is **not yet exercised** — it needs a full exe
  build + deploy (close → build → relaunch), which requires the user's permission. Once deployed, verify:
  click a numbered prompt → the terminal centers on that prompt's render; a duplicate prompt jumps to the
  right occurrence; an off-screen prompt no-ops (no chime).

## 7. Follow-ups (non-blocking)

- **Flash highlight** on the landed match (transient, separate from the user's Ctrl+Shift+F highlight) so
  the jump's target is visually obvious — center-only is v1 to keep the unverifiable surface minimal.
- **Epoch cache** in `ControlCore` (mutation-id keyed) → repeated clicks on a quiet buffer become O(1).
- **"End of turn" jumps** — derive from neighbors (≈ just above prompt N+1's anchor), reusing the prompt
  anchors; only the final in-progress turn needs a direct search.
- **Codex** prompts (wire `_summaryUserMsgs` from the rollout's human prompts).
- **Ctrl+F** — a context-sensitive find that, on a managed tab, reuses this resolve+center path.
