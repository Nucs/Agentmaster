# Agentmaster end-to-end flows — what the suite must prove

This folder is the **catalog of end-to-end flows** for the features of Agentmaster's recent releases — the
pre-releases **0.7.7** (2026-09-07), **0.7.9** (2026-10-11) and **0.7.10** (2026-10-14), and **0.8.0** (2026-10-22,
the stable that graduated the whole 0.7.x line; the 0.7.1–0.7.6 and 0.7.8 nightlies are folded into the release that
shipped them) — plus the features committed after 0.8.0 that no release holds yet (marked **NOT BUILT**). Each flow
states a behavior a user relies on, the phases it goes through, what must be true at each phase and how to check it,
and the edge cases around it.

It is the **what**. The **how** — the scripted model (`mockllm`) that makes Claude Code answer instantly with
whatever the prompt asks for, the disposable Windows 11 test desktop, the driver library, the runner and the lanes —
is [`../E2E_TESTING.md`](../E2E_TESTING.md). A flow here becomes one or more tests there; its phases are the test's
steps and its checks are the test's assertions.

Written 2026-10-23 from: the per-version changelogs of the four releases
(`.claude/skills/release-version/am_release_080_notes.draft.md` holds them all), the commits after `v0.8.0`
(9245a890f, 381c6c715, 57a40dd30, 0502c9380, 3b042ea32), every spec document under `doc/agentmaster/`, the project
CLAUDE.md, and the code wherever a document and the code disagreed (the disagreements are listed at the end).
The catalog started as a design; the section **The suite that runs these flows** below records the implemented
subset (67 tests on 2026-10-23, all green on a test build with that day's fixes). Other flows remain specifications,
not claims of automated coverage.
The shared reliability guard and dedicated crash/corruption/leak scenarios are separately planned in
[E2E_RELIABILITY.md](../E2E_RELIABILITY.md); its G/C/L/R scenario IDs do not add implemented flows to this table.

## Contents

| File | Area (code) | Flows | P0 / P1 / P2 | Phases | Edge cases | Known defects |
|---|---|---|---|---|---|---|
| [01-agent-manager-page.md](01-agent-manager-page.md) | The Agent Manager page, Home, jump back, zero-tab windows (MGR) | 7 | 4 / 2 / 1 (+1 NOT BUILT) | 29 | 22 | 1 (fixed in code) |
| [02-side-panel.md](02-side-panel.md) | The side panel, its cards, Sum, the + launcher (SP) | 8 | 3 / 3 / 1 (+1 NOT BUILT) | 37 | 27 | 2 (fixed in code; SP-K2 reclassified as a feature) |
| [03-prompt-queue-runner.md](03-prompt-queue-runner.md) | The Prompt Queue Runner (PQR) | 16 | 10 / 6 / 0 | 60 | 67 | 5 |
| [04-drafts-and-delivery.md](04-drafts-and-delivery.md) | Unsent drafts, the draft swap, the mail button, the fork carry (DRF) | 9 | 5 / 3 / 1 | 33 | 42 | — |
| [05-session-state.md](05-session-state.md) | Session state on every surface, through Claude Code's own events (STA) | 16 | 8 / 7 / 1 | 42 | 50 | 4 |
| [06-interrupted-work.md](06-interrupted-work.md) | API-error rewinds, the restore wake, interrupted turns (INT) | 7 | 2 / 2 / 0 (+3 NOT BUILT) | 21 | 37 | — |
| [07-tabs-park-close-dock.md](07-tabs-park-close-dock.md) | Deactivate, Close, drag / dock, the strip, quitting (TAB) | 10 | 5 / 2 / 3 | 39 | 27 | 4 (3 fixed in code; TAB-K2 reclassified as a feature) |
| [08-telegram-topics.md](08-telegram-topics.md) | Telegram: setup, topics, history, the mirror (TGT) | 11 | 4 / 6 / 1 | 37 | 25 | 2 |
| [09-telegram-card-and-control.md](09-telegram-card-and-control.md) | Telegram: the card, prompts, /steer, /commands, the screen (TGC) | 6 | 4 / 2 / 0 | 25 | 25 | 2 |
| [10-telegram-questions-and-menus.md](10-telegram-questions-and-menus.md) | Telegram: question cards, /add-dir menu cards (TGQ) | 6 | 3 / 3 / 0 | 24 | 21 | — |
| [11-telegram-search-and-new-threads.md](11-telegram-search-and-new-threads.md) | Telegram: 🔎 Search, fleet commands, the new-thread wizard, the pick menu (TGS) | 5 | 2 / 1 / 1 (+1 NOT BUILT) | 25 | 29 | — |
| [12-telegram-file-links.md](12-telegram-file-links.md) | Telegram: file links and the file server (TGF) | 4 | 0 / 3 / 1 | 7 | 6 | — |
| [13-ai-search.md](13-ai-search.md) | The Sessions page's AI search (AIS) | 5 | 2 / 3 / 0 | 19 | 10 | — |
| [14-presets.md](14-presets.md) | Presets: other models and providers (PRE) | 5 | 3 / 2 / 0 | 15 | 11 | — |
| [15-handover-family.md](15-handover-family.md) | /handover, /handover-here, /handover-standby (HND) | 8 | 1 / 6 / 1 | 29 | 28 | — |
| [16-transcripts-and-summaries.md](16-transcripts-and-summaries.md) | Show Transcript, Copy ▸ Transcript, the summary (TRN) | 7 | 4 / 3 / 0 | 26 | 21 | 2 |
| [17-sessions-browser.md](17-sessions-browser.md) | The Sessions browser (SES) | 6 | 3 / 3 / 0 | 31 | 20 | — |
| [18-notifications-taskbar-display.md](18-notifications-taskbar-display.md) | Rings, the taskbar count and frame, toasts, rendering, tab size (DSP) | 5 | 1 / 3 / 1 | 23 | 21 | 1 |
| [19-install-update-profiles.md](19-install-update-profiles.md) | Install, update, uninstall, profiles (INS) | 10 | 4 / 5 / 1 | 38 | 20 | — |
| [20-robustness.md](20-robustness.md) | Freezes, crashes, leaks, hostile environments (ROB) | 11 | 3 / 3 / 5 | 28 | 8 | 1 |
| [21-small-features.md](21-small-features.md) | Tag chips, the Tags panel, sorts, Specify a model, the launch bar, trust (MSC) | 9 | 2 / 4 / 3 | 35 | 10 | — |
| [22-cli-and-session-database.md](22-cli-and-session-database.md) | The `agentmaster` CLI and the session database (CLI, SDB) | 6 | 0 / 1 / 5 | 18 | 7 | 2 (1 fixed in code) |
| [23-tab-context-menu.md](23-tab-context-menu.md) | The tab's context menu: what it shows where, and what each item does (TCM) | 9 | 5 / 4 / 0 | 36 | 27 | 2 (1 fixed in code) |
| **Total** | | **186** | **78 / 77 / 26** (+6 NOT BUILT) | **≈ 677** | **≈ 565** | **28** (8 fixed in code) |

---

## How a flow reads

```text
### PQR-01 · One queued prompt is delivered into an idle session        ← the id (area code + number) and its promise in a phrase

> **Since** 0.7.7 / 0.7.9 · **P0** · **Needs** mock · **Spec** DELIVERY.md §3 …   ← first release · priority · fixtures and infrastructure · the spec

**Promise.**        what the user can rely on, in one or two sentences — the thing a failing test means is broken
**Preconditions.**  the fixtures and settings the flow starts from
**Phases**          the lifecycle, in order; each phase:
  1. **Name.**  *Do:* what the test (the user) does · *Expect:* what must be true afterwards · *Check:* where to read it
**Edge cases**      E1, E2 … a variation and its expected outcome (each becomes its own test or a parameter)
```

- **Lifecycle model.** Each file opens with the area's lifecycle as a diagram: the states a thing goes through and
  the events that move it. A flow's phases walk a path through it; the edge cases cover the transitions the main path
  does not.
- **Known defects** close a file: behaviors that are wrong today, found by reading or seen live, each with a repro and
  the expected behavior. They become tests marked `known_defect` (E2E_TESTING.md §8.1): green while the defect exists,
  red when it is fixed, so the marker is removed with the fix.
- **NOT BUILT** flows describe features committed after 0.8.0 that no install holds yet; their tests are written with
  the feature's build.
- **Since** names the first release that shipped the behavior. `≤ 0.7.7` means older than this catalog's range: a
  regression surface that the recent releases changed around (the Sessions browser, the /handover family, trust).

## Oracles — where a check reads

A check names its oracle. The rule of E2E_TESTING.md §1: wherever possible the evidence comes from something the
app does not control (the mock, the transcript, the operating system), so the app never grades itself.

| Tag | Reads | How |
|---|---|---|
| **[UI]** | what the user sees | UI Automation names, ids and `ItemStatus` (E2E_TESTING.md §6.3), screenshots and pixel crops (the flash ring, the badge disc, the preview frame) |
| **[ENG]** | the engine's own record | the read-only test bridge (§6.2: `census`, `sessions`, `session <id>`, `ui`), else `sessions.json` (≤ ~1 s behind) and `windows/<id>.json` |
| **[LOG]** | the app's logs | `<profile>\hooks.log` (`[nav]` and the mechanism tags), `autorunner.log`, `scanner.log`, `sessiondb\sessiondb.log`, `forwarder-errors.log` |
| **[TR]** | Claude Code's transcript | `<CLAUDE_CONFIG_DIR>\projects\<encoded cwd>\<session id>.jsonl` (and its `subagents\`) |
| **[MOCK]** | what reached the model | the mock's classified request log: each request's messages, model, effort, thinking, tools, tool results; `expect` steps |
| **[OS]** | the machine | processes (tree, command line, environment, owner, elevation), files and their times, the registry, Credential Manager, the clipboard, window state, DWM / taskbar state |
| **[TG]** | what the bot did in Telegram | the fake Bot API's record of every call (sendMessage, edits, reactions, topics, pins, photos), in order |

## Fixtures

Named starting points the flows reuse. Work folders: `w1` = `C:\e2e\w\t1`, `w2` = `C:\e2e\w\t2`, `w3` = `C:\e2e\w\t3`.
Every test gets a fresh profile (`AGENTMASTER_PROFILE=C:\e2e\p\<test>`) and a fresh Claude config
(`CLAUDE_CONFIG_DIR`), seeded as E2E_TESTING.md Appendix A says.

| Fixture | What it is |
|---|---|
| **F-S** | One managed Claude session in `w1`, launched through the Agent Manager's launch bar (the real path), its first prompt `[[mock reply "ready"]]` completed: Waiting for you, the runner in Auto (the cog default), one Typed row in its queue's history. Titled by its folder unless a flow renames it. |
| **F-S2** | F-S plus a second session B in `w2`, both in one window, both Waiting for you. |
| **F-W2** | Two windows: window 1 holds A (`w1`) and B (`w2`), window 2 holds C (`w3`). |
| **F-RS** | A restart: quit Agentmaster (the window's ✕ with "close all", or the app's quit) and start it again on the same profile; "Reopen your N previous windows?" answered Yes when it asks. A **crash** variant ends the process instead (the quit path skipped). |
| **F-TG** | The bot configured against a **fake Telegram Bot API** (below): a token, the owner's id, Threaded Mode; the fake plays the phone — the test posts the owner's messages, taps, reactions, edits, new threads and topic deletions through it, and reads [TG]. |
| **F-CORP** | A seeded corpus of transcripts under the test `CLAUDE_CONFIG_DIR\projects`: ~200–2,000 sessions over 90 days in several folders, forks, plan restarts, a Hebrew session, a 50 MB transcript, one with a single 1.2 MB line, a session that repeats a prompt's opening words 500 times, sessions whose tool calls work outside their launch folder. Built once by a generator (or sanitized real transcripts — never with secrets). |
| **F-CHAIN** | A chain: a session forked twice, a plan-mode restart, a `/clear` successor and a `/handover` successor (the last two through real commands in a setup step, since their links live in the registry, not on disk). |
| **F-PRE** | A CUSTOM provider "Mock" whose Base URL is the mock and a preset on it with model `mock-model-1` and a key `test-key` (Credential Manager), so the mock logs the URL, the `Authorization` header and the model each preset launch used. |
| **F-PKG** | The MSIX package of the build under test, signed with a throwaway certificate the guest trusts (`tools\install-qa\mkmsix.ps1`). Needed for toasts and the package paths. |
| **F-ZIP** | Portable zips of the build under test stamped at chosen versions (`tools\install-qa\amqa-guest.ps1` `New-AmQaCopy`), for install, upgrade, downgrade and update flows. |

## The mock's script, as the flows use it

A prompt carries its own answer: `[[mock <steps>]]` (E2E_TESTING.md §4.3). The flows use: `reply "<text>"`,
`say`, `think`, `tool <Name> {json}`, the shorthands `bash`, `write <path> "<text>"`, `edit`, `read`, `ask "<q>" A,B`,
`agent "<subagent prompt>"`, `delay <dur>`, `stream <n> <ms>`, `hold <gate>` (released by the test), `error <status>
<type> "<msg>" [times=N]`, `drop`, `usage …`, `title "<t>"`, `expect "<regex>"` (asserts what Claude Code sent
back for the step), `stop <reason>` (`stop refusal` sends the API's `stop_details` too) and `next` (ends a response
without a tool call: the steps after it answer the request Claude Code sends by itself — a max_tokens continuation, a
refusal's retry, a blocked Stop's continuation; STA-13, STA-14). Agentmaster's own prompts ("Continue from where you left off.", a /handover's first message) need
no script: the mock answers `OK`. A turn's final response is held ≥ 2.5 s after its first request (the 2-second floor,
§4.4) unless the flow asks for `fast`.

