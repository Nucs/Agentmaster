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
  confidence) rather than guessing silently. A planned **v2** replaces this single-pass greedy with a
  two-pass *neighbor-bracketing* scheme that fixes a tail-retention mis-assignment — see **§3a**.
- **Re-validation tiers** (the answer to "refresh on click + interval without hurting performance"):
  1. **Epoch gate** (caller, O(1)) — buffer mutation-id unchanged ⇒ a cached row is still valid ⇒ no work.
  2. **`ValidatePromptAnchor`** (O(needle)) — the cheap per-click re-check at the cached offset.
  3. **`ResolvePromptAnchors`** (O(haystack)) — the full re-find, only on a validate miss / gated idle
     refresh, never per render frame.

  > The shipped path resolves **on click** only (so steady state is **0** by construction). The epoch
  > cache (tier 1/2) is a documented optional optimization for rapid repeated clicks on a quiet buffer —
  > not needed for correctness and omitted from v1 to keep the unverifiable surface small.

## 3a. Duplicate disambiguation v2 — two-pass neighbor bracketing (PLANNED, not yet implemented)

> **Status: design only.** The shipping resolver is the single-pass greedy of §3. This section
> specifies the planned replacement of the duplicate handling **inside `ResolvePromptAnchors`** (the
> *batch*). It changes only that function's internals — the `AnchorMatch` shape, the `ControlCore` /
> `TermControl` callers, `ValidatePromptAnchor`, and `ResolveOnePromptAnchor` (the single-message
> helper) are all **unchanged**.

### Why v1's greedy can mis-assign (the tail-retention failure)

The buffer keeps the **tail**, not the head: `ControlCore` caps the haystack to
`kAnchorRecentWindowChars` (recent scrollback), so the oldest prompts have scrolled off. v1 walks
prompts left-to-right with a single **lower** bound (the greedy `cursor`) and **no upper** bound, so
when an early prompt's own render has scrolled off but its **text is a duplicate** of a later,
still-on-screen send, v1 binds the early prompt to the **later** render:

```
buffer (tail only):   … [ render of msg 25, whose text == T,  @ p2 ] …       (msg 10's own render scrolled off)
msg 10 (text T):  v1 → find(T, cursor=0) = p2   ⇒ msg 10 mis-binds to msg 25's render; cursor jumps past p2
                                                ⇒ msgs 11..24 (renders precede p2) now miss in-order → outOfOrder cascade
                                                ⇒ msg 25 itself finds nothing ≥ cursor → wrong fallback
```

One scrolled-off duplicate corrupts the whole tail. The fix is to give each prompt an **upper** bound
too — the position of its nearest *resolved* successor — so a head prompt can never reach forward into
a successor's render.

### Definitions

- **Candidate set** `C[i]` — every offset in the normalized haystack where prompt *i*'s winning needle
  occurs, in the winning orientation (forward, or the RTL reversal — §5), at the most-specific backoff
  length that yields ≥1 hit. `|C[i]| == 0` ⇒ scrolled off / absent (the normal case for head prompts).
- **Anchor** — a prompt with `|C[i]| == 1`: its location is forced (unambiguous).
- **Deferred** — a prompt with `|C[i]| ≥ 2`: ambiguous; its occurrence is chosen in pass 2.
- **Bracket** `(Lo, Hi)` for prompt *i* — `Lo` = position of the nearest **resolved** prompt at an
  index `< i` (or −∞ if none — the head case); `Hi` = position of the nearest **resolved** prompt at an
  index `> i` (or +∞ if none — the live-tail case). Both bounds are **strict**.

This restates the user's rule "message *10* must fit between *9* and *11*" precisely: *9* and *11* are
the nearest **resolved** neighbors (which may be further than ±1 when adjacent prompts are themselves
deferred or scrolled off), and "fit between" is the strict open interval `(Lo, Hi)`.

### Pass 0 — classify (cheap)

For each prompt, determine its winning needle (orientation + backoff length, exactly as v1's
`LocateOriented` does on its first hit) and count occurrences with **at most two** `find`s:

