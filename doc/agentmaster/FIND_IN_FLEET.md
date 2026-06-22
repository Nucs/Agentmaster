# Find in Fleet — global Find‑in‑Files across terminals **and** sessions

> Status: **DESIGN** (pre‑implementation). The IntelliJ/Rider *Find in Path* + Notepad++
> *Find in Files* analog for Agentmaster: one search box that spans **every open terminal
> buffer** and **every session transcript** across **all windows**, with a grouped results
> list and click‑to‑jump — not the current‑tab‑only `Ctrl+F` box.

Companion to: per‑tab search (the restyled `SearchBoxControl`, `Ctrl+F`), the **Sessions browser**
([`SESSIONS.md`](SESSIONS.md)), the **Archive page**, and the **summary‑panel jump**
([`SUMMARY_JUMP.md`](SUMMARY_JUMP.md), the center‑on‑prompt work this reuses for jumping).

---

## 0. Locked decisions

Decided with the user before this doc:

1. **Domains — Both, default Both.** Search live terminal **buffers** *and* session **transcripts**;
   the page opens with both in scope.
2. **Agent buffers — always included.** A managed Claude/Codex tab's raw buffer is searched too
   (not just its transcript). Consequence: a managed session can match in **both** domains (its
   rendered buffer *and* its conversation). They are kept as **distinct, labeled groups** — the
   on‑screen render and the transcript are different corpora — with an optional "collapse
   duplicates" toggle deferred to polish (§10.5).
3. **Hotkey — `Ctrl+Shift+F` (rebind).** The IntelliJ/Notepad++ convention: `Ctrl+F` = this tab,
   `Ctrl+Shift+F` = Find in Fleet. `Ctrl+Shift+F` moves **off** the per‑tab box (which `Ctrl+F`
   already covers — see `defaults.json`).

---

## 1. Goal

One surface that answers, across the **whole fleet** and regardless of which window/tab is focused:

- "Which terminal has `error CS0246` on screen right now?" → **Terminals** (live buffer).
- "Where did I ask about the auth bug, in any conversation, ever?" → **Sessions** (transcript).
- "Find `JWT` anywhere — running output *or* past conversation." → **Both**.

Result = a grouped match list (IntelliJ‑style: source → matches‑with‑context) + a preview pane +
**jump**: focus the hosting tab (cross‑window) and **center** the match in view.