## Priorities and lanes

- **P0** — a broken P0 loses or corrupts the user's work, sends something the user did not mean, or blocks a core
  path (launch, resume, delivery, state, restore, install). Runs in every lane; a release gate.
- **P1** — a feature does not do what it says, or a recovery path fails. Runs in the fast lane before a release and
  nightly.
- **P2** — polish, performance at scale, rare environments, or flows that need infrastructure not built yet. Nightly or
  weekly; never a gate until its infrastructure exists.

Lanes (E2E_TESTING.md §8.4): **fast** (E2E timings: settle 2 s, spacing 1 s — proposed `AGENTMASTER_E2E_*` variables),
**realtime** (production timings, the timing-sensitive subset), **latest** (the newest Claude Code), **renderer**
(`CLAUDE_CODE_NO_FLICKER=1`, Claude's ALT-buffer renderer), **contract** (the mock + claude.exe alone). This catalog
adds two: **perf** (hundreds of tabs: MGR-06, SP-07, ROB-06, ROB-07) and **soak** (hours: ROB-08).

## Infrastructure the flows need

Beyond E2E_TESTING.md's Phases 1–3 (the mock, the desktop and runner, the test bridge, timings and AutomationIds):

| Need | For | State |
|---|---|---|
| A **fake Telegram Bot API** (getUpdates long-poll, every method the bot calls, file downloads, Bot API 10.x rich messages, flood answers with `retry_after`) and a way to point the bot at it | 08–12 (40 flows) | NOT BUILT. The host is hard-coded (`TelegramApi.cpp`: `kHost = L"api.telegram.org"`, HTTPS). Either an E2E host override (with plain HTTP to loopback) or, with no app change, the hosts file plus a certificate for `api.telegram.org` trusted in the guest. |
| A **fake GitHub releases API** (and asset downloads) | INS-06, updater edge cases | NOT BUILT. `Updater.h` `kApiHost = L"api.github.com"`; the installer's asset allowlist wants `https://github.com/<repo>/releases/download/…`, so a fake must answer both hosts (hosts file + trusted certificate). Until then: real releases with copies stamped older (PROFILES.md §2c's method). |
| The **MSIX package** build | DSP-02 (toasts), INS-08, F-PKG | `mkmsix.ps1` exists (install QA). |
| **Several accounts** and the **secure desktop** | INS-03, INS-04, INS-05, ROB-04 | `tools\install-qa` (accounts, `qm.py` for UAC through QEMU's monitor). |
| A **clipboard** oracle | TRN-03, DRF-03, TRN-05, SES, TCM-04 | BUILT: `e2e/amdriver/win32.py` (`clipboard_text`, `set_clipboard_text`). |
| **Pixel sampling** | DSP-01 (rings, disc, frame), MGR-02 (glyph) | screenshot crops; the taskbar flyout and Alt+Tab need input that opens them. |
| **Fault injection** (a startup that blocks; a stalled UI dispatcher; the TUI's input held) | ROB-09, ROB-10, PQR-11 E3 | NOT BUILT; env-gated like the test bridge, never in a normal run. |
| **Read-only probes** of a live process | DSP-04, ROB-07 | `tools\threadcensus.cs`, `tools\renderwait.cs`, `tools\cpuhunt`, `tools\leakhunt\xamltree.py` (all exist; never `livesample.exe` on a run under test — it suspends threads). |
| A **seeded corpus** generator | F-CORP, F-CHAIN | NOT BUILT. |
| A guest account with a **Hebrew name** | ROB-04 | a provisioning step. |
| An **RDP** client against the guest | ROB-06 E1 | optional. |

## What each release brought, and the flows that cover it

Every item of the release notes' "New" and "Fixes" sections, by release. A flow id in *italics* covers the item in
part.

**0.8.0** — New: the fleet in Telegram (TGT-01–TGT-09, TGT-11) · the conversation readable on a phone (TGT-04, TGT-07)
· driving a session from its topic (TGC-01–TGC-06) · answering questions and `/add-dir` from Telegram (TGQ-01–TGQ-06) ·
Search, new threads, the directory, the start note, the secretary (TGS-01–TGS-04, TGT-10, TGT-08) · file links
(TGF-01–TGF-04) · the AI search reads, explains, reopens (AIS-01, AIS-03) · the thinking level (AIS-02) · an API-errored
turn continues (INT-01–INT-03) · busy tabs wake after a restart (INT-04) · the session database (SDB-01–SDB-04) · install
just for you / all users / the signed-in user (INS-01–INS-04, INS-07) · a user's own install wins (INS-05) · the + card's
folder list ready (SP-05).
Fixes: the October 14 freeze (STA-08, *ROB-06*) · the leak in every terminal (ROB-08) · a killed fullscreen claude
(STA-11) · updates and uninstalls that could lose things (INS-06, INS-07) · the side panel at hundreds of tabs (SP-07,
ROB-07) · AI search answers that missed (AIS-04) · a queued `/add-dir` or `/model x` (PQR-12) · hidden content matches
(SES-03) · Launch needing a double-click (MSC-08, SP-05) · the Settings card (MSC-06).

**0.7.10** — New: Home is also the way back (MGR-02) · the folder filter (SES-04) · a fork's separator says `fork`
(TRN-04). Fixes: the + launcher covering its box (SP-05) · the AI search's hidden process tightened (AIS-05).

**0.7.9** — New: the Agent Manager as a page (MGR-01, MGR-03, MGR-05) · Alt+Left / Ctrl+Home back to Windows Terminal
(MGR-04) · the side panel (SP-01–SP-06) · the Prompt Queue Runner in every build (PQR-01–PQR-16) · presets (PRE-01–PRE-05)
· the AI search (AIS-01) · Show Transcript (TRN-01, TRN-02) · Copy ▸ Transcript at five levels (TRN-03) · the whole
conversation in the summary (TRN-04) · unread work on the taskbar (DSP-01) · rendering settings (DSP-03, DSP-04) · tab
size (DSP-05) · Deactivate's scopes (TAB-02) · unsent prompts travel (DRF-07, DRF-03) · sorting and cards (MSC-03,
MSC-04) · "Specify a model…" remembers (MSC-05). Fixes: the hung launch (ROB-09) · the crash on a mouse move (ROB-05) ·
the 8-hour freeze (*ROB-06*) · a slow UI on large fleets (ROB-07, SP-07, MSC-02) · wrong states (STA-02, STA-06, STA-07,
STA-09) · hooks timing out (ROB-03) · Ctrl+G and PATH (ROB-02) · refused and garbled prompts (DRF-08, ROB-04) · Deactivate
and Restart unbinding tabs (TAB-01) · the tab strip (TAB-06, TAB-07, TAB-08) · closing a tab starting another claude
(TAB-04) · `/compact`, `/clear` and resume (STA-06, STA-10, TRN-07) · summaries (TRN-05, TRN-06) · drafts under the
fullscreen renderer (*DRF-01*, renderer lane) and Send now on a dormant tab (PQR-10).

**0.7.7** — New: Deactivate (TAB-01) · a portable copy installs like an app (INS-02) · `Agentmaster.exe` and the
forwarder (INS-09) · cross-window docking (TAB-05) · batch-closing a folder (TAB-03) · Transcript Followup (superseded:
no menu offers it since 0.7.9; TRN-03) · the unsent-draft pipeline (DRF-04, DRF-05, DRF-06, PQR-13) · the interrupt hold
(PQR-07) · the board's tag chips (MSC-01) · the toast's third line (DSP-02) · Skills Loaded and Copy Prompt (TRN-05) · the
"unavailable" cursor (DRF-02) · the color picker (TAB-09) · the updater's choices (INS-06) · the `EDITOR` default (MSC-07).
Fixes: three UI-freeze classes (*ROB-06*, ROB-10, MGR-06) · the tab-drag crash class (TAB-05 E1) · orphaned consoles
(ROB-01) · atomic, verified delivery (PQR-01, PQR-02, PQR-11, DRF-02) · stuck states (STA-04, STA-09) · draft clearing
(DRF-05) · a two-file `/handover` and the delete-after disarm (HND-02, HND-06).

**After 0.8.0 (NOT BUILT into a release)** — interrupted turns: Restart's question, the fork question and its stop
watch, their Telegram versions (INT-05–INT-07; 381c6c715) · a Search pick's menu by the session's state: Activate,
Wake, Go to topic (TGS-05; 0502c9380) · `agentmaster <verb>` answered by an unpackaged install's own exe, and the
release CLI on the right profile (CLI-02 phase 3, CLI-K2; 3b042ea32) · the installer's edge cases (INS-01 E3–E6;
9245a890f — the script on the branch; the app's embedded copy not built) · a side-panel card's dot is the tab strip's
status icon: the draft's three dots, the red unread ring, the cache-warm spark crown (SP-08; SIDE_PANEL.md §2g) · a
launch made on the Agent Manager page stays on the page and selects the new session there, and a launch from a tab is
selected on the page too (MGR-07; MGR-K1) · ONE click on the + card's Launch while its presets menu is open (SP-05
phase 5; SP-K1) · a Shift+Click on a tab starts its session in place and keeps the selection (TAB-01 phase 4; TAB-K4)
· a side-panel card menu's action (Move to Idle/Done, Esc, Show Transcript …) keeps that card selected and the list
where it was (SP-02 phase 8; SP-K3).