```
first  = find(needle_i, 0)
if first == npos:                 count = 0   → UNRESOLVED            (PresentEither short-circuits this)
else:
    second = find(needle_i, first + 1)
    if second == npos:            count = 1   → ANCHOR  (pos[i] = first)
    else:                         count ≥ 2   → DEFERRED
```

Adds **one** extra `find` per on-screen prompt over v1's first-hit; an absent prompt still costs the
single membership scan (`PresentEither`). The most-specific (longest-backoff) needle is used on purpose
— it **maximizes** the number of unique anchors (a longer needle has fewer coincidental hits).

### Pass 1 — anchor the uniques (+ resolve conflicts)

Anchors should already be monotonic (a unique match *is* the position). A non-monotonic pair means one
"unique" hit is coincidental — e.g. the head-duplicate above, momentarily unique because its sibling's
render is its only survivor. Keep the largest monotonic set; demote the rest:

```
anchors = [ i where count == 1 ], in index order
keep    = a longest STRICTLY-INCREASING-by-pos subsequence of anchors        (LIS over pos[])
for i in anchors \ keep:  state[i] = DEFERRED ; clear pos[i]                  (re-resolved under a bracket in pass 2)
```

The demoted head-duplicate carries its lone candidate `{p2}` into pass 2, where its bracket excludes it
(next). LIS over the anchor positions is `O(A log A)`, `A` = anchor count — negligible.

### Pass 2 — bracketed monotonic assignment

The kept anchors split `0..N-1` into **gaps**. For each gap bounded by `Lo` (left anchor pos, or −∞)
and `Hi` (right anchor pos, or +∞), assign its deferred prompts **monotonically** within `(Lo, Hi)`:

```
prev = Lo
for i in the gap's prompts, ascending index:
    cand = first occurrence in C[i] with  prev < cand < Hi      (enumerate C[i], capped at K — below)
    if cand exists:  pos[i] = cand ; prev = cand ; state[i] = BRACKETED
    else:            state[i] = UNRESOLVED        (no occurrence fits between its resolved neighbors)
```

`prev` advances only on a successful pick, so a skipped prompt doesn't block its successors. **Strict**
`< Hi` is the crux of the tail-retention fix: the head-duplicate's only candidate `p2` equals its
successor-anchor's position (`Hi == p2`), so `p2 < Hi` is false ⇒ it stays **UNRESOLVED** (dimmed),
never stealing the tail render. The same strictness keeps two genuine sends of one text on their own
two renders (msg 10 → the occurrence below msg 11's anchor; msg 25 → the one below msg 26's). Because a
gap is bounded by *resolved* anchors at both ends, resolving the uniques first (pass 1) supplies the
rails the greedy lacked — no iteration/fixpoint is needed: each gap is independent and one ascending
sweep places its deferred prompts in order.

### Tail-not-head, concretely

- Head prompts have `|C[i]| == 0` (cap + truncation) ⇒ UNRESOLVED for free; they never enter a gap's
  assignment.
- The **first** gap (before the earliest anchor) has `Lo == −∞`: its prompts get only an upper bound —
  expected, since the head is where data thins out.
- The **last** gap (after the latest anchor) has `Hi == +∞`: the live, actively-rendering tail, where
  duplicates are most likely; monotonic-from-`Lo` assignment keeps them ordered.
- A deferred prompt whose nearest resolved successor sits *before* all its candidates ⇒ no in-bracket
  candidate ⇒ UNRESOLVED — the correct outcome for a scrolled-off head duplicate (vs v1, which bound it
  forward into the tail).

### Confidence — no `AnchorMatch` change

The existing flags carry the new outcomes (no struct/ABI change, so every caller is untouched):

| outcome | `found` | `outOfOrder` | meaning |
|---|---|---|---|
| ANCHOR / BRACKETED | `true` | `false` | confidently placed (unique, or bounded by neighbors) |
| degenerate fallback | `true` | **`true`** | no anchors in this gap, or `>K` occurrences (below) — dim-distinct, as today |
| UNRESOLVED | `false` | — | not on screen → icon dimmed (§4a) |