Non‑goals (v1): replacing the dedicated Sessions browser (it keeps its rich session‑centric UI);
editing/replacing matched text (read‑only, like the rest of Agentmaster — Rule #13).

---

## 2. Domains

A scope selector at the top: **Terminals · Sessions · Both** (default **Both**).

| Domain | Corpus | Engine | Status |
|---|---|---|---|
| **Sessions** | every Claude/Codex transcript — open + archived + on‑disk + external (`~/.claude/projects/**`, codex rollouts) | `SessionSearch` (two‑phase, rg‑prefiltered, scope‑attributed, cancellable) | **exists** |
| **Terminals** | live scrollback of **every** open tab/pane, **across all windows** (shells, agents, external) | per‑buffer `ControlCore::Search` + a **new cross‑window fan‑out** | **new** |

The **Sessions** domain additionally exposes the existing scope chips (`SessionQuery`):
🏷 title · 👤 user · 🤖 agent/tools · 📁 dirs · 📄 files · (F) fuzzy. They are shown only while
Sessions is in scope and apply to the session half only.

---

## 3. Engines

### 3a. Sessions — reuse `SessionSearch` wholesale

No new search code. Drive the existing two‑phase pipeline (`SessionSearch.h`):

- `ParseSessionQuery(text)` → terms (AND‑combined, quoted‑phrase, bare‑GUID, fuzzy).
- **Fast:** `SearchIndexFast(entries, q)` over the per‑session sidecar indexes +
  `SearchHistoryPrompts(historyPath, q, …)` (the `history.jsonl` 👤 accelerator) → instant rows.
- **Slow:** `SearchTranscriptsSlow(refs, q, …, cancelled)` — `rg -il` file filter, each survivor
  re‑scanned in‑process so every hit is scope‑attributed (👤 vs 🤖) — `cancelled` polled so a
  re‑type kills the stale run.

The session corpus is enumerated by `TranscriptStore` exactly as the Sessions page does.

### 3b. Terminals — new cross‑window buffer fan‑out

Each buffer lives in a `ControlCore` **on its own window's UI thread**, so a global buffer search
is a fan‑out + gather. The engine already has the precise pattern to mirror: the **activate‑sink**
mechanism (`Engine::activateSinks`, `RegisterWindowActivateHandler` / `ActivateSessionInOtherWindows`,
`Engine.h:152‑170`) — per‑window sinks, snapshot‑under‑lock, invoke‑outside, each marshals onto its
window's dispatcher, token‑detached in `~TerminalPage` (Rule #10).

New sibling seam on `Engine` (§4), plus a new **read‑only** `ControlCore` collector (§5).

---

## 4. The cross‑window buffer‑search seam (Engine)

Unlike `activateSinks` (fire‑and‑forget, **excludes** the source window), buffer search needs a
**request → streamed responses** shape and **includes** the source window (it has tabs too).

```cpp
// Engine.h (new) — mirrors the activate/settings sink lifetime + locking.
struct BufferSearchRequest {
    std::wstring query;
    bool caseSensitive{}, regex{}, wholeWord{};
    uint64_t generation{};                                   // re-type bumps it; stale responses dropped
    std::function<void(BufferSearchResult)> collector;       // marshaled back to the requesting window
    std::function<void(const std::wstring& windowId)> done;  // a window finished its tabs
};
struct BufferSearchResult {
    std::wstring windowId, tabId, sessionId;                 // sessionId set for managed tabs (dedup/jump)
    AgentKind kind; std::wstring title; std::wstring cwdColorKey;
    std::vector<BufferMatch> matches;                        // {bufferRow, lineText, matchColumns}
};

uint64_t RegisterWindowBufferSearchHandler(windowId, fn);    // each TerminalPage at init; detach in dtor
void     UnregisterWindowBufferSearchHandler(token);
void     SearchBuffersInAllWindows(const BufferSearchRequest&);  // fan to ALL sinks (incl. source)
```

- The requesting page builds the request with a **generation** from an atomic counter (bumped on
  every keystroke‑debounced re‑search) and a `collector`/`done` that **marshal back onto the
  requesting window's dispatcher**, drop anything whose generation != current, and append to the
  results model.
- Each sink (a `TerminalPage`) hops to **its** UI thread, walks `_tabs`, and for each terminal
  pane calls `ControlCore::CollectSearchMatches` (§5), assembling a `BufferSearchResult` per tab,
  then `collector(...)`; a final `done(windowId)` lets the UI show "k of N windows searched".
- **Cancellation** = bump the generation (the page ignores late results); optionally pass a
  `cancelled` predicate the per‑tab collector polls between rows for very large buffers.

Threading guard: the sink **snapshots** each buffer's text under the terminal lock (`ReadEntireBuffer`
is the lock‑safe primitive) and then **matches off the UI thread** on a worker, so a multi‑MB
scrollback never janks that window; only the cheap snapshot is on the UI thread.

---

## 5. `ControlCore::CollectSearchMatches` (new, read‑only)

The live per‑tab box drives `ControlCore::Search` (it mutates highlight/focus state). The global
search must **not** disturb that, so add a sibling that returns matches **with context** and
**touches no live state**:

```cpp
// ControlCore — returns matches without altering the live SearchBox highlight/focus state.
std::vector<BufferMatch> CollectSearchMatches(query, caseSensitive, regex, wholeWord);
// BufferMatch { til::CoordType row; std::wstring lineText; std::vector<std::pair<int,int>> cols; }
```

Built on the same ICU `TextBuffer::SearchText` the live box uses (so **multiline/soft‑wrap
semantics match** the per‑tab box — see the per‑tab search notes), but it writes results into a
local vector instead of `SetSearchHighlights`. For each span it extracts the row's text
(`GetText`) for the result/preview. A separate `Search` instance (or a direct `SearchText` call)
keeps the live `_searcher` untouched.

---

## 6. The UI — a full‑window page

Reuse the **Archive/Sessions page chrome** verbatim: the window‑level overlay registry +
`_DismissAgentPageOverlays` tab‑switch seam, the **deferred‑pointer‑handler** discipline (a
synchronous mid‑click tree mutation AVs the XAML‑Islands hit‑test), Up/Down row nav (wrapping,
tunneling `PreviewKeyDown`), focus‑search‑on‑show, and the draggable, globally‑persisted splitter.

