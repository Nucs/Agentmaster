# 13 · The AI search (AIS)

> Conventions and fixtures: [README.md](README.md). Spec: SESSIONS.md §9 (9.1–9.15); SDK_HARNESS.md §5.1, §5.3;
> CLAUDE.md *The agent harness + the Sessions page's AI search*. Needs the mock (the agent's model requests are
> scripted: the request line carries `[[mock tool mcp__sessions__search {…}; tool mcp__display__show {…}; reply
> "…"]]`), **F-CORP**, and the harness pointed at the mock (Agentmaster started with the mock's URL in its
> environment — the harness keeps `ANTHROPIC_BASE_URL`).

**What shipped.** 0.7.9: an **AI** toggle inside the Sessions page's search box turns it into a request line; a hosted
Sonnet 5.5 (one hidden `claude.exe` per Agentmaster process, `--no-session-persistence`, auto-compaction at 500K,
parked 10 minutes after no page uses it) runs the page's own search and fills the table with the sessions that fit,
with a one-sentence answer in a strip; a follow-up refines; Escape stops; ⊘ resets. 0.7.10: exactly its tools (no
stray MCP-resource tools), bypass → auto → dontAsk. 0.8.0: it reads before it shows (`read`, `grep`), each row's
one-line reason, a plan line while it works and a descriptive answer, window / tab tools (list, resume, fork,
deactivate) with guards, the thinking level (off → max, live, global, remembered), one shared index that is never
partial, every session searched by default.

## Lifecycle model

```text
 AI toggle on ─► request + Enter ─► (process: start or reuse) ─► the plan line in the strip ("I'll 1. … 2. …")
     ─► tool calls (search / grep / read / tabs) ─► show (ids + why) ─► the table = exactly those sessions ─► the answer
     ├─ follow-up ─► refined (context kept)        ├─ Esc ─► stopped        ├─ newer request ─► the older one stopped
     ├─ ⊘ ─► conversation thrown away              └─ no page shows the AI search for 10 min ─► process parked (digest kept)
 one conversation per PROCESS: every window's Sessions page is a view of it
```

---

### AIS-01 · Ask for a session in plain words

> **Since** 0.7.9 / 0.8.0 · **P0** · **Needs** mock, F-CORP · **Spec** SESSIONS.md §9.1, §9.5, §9.12, §9.14

**Phases**
1. **Toggle.** *Do:* Sessions page → click **AI** inside the box. *Expect:* the toggle accent-filled; the box is a
   request line (typing filters nothing); ⊘ appears right of the box; the strip shows `thinking off  ctx …`.
2. **Request.** *Do:* `the session where I fixed the tab drag crash` + the script, Enter. *Expect:* the strip shows the
   request, then the agent's plan line while it works ("Searching: …"); a preview of the search rows ("choosing…"),
   replaced about a second later by EXACTLY the shown sessions, best first, the first row selected (its detail pane
   visible); each row with a quote line (its evidence) under the title; the strip ends with the answer.
   *Check:* [MOCK] the harness's requests (tools: `mcp__sessions__*`, `mcp__display__show`, `mcp__tabs__*` — nothing
   else); [LOG] `[ai-search] …`.
3. **Follow-up.** *Do:* `only the open ones`. *Expect:* the list refined; the conversation kept its context
   ([MOCK] the history grows).
4. **Sort.** *Do:* click a column. *Expect:* the list re-sorts; the model's order returns when the sort is set back.
5. **Off.** *Do:* turn AI off. *Expect:* the normal filter box and list; the conversation kept (back on → the strip
   shows it).

**Edge cases**
- **E1 Esc during a request** → stopped; a new request also stops the previous one.
- **E2 ⊘** → the conversation thrown away; the normal list returns.
- **E3 A listed session outside the page's date window** → a real row (detail, resume, fork, rename, favorite, tags
  work on it).
- **E4 Two windows** → a request in one shows its list in the other's AI-on page too.
- **E5 A restart** → the record and the last shown view are kept (`<profile>\harness\sessions-search*.json`); the toggle
  starts off.
