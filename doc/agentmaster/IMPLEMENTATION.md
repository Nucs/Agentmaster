# Agentmaster — Implementation Plan

**Agentmaster** is a fork of Windows Terminal (`microsoft/terminal`, MIT) that adds
first-class management of multiple **Claude Code** sessions across many working
directories — with full user interaction *and* full programmatic control.

- Upstream: forked at `v1.24.2372` (`main`), local work branch `agentmaster`.
- Build entry: **`OpenConsole.slnx`** (new XML solution format).
- License: MIT — keep the upstream copyright/`NOTICE`. Our additions are marked
  `Agentmaster` and grouped under `doc/agentmaster/` and `src/cascadia/TerminalApp/AgentMaster/`.

## Converged design

- **Design A — Native Graft.** The Claude-management "brain" lives *inside* the
  forked app in C++/WinRT. One process, native fidelity, deepest control.
- **Manager tab = C1 "Linked Lenses".** A pinned, leftmost, non-closable tab
  (tab 0, open by default) split into three synced regions:
  - **Triage Board** (top) — sessions as cards in state columns
    (Running · Waiting-for-you · Needs-approval · Error).
  - **Explorer Tree** (bottom-left) — `M` working directories → their `N` sessions.
  - **Flight Plan** (bottom-right) — per-session prompt queue + Autopilot.
  - Selection is bidirectional: pick a card ⇄ tree node; pick a dir ⇒ filter the board.
- **Flight Plan / Autopilot.** A per-session queue of prompts. On **turn-complete**
  the scheduler auto-sends the next prompt. See "Correctness rules".

## Integration points (grounded in the 1.24 codebase)

WT 1.24 uses a pluggable **pane-content** model. A `Tab` hosts panes; each pane's
content implements `IPaneContent`. Prior art: `ScratchpadContent`, `SettingsPaneContent`,
`SnippetsPaneContent`.

| Concern | Location |
| --- | --- |
| Content interface | `src/cascadia/TerminalApp/IPaneContent.idl` |
| Simplest example to mirror | `src/cascadia/TerminalApp/ScratchpadContent.{h,cpp}` |
| Content dispatch by type string | `TerminalPage::_MakePane` — `TerminalPage.cpp:3815` (add `"agentManager"`) |
| Create tab at a position | `TerminalPage::_CreateNewTabFromPane(pane, insertPosition)` — `TabManagement.cpp:215` |
| Track a special tab (pattern) | `_settingsTab = _CreateNewTabFromPane(...)` — `TerminalPage.cpp:4559` |
| Project that compiles content classes | `src/cascadia/TerminalApp/TerminalAppLib.vcxproj` |
| Per-session terminal I/O | `src/cascadia/TerminalConnection/ConptyConnection.{h,cpp,idl}` |

Notes:
- `ScratchpadContent` has **no `.idl`** — content classes are implementation-only
  (`winrt::implements<…, IPaneContent>` + `BasicPaneEvents`). `AgentManagerContent`
  follows suit: no projection/MIDL changes needed.
- Input injection uses `ITerminalConnection::WriteInput(text)` (UTF‑16). Mind the
  null-termination crash (upstream issue #17697): always send a null-terminated buffer.

## Data model

`src/cascadia/TerminalApp/AgentMaster/SessionModels.h` (plain C++ for now; the XAML
layer in M6 wraps these in observable view-models):
`SessionState`, `AutopilotMode`, `PromptGate`, `PromptStatus`, `QueuedPrompt`,
`ApprovalPolicy`, `AutopilotState`, `SessionInfo`.

## Hooks bridge

State is authoritative via **Claude Code hooks**, never screen-scraping. See
[`HOOKS.md`](./HOOKS.md). Each `claude.exe` is spawned with `CCMGR_SESSION_ID`; hooks
report `{sessionId, cwd, event}` over a local named pipe; the registry maps events to
sessions and drives the Triage Board + Autopilot.

## Correctness rules (do not regress)

1. **"Waiting-for-you" is three states.** Auto-send fires only on **turn-complete**
   (`Stop` hook). `Notification(permission)` → Approval Policy (NOT the queue). A
   `Stop` whose last message is a question → **Held** by the question-guard.
2. **Tree `Enter` = Activate** (jump to the session's live tab). It must **never** be
   forwarded to `ITerminalConnection::WriteInput` (that would submit a stray CR).
3. **Bind queue → sessionId**, never "the selected session" at send time.
4. **Idempotent sends.** Mark `Sent` atomically + persist; survive restart without replay.
5. **Backstops.** `stopOnError`, `maxAutoSends`, global pause/kill, `pauseOnHumanInput`.

## Milestones (mirror the task list)

- **M0** ✅ Fork, branch, toolchain, map integration points.
- **M1** ⏳ This doc + scaffold `AgentManagerContent` & data model (unregistered drafts).
- **M2** Baseline build of vanilla WT (`OpenConsole.slnx`), confirm runnable.
- **M3** Register `AgentManagerContent` in `TerminalAppLib.vcxproj` + `_MakePane` dispatch.
- **M4** Auto-open Manager tab pinned leftmost, non-closable, on startup.
- **M5** `SessionRegistry` + `claude.exe` spawn + hooks bridge (live state).
- **M6** C1 UI: Triage Board + Explorer Tree + Flight Plan, selection-synced.
- **M7** Autopilot scheduler (queue + Stop trigger + guards + approval policy).
- **M8** Persistence, plan templates, apply-to-many.

## Build & run

```powershell
# from K:\source\Agentmaster, in PowerShell
Import-Module .\tools\OpenConsole.psm1
Set-MsBuildDevEnvironment            # puts msbuild + SDK on PATH for this session
Invoke-OpenConsoleBuild              # builds OpenConsole.slnx (Debug|x64 by default)
# Deploy/run the dev package (CascadiaPackage) — debug via F5 in VS on CascadiaPackage,
# or deploy the built appx layout. See doc/building.md.
```

> First build is long (NuGet restore of Microsoft.UI.Xaml etc. + cppwinrt). Build the
> vanilla baseline (M2) **before** wiring in our files (M3+) so build issues are isolated.