```
┌ Find in Fleet ───────────────────────────────────────────────────────┐
│ [ query…………… ]  Aa  .*  ⌷w   ◉ Both  ○ Terminals  ○ Sessions          │
│                                       👤 🤖 📁 📄 🏷 (F)   [scope: dir…] │
├──────────────────────────────┬────────────────────────────────────────┤
│ 1,240 matches · 18 sources   │  PREVIEW                                 │
│  (12 terminals · 6 sessions) │     142  …earlier output line            │
│ ▾ ⛓ build-agent  W1 · pwsh   │  ▸ 143  error CS0246: type 'Foo' could…  │
│      142  error CS0246 …      │     144  the using directive is missing  │
│      268  error CS0246 …      │                                          │
│ ▾ 📄 auth refactor  session   │  [ Jump  ⏎ ]   [ Open per-tab search ]   │
│     🤖 "…the JWT validation…" │                                          │
│     👤 "fix the auth bug"     │                                          │
└──────────────────────────────┴────────────────────────────────────────┘
```

- **Top bar:** query + **case / regex / whole‑word** toggles + **domain selector** + (when
  Sessions in scope) the scope chips + an optional **mask/scope** filter (limit to tabs/sessions
  under a directory — IntelliJ's "Directory" scope). 200 ms debounce, generation‑cancelled.
- **Left — grouped results:** group by **source** (a terminal tab or a session), collapsible,
  per‑group match count, **streamed** as windows respond and as the slow transcript phase lands.
  Each group header carries the source's **per‑dir color chip** + **kind pill**
  (pwsh/cmd/claude/codex/external — the Triage‑Board palette via `AgentStatusColors.h`) and a
  `W{n}` window chip for terminals. A tally: "N matches · M sources (T terminals · S sessions)".
- **Right — preview:** the selected match **in context** — surrounding buffer lines (terminals) or
  the conversation snippet (sessions), with the match highlighted and 👤/🤖 scope‑tagged.
- Detail caching + a registry observer + refresh, exactly like the Archive/Sessions pages.

---

## 7. Query semantics — reconciling two models

The two engines have **different** native query models; the unified box exposes a **common core**
and lets each engine honor what it can:

| Control | Terminals (`ControlCore`) | Sessions (`SessionSearch`) |
|---|---|---|
| literal text | ✓ (ICU literal) | ✓ (substring terms) |
| **case** toggle | ✓ (`CaseSensitive` flag) | ✗ today (always case‑insensitive) → P4 add |
| **regex** toggle | ✓ (ICU `UREGEX_MULTILINE`) | ✗ today (`BuildSearchRegex` escapes specials) → P4 add |
| **whole‑word** | ✗ today → P2 add to `SearchText` | ✗ today → P2 add |
| multi‑term AND / "phrase" / `<guid>` / (F) fuzzy | ✗ (single regex/literal) | ✓ (`ParseSessionQuery`) |
| 👤🤖📁📄🏷 scopes | n/a | ✓ |

**v1 reconciliation:** the unified box is **case + regex + whole‑word** applied per‑line
(terminals) / per‑message (sessions). For the session half, the same text is run through
`SessionSearch`'s existing (case‑insensitive, term‑AND) model + the scope chips; case‑sensitive +
raw‑regex parity on the session side is a `SessionSearch` enhancement (**P4**). The asymmetry is
documented in the UI (a small "ⓘ sessions: case‑insensitive" hint when case is on + Sessions in
scope) rather than silently diverging.

---

## 8. Jump‑to‑result

Selecting a match (click / `Enter`) → **focus + center**:

- **Terminal match** → Activate the hosting tab: local select when this window hosts it, else the
  engine's cross‑window fan‑out (`ActivateSessionInOtherWindows` for managed; a parallel
  tab‑activate for shells, keyed by `windowId`+`tabId`) → then **center the buffer on the match
  row** via the center‑on‑line primitive (`Terminal::UserScrollViewport(row − viewHeight/2)`,
  exposed as `TermControl.ScrollBufferToRowCentered(row)`), and optionally open the per‑tab
  `SearchBox` pre‑filled so the match highlights in place. Re‑resolve the row at click time (the
  buffer may have scrolled/trimmed since the search snapshot — §10.2).
- **Session match** → if the session is **open** (a live tab), Activate it and **jump the
  transcript** — dovetails with the in‑flight `JumpToConversationPrompt` /
  `ScrollToAdjacentConversationPrompt` work (`TermControl.idl`, `SUMMARY_JUMP.md`); if
  **archived/on‑disk**, open the Sessions/Archive detail for that session or offer **Resume here**
  (the existing `_ResumeSessionFromDisk` seam).

---

## 9. Triggering

- **`Ctrl+Shift+F`** → Find in Fleet. Rebind in `defaults.json`: move `ctrl+shift+f` off
  `Terminal.FindText` (the per‑tab box, still on `ctrl+f`) onto a new
  `Terminal.FindInFleet` action.