`quality` / `partial` are still computed by `ScoreHit` at the chosen offset, exactly as v1.

### Degenerate fallbacks (never worse than v1)

- **A gap with no bounding anchors at all** (`Lo == −∞ && Hi == +∞` — i.e. *zero* unique anchors in the
  whole batch): the single gap degenerates to v1's order-preserving greedy (smallest candidate `> prev`,
  global `rfind` fallback), flagged `outOfOrder`. So v2 ≥ v1 by construction.
- **A too-common needle** (`|C[i]| > K`; `K` a tunable, e.g. 64, naturally living in `AnchorOptions`):
  enumerating every occurrence is unbounded, so cap it — take the first `K`, and if the bracket selects
  none, fall back to the in-bracket greedy pick flagged `outOfOrder`. Prevents a pathological short
  prompt from blowing the budget.

### Performance

- **Pass 0** adds ~1 extra `find` per on-screen prompt (the second-occurrence probe); an absent prompt
  is unchanged (one `PresentEither` scan). The dominant cost is still the one-time `NormalizeWithMap`
  (§4), which is untouched.
- **Pass 1** is an LIS over the anchor count — `O(A log A)`, negligible.
- **Pass 2** enumerates candidates **only for deferred prompts** (usually few), each capped at `K`.
- Net: the same asymptotic class as v1 (`O(haystack)`, dominated by normalization) plus a small bounded
  additive term. Before it ships, `BenchPromptAnchor` (§4) gets a new `resolve(dupes)` row — a haystack
  seeded with repeated prompt texts and a truncated head — to confirm it stays within the §4 ceiling.

### Integration & test plan

- **Code touched:** only `ResolvePromptAnchors` internals + new `detail::` helpers (an
  `EnumerateOccurrences` capped at `K`, an LIS-by-position, the gap walk). `ResolveOnePromptAnchor`
  (single message, no neighbors) is **unchanged** — it is the degenerate one-prompt case and not the
  production path (jump + eligibility both call the batch via `ControlCore::ResolveConversationPromptRow(s)`).
- **Unchanged:** `AnchorMatch`, `ControlCore` (offset→row mapping), `TermControl`, the overlay, and
  `ValidatePromptAnchor` (per-click re-check) — they consume the same per-prompt `AnchorMatch` vector.
- **Tests** (`m5_tests.cpp`, extend `TestPromptAnchor`): (1) two genuine sends of one text → the two
  renders, in order; (2) a scrolled-off head duplicate (its text present only as a later send) → that
  early prompt resolves **UNRESOLVED**, the later one to the render; (3) one-sided brackets (`Lo` only /
  `Hi` only); (4) a conflicting "unique" pair → LIS keeps the consistent one; (5) a no-anchor batch →
  v1-equivalent greedy; (6) `>K` occurrences → capped + flagged; (7) an RTL duplicate; plus the new
  `resolve(dupes)` benchmark row.

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

**Uninitialized-core safety (regression fix, commit `80856ae92`).** A tab restored on relaunch but never
activated has a live `ControlCore` whose `TextBuffer` isn't created yet (`Initialize()` is gated on the
SwapChainPanel's first non-zero layout — it only runs when the tab is first shown). The eligibility refresh
can fire on such a **background** overlay (the panel is a GLOBAL toggle and a resumed session's transcript
keeps growing off-screen), so reading the buffer there dereferenced a null `_mainBuffer` → a deterministic
AV in `TextBuffer::GetSize`. Two layers now prevent it: `ControlCore::ResolveConversationPromptRows` **guards
on `_initializedTerminal`** (like every other buffer reader) and returns all-`-1` (every icon dims, no buffer
touch); and `TerminalPage::_JumpEligibilityInSession` additionally **skips a `NotConnected` control** (the
connection starts on the same first-layout gate, so this avoids even the no-op cross-ABI call for dormant
restored tabs; only `NotConnected` is skipped, so a `Closed`/ended session — whose scrollback persists — still
resolves). Pattern: any app/overlay-layer buffer reader MUST guard on `_initializedTerminal` / `ConnectionState`.

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

