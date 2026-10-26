# 24 · Claude Code's life cycle in a tab (LIF)

> Conventions, fixtures (F-…), oracle tags and the mock's script: [README.md](README.md). Spec: CLAUDE.md
> *Persistence + archive/restore*, *Adopt any `claude`*, *Fleet Observer*, *Compaction, /clear and resume*;
> PERSISTENCE.md; FAVORITES.md §4; HOOKS.md *Session correlation*, *Retired hosts*; OBSERVER.md §11d; DELIVERY.md
> §16–§17 (the Prompt Queue Runner's gates across these edges). Related flows: TAB-01 (Deactivate and wake), TAB-10
> (quitting keeps the workspace), TCM-07 (fork), TCM-08 (Restart session), STA-01 / STA-11 (a turn's states, a killed
> claude), STA-07 and INT (turns cut off), PQR-09 / PQR-10 / PQR-15 (the runner around these edges).

**What it is.** A managed session's `claude.exe` is born at a launch, lives through turns, and ends — by `/exit`, by a
kill, by a park (Deactivate), by a Close, by the app's quit — and is born again by Restart session, a wake, Resume here
on the Sessions page, or the reopen. A claude the user types by hand into a shell tab is adopted. These flows walk each
edge and check the three things a user relies on across it:

- **the process** — which claude runs, with which command line: `--session-id <id>` starts a conversation, `--resume
  <id>` continues one, and nothing is left behind;
- **the conversation** — the same id with its history (the mock's next request carries the earlier messages:
  `n_msgs > 1`), or a fresh one where nothing was ever said (CLAUDE.md Rule #6: resume is transcript-gated);
- **the queue** — a prompt queued across the edge is never typed into what is left behind (the tab's PowerShell host
  after claude ended) and goes out once a claude runs again (flows/03).

**Measured** against 0.8.0 + Claude Code 2.1.291 in the test desktop of a loaded PC (2026-10-24, probe run
`pqprobe1`); each flow lists what the probe saw.

## Lifecycle model

```text
 launch (Agent Manager)  ──► claude.exe --dangerously-skip-permissions --settings <profile>\hooks-settings.json --session-id <id>
        │                   [SessionStart] <id> state=0 → idle, NO transcript yet
        ▼ first prompt      the transcript <config>\projects\<cwd>\<id>.jsonl appears; running ⇄ waiting-for-you
   ┌──────────────┬───────────────┬───────────────────┬──────────────────┬──────────────────┬──────────────────────┐
 /exit          kill          Deactivate         Close              /clear             the app's quit
   │              │               │                  │                  │                  │
 SessionEnd →   no SessionEnd   parked: a never-   archived           a NEW conversation  every session saved;
 done — or,     (STA-K1: the    started --resume   (live = false,     in the SAME tab:    the reopen resumes the
 when the hook  state stays);   connection; the    record + queue     tab, card, topic    focused tab, leaves the
 is cancelled,  the console     state normalized   kept, the          follow it; the old  others dormant; a never-
 still waiting  guard's note    (TAB-01)           transcript kept)   one is archived     prompted one comes back
   │              │               │                  │                with its QUEUE      FRESH (Rule #6)
   ▼              ▼               ▼                  ▼                (a feature)
 the tab's PowerShell host stays  enter / Activate   Sessions page →
   │                              Tab → claude       Resume here →
   ▼                              --resume <id>      claude --resume <id>
 Restart session → claude --resume <id>              (a new tab, same title)

 a claude TYPED into a shell tab ──~2 s after it starts (2.1.291's heartbeat names its conversation)──► adopted:
     [adopt] <id> bound via WT_SESSION <wt> (external, driven like a launched one) ──/exit──► the shell's prompt again

 the runner at every edge: never into a claude that does not run — no Claude input box on the screen ("input box not
 visible"), a parked tab ("tab dormant"), a Done or closed session (nothing asks the runner at all: no line) — and
 after a claude starts again the settle window counts from the start for a WAKE (`started` changed) but not for a
 RESTART (accepted, PQR-03 E5); every reopen and Resume here keeps the session's own runner mode (PQR-K8's fix)
```

---

### LIF-01 · A launch starts a fresh claude, and its first prompt makes the transcript

> **Since** ≤ 0.7.7 · **P0** · **Needs** mock · **Spec** CLAUDE.md *End-to-end pipeline*, *Launch*; Rule #6

**Promise.** Launching a session starts exactly one claude for a new conversation; nothing is on disk until the first
prompt, and the first prompt — typed or queued — starts the conversation.

**Phases**
1. **Launch.** *Do:* the Agent Manager's launch row, a new folder `w1`, Launch Claude (no prompt). *Expect:* one
   claude.exe whose command line holds `--session-id <id>`, `--settings` (Agentmaster's hooks) and
   `--dangerously-skip-permissions`, never `--resume`. *Check:* [OS] the command line; [LOG] `[nav] open-new claude done
   new=<sid8>`, `[spawn] <id>`, `[SessionStart] <id> state=0` (measured ~12 s after the launch on the loaded desktop).
2. **Idle, nothing on disk.** *Expect:* the side panel reads idle; no transcript. *Check:* [UI]; [TR] no
   `<id>.jsonl`.
3. **The first prompt.** *Do:* type `first [[mock say "working"; delay 3s; reply "hello"]]` + Enter. *Expect:*
   running, then waiting-for-you; the transcript exists and holds the prompt. *Check:* [UI] the timeline idle →
   running → waiting-for-you; [TR].

**Edge cases**
- **E1 A queued first prompt.** *Do:* launch without a prompt, queue `opening [[mock reply "first words"]]` on the
  Prompt Queue tab. *Expect:* idle is ready: the prompt goes a settle window after it was queued and is the
  conversation's first message (the runner's idle start — a fresh session ends no turn, so nothing else would start
  its queue). *Check:* [MOCK] 37–85 s after the queue; [TR] the first user prompt.

---

### LIF-02 · /exit ends claude; the tab keeps its shell; Restart session resumes the conversation

> **Since** ≤ 0.7.7 · **P0** · **Needs** mock · **Spec** CLAUDE.md *Persistence*; TCM-08; DELIVERY.md §17 (the box gate)

**Promise.** `/exit` ends the claude, not the session: the tab and its card stay, nothing the runner holds is typed
into the shell left behind, and Restart session continues the same conversation, queue included.

**Phases**
1. **/exit.** *Do:* type `/exit` + Enter in F-S. *Expect:* the claude exits; the tab shows Claude Code's "Resume this
   session with: claude --resume <id>" and the PowerShell prompt; the tab and the card stay. *Check:* [OS] no claude
   for the session; [UI] the terminal text. **Measured:** on the loaded desktop Claude Code CANCELLED the SessionEnd
   hook at its 5 s budget ("SessionEnd hook [...] failed: Hook cancelled"), so the session kept reading
   waiting-for-you (flows/05 STA-K1); on a fast PC it reads done.
2. **A queue while it is gone.** *Do:* queue `back [[mock reply "I am back"]]`. *Expect:* never typed into the shell.
   Two outcomes were measured: the SessionEnd hook cancelled (the session still reads waiting-for-you) — the runner is
   asked and holds on "input box not visible" (no Claude box on the screen; after 60 s also the warning
   `[send-verify] <sid8> queued prompts HELD: no parseable input box for >60s …`); the hook arrived (Done, run pqr1) —
   nothing asks the runner at all, so no line is written. *Check:* [MOCK] silence past a whole settle window; [LOG] no
   `[send]`, and any reason one of those; [UI] the terminal never shows the prompt.
3. **Restart session.** *Do:* the tab's menu → Restart session. *Expect:* a new claude with `--resume <id>`; the
   conversation continues. *Check:* [LOG] `[restart] claude <id> (resume)`, `[retired-host] <id> no longer hosted by
   ConPTY <old wt> …`; [OS] the command line.
4. **The queue goes.** *Expect:* the held prompt is delivered once, into the resumed conversation. *Check:* [MOCK] one
   turn, `n_msgs > 1`. **Measured:** 8 s after the restart, a second before the new claude's SessionStart hook ran
   — the restart started no settle window (accepted by the owner on 2026-10-26: flows/03 PQR-03 E5).

---

### LIF-03 · A killed claude: its shell never gets the queue; Restart session recovers

> **Since** 0.7.9 (the console guard: 0.8.0) · **P0** · **Needs** mock · **Spec** STA-11; flows/05 STA-K1; DELIVERY.md §17

**Promise.** A claude that dies without its SessionEnd never gets a queued prompt typed into the PowerShell host left
behind, and Restart session brings the conversation back.

**Phases**
1. **The kill.** *Do:* ≥ 10 s after F-S's turn, kill the session's claude.exe. *Expect:* the tab shows the console
   guard's note "[Agentmaster] The agent exited …" and the PowerShell prompt; the session keeps reading
   waiting-for-you (no SessionEnd — STA-K1; killed within ~2 s of a turn's end it reads running for good).
2. **The queue holds.** *Do:* queue `revived [[mock reply "alive again"]]`. *Expect:* "input box not visible" (or "not
   ready"); nothing reaches the model; the terminal never shows the prompt.
3. **Restart session.** *Expect:* `--resume <id>`; the prompt is delivered once into the resumed conversation.

---

### LIF-04 · Close keeps the session; Resume here brings it back with its queue

> **Since** 0.7.7 (Close always archives: FAVORITES.md) · **P0** · **Needs** mock, the Sessions page · **Spec**
> FAVORITES.md §4; SESSIONS.md; CLAUDE.md *Archive removed — Favorite + Close*

**Promise.** Closing a tab ends its claude but keeps the session — record, transcript and queue — and Resume here
continues it, queue included.

**Phases**
1. **A queue, then Close.** *Do:* in F-S2's t1 (runner on Auto): queue `later [[mock reply "resumed and sent"]]`,
   then — inside the prompt's 40 s settle window — the tab's ✕ → Close. *Expect:* the claude exits, the card leaves
   the board, nothing is sent (the window held it; a closed session asks the runner nothing). *Check:* [LOG]
   `[nav] close-begin <sid8> (archived, resumable)`, `[archive] <id>`, `[nav] close-done <sid8>`.
2. **The Sessions page.** *Do:* the toolbar's Sessions. *Expect:* the table lists t1 (and t2), the closed one with
   "Resume here" in its detail pane. *Check:* [UI] (a row is no element: its cells are texts — e2e/README.md).
3. **Resume here.** *Expect:* a tab titled t1 again, its claude with `--resume <id>` (the same id — not a fresh
   conversation); the Prompt Queue tab shows `later` in UPCOMING. *Check:* [LOG] `[nav] sessions resume-click row=<sid8>
   …`, `[nav] sessions resume-done new=<sid8>`; [OS].
4. **The mode and the send.** *Expect:* the resumed session keeps its own mode (Auto here), so `later` goes once into
   the resumed conversation. *Check:* [UI] the toggle; [MOCK] one turn, `n_msgs > 1`.

**Edge cases**
- **E1 Closed Paused.** *Expect:* Resume here brings it back Paused — the runner's mode is per session and persisted
  (the owner's rule of 2026-10-26; PQR-K8's fix, NOT BUILT) — so nothing goes by itself; switched to Auto, the prompt
  goes once. Test (`needs_fix`): `test_resume_here_keeps_the_sessions_paused_mode`. (0.8.0, measured 2026-10-24: the
  restore re-stamped the cog's default, so a session closed Paused came back Auto and its queue went by itself.)

---

### LIF-05 · A claude typed into a shell tab is adopted and driven

> **Since** ≤ 0.7.7 (the Fleet Observer: 0.7.x) · **P1** · **Needs** mock · **Spec** CLAUDE.md *Adopt any `claude`*,
> *Fleet Observer*; OBSERVER.md §11d; DELIVERY.md §17.2 (the adopted carve-out)

**Promise.** A claude the user starts by hand in a shell tab is recognized as soon as it has a conversation: the tab
becomes a managed session (its name, its card, its badge), and the runner can drive it.

**Phases**
1. **A shell tab, claude by hand.** *Do:* Ctrl+Shift+T; in the new tab `& <claude.exe>` in a folder nobody
   pre-trusted. *Expect:* Claude Code asks whether to trust the folder — its FIRST option, "No, exit", is the selected
   one (Enter alone ends claude); the user picks "Yes, I trust this folder". **Measured:** the `+` tab's shell does not
   carry the app's per-test environment (Windows Terminal builds a new tab's environment from the registry), so the
   test sets `CLAUDE_CONFIG_DIR` itself.
2. **Adopted.** *Do:* `hello [[mock reply "hi from the shell tab"]]`. *Expect:* the observer binds the claude to the
   tab by its `WT_SESSION` (no hooks: it has no `--settings`; its state comes from the transcript, the scanner's
   pull); the tab takes the session's name (its folder's), a card appears. *Check:* [LOG] `[bind-try] <id> token=<wt>
   …`, `[adopt] <id> bound via WT_SESSION <wt>`; [ENG] the record. **Measured (run pqr1):** the bind came ~2 s after
   claude STARTED, before its first prompt — Claude Code 2.1.291's presence heartbeat names the conversation from the
   start, and the observer prefers it (OBSERVER.md §8b) — so a test marks the log before claude starts.
3. **Driven.** *Do:* queue `driven [[mock reply "sent by the runner"]]`. *Expect:* delivered once (an adopted session
   is exempt from the dormant gate: its claude runs in the user's own shell). *Check:* [MOCK].
4. **/exit.** *Expect:* the claude exits; the tab shows its shell's prompt again.

---

### LIF-06 · A never-prompted session comes back fresh after a restart

> **Since** ≤ 0.7.7 · **P1** · **Needs** F-RS · **Spec** CLAUDE.md Rule #6 (transcript-gated resume)

**Promise.** A session that was never prompted has nothing to resume, so a reopen starts a fresh claude in its place —
same folder, same title — instead of a `--resume` that would die on "No conversation found".

**Phases**
1. **Launch, no prompt.** *Expect:* idle, no transcript.
2. **Quit and reopen.** *Expect:* the restored tab's claude has a NEW `--session-id` and no `--resume`; the stale
   record is dropped. *Check:* [LOG] `[restore-fresh] <new id> "<title>" cwd=…`; [OS] the command line (measured:
   `--session-id 0f7fd620…` for the never-prompted `c8ecfa7d…`); [ENG] the old id is gone from `sessions.json`
   (measured, run pqr1: a test that reads the title from the old record after the reopen finds no record).

---

### LIF-07 · /clear starts a new conversation in the same tab

> **Since** 0.7.9 (the tab follows: STA-10) · **P1** · **Needs** mock · **Spec** CLAUDE.md *Compaction, /clear and
> resume*; HOOKS.md; MESSAGING.md §21.6 (the topic follows its tab)

**Promise.** `/clear` gives the tab a new conversation and keeps the tab managed: its title, its card, its topic.

**Phases**
1. **/clear.** *Do:* type `/clear` + Enter in F-S. *Expect:* the tab swaps to the new conversation; the old one is
   archived; the lineage is recorded. *Check:* [LOG] `[rehome] <old> -> <new> (same tab, new conversation id)`, `[nav]
   tab-swap <old8> -> <new8> (same tab; claude switched conversation — /clear or /resume)`, `[rehome] <new> inherits
   MANAGED provenance from <old> …`, `[lineage] <new8> continues <old8> (/clear on ConPTY …)`, `[SessionStart] <new>
   state=0`; [ENG] the new record's `continuedFromId` / `continuedVia` = clear.
2. **The queue.** *Expect (a FEATURE — the owner's ruling of 2026-10-26: "no don't follow tab across /clear"):* the
   prompts queued for the tab stay in the OLD conversation's (archived) record, with its runner mode: the tab's new
   conversation shows an empty queue, its runner on the cog's default (a new conversation), and the queued prompt is
   never sent into it. It stays resumable with the old conversation (Sessions page → Resume here). Test:
   `test_clear_leaves_the_tabs_queue_with_the_old_conversation` (flows/03 PQR-13 E5).

---

## Known defects

The life cycle's defects are filed where the behavior lives:
- **STA-K1** (flows/05): a claude that ends without its SessionEnd — killed, or its `/exit` hook cancelled on a busy PC
  — never reads done (LIF-02 phase 1, LIF-03 phase 1).
- **PQR-K8** (flows/03; **fixed in code 2026-10-26**, NOT BUILT): a session closed Paused came back from Resume here on
  the cog's default mode (LIF-04 E1).

Two behaviors filed as defects on 2026-10-24 are the owner's accepted behavior (2026-10-26; flows/03 *Reclassified*):
Restart session starts no settle window (PQR-K6 → LIF-02 phase 4), and `/clear` leaves the tab's queue with the old
conversation (PQR-K7 → LIF-07 phase 2).