- **Toolbar button** in the Manager toolbar (next to **Sessions** / **Archived**) — "Find in Fleet".
- **Command Palette** entry (P4).

The page is a `TerminalPage`‑hosted full‑window surface (like Archive/Sessions), so the action
handler is symmetric with `_ShowArchivePage` / the Sessions page.

---

## 10. Hard problems & mitigations

1. **Agent‑buffer noise (accepted).** Per the locked decision, agent tabs' raw buffers are
   searched. They will yield noisy, redraw‑fragmented lines. Mitigations: group/label by kind so
   the user can fold agent‑buffer groups; the same session's clean **transcript** sits in the
   Sessions groups; a future "collapse buffer⇄transcript duplicates" toggle (§10.5).
2. **Liveness/staleness.** Buffers stream; results snapshot at search time. Mirror the Sessions
   page: a registry/observer‑driven **refresh** button rather than re‑searching every frame; at
   **jump** time re‑run the per‑tab search to re‑resolve the row (don't trust the snapshot's row).
3. **Cost.** Fanning a search to every tab in every window per keystroke. Debounce (200 ms) +
   **generation‑cancel** + **snapshot‑under‑lock‑then‑match‑off‑thread** + a **scrollback cap**
   (configurable; default e.g. last N k rows) + skip unchanged buffers via a cheap
   `GetLastMutationId()` check (the same id `Search::IsStale` uses).
4. **whole‑word** isn't in either engine → small add (`SearchFlag::WholeWord` in `SearchText`;
   a `\b…\b` wrap in `SessionSearch`'s regex + an in‑process boundary check).
5. **Dedup (deferred).** A managed session matching in both domains shows two groups; an optional
   "merge by sessionId" view collapses them (buffer matches as a sub‑section under the session).
6. **Generation/threading correctness.** All result mutation happens on the requesting window's UI
   thread (the collector marshals there); the model is single‑writer. The completion tally uses
   the `done(windowId)` count vs `LiveWindowIds()`.

---

## 11. Phasing

- **P1 — Sessions‑as‑global (quick win).** Stand up the Find‑in‑Fleet **page shell** + the
  `Ctrl+Shift+F`/button trigger, wired to `SessionSearch` only (the session half is done). Ships a
  real global content search immediately; Terminals shown as "coming" or hidden.
- **P2 — Terminals domain.** `Engine` buffer‑search seam (§4) + `ControlCore::CollectSearchMatches`
  (§5) + the per‑window sink in `TerminalPage` + the collector/aggregator. whole‑word added here.
- **P3 — Fusion + jump.** Unified grouped results + preview pane + terminal **center‑jump**
  (`ScrollBufferToRowCentered`) + session **transcript‑jump**.
- **P4 — Polish.** Streaming refinement, mask/scope filters, session‑side case/regex parity,
  Command Palette, the dedup/merge view, persisted page layout (splitter fraction) + last‑query.

---

## 12. Files (anticipated)

- `doc/agentmaster/FIND_IN_FLEET.md` (this doc).
- `src/cascadia/TerminalControl/ControlCore.{idl,h,cpp}` — `CollectSearchMatches` (+ `SearchFlag::WholeWord` in `buffer/out/search.{h,cpp}` / `textBuffer.cpp`).
- `src/cascadia/TerminalApp/AgentMaster/Engine.{h,cpp}` — the buffer‑search sink seam.
- `src/cascadia/TerminalApp/AgentMaster/SessionSearch.{h,cpp}` — P4 case/regex parity; P2 whole‑word.
- `src/cascadia/TerminalApp/TerminalPage.AgentFindPage.cpp` (new TU, mirroring `…AgentArchivePage`/`…AgentSessionsPage`) — the page + per‑window sink registration + the aggregator + jump.
- `src/cascadia/TerminalApp/TerminalPage.{h,cpp}` — the action/button seam (`_ShowFindInFleetPage`), overlay registration.
- `src/cascadia/TerminalSettingsModel/defaults.json` — `Terminal.FindInFleet` action + `ctrl+shift+f` rebind.

---

## 13. Open questions (deferred)

- **Scrollback cap default** (rows searched per buffer) — perf vs completeness.
- **Result ordering** — by match count, by recency (last‑activity), or by source kind first.
- **Persist last query / page size** across restart (the `AppSettings` + WindowRecord idioms).
- **Replace** (find‑and‑replace) — explicitly out of scope for v1 (read‑only invariant); if ever,
  buffers can't be edited anyway, so it would be sessions‑file editing — almost certainly never.