**The regression base** — older behaviors the recent releases build on, covered because every new feature leans on
them: the /handover family (HND-01, HND-03, HND-04, HND-05, HND-07, HND-08), the Sessions browser (SES-01, SES-02,
SES-05, SES-06), Needs approval and long quiet work (STA-03, STA-05), drafts remembered (DRF-09), profiles (INS-10), the
workspace trust seed (MSC-09), the CLI (CLI-01, CLI-02).

**Not covered here** (older than the range and unchanged by it): Codex's managed lifecycle, the per-tab badge's basics,
the Explorer tree, templates, adopting a hand-typed claude. They belong to the suite's later breadth (E2E_TESTING.md §11).

## Known defects the suite should show on its first day

| Id | What | Source |
|---|---|---|
| PQR-K1 | Send now re-sends a selected history row | read, 2026-10-21 (memory `prompt-queue-runner-study-2026-10-21`) |
| PQR-K2 | Move up / down swap by raw queue index (history rows included) | read |
| PQR-K3 | The board's ⚙ sent/total counts history | read |
| PQR-K4 | Templates copy history and reach archived sessions | read |
| PQR-K5 | Records the registry mints get only the cog's mode | read |
| TGT-K1 | A session's topic the user deleted is never made again | MESSAGING.md §21.21 |
| TGT-K2 | A send that gets no answer is lost | MESSAGING.md §21.21 |
| TGC-K1 | `/newsession` / `/forksession` take any text as the model | MESSAGING.md §21.21; memory `telegram-remote-launch-bugs` |
| TGC-K2 | A new thread's held messages replay as topic commands (0.8.0: to verify) | MESSAGING.md §21.21 |
| TRN-K1 | A board / tree sort click reverts the summary panel's dragged size | read, 2026-10-05; still in the code |
| TRN-K2 | A ▸ jump in the side panel's peek mode scrolls a hidden tab | read, 2026-10-05 |
| DSP-K1 | A portable / per-user install shows no session toast; its clicks would do nothing | probe, 2026-09-27 |
| CLI-K1 | The dev alias's CLI crashes on every real verb (Debug build, Control Flow Guard) | measured, CLI.md §11 |
| CLI-K2 | An unpackaged release CLI reads the DEV profile — **fixed in code** by 3b042ea32, verified on the test build 0.8.1.1 (run `fixall-0811`; not in a release) | measured, CLI.md §11 |
| MGR-K1 | A session launched from the Agent Manager page is not selected on the page (Home offers no way back) until a tab switch — **fixed in code 2026-10-23**, verified on the test build 0.8.1.1 (run `fixall-0811`; MGR-07; not in a release) | e2e suite, 2026-10-23 (`e2e/suites/test_manager_page.py`) |
| SP-K1 | The + card's Launch needs a second click while its presets menu is open (the page's own Launch takes one) — **fixed in code 2026-10-23**, verified on the test build 0.8.1.1 (run `fixall-0811`; SP-05 phase 5; not in a release) | e2e suite, 2026-10-23 (`e2e/suites/test_side_panel.py`) |
| TCM-K1 | A blank tab rename leaves the tab showing Claude Code's own terminal title (the session keeps its title) | e2e suite, 2026-10-23 (`e2e/suites/test_tab_menu.py`) |
| TCM-K2 | Tags from the tab menu leaves the keyboard in the terminal (typed text and Esc reach claude) — **fixed in code 2026-10-23**, verified on the test build 0.8.1.1 (run `fixall-0811`; TCM-05 phase 5; not in a release) | e2e suite, 2026-10-23 (`e2e/suites/test_tab_menu.py`) |
| TAB-K1 | A window close can save its Claude tabs as plain shell tabs (an autosave still pending at the close fired after the teardown archive) — **fixed in code 2026-10-23**, verified on the test build 0.8.1.1 (run `fixall-0811`: 9 of 9; TAB-10 E2, E3; not in a release) | e2e suite, 2026-10-23 (seen once, then reproduced on 0.8.0: 3 of 9 closes in the first sweep, 6 of 9 in run `nf-080`; `e2e/suites/test_workspace.py`) |
| TAB-K3 | The quit confirmation ("Do you want to close all windows?") is invisible to UI Automation — **fixed in code 2026-10-23**, verified on the test build 0.8.1.1 (runs `fixall-0811`, `fixall-0811b`; every page dialog is built in code: a declared one was shown nested in the page, not as a window-level popup, so the driver — and a screen reader looking for the window's dialog — did not find it; TAB-10 phase 3; not in a release) | e2e suite, 2026-10-23 (`e2e/suites/test_accessibility.py`) |
| A11Y-K1 | Agentmaster's own surfaces say little to UI Automation: the cog, the compose icons, ⋯ and every board card and tree row have no name, nothing has a stable id, and a card's selection, a tab's selection pill, an unread ring or an unsent draft are pixels only (a screen reader cannot tell them; the suite read them from side effects) — **fixed in code 2026-10-23** (AgentAutomation.h: names, `am.*` AutomationIds, item status — [`../E2E_TESTING.md`](../E2E_TESTING.md) §6.3 "As built"; verified on the test build 0.8.1.1, run `fixall-0811`; not in a release) | e2e suite, 2026-10-23 (`e2e/suites/test_accessibility.py`) |
| A11Y-K2 | UI Automation's hit test (ElementFromPoint — a screen reader's mouse and touch exploration) passes through every ContentDialog to the terminal behind it, a dialog built in code included; the tree walk and the keyboard reach the dialog | e2e suite, 2026-10-23 (`e2e/suites/test_accessibility.py`, `probe_dialogs.py`) |
| TAB-K4 | Shift+Click on a parked tab switches to it instead of starting it in place (its menu item names the gesture) — **fixed in code 2026-10-23**, verified on the test build 0.8.1.1 (run `fixall-0811`; TAB-01 phase 4; not in a release) | e2e suite, 2026-10-23 (`e2e/suites/test_tab_lifecycle.py`) |
| TAB-K5 | A reopen with a shell tab in the window starts the last Claude tab even when another Claude tab was focused (TAB-K2's mechanism; with a SHELL focused it is a feature) | e2e suite, 2026-10-23 (`e2e/suites/test_tab_menu.py`) |
| ROB-K1 | A reader holding `sessions.json` makes the save fail, with or without delete sharing (measured 2026-10-23: the app's replace fails while any handle is open), and nothing re-saves until the next change | e2e suite, 2026-10-23 (`e2e/suites/test_robustness.py`) |
| STA-K1 | A claude that ends without its SessionEnd never reads Done; killed mid-turn, or within ~2 s after a turn, it reads **Running** for good (its last `busy` heartbeat outlives the process and the scanner's "work outlived the turn" rule trusts it); `/exit` on a busy PC whose SessionEnd hook is cancelled stays Waiting | e2e suite, 2026-10-23 (`e2e/suites/test_session_state.py`) |
| STA-K2 | Esc before any output reads Running ~30 s more (Claude Code writes nothing; the presence-idle release needs 30 s of quiet) | e2e suite, 2026-10-23 (`e2e/suites/test_session_state.py`) |
| STA-K3 | A manual `/compact` reads Running ~30 s after it ended (PostCompact sets Waiting; 2 ms later the still-`busy` heartbeat promotes Running again) | e2e suite, 2026-10-23 (`e2e/suites/test_session_state.py`) |
| STA-K4 | A Stop hook that blocks leaves the session Waiting for you while Claude Code continues the same turn (~20 s until the busy heartbeat outlives the turn) | e2e suite, 2026-10-23 (`e2e/suites/test_session_state.py`) |

**Checked both ways (2026-10-23).** Every defect fixed in code (MGR-K1, SP-K1, TCM-K2, TAB-K1, TAB-K3, A11Y-K1,
TAB-K4, CLI-K2) has a `needs_fix` test that asserts the CORRECT behavior, and each was run on both builds. On 0.8.0
every one fails on its defect (runs `nf-080`, `nf-080b`; TAB-K1's sweep on 6 of its 9 moments — the defect is a race,
which is why it is a sweep). On the test build 0.8.1.1 — a purged Release build whose `src/` equals HEAD 1f4c68678,
zipped and run only in a claude-fleet desktop, never installed on this PC — every one passes (runs `fixall-0811`,
`fixall-0811b`). The open defects the suite found (TCM-K1, ROB-K1, TAB-K5, A11Y-K2) keep a known-defect test that
asserts TODAY's wrong behavior; each passes on both builds, so none of the fixes touched them. The defects found by
reading or measuring (PQR-K1–K5, TGT-K1–K2, TGC-K1–K2, TRN-K1–K2, DSP-K1, CLI-K1) have no test yet.

**Reclassified.** TAB-K2 — a reopen whose focused tab was a shell starts a Claude tab: the LAST restored one ("the
first" as filed, measured with one Claude tab) — is a FEATURE, not a defect (the user's decision of 2026-10-23, the
day the suite filed it): TAB-10 E1 expects it, and
`e2e/suites/test_tab_menu.py::test_a_shell_focused_reopen_starts_the_last_claude_tab` proves it
([07-tabs-park-close-dock.md](07-tabs-park-close-dock.md) *Reclassified: not defects*). SP-K2 — with Sum off, a
side-panel card's hover opens no rich card (the tab's hover does) — is a FEATURE too (the user's ruling of
2026-10-24): SP-04 phase 2 expects it, and `e2e/suites/test_side_panel.py::test_sum_off_card_hover_opens_no_rich_card`
proves it ([02-side-panel.md](02-side-panel.md) *Reclassified: not defects*).

## Where the documents disagree with the code (found while writing)

- **The Waiting-for-you timeout's default** is **4320 minutes (3 days)** in `SessionModels.h`
  (`waitingForYouTimeoutMinutes{ 4320 }`), and the cog's slider runs to 7 days; CLAUDE.md said "a 1m–3d slider … default
  60 = 1h" (corrected in b4fb6b941). The flows use the code's default (STA-01).
- **The queue history's origin pill** reads **`auto`** (blue) for a prompt the runner sent, not `flight`
  (`AgentManagerContent.AutoTesting.cpp`); CLAUDE.md said "flight" (corrected in b4fb6b941), and a comment there still
  does.
- **MESSAGING.md §21.19, §21.20, §21.22, §21.23 and §21.25, and SESSIONS.md §9.15** still carry "NOT BUILT yet" in their
  headings although 0.8.0 shipped them (the status note at the top of MESSAGING.md says to read such markers as older
  than the builds; §21.19 / §21.20 / §9.15 were found by the 2026-10-23 Search study in the project catalog).
- **MESSAGING.md §21.20 says the shared session index is read when the bot starts;** the code reads it at the first
  Search request of a run (the bridge is made lazily) — production's first request of 2026-10-23 waited 31 s (TGS-01 E4).
- **NOTIFICATIONS.md §9.7** still says the preview frame is "NOT BUILT; never run against a real Agentmaster window";
  it shipped in 0.7.9 (production 0.6.8.19).
- **SESSIONS.md §1**'s toolbar line names an "Archived" button that FAVORITES.md removed.

## The suite that runs these flows

The executable suite is [`e2e/`](../../../e2e/README.md) (the scripted model, the driver and its page objects, the
fixtures, the guest runner, the host orchestrator). Each test is marked with its flow (`@pytest.mark.flow("…")`).
Status on 2026-10-23 against the published 0.8.0 portable build and Claude Code 2.1.291 in a claude-fleet Windows 11
desktop: **49 of 49 tests pass in 37 min** (the suite of f710660e0); a known-defect test passes while its defect
exists. A `needs_fix` test proves a fix that no release holds yet, so it fails on 0.8.0 by design (deselect those
tests there with `-m "not needs_fix"`). A TEST build of 8932908db with MGR-K1's and SP-K1's fixes passed all 41
tests of the suites of 3773f7149, its three new `needs_fix` tests included, and all 28 tests of the tab life cycle,
robustness and tab menu suites of f710660e0 (the Deactivate scopes test on a rerun, once its own shell count stopped
counting a hook forwarder; [`../E2E_TESTING.md`](../E2E_TESTING.md) status).

**Later the same day the suite had 67 tests**, and all 67 pass on the test build 0.8.1.1 — a purged Release build
whose `src/` equals HEAD 1f4c68678, so it holds every fix of 2026-10-23 (MGR-K1, SP-K1, TCM-K2, TAB-K1, TAB-K3,
A11Y-K1, TAB-K4, CLI-K2) — in a claude-fleet desktop: run `fixall-0811` (65 tests in 63 min: 63 passed; the two
failures were test versions since corrected — the quit test's hit-test check, now A11Y-K2's own test, and a TAB-K2
test that expected the FIRST Claude tab to start) and run `fixall-0811b` (the corrected two, A11Y-K2's and TAB-K5's
new known-defect tests and the dialog probe: 5 of 5). On 0.8.0 every `needs_fix` test fails on its defect (runs
`nf-080`, `nf-080b`) and the new tests pass (`nf-080b`, `nf-080c`): the defects table above, *Checked both ways*.

**The session-state suite** (2026-10-23, `test_session_state.py`, 29 tests written from three probe rounds of
Claude Code 2.1.291's own events — [05-session-state.md](05-session-state.md) "Claude Code's events, measured"): with
the smoke test, 30 of 30 pass on the published 0.8.0 in 20.5 min (run `sta-final2`); its four known-defect tests
(STA-K1–K4) assert today's wrong behavior and fail once each is fixed.

| Flow | Automated | Tests (`e2e/suites/`) |
|---|---|---|
| F-S | the fixture on every surface | `test_smoke.py` |
| MGR-01 | phases 1–3, E1 | `test_manager_page.py::test_open_the_page_and_leave_it` |
| MGR-02 | phases 1–4, E1 | `…::test_home_turns_into_the_way_back`, `…::test_double_click_home_opens_the_page_and_stays` |
| MGR-03 | phases 1–4 (phase 2's restart too) | `…::test_closing_the_last_tab_keeps_the_window_on_its_page`, `…::test_a_page_only_last_window_is_not_reopened`, `…::test_deactivating_the_focused_tab_shows_the_page_first`, `test_tab_lifecycle.py::test_a_second_window_left_without_a_tab_closes_itself`, `…::test_closing_the_focused_tab_never_starts_a_dormant_one` |
| MGR-04 | phases 1–2 | `…::test_alt_left_and_ctrl_home_are_windows_terminals` |
| MGR-05 | phases 1, 3, 4; the board follows the state | `…::test_linked_lenses`, `…::test_board_follows_the_session_state` |
| MGR-07 | phases 1–6 (`needs_fix`: a build with MGR-K1's fix) | `…::test_a_launch_from_the_page_stays_on_the_page`, `…::test_a_plus_card_launch_follows_where_the_user_looks` |
| SP-01 | phases 1, 3, 4 | `test_side_panel.py::test_panel_lists_this_windows_sessions` |
| SP-02 | phases 1–5 | `…::test_follow_peek_and_jump`, `…::test_card_context_menu` |
| SP-04 | phases 1–2 (phase 2: no rich card on a card's hover — SP-K2, a feature since 2026-10-24) | `…::test_sum_off_lets_the_cards_fill_the_panel`, `…::test_sum_off_card_hover_opens_no_rich_card` |
| SP-05 | phases 1, 3, 4, 5, E2, E3 (phase 5: `needs_fix`, a build with SP-K1's fix) | `…::test_plus_launcher_card`, `…::test_plus_card_launch_takes_one_click_with_its_menu_open` |
| SP-06 | phases 3–5 | `…::test_collapse_and_dock`, `…::test_panel_state_survives_a_restart` |
| TCM-01 | phases 1–3, E1 | `test_tab_menu.py::test_menu_fits_the_tab`, `…::test_triage_item_follows_the_state` |
| TCM-02 | phases 1–3 | `…::test_position_decides_what_is_enabled` |
| TCM-03 | phases 1–2, E2, E3; TCM-K1 | `…::test_rename_renames_the_session_everywhere`, `…::test_restart_restores_the_renamed_tab_dormant`, `…::test_known_defect_blank_rename_shows_claudes_own_title` |
| TCM-04 | phases 1–7 | `…::test_copy_puts_the_sessions_facts_on_the_clipboard` |
| TCM-05 | phases 1–5 (phase 5: `needs_fix`, a build with TCM-K2's fix) | `…::test_show_transcript_mark_unread_favorite`, `…::test_open_in_explorer_opens_the_folder`, `…::test_tags_from_the_tab_menu_take_the_keyboard` |
| TCM-06 | phases 1–3 | `…::test_move_tab_reorders_the_strip`, `…::test_move_tab_to_new_window` |
| TCM-07 | phases 1–4, E1 | `…::test_fork_session_branches_the_conversation`, `…::test_new_session_here_starts_a_fresh_session`, `…::test_fork_on_a_model_and_on_a_specified_model` |
| TCM-08 | phases 1–2 | `…::test_restart_session_resumes_the_same_conversation` |
| TCM-09 | phases 1–5 | `…::test_close_tab_asks_and_keeps_the_session`, `…::test_close_other_tabs_asks_once`, `…::test_triage_item_follows_the_state` |
| TAB-01 | phases 1–4, E4, E7, E8 (E8 in the scopes test; phase 4's Shift+Click on the tab: `needs_fix`, a build with TAB-K4's fix) | `test_tab_lifecycle.py::test_deactivate_parks_the_session_and_a_click_wakes_it`, `…::test_a_parked_tab_starts_in_place_and_survives_a_restart`, `…::test_shift_click_on_a_parked_tab_starts_it_in_place` |
| TAB-02 | phases 1–6, E1, E3 | `test_tab_lifecycle.py::test_deactivate_scopes` |
| TAB-03 | phases 3–5, E1, E5 (phases 1–2: TCM-09's tests) | `test_tab_lifecycle.py::test_unfavorite_and_close`, `…::test_close_of_same_folder_across_windows` |
| TAB-04 | phase 2 (both branches) | `test_tab_lifecycle.py::test_closing_the_focused_tab_never_starts_a_dormant_one` |
| TAB-10 | phases 1–3, E1 (TAB-K2, a feature since 2026-10-23), E4 (TAB-K5; TAB-K1 is named by the restart test's record check); phase 3's quit confirmation read by UI Automation (`needs_fix`, a build with TAB-K3's fix) | `test_workspace.py::test_close_all_windows_keeps_the_claude_tab`, `test_tab_menu.py::test_restart_restores_the_renamed_tab_dormant`, `…::test_a_shell_focused_reopen_starts_the_last_claude_tab`, `…::test_known_defect_a_claude_focused_reopen_also_starts_the_last_claude_tab` (E4, TAB-K5), `test_accessibility.py::test_the_quit_confirmation_is_readable` |
| TAB-10 E2, E3 / TAB-K1 | the fix's own checks (`needs_fix`): Close Window right after a tab switch, swept over 5 moments, and on one of two windows over 4 | `test_workspace.py::test_a_close_right_after_a_tab_switch_keeps_the_claude_tabs`, `…::test_closing_one_of_two_windows_keeps_its_claude_tab` |
| A11Y-K1 / TAB-K3 | the fixes' own checks (`needs_fix`), MGR-05 phase 2 among them (the selection on every surface and the tab's pill, read from item status) | `test_accessibility.py::test_the_selection_is_readable_on_every_surface`, `…::test_the_large_paste_warning_is_readable`, `…::test_the_page_names_its_glyph_buttons_and_cards`, `…::test_the_tab_says_its_session_state`, `…::test_known_defect_a_hit_test_passes_through_a_dialog` (A11Y-K2) |
| STA-01 | phases 1–3, 5, E2, E3 (phase 4, the unread ring: DSP) | `test_session_state.py::test_turn_reads_running_then_waiting_with_no_blip`, `…::test_turn_under_two_seconds_settles_to_waiting`, `…::test_type_ahead_is_absorbed_into_the_running_turn`, `…::test_exit_reads_done` |
| STA-02 | no blip while a turn is held 10 s (not the 20-prompt sample) | `…::test_turn_reads_running_then_waiting_with_no_blip` |
| STA-03 | phases 1–2, E3 | `…::test_question_reads_needs_approval_until_answered`, `…::test_permission_prompt_reads_needs_approval` |
| STA-04 | phases 1–2, E1, E2 (a cut, a server error mid-stream), E4, E6 | `…::test_final_api_error_reads_error_with_its_message_and_recovers`, `…::test_retried_overload_never_reads_error`, `…::test_stream_cut_mid_response_reads_error`, `…::test_server_error_mid_stream_reads_error`, `…::test_model_not_found_reads_error`, `…::test_repeated_overload_reads_error`, `…::test_a_retrying_request_reads_running_until_esc` |
| STA-05 | phase 3 | `…::test_background_agent_that_keeps_working_relights_running` |
| STA-06 | phase 1 (STA-K3), phase 2 | `…::test_known_defect_compact_reads_running_half_a_minute_after_it_ended`, `…::test_auto_compaction_mid_turn_keeps_running` |
| STA-11 | E3 (STA-K1) | `…::test_known_defect_a_killed_claude_keeps_reading_running` |
| STA-12 | phases 1–2, E1 (STA-K2) | `…::test_esc_mid_stream_reads_waiting_and_holds_the_runner`, `…::test_esc_during_a_tool_reads_waiting`, `…::test_known_defect_esc_before_any_output_reads_running_for_half_a_minute` |
| STA-13 | phases 1–2 (phase 2: STA-K4) | `…::test_background_bash_completion_starts_a_turn_nobody_typed`, `…::test_known_defect_a_blocking_stop_hook_reads_waiting_while_claude_continues` |
| STA-14 | phases 1–4, E2 | `…::test_max_tokens_continuation_stays_running`, `…::test_refusal_retried_once_then_answered_never_reads_error`, `…::test_refusal_twice_reads_error`, `…::test_prompt_too_long_compacts_and_continues`, `…::test_prompt_too_long_that_compaction_cannot_fix_reads_error` |
| STA-15 | phase 1 | `…::test_model_switch_dialog_changes_no_state` |
| STA-16 | phase 1 | `…::test_state_follows_with_every_hook_disabled` |
| ROB-11 | phases 1–2 (phase 2 today: ROB-K1) | `test_robustness.py::test_known_defect_a_reader_holding_sessions_json_loses_the_save` |
| CLI-02 | CLI-K2's fix (`needs_fix`): the release CLI beside an unpackaged install, from a plain shell, reads the install's own profile | `test_cli.py::test_a_plain_shell_reads_the_installs_own_profile` |

Not automated yet in these areas: MGR-05 phase 5
(folder scope; phase 2 waits for a build with A11Y-K1's fix — 0.8.0 shows the tab's selection pill to no reader), MGR-06, SP-03, SP-07 (hundreds of sessions), SP-04
phase 3–4, SP-05 E1, E4–E8, SP-06 phases 1–2 (splitter drags), TCM-01 phase 4 (an adopted shell claude),
TAB-01 E1–E3, E5, E6 (the focused tab — MGR-03 phase 3 —,
a turn cut off, an adopted session, a delivery in flight, the strip's place), TAB-02 E2, TAB-03 E2–E4 and E6, TAB-04
phase 3 (MRU mode), TAB-05–TAB-09, STA-02's 20-prompt sample, STA-03 E1, E2, E4, STA-04 E3, E5, STA-05 phases 1–2
(2.1.291 runs every Agent call in the background), STA-06 E1–E3, STA-07–STA-10 (a cut-off turn, one conversation in two
tabs, large transcripts, `/clear`), STA-11 phases 1–3, STA-12 phase 3, E2, E3, STA-13 E2–E4 (scheduled tasks and
teammates are not mocked yet), STA-14 E1, E4, STA-15 E1–E2, STA-16 E1–E2, the edge cases not listed above.

## Adding a flow

1. Find the area's file (or start a new `NN-<area>.md` with its code, a header block and a lifecycle model).
2. Give the flow the next number, a promise a user would recognize, the release that shipped it, a priority, its needs
   and its spec.
3. Write phases a person could follow at the keyboard, each with what to expect and where to check it; prefer an
   oracle the app does not control.
4. List the edge cases: the other paths through the lifecycle, the boundaries, the failures, a restart, a second
   window, Hebrew, a huge input.
5. Add it to the table above and to the release's line under "What each release brought".