- **E6 The hidden process** → never listed among sessions, not in the External census.

---

### AIS-02 · The thinking level

> **Since** 0.8.0 · **P1** · **Needs** mock (it logs the request's `thinking` and `output_config.effort`) · **Spec** SESSIONS.md §9.1; SDK_HARNESS.md §5.3

**Phases**
1. **Rotate.** *Do:* click `thinking off` repeatedly. *Expect:* off → low → medium → high → xhigh → max → off; every window's
   strip follows; the setting survives a restart (`sessionsAiThinkingLevel`).
2. **Applied live.** *Do:* at `high`, send a request. *Expect:* the SAME process (no restart, context kept); [MOCK] the
   request carries adaptive thinking at effort high; at `off`, the request body carries `{"thinking": {"type":
   "between_tools"}}` and effort low.
3. **Unsupported level** (a model reporting fewer levels via the mock's `initialize` answer) → skipped.

---

### AIS-03 · Reopen sessions into the right window

> **Since** 0.8.0 · **P0** · **Needs** mock, F-W2 · **Spec** SESSIONS.md §9.14

**Promise.** The search can list windows and tabs and resume or fork a session into a named window right after a
chosen tab — in the background unless asked; with several windows and no window named, it asks; it never cuts off a
running turn without asking.

**Phases**
1. **List.** Script `tool mcp__tabs__list {}` → the tool result names every window (number, short id, `last_used`,
   `asked_from`) and every tab (pos, title, session id, state, dormant, dir, works_in, branch, last_turn, selected).
2. **Resume into a window.** Script `tool mcp__tabs__resume {"session": "<id>", "window": 2, "after": "<tab>"}` → a tab in
   window 2 right after that tab; not selected (background); the tool reports the opened id.
3. **No window named, two open.** Script `tool mcp__tabs__resume {"session": "<id>"}` → the tool REFUSES and describes
   every window; the reply asks the user which.
4. **Fork.** `mcp__tabs__fork` → beside its source by default; `focus:true` selects it and brings the window forward.
5. **Deactivate.** `mcp__tabs__deactivate {"session": <running>}` → refused without `cut_off_turn:true`; with it, parked.

**Edge cases**
- **E1 More than 5 opens or 2 deactivates in one request** → refused past the cap.
- **E2 An adopted tab** → deactivate refused.
- **E3 A resume whose transcript is gone** → a FRESH session (new id) reported (Rule #6).
- **E4 Interrupted-turn fork** (NOT BUILT, INT-06) → the tool forks uninterrupted, no dialog.

---

### AIS-04 · Searches never answer from a partial index

> **Since** 0.8.0 · **P1** · **Needs** mock, a large F-CORP (2,000 sessions), a cold start · **Spec** SESSIONS.md §9.15

**Phases**
1. **Cold.** *Do:* start the app, immediately send a request. *Expect:* the tool call waits for the complete index (up to
   90 s) — or answers "call again" — never searches a part; [LOG] no "still being indexed" answer.
2. **After a park.** *Do:* leave the page 11 minutes (process parked), ask again. *Expect:* the index is still there
   (never freed); the conversation continues from its digest.
3. **Every session** → no 90-day cut; a dated row says `in_period`.

---

### AIS-05 · The process's permissions and tools

> **Since** 0.7.10 · **P1** · **Needs** mock (logs the tool list), a policy forbidding bypass (managed settings) · **Spec** SDK_HARNESS.md §5.1

**Phases**
1. **Tool list.** [MOCK] the first request lists exactly the harness's tools (no `ListMcpResources`-style tools).
2. **Mode.** [LOG] `[harness] sessions-search: permission mode bypassPermissions` (or `auto` / `dontAsk` under a policy);
   never a mode that asks.
3. **Policy forbids bypass** (machine policy file in the guest) → the process runs in auto; a tool call never waits for
   an answer.
