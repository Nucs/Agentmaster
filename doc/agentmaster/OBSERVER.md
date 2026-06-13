# Agentmaster — The Fleet Observer (polling correlation + activity)

> The **pull** half of Agentmaster's state engine: a process- and transcript-driven observer that
> detects, correlates, and enriches **every Claude session and every tab activity** — with **no
> hooks, no shim, no settings changes, and nothing the user can feel** (all reads are out-of-band).
> It is the always-correct floor beneath the lossy hook **push** (`HooksBridge`), and it subsumes the
> old per-tab Toolhelp walk + the global `projects/` discovery scan.
>
> Companions: [`DESIGN.md`](./DESIGN.md) · [`IMPLEMENTATION.md`](./IMPLEMENTATION.md) ·
> [`HOOKS.md`](./HOOKS.md) · [`TAB_OVERLAY.md`](./TAB_OVERLAY.md) · [`PERSISTENCE.md`](./PERSISTENCE.md).

---

## 1. Why this exists (the empirical case)

State is **hook-derived** (Correctness Rule #7), but hooks are lossy and require the transparent
`claude` PATH shim — which a shell `claude` function/alias can shadow, and which `reloadEnvironmentVariables`
can strip. We proved live that a hand-typed `claude` then fires **zero** hooks and is undetectable
by the push path. We then measured the out-of-band alternatives on the live machine (636 processes,
22 live `claude.exe`):

| Technique | Works? | Cost | Notes |
| --- | --- | --- | --- |
| **PEB → cwd** | ✅ | **5.9 µs** | accurate even with no OSC 9;9; **tracks `cd`** across a claude relaunch |
| **PEB → cmdline** | ✅ | ~6 µs | exposes `--resume <path>` / `--session-id` / `--model` / `--effort` / `--permission-mode` |
| **PEB → env** | ✅ | **13.8 µs** | carries **`WT_SESSION`** (exact tab id) + all `CLAUDE_CODE_*` / `CLAUDE_BG_*` |
| **cwd → newest transcript** | ✅ | filesystem | resolves the conversation `sessionId`; content validated ("testy") |
| **open `.jsonl` handle** | ❌ | — | claude opens→appends→**closes** the transcript; never held open (dead end) |
| **Toolhelp snapshot** | ✅ | **~12 ms** | the *only* expensive primitive; PEB reads are µs |
| **`GetProcessesByName`** | ✅ | ~9 ms | full enumeration |
| **cached PID → cwd** | ✅ | **4.6 µs** | steady-state; ~3000× cheaper than re-enumerating |

**Stability proof.** Across *close claude → `cd ..` → reopen claude*, the process changed
(PID 31240→27224) and cwd changed (`Desktop\temp`→`Desktop`), but **`WT_SESSION` stayed identical**
(`e9c916dd…`). `WT_SESSION` is the one identifier that never drifts.

**Conclusions that drive every decision below:**

1. **Key on `WT_SESSION`** (exact tab id; `== ITerminalConnection::SessionId()`), not PPID ancestry
   (reuse/transient) and not the stale tab cwd.
2. **Enumeration is the only cost (~10–12 ms); PEB reads are µs.** Enumerate on a slow heartbeat /
   on change; cache PIDs; steady state is µs.
3. **Read claude's *process* cwd, not the shell's** — PowerShell never syncs its process cwd with
   `Set-Location`, but spawns claude with the right cwd.
4. **Ownership needs a positive stamp** — the real Windows Terminal also sets `WT_SESSION`, so add
   **`AM_SESSION`** to tell *our* claude from an external one.

---

## 2. Goals & non-goals

**Goals**
- Detect + correlate every Claude session in one of our tabs, hooked or not, within ≤ ~3 s.
- Classify **every** tab's foreground activity (Powershell · Cmd · ClaudeCode · Codex · Other) and
  emit **enriched events** with all info extractable at observation time.
- Distinguish **our** claudes (`RunningApp::Agentmaster`) from external ones
  (`RunningApp::WindowsTerminal`).
- Feed the **one** `SessionRegistry`; change nothing downstream architecturally.
- Stay invisible (no stdin writes to any shell) and cheap (µs steady-state).

**Non-goals**
- No screen scraping (Rule #7). State comes from the transcript + process facts only.
- No *driving* of Codex (yet). A Codex session is now **managed** — observe (C1) + state (C2) + the
  launch / restore / window-restore / adopt lifecycle (§11f / §19-Q3), all read out-of-band from its
  rollout (**zero `~/.codex` writes**) — but it is never **driven**: no stdin injector / Autopilot /
  Send-now (that is C4). Launch/restore/adopt spawn or `codex resume` a real codex; the **observer
  itself** still never registers or binds a codex (a managed codex is registered by the launch path,
  reconciled by `_ReconcileManagedCodex` — never `ObserveClaude`).
- No new persisted state — the observer's tables are runtime-only.
- No injection by the observer; binding stays on the UI lane (`_BindClaudeSessionToTab`).

---

## 3. Architecture

Three lanes. The two **engine** lanes are plain C++/Win32 (off the UI thread, unit-testable); the
**UI** lane is the only one allowed to touch WinRT, marshaled through the `DispatcherQueue`.

```
 ┌─ M-lane · SessionScanner (EXISTING) ───────────── fast, adaptive (300ms/1.5s) ─┐
 │   per known LIVE session:  ② liveness(cached PID)   ③ transcript tail → state  │
 └─────────────────────────────────────────┬──────────────────────────────────────┘
                                            │ registry deltas (OnHookEvent / Update)
 ┌─ S-lane · ProcessObserver (NEW) ─────────┼───────── slow heartbeat ~2s + on-event ┐
 │   ONE Toolhelp snapshot + PEB reads:                                             │
 │     ① claude census → ClaudeProcessFacts → RunningApp (AM_SESSION/WT_SESSION)    │
 │     classify every roster tab's shell tree → TabActivity                        │
 │     resolve sessionId (cwd → newest .jsonl, tie-break by start≈ctime)           │
 │   feeds registry.ObserveClaude(...) ; publishes the two tables                  │
 └──────────────┬───────────────────────────────────────▲───────────────────────────┘
   CorrelationTable + TabActivityTable ▼ (mutex snap)    │ TabRoster ↑ (mutex snap)
 ┌─ UI lane · TerminalPage probe (per window) ── DispatcherQueue, ONLY WinRT lane ──┐
 │   build roster {WT_SESSION, shellPID, bound}  → PublishRoster                    │
 │   read tables → _BindClaudeSessionToTab + activity badge + sweep dead            │
 └───────────────────────────────────────────────────────────────────────────────────┘
                                            ▼
                              ┌──────────────────────────┐
   PUSH (HooksBridge) ──────► │      SessionRegistry      │ ──► Scheduler · Manager · Overlay · Persist
                              │  one source of truth      │
                              └──────────────────────────┘
```

**Ownership.** All three pieces hang off the process-wide `SharedEngine` (M9). `SessionScanner`
already exists there; `ProcessObserver` is added next to it. Each window registers its UI-lane probe
+ roster channel **by token** and detaches on teardown (Rule #10).

---

## 4. Concepts & taxonomy

```cpp
// AgentMaster/Activity.h   (NEW — plain C++, no WinRT)
namespace Agentmaster
{
    enum class TabActivity { Unknown, Powershell, Cmd, ClaudeCode, Codex, Other };
    enum class RunningApp  { Unknown, Agentmaster, WindowsTerminal, Other };
}
```

- **`TabActivity`** — what a tab is *doing right now*, derived from the deepest meaningful descendant
  of the tab's shell: `claude.exe`→ClaudeCode, `codex.exe`→Codex (enriched from its rollout + a C2
  3-state floor; lifecycle-managed but never DRIVEN — C4; §11f), else the shell
  image → Powershell / Cmd, else Other.
- **`RunningApp`** — who *hosts* a claude: `Agentmaster` (our `AM_SESSION`), `WindowsTerminal`
  (`WT_SESSION` but not our `AM_SESSION`), `Other` (neither). External ones are acknowledged and shown
  observe-only; they never get an injector.

---

## 5. Data models

### 5a. Engine-side (plain C++, `AgentMaster/Activity.h`)

```cpp
// Raw facts read out-of-band from one claude.exe (S-lane, ~15 µs/process).
struct ClaudeProcessFacts
{
    uint32_t     pid{};
    uint32_t     parentPid{};
    int64_t      startUnixMs{};       // GetProcessTimes(creation)
    std::wstring wtSession;           // env WT_SESSION   (exact tab / ConPTY id)
    std::wstring amSession;           // env AM_SESSION   (our ownership stamp; empty if external)
    std::wstring cwd;                 // PEB CurrentDirectory (tracks cd across relaunch)
    std::wstring commandline;         // PEB CommandLine
    // parsed from commandline + CLAUDE_* env:
    std::wstring model;               // --model / CLAUDE_CODE_*
    std::wstring effort;              // --effort / CLAUDE_CODE_EFFORT_LEVEL
    std::wstring permissionMode;      // --permission-mode
    std::wstring resumeTarget;        // --resume <path>
    std::wstring sessionIdArg;        // --session-id <id>  (when explicitly passed)
    bool         background{};        // CLAUDE_CODE_SESSION_KIND=bg / CLAUDE_BG_* / "daemon run"
    std::wstring sessionName;         // CLAUDE_CODE_SESSION_NAME (bg jobs)
    uint16_t     subsystem{};         // PE subsystem: 3 = console (the CLI), 2 = GUI (the desktop
                                      //   Electron app, ALSO Claude.exe — excluded), 0 = unknown
    bool         alive{ true };
    RunningApp   runningApp{ RunningApp::Unknown };
};

// One correlated tab→claude→session row (CorrelationTable; published to the UI lane).
struct CorrelationRow
{
    std::wstring wtSession;           // key
    uint32_t     claudePid{};
    std::wstring cwd;
    std::wstring sessionId;           // resolved from transcript; EMPTY until first prompt
    RunningApp   runningApp{ RunningApp::Unknown };
    bool         alive{ true };
    int64_t      observedUnixMs{};
};

// Per-tab activity for EVERY tab, claude or not (TabActivityTable; published to the UI lane).
struct TabActivityRow
{
    std::wstring wtSession;           // key
    uint32_t     shellPid{};
    TabActivity  activity{ TabActivity::Unknown };
    std::wstring image;               // foreground / shell image leaf ("pwsh.exe", "claude.exe", …)
    std::wstring cwd;                 // for Powershell / Cmd / ClaudeCode
    bool         busy{};              // shell has a running child (a command in progress)
    std::wstring sessionId;           // when activity==ClaudeCode (mirror of the CorrelationRow)
    CodexState   codexState{};        // when activity==Codex: the C2 rollout-tail state (Idle/Running/Waiting)
    int64_t      observedUnixMs{};
};
// Codex also adds `enum class CodexState { Unknown, Idle, Running, Waiting }` + `CodexProcessFacts`, and
// `ExternalClaudeRow` gained `kind` (Claude default), `sandbox`, `approvalMode`, `rolloutPath`, `codexState`.

// UI → engine: this window's tabs, published each probe tick.
struct TabRosterEntry
{
    std::wstring wtSession;           // ITerminalConnection::SessionId() (GuidToPlainString, lower)
    uint32_t     shellPid{};          // GetProcessId( (HANDLE)ConptyConnection::RootProcessHandle() )
    bool         bound{};             // already has an injector in this window
};
```

### 5b. The registry-feed delta (`AgentMaster/Activity.h`)

```cpp
// What the S-lane upserts into the registry for an OUR claude (mirrors HookMessage's role).
struct ObservedClaude
{
    std::wstring sessionId;           // key (may be empty until the transcript exists — see §11d)
    std::wstring tabToken;            // == wtSession (SessionInfo::tabToken)
    std::wstring amSession;
    std::wstring cwd;
    uint32_t     pid{};
    RunningApp   runningApp{ RunningApp::Unknown };
    bool         background{};
    std::wstring model, effort, permissionMode, sessionName;
    int64_t      observedUnixMs{};
};
```

### 5c. `SessionInfo` enrichment (`AgentMaster/SessionModels.h`)

All **transient** (NOT serialized — PIDs/`WT_SESSION`/`AM_SESSION` are per-run; `Persistence.cpp`
must not write them):

```cpp
// --- live enrichment (runtime only; provenance + process facts) ---
uint32_t     pid{};                   // claude.exe PID (0 = unknown / not running here)
std::wstring liveCwd;                 // PEB cwd (authoritative live dir)
std::wstring model, effort, permissionMode, sessionName;
bool         background{};
RunningApp   runningApp{ RunningApp::Unknown };
std::wstring amSession;               // owning Agentmaster instance
bool         hookWired{};             // have we received ANY hook for this id this run?
int64_t      lastHookUnixMs{};        // provenance: last authoritative push
int64_t      lastObservedUnixMs{};    // last pull observation
```

`SessionInfo::tabToken` already exists (added earlier) and **is** `WT_SESSION`.

**Managed-Codex fields (PERSISTED — durable session identity, NOT transient enrichment; §11f).** The
managed lifecycle added `SessionInfo::kind` (`AgentKind`, Claude default) + `SessionInfo::codexSessionId`
(the rollout uuid = the `codex resume` target, filled by `_ReconcileManagedCodex` on first prompt). Unlike
the fields above, these **are** serialized by `Persistence.cpp` (a Codex record must survive archive →
restore). `TabKind` likewise gained `Codex` for the `WindowRecord` tab refs. The transient enrichment
above is shared by both agent kinds.

---

## 6. Process-inspection primitives — `AgentMaster/ProcessInspect.{h,cpp}` (NEW)

Promote the PEB/Toolhelp helpers currently living anonymously in `ClaudeSpawn.cpp`
(`ReadProcessCwd`, `FindClaudeDescendantPid`, `NameIsClaude`) into a dedicated, reusable, **testable**
TU and extend them. Centralize the x64 PEB offsets in **one** place (`0x20` ProcessParameters, `0x38`
CurrentDirectory, `0x70` CommandLine, `0x80` Environment, `0x3F0` EnvironmentSize) with a one-line
provenance comment ("same technique as WT's `ConptyConnection::_commandlineFromProcess`").

```cpp
namespace Agentmaster
{
    struct ProcEntry { uint32_t pid; uint32_t ppid; std::wstring image; }; // one Toolhelp row

    // ONE snapshot of all processes (the only ~10 ms call). Reused for census + every tab's tree.
    std::vector<ProcEntry> SnapshotProcesses();

    // PEB reads (x64; empty/false on failure — same-user, non-elevated succeeds).
    std::wstring ReadProcessCwd(uint32_t pid);                       // already exists — move here
    std::wstring ReadProcessCommandLine(uint32_t pid);
    std::unordered_map<std::wstring, std::wstring> ReadProcessEnv(uint32_t pid); // NAME->VALUE
    int64_t      ProcessStartUnixMs(uint32_t pid);                  // GetProcessTimes(creation)
    bool         ProcessAlive(uint32_t pid);                        // OpenProcess + GetExitCodeProcess

    // Tree helpers over a snapshot (no extra syscalls). Descendant-OR-SELF: the root itself
    // may BE the image — a Manager-launched claude is the ConPTY root (no shell in between).
    uint32_t                 FindDescendantByImage(const std::vector<ProcEntry>&, uint32_t root,
                                                   std::wstring_view imageLeaf); // e.g. L"claude.exe"
    std::vector<uint32_t>    ChildrenOf(const std::vector<ProcEntry>&, uint32_t parent);

    // Fill a ClaudeProcessFacts for one claude pid (cmdline + env parse). Pure-ish: takes the pid.
    ClaudeProcessFacts ReadClaudeFacts(uint32_t pid);
}
```

`ClaudeSpawn::ClaudeCwdForShell` becomes a thin wrapper:
`FindDescendantByImage(snap, shellPid, L"claude.exe")` → `ReadProcessCwd`. Keep `ClaudeProjectsDir`
and `ResolveClaudeTranscriptPath` where they are.

**`ReadClaudeFacts` parsing rules** (string matching on `commandline` + `env`):
- `model` ← `--model X` else env `ANTHROPIC_MODEL`/`CLAUDE_CODE_*MODEL*` if present.
- `effort` ← `--effort X` else env `CLAUDE_CODE_EFFORT_LEVEL`.
- `permissionMode` ← `--permission-mode X`.
- `resumeTarget` ← `--resume <path>`; `sessionIdArg` ← `--session-id <id>`.
- `background` ← env `CLAUDE_CODE_SESSION_KIND==bg` OR any `CLAUDE_BG_*` present OR commandline
  contains `daemon run` / `--bg-pty-host`.
- `sessionName` ← env `CLAUDE_CODE_SESSION_NAME`.
- `wtSession`/`amSession` ← env `WT_SESSION` / `AM_SESSION`.

---

## 7. `AM_SESSION` — the ownership stamp (`Engine.cpp`)

At engine init (the same block that exports `CCMGR_HOOK_PIPE` and prepends the PATH shim), mint a
per-process GUID and export it so every ConPTY child inherits it (guaranteed by our forced
`reloadEnvironmentVariables=off`):

```cpp
// once, in SharedEngine init, alongside SetEnvironmentVariableW(L"CCMGR_HOOK_PIPE", …)
e->amSession = NewGuidPlainLower();                  // CoCreateGuid -> "xxxxxxxx-...."
::SetEnvironmentVariableW(L"AM_SESSION", e->amSession.c_str());
AppendStateLog(L"hooks.log", L"[engine] AM_SESSION " + e->amSession + L"\n");
```

- Store `e->amSession` on the `Engine` struct; the `ProcessObserver` reads it to classify
  `RunningApp`.
- **Optional richer form:** `AM_SESSION = <instanceGuid>` (one per process). If we later attribute
  the owning *window*, extend to `<instanceGuid>:<windowId>` — but v1 is process-scoped.
- `Launch`'s direct `CreateProcessW("claude …")` already inherits the process env, so launched
  sessions are also stamped (and trivially classified `Agentmaster`).

Classification (S-lane, per claude facts):
```
amSession == e->amSession                  -> RunningApp::Agentmaster      (ours; correlate + bind)
amSession.empty() && !wtSession.empty()    -> RunningApp::WindowsTerminal  (external; acknowledge)
else                                       -> RunningApp::Other            (bare console; ignore)
```

---

## 8. `ProcessObserver` — the S-lane (`AgentMaster/ProcessObserver.{h,cpp}`, NEW)

A single background worker (mirror `SessionScanner`'s thread/condvar shape) that owns the two tables
and the roster.

```cpp
namespace Agentmaster
{
    class ProcessObserver
    {
    public:
        ProcessObserver(std::shared_ptr<SessionRegistry> registry, std::wstring amSession);
        ~ProcessObserver();                          // Stop()

        void Start();
        void Stop() noexcept;
        void Wake() noexcept;                        // event-driven survey (roster change / PID death)

        // UI lane -> observer (per window, each probe tick). Thread-safe; replaces that window's set.
        void PublishRoster(const std::wstring& windowId, std::vector<TabRosterEntry> roster);
        void UnpublishWindow(const std::wstring& windowId);   // on ~TerminalPage

        // observer -> UI lane (snapshots; copy under lock).
        std::vector<CorrelationRow> Correlation() const;
        std::vector<TabActivityRow> Activity() const;

    private:
        void _worker() noexcept;                     // heartbeat ~2s + Wake()
        void _surveyOnce();                          // the whole pass (below)
        // ... registry, amSession, mutex-guarded _rosterByWindow, _correlation, _activity,
        //     _knownPids cache, _lastSurveyMs, condvar/_woken/_running like SessionScanner.
    };
}
```

### 8a. `_surveyOnce()` algorithm

```
snap = SnapshotProcesses()                                  // ~10 ms, ONE call
claudes = snap.filter(image == "claude.exe")                // case-insensitive -> ALSO the desktop app
factsByPid = { pid: ReadClaudeFacts(pid) for pid in claudes }   // ~15 µs each
# Drop the Claude DESKTOP app: an Electron GUI binary ALSO named Claude.exe, whose main + renderer/
# gpu/utility/crashpad children all run with cwd C:\WINDOWS\system32 and would otherwise surface as
# bogus "system32 sessions" in the External group. The CLI is a console app, the desktop app a GUI
# app -> tell them apart by PE subsystem (IsClaudeDesktopGuiApp). 0 (denied/elevated/WOW64) stays a
# candidate CLI so a real elevated session is never hidden.
factsByPid = { pid: f for pid, f in factsByPid if not IsClaudeDesktopGuiApp(f) }
for f in factsByPid: f.runningApp = classify(f.amSession, f.wtSession, amSession)

roster = merge(_rosterByWindow)                             // all windows' tabs
corr = {}; act = {}
for tab in roster:
    # activity: the tab's shell itself or its deepest meaningful descendant (descendant-or-
    # self: a Manager-launched claude IS the ConPTY root — shellPid == claude pid)
    cpid = FindDescendantByImage(snap, tab.shellPid, "claude.exe")
    if cpid:
        f = factsByPid[cpid]
        sid = ResolveSessionId(f.cwd, f.startUnixMs)        # §8b
        corr[tab.wtSession] = CorrelationRow{ tab.wtSession, cpid, f.cwd, sid, f.runningApp, true }
        act[tab.wtSession]  = TabActivityRow{ ClaudeCode, "claude.exe", f.cwd, sid, ... }
        if f.runningApp == Agentmaster:
            registry.ObserveClaude(ObservedClaude{ sid, tab.wtSession, f.amSession, f.cwd,
                                                   cpid, f.runningApp, f.background,
                                                   f.model, f.effort, f.permissionMode, f.sessionName })
    elif xpid = FindDescendantByImage(snap, tab.shellPid, "codex.exe"):
        # Codex: activity badge enriched with model + C2 state; the FULL External row is emitted by the
        # codex census below (§11f). codexInfo(xpid) is resolved+cached per pid (+ a rollout-tail state).
        act[tab.wtSession] = TabActivityRow{ Codex, "codex.exe", model = codexInfo(xpid).model, codexState }
    else:
        shell = imageOf(snap, tab.shellPid)
        act[tab.wtSession] = TabActivityRow{ classifyShell(shell), shell,
                                             ReadProcessCwd(tab.shellPid),
                                             busy = HasNonShellChild(snap, tab.shellPid) }

# external census: claudes whose wtSession is NOT in our roster but RunningApp==WindowsTerminal
#   -> optionally surface as "external N" (no registry session, observe-only)
# codex census (§11f): EVERY codex.exe -> an ExternalClaudeRow{kind=Codex}, enriched from its rollout
#   (codexInfo + C2 state), same orphan-skip + host-label as claude. The OBSERVER never feeds a codex to
#   ObserveClaude; a MANAGED codex (registered by the launch path) is deduped out via managedCodexTokens.

publish(corr, act)                                          # under the table mutex
_knownPids = { cpid for tabs that correlated }              # for the M-lane / liveness cross-check
```

### 8b. `ResolveSessionId(cwd, startUnixMs)`

```
dir = ClaudeProjectsDir() + "/" + encode(cwd)              // C:\..\Desktop -> C--..-Desktop
candidates = glob(dir + "/*.jsonl")
if empty: return ""                                        // never-prompted yet (see §11d)
# newest by mtime is the active session; if >1 with mtime within a small window AND multiple
# claudes share this cwd, tie-break by |file.ctime - startUnixMs| minimal.
return stem(pick(candidates, startUnixMs))
```

`encode(cwd)`: replace every `:` and `\`/`/` with `-` (matches Claude's project-dir naming, e.g.
`C--Users-ELI-Desktop`). If encoding ever drifts, fall back to scanning each project dir's head line
for a matching `"cwd"` (slower; cache id→cwd).

### 8c. Trigger & cadence

| Trigger | Action |
| --- | --- |
| heartbeat (default **2000 ms**) | full `_surveyOnce` (catches claude *birth* in a stable tab) |
| `Wake()` from UI (roster changed) | immediate survey |
| `Wake()` from M-lane (a known PID died) | immediate survey (reclassify the tab) |

Cost: one ~10 ms snapshot per 2 s ≈ **0.5 % CPU**, off the UI thread. *Optimization (later):* skip the
Toolhelp snapshot when the roster is byte-identical to last tick **and** every correlated PID is still
alive **and** it isn't the slow heartbeat — gets steady-state to µs (benchmark §1). Ship the simple
unconditional heartbeat first.

---

## 9. Registry integration — `SessionRegistry::ObserveClaude` (provenance-aware upsert)

```cpp
// SessionRegistry.{h,cpp}
void SessionRegistry::ObserveClaude(const ObservedClaude& o);
```

```
guard lock
it = _sessions.find(o.sessionId)
if not found:
    s = new SessionInfo{ id=o.sessionId, workingDir=o.cwd, external=true, live=true }
    _sessions[o.sessionId] = s
    fireAdoption = true
s = _sessions[o.sessionId]
# ENRICHMENT (always safe — facts, not state):
s.tabToken     = o.tabToken
s.amSession    = o.amSession
s.runningApp   = o.runningApp
s.pid          = o.pid
s.liveCwd      = o.cwd
s.model = o.model; s.effort = o.effort; s.permissionMode = o.permissionMode
s.background = o.background; s.sessionName = o.sessionName
s.live = true
s.lastObservedUnixMs = o.observedUnixMs
if s.workingDir.empty(): s.workingDir = o.cwd
unlock
if fireAdoption: fireAdoptionHandlers(s)     // OUTSIDE the lock (existing pattern)
fireObservers(s, HookEvent::Unknown)         // UI refresh
```

**Provenance rule (push wins for state):** `ObserveClaude` never sets `SessionState`. State stays
owned by `OnHookEvent` (real hooks) and the M-lane's transcript-tail → synthesized `Stop`
(`NextSessionState`) — exactly as today. `hookWired`/`lastHookUnixMs` are set in `OnHookEvent`; the
observer reads them only to *log* provenance, never to override. This keeps Rule #1/#7 intact and
makes hooked and un-hooked sessions converge on the same record.

**Idempotency:** keyed by `sessionId`; re-observing is a cheap merge. Back-filled prompts continue
to flow through the existing idempotent `NoteExternalPrompt` (M-lane), not `ObserveClaude`.

---

## 10. UI lane — `TerminalPage` (the only WinRT thread)

Retire `_DiscoverClaudeTabsByCwd` (the per-tab Toolhelp walk) and the `_ReconcileClaudeTabs`
tabToken poll's *discovery* duties. The existing scanner-ticked probe now does **publish → read →
bind**:

```cpp
// new members
std::shared_ptr<::Agentmaster::ProcessObserver> _observer;     // from SharedEngine
winrt::fire_and_forget _ObserverProbe();                       // replaces _DiscoverClaudeTabsByCwd
```

```
_ObserverProbe():                                  // co_await resume_foreground(Dispatcher())
    roster = []
    for tab in _tabs (skip _managerTab):
        conn = firstTerminalConnection(tab)
        if !conn: continue
        wt  = lower(GuidToPlainString(conn.SessionId()))
        sh  = GetProcessId( (HANDLE)conn.as<ConptyConnection>().RootProcessHandle() )
        roster.push({ wt, sh, bound = tabIsBound(tab) })
    _observer->PublishRoster(_windowId, roster)         // ↑ + Wake() if changed

    corr = _observer->Correlation()                     // ↓ snapshot
    act  = _observer->Activity()
    for tab, wt in rosterTabs:
        c = corr[wt]
        if c && c.runningApp==Agentmaster && !c.sessionId.empty() && !bound(tab):
            _BindClaudeSessionToTab(tab, conn, c.sessionId, c.cwd)   // injector+overlay+title+color
        _ApplyTabActivity(tab, act[wt])                 // badge / overlay mode (§11b)
    _SweepClaudeLiveness()                              // existing archive-on-dead seam
```

- **Roster build** is the only added per-tab WinRT work (`SessionId()` + `RootProcessHandle()` —
  both µs). `tabIsBound` = `_claudeTabs` has an entry whose weak tab == this tab.
- `_BindClaudeSessionToTab` is **unchanged** — the observer only changes the *trigger* and the *key*.
- Register/unregister with the observer in `_InitAgentmasterEngine` / `~TerminalPage` by token
  (Rule #10): `_observer = engine.observer; … _observer->UnpublishWindow(_windowId)`.

---

## 11. Downstream consumers

### 11a. `Scheduler` / Autopilot — no change
Reacts to registry state on the advance seam; the observer feeding the registry drives it. Free
refinements: `s.background==true` or no injector ⇒ never auto-driven (nothing to inject into);
`runningApp==WindowsTerminal` never gets an injector, so it's inert to Autopilot.

### 11b. `AgentTabOverlay` — richer badge + activity
Already an id-filtered registry observer (`AddObserver`). Additions:
- Show `model`/`effort`/`kind` in the expanded panel; `runningApp==WindowsTerminal` → `observe`
  (disabled controls, tooltip "external — Windows Terminal").
- `_ApplyTabActivity` lets the host tab render a dim activity chip even for non-claude tabs
  (`pwsh` / `cmd` / `codex`) or keep the overlay hidden when `activity != ClaudeCode` (per
  `AppSettings.showTabOverlay`).

### 11c. `AgentManagerContent` (Manager tab) — richer snapshot
Already snapshot-driven from the registry. New fields become card/row adornments (`model · effort ·
kind`); add an optional **"External (N)"** group for `runningApp==WindowsTerminal` claudes
(observe-only, no Flight Plan). No structural change.

**Host label (release/dev/real-WT distinction).** An external row's host must NOT be named by the
parent process's image leaf — our fork's exe is literally `WindowsTerminal.exe`, so an external claude
in another **Agentmaster** instance read "WindowsTerminal" (indistinguishable from real WT, and from a
dead-host orphan that read "ext"). `ResolveExternalHostLabel(snap, claudePid, amSessionPresent)` instead
walks UP to the hosting terminal (`FindTerminalHostPid` — nearest `WindowsTerminal.exe`/`wt.exe`
ancestor) and names it by **package family** (`ReadProcessPackageFamily`, authoritative for loose-
registered AND MSIX): `Agentmaster_*` → **Agentmaster**, `AgentmasterDev_*` → **Agentmaster Dev**,
`Microsoft.WindowsTerminal*` → **Windows Terminal**; image path is the unpackaged fallback. No live
terminal ancestor + our `AM_SESSION` stamp ⇒ **Agentmaster** (an orphan whose host exited); else the
nearest shell leaf (`cmd`/`pwsh`). Published as `ExternalClaudeRow.hostLabel`; the board ("via …") and
the EXTERNAL tree pill render it.

### 11d. The "session before transcript" case
A correlated claude with **no transcript yet** (never prompted) has `sessionId==""`. Handle by:
- `CorrelationRow.sessionId` empty ⇒ the UI shows the tab as **ClaudeCode (starting…)** via the
  activity badge, but does **not** bind a registry session yet (no id to key on).
- The S-lane's `ResolveSessionId` re-runs each survey; the moment the transcript appears (first
  prompt), `sessionId` fills, `ObserveClaude` creates the record, and the next probe binds.
- If hooks fire first (`SessionStart`), the id arrives via the push path immediately — same record.

### 11e. `Persistence` — no change
Only the durable `SessionInfo` subset persists (`Persistence.cpp` ToJson/FromJson untouched for the
new transient fields). On restart the observer re-derives everything.

### 11f. Codex (observe C1 + state C2 + the managed launch/restore/adopt lifecycle; resolves §19-Q3)

A second agent — the OpenAI **Codex CLI** (`codex.exe`) — is now a first-class **managed** entity. It
began as the Codex analog of the external census (C1, observe-only) and has since graduated to **state**
(C2) and the **full launch / restore / window-restore / adopt lifecycle** — *lifecycle + state only*:
Agentmaster launches, resumes, archives, window-restores, and adopts Codex sessions, but does **not**
yet DRIVE one (no stdin injector / Autopilot / Send-now — that is C4). Everything is read entirely
out-of-band (**zero writes to `~/.codex`**, Rule #13). The three divergences from Claude that shape it:
Codex's config home is **`CODEX_HOME`** (else `~/.codex`), not `~/.claude`; it **cannot pin a session id
at launch** (the id is auto-minted, embedded in the rollout filename — hence the two-id model below); and
model/effort/sandbox/approval usually come from `config.toml`, so they are read from the **rollout**,
not the command line.

- **Primitives (`ProcessInspect`).** `ReadCodexFacts`/`ParseCodexFacts` (PEB cwd/cmdline/env → model
  `--model`/`-m`, sandbox `--sandbox`/`-s`, approval `--ask-for-approval`/`-a`, `WT_SESSION` /
  `AM_SESSION` / `CODEX_HOME`, and an explicit `codex resume <guid>` as the authoritative id);
  `CodexRolloutUuid` (the trailing UUIDv7 of a `rollout-<ISO-ts>-<uuid>` stem); `ResolveCodexSessionIn`
  — Codex shards rollouts by **LOCAL date** (`sessions/YYYY/MM/DD/rollout-*.jsonl`) and the cwd lives
  *inside* the file, so the Claude cwd-encoded glob does NOT apply: scan the start's local day **± 1**,
  confirm each candidate's cwd by a cheap head-scan of its `session_meta` line
  (`ExtractCwdFromTranscriptHead` — cwd precedes the bulky `base_instructions`, so 4 KB suffices), then
  pick by **ctime ≈ start IDENTITY** (`PickNewestTranscript`), falling back to **newest-mtime-in-cwd**
  for a `resume`d session whose rollout predates the process; `ResolveCodexRolloutPathIn` (find a rollout
  by a known uuid — the explicit-resume path); `ParseCodexRolloutText`/`ReadCodexRolloutInfo` (the
  `RolloutLine`/`payload` JSONL: `session_meta`→cwd, the FIRST `turn_context`→model/effort/sandbox/
  approval, `event_msg`/`user_message`→the human prompts + title — the AGENTS.md / context blobs are
  `response_item` user messages and are skipped). All pure parts are unit-tested.
- **S-lane (`ProcessObserver`).** A Codex census parallel to the claude one: in the census EVERY
  `codex.exe` is enriched ONCE per pid (`_codexInfoByPid`, mtime re-stat'd each survey + the C2
  rollout-tail state), classified ours/external by `AM_SESSION`, with the SAME orphan-skip + host-label
  (`ResolveExternalHostLabel`) as the claude census, and emitted as an `ExternalClaudeRow{kind=Codex}`.
  Codex pids join the O7 liveness set (a codex birth/exit forces a full survey); the census log carries
  `codex=N`. The **observer never feeds a codex to `ObserveClaude`** (Rule #13) — a MANAGED codex is
  registered by the launch path and **deduped out** of this census (`managedCodexTokens` — a registry
  record with `kind==Codex && live && tabToken` set), so it shows once; the UI lane's
  `_ReconcileManagedCodex` (not the observer) folds its state onto the record.
- **UI (`AgentManagerContent`).** The External group (Triage Board + Explorer **EXTERNAL**) renders a
  teal **`codex`** pill + `model · sandbox · approval` + timing + the C2 state dot; the right-click menu
  is **kind-aware** — a Codex external offers **Adopt** (resume its rollout into a managed tab) / **Open
  New Codex Session Here** (a fresh managed codex in the cwd; both route to the Codex launch handler) +
  the agent-agnostic **Bring Window To Front**; **left-click → a read-only Flight Plan** sourced from the
  rollout. The per-tab observe badge reads `○ codex · <model> · unlinked` (an external/unbound codex); a
  MANAGED codex card/row wears the same teal pill + a 3-state dot, and its tab gets the bound overlay.
- **Data model (`Activity.h`).** New `enum AgentKind { Claude, Codex }` + `CodexProcessFacts`;
  `ExternalClaudeRow` gained `kind` (Claude default — every existing producer/consumer unchanged),
  `sandbox`, `approvalMode`, and the Codex `rolloutPath` (the date-sharded path, carried so the
  read-only plan + future actions need no re-resolve). The type name is kept for a minimal, rebase-cheap
  diff; `kind` discriminates. This is the **light "external-row" path** — see §19-Q3 for the
  external-row-vs-generalized-agent-abstraction choice the later control phases revisit.

**C2 — state via a rollout-tail PULL reconciler (DONE, commit `b91018a77`).** `ClassifyCodexLine` (pure:
an `event_msg` payload → a turn-boundary verdict) + `ReadCodexStateDelta` (a byte-cursor forward delta —
first-sight tail-seek of the last 1 MiB, a 4 MiB catch-up cap, a partial trailing line left unconsumed,
sticky on a quiet read) derive **Idle / Running / Waiting** from the rollout tail: `task_started` →
Running; `task_complete` / `turn_aborted(interrupted)` / `thread_rolled_back` → Waiting;
**last-boundary-wins** (proven over 33 live rollouts — timestamps 100 % monotonic, file-order = truth).
**NeedsApproval / Error are NOT PULL-derivable** (no approval/permission/error event exists in a
rollout), so Codex runs on a **3-state floor**. `CodexState` (`Activity.h`) lands on the board + tree
dot; still observe-only as facts, zero `~/.codex` writes.

**Managed lifecycle — launch / restore / window-restore / adopt (DONE, commits `64b2472e7` · `9922e4658`
· `51a8ca51d` · `efaa586e2`).** A managed Codex is a registry citizen via the **two-id model** (the
§19-Q3 first-class-`AgentKind` choice, NOT a parallel path): `SessionInfo.id` = OUR minted durable handle
(the registry / persistence / `_claudeTabs` key — never re-keyed, since Codex can't pin an id at launch),
`codexSessionId` = the real rollout uuid (the `codex resume` target + the transcript gate), filled by
`_ReconcileManagedCodex` on first prompt. `SessionInfo` gained `kind` (Claude default) + `codexSessionId`;
`TabKind` gained `Codex` (window-record refs + persistence (de)serialize it). Launch = the launch bar's
**Claude⇄Codex toggle** (directory-only — Codex has no typed-id resume/fork) or the EXTERNAL **Open New
Codex Session Here** → `_SpawnCodexSession` → `_LaunchCodexSession` (`BuildCodexCommandline` = `codex` /
`codex resume <uuid>`, immediate managed card, `AM_SESSION` stamp, no `CCMGR_*`); restore = the Archive
page (branch on `kind`, transcript-gated on the rollout, else fresh); window-restore = `_RestoreWindowTabs`;
adopt = `_AdoptExternalCodex` (resume an external's rollout, original left running). A managed codex is
**deduped out of the External census** (`managedCodexTokens`) and **never** fed to `ObserveClaude` (it is
registered by the launch path; `_ReconcileManagedCodex` folds its C2 state onto the record). **Lifecycle +
state ONLY** — no injector/Autopilot.

**Still deferred:** **C3** — low-latency PUSH by wiring Codex `notify` (turn-complete only) or
`~/.codex/hooks.json` (SessionStart/UserPromptSubmit/Stop/PreToolUse/PermissionRequest — payloads map
~1:1 to our wire line; the forwarder pattern is reusable) to the same bridge; **invasive** — it mutates
the user's GLOBAL Codex config (no per-session `--settings` like Claude) + hook-trust friction, so it is a
product decision. **C4** — bind a stdin injector + Autopilot (DRIVE the Codex TUI); launch is
PULL-correlated (no `--session-id` to pin), resume=`codex resume <id>`, fork=`codex fork <id>`, model
fixed on resume; typed-vs-flight echo accounting needs C3's `UserPromptSubmit`.

---

## 12. Threading & synchronization (contract)

| Data | Owner | Writers | Readers | Sync |
| --- | --- | --- | --- | --- |
| `SessionRegistry` | engine | M-lane, S-lane, HooksBridge | all | existing internal `_mtx` |
| `_rosterByWindow` | `ProcessObserver` | UI lanes (N windows) | S-lane | `_rosterMtx` |
| `_correlation`, `_activity` | `ProcessObserver` | S-lane | UI lanes | `_tableMtx` (copy-out) |
| `_knownPids` | `ProcessObserver` | S-lane | S/M-lane | `_tableMtx` |
| tab maps `_claudeTabs`/`_claudeOverlays` | `TerminalPage` | UI lane only | UI lane | UI thread confinement |

- Engine lanes **never** call WinRT. UI lane **never** blocks on a survey — it reads the last
  published snapshot (eventual consistency, ≤ one heartbeat stale).
- Snapshots are copy-under-lock (`std::vector` returns), so readers hold no lock while iterating.
- `PublishRoster` diffs against the window's last roster; on change it `Wake()`s the S-lane.
- Teardown: `Engine` `Stop()`s the observer (join thread); `~TerminalPage` `UnpublishWindow` +
  detaches its probe token before the engine shuts down.
- Async option: the two engine lanes may run as `winrt::resume_background` coroutine loops on
  `Windows::System::Threading::ThreadPool` instead of `std::thread`; keep the **plain-C++** engine on
  `std::thread` to preserve the standalone test harness.

---

## 13. Edge cases & safety

- **Same-cwd ambiguity → solved.** Two claudes in one dir have **different** `WT_SESSION` (different
  tabs) → bound to the right tabs. Their transcripts disambiguate by `start ≈ ctime` (§8b).
- **PowerShell stale cwd → solved** by reading the **claude** process cwd, not the shell's.
- **Elevated / cross-integrity claude:** `OpenProcess(PROCESS_VM_READ)` fails → facts empty → mark
  observe-only (`runningApp=Unknown`, no injector) rather than mis-bind. Log once.
- **WOW64 / 32-bit target:** offsets differ; `claude.exe` is x64 so this is moot — but guard
  `IsWow64Process` and skip (don't misread) if it ever isn't.
- **PID reuse:** never key on PID; PID is a *cache* validated by `ProcessAlive` + re-derived under
  the stable `WT_SESSION`. A dead PID under a still-present `WT_SESSION` ⇒ re-survey finds the new
  one (the close→cd→reopen case we proved).
- **Foreign / nested terminals:** `AM_SESSION` present but `WT_SESSION` not in our roster ⇒
  observe-only. Real-WT claude (no `AM_SESSION`) ⇒ `WindowsTerminal`, never bound.
- **Codex (observe C1 + state C2 + managed lifecycle):** ENRICHED out-of-band from its date-sharded
  rollout (`<CODEX_HOME|~/.codex>/sessions/YYYY/MM/DD/rollout-<ISO-ts>-<uuid>.jsonl`) —
  model/effort/sandbox/approval/title/timing + a 3-state floor (Running/Waiting/Idle — no
  approval/error event in a rollout), classified ours/external by `AM_SESSION` like claude. The
  **observer** never registers/binds a codex (the External census stays observe-only); a MANAGED codex
  (launched/restored/adopted by us) IS a registry citizen via the launch path + the two-id model, but is
  still **never driven** (no injector/Autopilot — that is C4). §11f / §19-Q3.
- **Debounce / caps:** survey ≤ 1 Toolhelp/known-interval; hard cap; abortable on `Stop`; the
  observer **never** writes to any shell stdin (invisibility invariant).

---

## 14. Phased rollout (milestones + acceptance)

Each phase ends with: **lib compile-check (no lock) → locked full deploy → live-verify in
`hooks.log`**. (Hold the `build-launch` mutex for any full build/launch/deploy.)

| Phase | Deliverable | Acceptance (live) |
| --- | --- | --- |
| **O1 — primitives** | `ProcessInspect.{h,cpp}` (snapshot, PEB cwd/cmdline/env/start, tree, `ReadClaudeFacts`); standalone tests | unit harness reads our own + a sample claude's cwd/env/cmdline correctly |
| **O2 — `AM_SESSION`** | mint + export in `Engine.cpp`; `[engine] AM_SESSION …` log | a spawned + a hand-typed claude both carry our `AM_SESSION`; real-WT claude does not |
| **O3 — `ObserveClaude`** | registry seam + `SessionInfo` enrichment + provenance | unit: upsert/merge idempotent; state never overwritten by an observation |
| **O4 — `ProcessObserver` S-lane** | census + classify + tables + `PublishRoster`/snapshots; wired into `SharedEngine` | `hooks.log` shows census + `RunningApp` classification each heartbeat |
| **O5 — UI lane swap** | `_ObserverProbe` replaces `_DiscoverClaudeTabsByCwd`; bind via table | hand-typed `claude` (pwsh, after `cd`) → board card + overlay ≤ 3 s; **the `Desktop\temp → cd → testy` scenario binds** |
| **O6 — activity + events** | full `TabActivity` taxonomy; enriched `[activity]` events; overlay/Manager adornments | overlay shows `pwsh → ClaudeCode → pwsh` transitions; external claudes grouped |
| **O7 — hardening** | debounce optimization; elevated/WOW64 guards; retire dead discovery scan | steady-state µs (no enum when unchanged); 24-h soak, no leak/wedge |

`TerminalAppLib.vcxproj`: register `Activity.h`, `ProcessInspect.{h,cpp}`, `ProcessObserver.{h,cpp}`
(the `.cpp` as `<PrecompiledHeader>NotUsing`, like the rest of `AgentMaster/`). Add the new pure
helpers to `AgentMaster/tests/`.

---

## 15. Testing

- **Standalone harness (`AgentMaster/tests/`)** — pure parts only, no WinRT:
  - `ReadClaudeFacts` cmdline/env parsing (feed canned strings → expected model/effort/kind/bg).
  - `ResolveSessionId` encoding + newest/tie-break (temp dir of fake `.jsonl`).
  - `classify(RunningApp)` truth table.
  - `ObserveClaude` upsert/merge + provenance (no state clobber).
  - Tree helpers (`FindDescendantByImage`, `ChildrenOf`) over a canned `ProcEntry` vector.
- **Live integration** — the `Desktop\temp` rig already used: hand-typed claude, `cd`, `--resume`,
  `/resume`, close→reopen; assert bind ≤ 3 s, correct cwd, correct id, stable across restart.
- **Negative** — a claude in the real Windows Terminal: must classify `WindowsTerminal`, never bind,
  never appear on the managed board.
- **Perf** — confirm survey ≈ 10 ms off-thread @ 2 s; UI probe < 1 ms; no UI jank.

---

## 16. Risks & mitigations

| Risk | Mitigation |
| --- | --- |
| Undocumented PEB offsets drift on a future Windows | one centralized helper; WT itself depends on the same read; guarded + best-effort (empty on mismatch, falls back to observe-only) |
| Toolhelp cost at scale | ONE snapshot per survey reused for all tabs + census; 2 s heartbeat; debounce optimization (O7) |
| `encode(cwd)` mismatch vs Claude's scheme | fallback to reading each project dir's head `"cwd"` (cached) |
| Two Agentmaster instances | `AM_SESSION` is per-process GUID → cleanly separable; each binds only its own |
| State flapping push vs pull | provenance: `ObserveClaude` never sets state; hooks + tail own it |
| Privacy (reading env/cmdline) | all local, same-user, never transmitted; redact secrets in any logging |

---

## 17. File-by-file change list

**New**
- `AgentMaster/Activity.h` — enums + `ClaudeProcessFacts`/`CorrelationRow`/`TabActivityRow`/`TabRosterEntry`/`ObservedClaude`.
- `AgentMaster/ProcessInspect.{h,cpp}` — snapshot + PEB reads + tree + `ReadClaudeFacts` (promote from `ClaudeSpawn.cpp`).
- `AgentMaster/ProcessObserver.{h,cpp}` — the S-lane.
- `AgentMaster/tests/*` — new unit cases.

**Modified**
- `AgentMaster/SessionModels.h` — `SessionInfo` enrichment fields (transient).
- `AgentMaster/SessionRegistry.{h,cpp}` — `ObserveClaude`; set `hookWired`/`lastHookUnixMs` in `OnHookEvent`.
- `AgentMaster/Engine.{h,cpp}` — `amSession` field; mint+export `AM_SESSION`; construct/own/Start/Stop `ProcessObserver`; expose `observer`.
- `AgentMaster/ClaudeSpawn.{h,cpp}` — delegate `ClaudeCwdForShell` to `ProcessInspect`; keep `ClaudeProjectsDir`/`ResolveClaudeTranscriptPath`.
- `TerminalApp/TerminalPage.{h,cpp}` — `_observer`; `_ObserverProbe` (replaces `_DiscoverClaudeTabsByCwd`); roster build via `RootProcessHandle`; register/unpublish by token.
- `TerminalApp/AgentTabOverlay.{h,cpp}` — render `model`/`effort`/`kind`/`runningApp`; activity-aware visibility.
- `TerminalApp/AgentManagerContent.{h,cpp}` — adornments + optional External group.
- `TerminalApp/TerminalAppLib.vcxproj` — register the three new files.
- `doc/agentmaster/DESIGN.md` / `IMPLEMENTATION.md` — link this doc; add the observer milestone row.

---

## 18. Correctness rules preserved (do not regress)

- **#7** state from transcript + process facts, never the TUI.
- **#1** "waiting is three states" — unchanged tail→`Stop` path; `ObserveClaude` never sets state.
- **#3 / #9** bind injector by `sessionId`, correlated by exact `WT_SESSION` (+ `AM_SESSION` owner) —
  never guessed.
- **#10** one engine; observer in `SharedEngine`; per-window roster/probe detached by token on teardown.
- **Invisibility invariant** — the observer only *reads* (PEB/Toolhelp/filesystem); it never writes
  to a shell.

---

## 19. Open questions

1. Window attribution in `AM_SESSION` (`:<windowId>`) now, or defer until multi-window observer needs it?
2. Surface external (`WindowsTerminal`) claudes in the Manager at all, or only count them?
3. ~~Codex: keep as bare acknowledge, or reserve an enrichment slot for a future Codex integration?~~
   **RESOLVED — C1 (observe) + C2 (state) + the managed launch/restore/adopt lifecycle shipped (§11f):**
   Codex sessions are detected, rollout-resolved, state-derived, and fully lifecycle-managed
   (launch / restore / window-restore / adopt) — lifecycle + state only, never driven (C4). The C1
   *census* stays the **light external-row** path; the **managed** path took the follow-on's other fork —
   graduating `SessionInfo` to a **first-class `AgentKind`** (`kind` + `codexSessionId`, `TabKind::Codex`),
   so a managed Codex is a registry / persistence / window-record citizen reusing the Claude seams, rather
   than a parallel Codex-managed path. The remaining open question is **C3/C4** (PUSH + driving).
4. O7 debounce: gate on roster-equality + PID-liveness only, or add a `ReadDirectoryChangesW` watch on
   `projects/` to event-drive transcript-appear instead of the heartbeat?