## 7. Keyboard prompt navigation — alt+up / alt+down (sibling feature)

Jump between sent prompts **without the summary panel**: **alt+up / alt+down** step the focused
session's terminal view to the **previous / next SENT prompt that is currently OFF-SCREEN**, centering
it; at the ends (no further off-screen prompt that way) a short **boundary sound** plays
(`SystemExclamation`, distinct from the jump chime). It reuses this unit's resolve verbatim — only the
trigger + the selection differ.

- **WT-native action** (rebindable in `settings.json`): two no-arg `ShortcutAction`s,
  `agentScrollToPrevPrompt` / `agentScrollToNextPrompt` (added in `AllShortcutActions.h` +
  `ActionAndArgs.cpp`; the enum value, the dispatch event, and the `_Handle…` decl are all
  X-macro-generated). `defaults.json` binds `alt+up` / `alt+down` to them, **replacing** the upstream
  `MoveFocusUp` / `MoveFocusDown` pane bindings.
- **Claude-only, else fall through**: the handler (`_HandleAgentScrollTo{Prev,Next}Prompt`,
  `TerminalPage.AgentObserver.cpp`) navigates only when the focused tab is a managed **Claude** session
  (`_FocusedPromptNavSession` — `_ClaudeSessionForTab` + `SessionInfo.kind == Claude`); on any other tab
  (Codex — no in-buffer prompt resolve in v1, §5; a shell; the Manager tab; an external) it falls back
  to `_MoveFocus`, preserving the default alt-arrow pane navigation **and** the GH#6129 keychord
  propagation when there is no pane to move to.
- **Selection** (`TermControl::ScrollToAdjacentConversationPrompt`): one `ResolveConversationPromptRows`
  (the §3 batch resolve — order-preserving, duplicate-aware) + the live viewport
  (`ScrollOffset` / `ViewHeight`). The viewport spans `[viewTop, viewTop+viewH−1]` in absolute buffer
  rows — the same coordinate space the resolver returns and the scrollbar centers in — and "off-screen"
  is **strictly** outside it: **up** picks the largest prompt row `< viewTop`, **down** the smallest
  `> viewBottom`. Center via `ScrollBar().Value(max(0, row − viewH/2))`; return the row, or `−1` (→ the
  boundary sound). Because the target is centered, the next press finds the next still-off-screen prompt
  — a prompt the centering brought on screen is, by definition, no longer a target.
- **Prompts source** (`_ScrollAdjacentPrompt`, off-thread): the transcript's user messages
  (`AnalyzeSessionTranscript().userMsgs`, the same list the panel numbers), **mtime-cached per session**
  (`_promptNavCache`) so stepping re-reads the file only when it GREW. A warm cache navigates instantly
  (off-screen targets are older prompts, so one-turn staleness is harmless) while a background refresh
  keeps it current; a cold session loads, then navigates. So nav works with the summary panel **off**
  (the panel's own prompt cache only exists while it is shown).
- **Cost**: per keypress ≈ one `ResolveConversationPromptRows` (~9 ms at a realistic scrollback, §4) on
  the UI thread, plus a stat / bounded transcript read off-thread. Steady state **0** (only on keypress).
- **Status**: lib-compiles green (settings model + `TerminalControlLib` + `TerminalAppLib`); runtime
  (the visible scroll + the boundary sound) pends the same deploy as the jump (§6).

## 8. Follow-ups (non-blocking)

- **Flash highlight** on the landed match (transient, separate from the user's Ctrl+Shift+F highlight) so
  the jump's target is visually obvious — center-only is v1 to keep the unverifiable surface minimal.
- **Epoch cache** in `ControlCore` (mutation-id keyed) → repeated clicks on a quiet buffer become O(1).
- **"End of turn" jumps** — derive from neighbors (≈ just above prompt N+1's anchor), reusing the prompt
  anchors; only the final in-progress turn needs a direct search.
- **Codex** prompts (wire `_summaryUserMsgs` from the rollout's human prompts).
- **Ctrl+F** — a context-sensitive find that, on a managed tab, reuses this resolve+center path.
