# Agentmaster end-to-end suite

Agentmaster driven like a user — real clicks, keys and menus in a disposable Windows 11 desktop — while every
Claude Code it starts talks to a **scripted model** whose answers are written into the prompts. The design is
[`doc/agentmaster/E2E_TESTING.md`](../doc/agentmaster/E2E_TESTING.md); WHAT the suite must prove is the flow catalog,
[`doc/agentmaster/flows/`](../doc/agentmaster/flows/README.md). This folder is the code.

The proposed shared crash/corruption guard and dedicated memory/lifecycle/failure suite are in
[`E2E_RELIABILITY.md`](../doc/agentmaster/E2E_RELIABILITY.md) (2026-10-23, **plan only**). It audits the current
fixture's limited crash checks and specifies the supervisor, expected exits, retained evidence and leak methodology;
these capabilities are not yet implemented by this README update.

| Folder | What it is |
|---|---|
| [`mockllm/`](mockllm) | The scripted model: an Anthropic Messages API on `127.0.0.1:8787` that plays `[[mock …]]` scripts (say, think, tool calls, holds, errors, drops), classifies Claude Code's side requests, logs every request, and has a control API (`/_mock/*`). Its unit tests run anywhere: `python -m pytest mockllm/tests`. |
| [`amdriver/`](amdriver) | The driver: real input (`inputs`), UI Automation (`uia`), waits, the app's process (`app`), its profile's files and logs (`profile`), Claude Code's config and transcripts (`claude`), a tab's terminal (`terminal`), sessions (`session`), page objects (`ui/`), and failure evidence (`evidence`). `python -m amdriver dump --out FILE` writes what UI Automation sees of a running Agentmaster. |
| [`suites/`](suites) | The tests (pytest), one file per flow area, and the fixtures (`conftest.py`). |
| [`guest/`](guest) | What runs in the test desktop: `provision.ps1` (Agentmaster, `claude.exe` 2.1.291, Git, PowerShell 7, the Python venv, hosts, environment, crash dumps), `run_suite.py` (copy the suite to C:, run pytest, zip the results onto the share) and `progress.py` (a pytest plugin: one line per finished test, for the host). |
| [`host/`](host) | `e2e_run.py`: stage the suite into a desktop, provision, run, wait (a change notification, no polling), collect the results to `Q:\agentmaster-e2e\runs\<run>\` and sum them up. |

## Running it

```text
# a new desktop, provisioned, the whole suite, deleted afterwards (the 49 tests of 2026-10-23: ~37 min + provisioning)
python e2e/host/e2e_run.py --desktop am-e2e-1 --create --provision --delete-after -- -v

# an existing, provisioned desktop: one area, or the P0 flows only
python e2e/host/e2e_run.py --desktop am-e2e-1 -- -v suites/test_tab_menu.py
python e2e/host/e2e_run.py --desktop am-e2e-1 -- -v -m p0

# rejoin a run already started (its orchestrator ended, or another session started it); or wait 270 s at most
python e2e/host/e2e_run.py --desktop am-e2e-1 --attach g1full
python e2e/host/e2e_run.py --desktop am-e2e-1 --attach g1full --wait-max 270
```

The results folder holds `summary.txt` (the run's end, below), `pytest.log`, `junit.xml`, the mock's request log
and, for every failed test, its evidence: `screen.png`, `uia.txt` (every window of the app), `terminal-<n>.txt`, the
profile's logs and records, `mock.json`, `processes.txt`. A test that passes leaves nothing.

### Waiting for a run

**The wait costs nothing, and nobody reads a log.** The guest writes into `Z:\e2e\out`, which is a folder of this
PC's disk (the desktop's persist folder), so `e2e_run.py` blocks on that folder's change notification
(`FindFirstChangeNotificationW`) and wakes the moment the guest writes there — no sleep interval, no polling of
`status.json`. The guest's pytest appends one line per finished test to `<run>.progress.jsonl` (`guest/progress.py`,
loaded by `run_suite.py` with `-p guest.progress`); the host prints each line as it arrives, then the summary — the
counts, every failed test with the first line of its failure and its evidence folder:

```text
[16:22:40] 13/49 FAIL  test_tab_lifecycle.py::test_unfavorite_and_close  40 s — AssertionError: …
[16:58:31] done: exit 1 — 49 tests: 48 passed, 1 failed, 0 errors, 0 skipped, 36.8 min
  FAIL  test_tab_lifecycle.py::test_unfavorite_and_close (40 s): AssertionError: …
        evidence: Q:\agentmaster-e2e\runs\<run>\tests\test_unfavorite_and_close
  results: Q:\agentmaster-e2e\runs\<run> (pytest.log, junit.xml, summary.txt)
```

- **From a Claude session:** start the run with the Bash tool's `run_in_background` — plain stdout, no `> file`
  redirection — and go on working: the tool's completion notification brings the summary. Never wait with `sleep`
  and a read of `status.json` or `pytest.log`. A call that must block only briefly (inside a prompt-cache window) is
  `--attach <run> --wait-max 270`, repeated while it exits 97.
- `--attach <run>` joins a run already started: no create, stage, provision or launch, and no fleet token. It
  prints one catch-up line (`so far 23/49 finished: 22 passed, 1 failed (…)`), then the live lines, collects and
  sums up. The guest's runner outlives the host's process, so a run whose orchestrator was stopped is never lost.
- `--wait-max <s>` stops one wait after that long and exits **97** (the run goes on; attach again). `--progress
  failures` prints only the failing tests' lines, `--progress none` none.
- Exit codes: pytest's; **97** still running; **98** the orchestration failed (also a runner that writes no status
  within 180 s of its launch).
- A one-minute belt (`BELT_S`) reads the files anyway, in case a notification never came.
- Measured 2026-10-23: a write in the guest woke the host's wait within 10 ms of the file's own timestamp; a run
  printed "running" 3 s after its launch, each test's line the moment it finished, and its summary in the same
  second as its last test.
- Provisioning still polls its marker on the guest's C: (`C:\e2e\provision.done`, which no notification of this PC
  can see). An empty marker is not a write caught midway: `echo %ERRORLEVEL%> file` with a code of 0–9 reads
  `echo 0> file`, a redirection of handle 0, so cmd creates the file EMPTY. The host then decides by
  `provision.ps1`'s last log line (`PROVISION {…}`, no step `FAILED`).

⚠ A Windows desktop takes 4–6 GB and several cores of the PC the user games on: delete it when done
(`--delete-after`, or the fleet's `delete_desktop`). The fleet token is read from the cluster into a variable and
never printed; desktop tokens and noVNC links never go into logs, commits or the catalog.

## Writing a scenario

A test takes the `am` fixture — a running Agentmaster on a fresh profile and a fresh Claude Code config, its window
maximized, every session pointed at the mock — and works it through the page objects:

```python
@pytest.mark.p0
@pytest.mark.flow("MGR-02")
def test_home_turns_into_the_way_back(am, s2):
    a, b = s2                                   # F-S2: sessions in t1 and t2, both Waiting for you
    page = am.window.manager
    a.select()                                  # a real click on t1's tab
    page.open()                                 # Home
    assert page.home_name() == "Jump back to t1"
    page.click_home()                           # ↶
    am.window.tabs.wait_selected("t1")
    am.profile.hooks.wait(rf"\[nav\] home-button jump-back -> {a.short}")
```

**Fixtures** (`suites/conftest.py`): `am` (the App), `mock` (the MockClient, reset per test), `iso` (the test's
folders), `s1` (F-S: a session launched through the Agent Manager's launch row, its first turn done), `s2` (F-S2:
`s1` plus a session in `t2`). More sessions: `am.launch("t3")`; a second session in the SAME folder:
`am.launch("fa", title="A2")` (the new tab is renamed before its first turn — two sessions of one folder otherwise
share its name); a git folder first: `am.iso.git_folder("t1", branch="feature/x")`. A second window:
`am.new_window()` (Ctrl+Shift+N, one shell tab) or `w.tabs.move_to_new_window("t1")`. A restart (F-RS):
`am.close()` (the ✕ → Close Window; `how="all"` for Close All Windows), read `am.profile.window_records()` if the
test checks the save, then `am.kill(); am.start()` — or `am.restart()`. Two markers set the app up BEFORE it starts:
`@pytest.mark.claude_env(NAME="value")` (more environment for the app and every claude it starts, e.g.
`CLAUDE_CODE_AUTO_COMPACT_WINDOW="100000"`) and `@pytest.mark.am_settings(key=value)` (Settings-cog values written into
settings.json, e.g. `skipPermissions=False`). Claude Code's own settings (a user hook, `disableAllHooks`) need no
restart: `am.claude.seed(settings={...})` before `am.launch(...)` — each claude reads them when it starts.

**The page objects** (`am.window` is the first window; `am.windows()` all of them):

| Object | Reads | Does |
|---|---|---|
| `window.tabs` | `titles()`, `items()` (title, selected, index), `selected()`, `find(title)`, `rename_box()` | `select(tab)`, `context_menu(tab)` → Menu, `rename(tab, new)`, `close(tab, answer)`, `deactivate(tab, scope)` (Deactivate ▸ …), `activate(tab)` (Activate Tab), `move_to_new_window(tab)` → MainWindow, `wait_tab / wait_gone / wait_selected`; a tab is a title or a `Tab` from `items()` |
| `Menu` (`amdriver.ui.menus`) | `names()`, `items()` (name, enabled, is_sub, expanded), `has`, `enabled` | `choose(name)`, `submenu(name)` → Menu, `choose_path("Close", "Close other tabs")`, `close()`; `Menu.open_menus(app)`, `Menu.dismiss_all(app)` |
| `window.manager` | `showing()`, `home_name()`, `columns()` / `cards()` / `card(title)`, `tree_rows()` / `tree_row(label)`, `board_scope()`, `tree_scope()`, `right_pane_texts()` | `open()`, `click_home(count)`, `launch(folder)`, `type_folder`, `clear()`, `show_all()`, `toggle_board_scope()`, `show_tab("Prompt Queue")`, `queue_prompt(text)` (the compose row's Add to queue), `activate_all(scope)` (Activate All Tabs (N); `dormant_count()`); a Card: `click`, `double_click`, `right_click` |
| `window.side_panel` | `cards()` / `card(title)` (title, state word), `titles()`, `count_line()`, `scope()`, `sum_on()`, `dock_side()`, `collapsed()`, `summary_title()` / `summary_texts()` | `button(name).click()` ('Sum', 'LOCAL', '⌖', '⇄', '«'), `toggle_scope()`, `set_sum(on)`, `open_launcher()` → LauncherCard (`box`, `folder_list`, `type_folder`, `launch`) |
| `window.terminal` | `text()`, `lines()`, `input_box()` (Claude's box and its draft) | `focus()`, `type(text)`, `submit(prompt)`, `wait_input_box()`, `wait_text(regex)` |
| `window.tags` / `window.specify` (`amdriver.ui.popups`) | the Tags panel: `texts()`, `count()` ('N of M tags'); the Specify card: `value()` | `wait_open()`; Tags: `add(name)`; Specify: `use(model)`, `press('Cancel')` |
| `App` | `windows()`, `window` (the first window while it lives), `popups()`, `tooltips()`, `claude_processes()`, `profile`, `claude` | `launch(name, title=…)`, `new_window()`, `close(how='window'/'all')`, `kill()`, `start()`, `restart()` (F-RS) |
| `Dialog` (`amdriver.ui.dialogs`) | `text()`, `buttons()` | `Dialog.wait(app, title=…)`, `press(name)`; `MessageBox` for Win32 boxes |
| `Session` (`amdriver.session`) | `record()` (sessions.json), `title`, `state`, `claude_pids()`, `running()`, `tab()` | `select()`, `ask(prompt, wait="turn"/"sent"/"none")`, `wait_state(*states)`, `wait_running()`, `wait_parked(since=mark)` |
| `StateTimeline` (`amdriver.timeline`) | a session's side-panel state word sampled every 150 ms in the background: `states()` (the sequence a user saw), `changes()` (with times), `current()`, `seen(state)`, `describe()`; `engine_events` / `engine_story(profile, id, hooks_mark, scanner_mark)` (the hook lines and scanner reconciliations behind it), `transcript_kinds(lines)` | `with StateTimeline(window, title) as tl:` … `tl.wait(state)`, `tl.wait_seen(state)`, `tl.hold(state, s)` (nothing else sampled meanwhile) |

**The oracles** — prefer one the app does not control (flows/README.md "Oracles"): the mock
(`mock.prompts()`, `mock.requests(session=…)`: what reached the model, with `n_msgs`), the transcript
(`am.claude.user_prompts(id)`), the processes (`session.claude_pids()`), the clipboard (`win32.clipboard_text()`),
then the engine's record (`am.profile.sessions()`, `session_store(id)`, `window_records()`, ≤ ~1 s behind) and the
logs read from a mark (`mark = am.profile.hooks.mark()` BEFORE the action, then `am.profile.hooks.wait(regex,
since=mark)`), then the UI.

**Rules that keep a test honest:**
- Wait for conditions (`wait_until`, the `wait_*` methods, `hold_true` for "nothing happens"); a `time.sleep` only
  to let a negative check have its chance, with a comment saying so.
- Act through the UI with real input. `Element.invoke()` / `set_value()` exist for the cases a flow says so.
- Mark every test with its flow (`@pytest.mark.flow("TCM-03")`) and priority (`p0` / `p1` / `p2`).
- A behavior that is wrong today gets a `known_defect("<ID>")` test that asserts TODAY's behavior, named
  `test_known_defect_…`, with the expected behavior in comments: it fails when the defect is fixed, and is then
  replaced by the real assertion. Add the defect to its flow file's "Known defects".
- The test that replaces it proves the FIX. Until a published release holds the fix, mark it `needs_fix("<ID>")`:
  it passes against a build of the branch and fails on the older release, where a run deselects it with
  `-m "not needs_fix"` (MGR-K1, SP-K1, TCM-K2, TAB-K4, TAB-K3, A11Y-K1 and CLI-K2 since 2026-10-23).
- A NEW FEATURE that no published release holds takes the same marker with an `-F<n>` id, so the same deselection
  covers it: TAB-F1 = Deactivate ▸ "Deactivate All Idle Tabs" (2026-10-24,
  `test_tab_lifecycle.py::test_deactivate_all_idle_tabs`; flows/07 TAB-02 phase 7). An existing test whose exact
  list the feature extends accepts both forms meanwhile (`test_tab_menu.py::test_menu_fits_the_tab`, `DEACTIVATE_IDLE`).
- A new surface: open it in a probe, `python -m amdriver dump --out Z:\e2e\out\dumps\<name>.txt`, read the names and
  the structure, then add a page object method — never pixel positions.

## What the driver knows (measured in the test desktop, 0.8.0, 2026-10-23)

- **Read a session's state from the UI, not from sessions.json.** The file lags the registry by up to ~1 s (a test
  that read it right after an Error read "Running"), and a read that collides with a save makes the save fail (ROB-K1).
  The side panel's state word (`StateTimeline`, `side_panel.card(title).state`), the board column and the tree row are
  read through UI Automation and cost the app nothing.
- **Hooks can be slow or lost on a busy PC.** On a desktop of a loaded PC every PowerShell hook forwarder took 10–22 s;
  Claude Code cancels a hook at its timeout (`hook_cancelled` in the transcript) and the scanner carries the state a
  few seconds later. Wait for states (up to ~2 min), never for a hook's latency.
- **Claude Code 2.1.291 runs every Agent call in the background**, even with `run_in_background: false`: the `agent`
  shorthand ends the parent's turn at once, and the agent's completion comes back as a `<task-notification>` turn,
  which the mock answers past the script's end ("mock: script done").

- **A click is a hand's click.** The pointer rests ~150 ms on the target before the press, and the button is HELD
  ~60 ms. A press on arrival races the target's hover work, and a down and an up sent together are both queued
  before the app handles the press — Agentmaster's one-click Launch over its presets menu reads the button's LIVE
  state (`GetAsyncKeyState`), which is then already "up", so the click is lost. (Builds with SP-K1's fix also read the
  press from its pointer message, so they would keep such a click; the hand's click stays the rule.)
- **A launch from the Agent Manager page stays on the page** on builds with MGR-K1's fix (`[nav] manager-page launch
  <sid8> (…): started in place, selected on the page`): `launch_session` then clicks Home ("Jump back to <title>") to
  reach the new tab, so fixture F-S ends on the tab with either build. A launch from a tab still opens in front.
- **Menus slide into place** for a few hundred ms after they open; a click read before that lands on the item above.
  `Menu` reads the items until two reads agree, and checks that a submenu belongs to the item it hovered.
- **Home ignores a click within the double-click time** of a click that opened the page or jumped back (by
  design); `click_home` spaces separate clicks past it.
- **Every tab close asks**: "Close session?" (Close · ★ Favorite & Close · Cancel) or, for a shell tab, "Close tab?"
  (Close · Cancel). The window's ✕ asks "Close this window?" (Close All Windows · Close Window · Cancel); Close All
  Windows then asks "Do you want to close all windows?", a dialog UI Automation cannot see on 0.8.0 (TAB-K3: Enter
  answers it). Builds with TAB-K3's fix show it, and every other dialog of the page, like any dialog: `App.close
  (how="all")` waits up to 4 s to see it and presses Close all, else answers with Enter.
- **Accessibility metadata (builds of 2026-10-23 or later, not 0.8.0 — A11Y-K1, `doc/agentmaster/E2E_TESTING.md`
  §6.3):** AutomationIds `am.<surface>.<element>[.<key>]` (key = the session id's first 8 characters: `am.tab.<key>`,
  `am.mgr.board.card.<key>`, `am.mgr.tree.row.<key>`, `am.mgr.queue.add`, `am.side.card.<key>` on the card's title, …),
  names for the controls that had none (the cog "Settings", the compose icons, ⋯ "More actions", a card or a row = its
  title, the side panel's ⌖ ⇄ and chevrons by what they do — so their 0.8.0 glyph names go: `SidePanel.button` maps
  them), every tooltip's text as its control's help text, and ItemStatus: a tab "running; unsent draft; selected on
  the Agent Manager page" (the pill), a card / row / side card "waiting-for-you; selected", a queue row "sent; auto".
  The page objects read them first and fall back to the 0.8.0 way (texts, positions, side effects): `Tab.facets` /
  `pill`, `TabStrip.by_session`, `ManagerPage.card_by_session` / `selected_card`, `Card.selected`,
  `PanelCard.selected`, `compose_buttons`.
- **A tooltip opens on a hand's arrival**: `hover(glide=True)` moves in steps; `am.tooltips()` lists what shows (a
  rich session card has no name — read its texts).
- **A read of the app's files can make its save fail.** The app saves `sessions.json`, `windows\<id>.json`,
  `settings.json` … by replacing them, and the replace FAILS while any handle holds the file, delete sharing or not —
  `[persist-fail] … atomic replace failed` — and nothing retries it (ROB-K1; measured 2026-10-23, flows/20). Every
  read in `amdriver` goes through `win32.read_bytes_shared`, which holds the file only for the read itself (and would
  let a POSIX-semantics replace through, which the app does not use yet), so a wait that polls a file for a value the
  app is about to save can still lose that save: run fix4's wait for a rename on disk timed out that way. Wait on the
  logs where the fact is logged, and keep file polls sparse. A test that holds a file with `open()` does it on purpose.
- **Shift+Click on a TAB switches to it on 0.8.0** (TAB-K4), with Shift held as long as you like; builds with TAB-K4's
  fix start the session in place and keep the selection (`[nav] tab-shift-click <sid8> …: started in place, no
  switch`). The tree row's and the board card's Shift+Click start a parked session in place on both. To wake a parked
  tab in place on either build, use `tabs.activate(tab)` (its menu's Activate Tab) or `manager.activate_all()`.
- **Activate All Tabs (N)** asks "Activate dormant tabs" (This window (N) · All windows (M) · Cancel) when another
  window has dormant tabs too; with this window's only, it starts them at once, ~500 ms apart.
- **Close ▸ Of Same Folder / Other of Same Folder** asks "Close N [other] sessions in this folder?" (Close All ·
  Cancel), the titles as bullets; **Deactivate ▸** asks nothing, in any scope.
- **A window left without a tab closes itself** (`[manager-only] self-close window {…}`) unless it is the last one.
- **The Prompt Queue's compose box** is named by its placeholder; a pointer or keyboard focus on the EMPTY box pulls
  the session's unsent draft in, so `queue_prompt` sets the text through UI Automation and clicks the envelope (the
  third icon left of the box).
- **The tab menu's Move tab to new window** logs `[nav] move-out <sid8>`; the drag gesture's tear-out logs
  `tab-send-to-window … win=-1`.
- **A reopened window re-homes its Claude tabs first, its shells after** (PERSISTENCE.md §13.5), and selects the tab
  that was focused at the quit (by session id; a shell by position).
- **One Agentmaster per exe path**: a second start hands its command line to the running one; the suite ends the
  previous test's app first. **Ending it** kills the GUI only; the ConPTY hosts end their shells and claudes.
- **Where things are in UI Automation**: tab items named by their titles under `#TabListView`, Home
  `#ManagerHomeButton`; everything below the strip is ONE content element's flat child list (the page, the side panel,
  the terminal `TermControl`, the badge, in-window popups); menus and submenus in their own popup windows; a
  ContentDialog is a `window` element of class Popup named by its title. The side panel's cards are not controls:
  each is its title, its state word, its timing and its subline as texts, clicked through the title.
- **Two sessions in one folder have the same title** (the folder's name); a fork is `<title> (fork)`, then
  `(fork 2)`. Rename one when a test needs to tell them apart.
- **The guest's share (Z:) cannot create folders from Python**; files in existing folders are fine. The runner
  works on C: and zips its results onto the share.
- **Start long guest work detached** (the desktop's `launch`), never `run_command`, whose call waits on every child
  and gives up after a few minutes.

## Known defects these suites found

| Id | Flow | What | Test |
|---|---|---|---|
| MGR-K1 | MGR-05, MGR-07 | A session launched from the Agent Manager page is not selected on the page: Home offers no way back and the right pane asks for a selection, until a tab switch. **Fixed in code 2026-10-23** (not in a release): a launch on the page stays on the page and selects the new session there | `test_manager_page.py::test_a_launch_from_the_page_stays_on_the_page`, `…::test_a_plus_card_launch_follows_where_the_user_looks` (`needs_fix`) |
| SP-K1 | SP-05 phase 5 | In the + card, one click on Launch while its presets menu is open does not launch (a second click does); the page's own Launch takes one. **Fixed in code 2026-10-23** (not in a release): measured, no release reaches the card's Launch; a release watch completes the click | `test_side_panel.py::test_plus_card_launch_takes_one_click_with_its_menu_open` (`needs_fix`) |
| TCM-K1 | TCM-03 E1 | A blank tab rename is refused for the session, but the tab then shows Claude Code's own terminal title (`✳ …`) | `test_tab_menu.py::test_known_defect_blank_rename_shows_claudes_own_title` |
| TCM-K2 | TCM-05 phase 5 | Tags from the tab menu leaves the keyboard in the terminal: a typed name lands in Claude's box, Esc reaches claude. **Fixed in code 2026-10-23** (not in a release): the tab menu's late Closed no longer pulls the keyboard out of an open popup, and the panel closes on the Esc key-up and gives the keyboard back to the terminal | `test_tab_menu.py::test_tags_from_the_tab_menu_take_the_keyboard` (`needs_fix`) |
| SP-K2 | SP-04 | With Sum off, hovering a side-panel card opens no rich card (the tab's hover does, under the same input) | `test_side_panel.py::test_known_defect_sum_off_card_hover_opens_no_rich_card` |
| TAB-K1 | TAB-10 E2, E3 | A window close can save its Claude tabs as plain shell tabs: an autosave still pending at the close fired after the close archived the sessions (seen once, then reproduced on 0.8.0 by a sweep of the close's moment: 3 of 9 closes). **Fixed in code 2026-10-23** (not in a release): once the close has saved, nothing writes the record again, and a late save logs `[window-save] <id> late save refused` (a test build: 9 of 9, three late saves refused) | `test_workspace.py::test_a_close_right_after_a_tab_switch_keeps_the_claude_tabs`, `…::test_closing_one_of_two_windows_keeps_its_claude_tab` (`needs_fix`); `test_tab_menu.py::test_restart_restores_the_renamed_tab_dormant` (its record check) |
| TAB-K3 | TAB-10 | The quit confirmation is invisible to UI Automation (the driver presses Enter on 0.8.0). **Fixed in code 2026-10-23** (not in a release): the seven dialogs declared in `TerminalPage.xaml` were shown nested in the page, not as window-level popups, so `Dialog.wait` (and a screen reader looking for the window's dialog) did not find them — a search of the whole window does (`probe_dialogs.py`); all are built in code now | `test_accessibility.py::test_the_quit_confirmation_is_readable`, `…::test_the_large_paste_warning_is_readable` (`needs_fix`) |
| A11Y-K1 | MGR-05 phase 2, every page | Glyph buttons, cards and rows had no names, nothing had a stable id, and selection / pill / unread / draft were pixels only. **Fixed in code 2026-10-23** (not in a release): names, `am.*` AutomationIds and item status (`AgentAutomation.h`) | `test_accessibility.py::test_the_page_names_its_glyph_buttons_and_cards`, `…::test_the_selection_is_readable_on_every_surface`, `…::test_the_tab_says_its_session_state` (`needs_fix`) |
| A11Y-K2 | every dialog | UI Automation's hit test at a dialog's button returns the terminal behind the dialog, a code-built one ("Close tab?") included; the tree walk and the keyboard reach it | `test_accessibility.py::test_known_defect_a_hit_test_passes_through_a_dialog` |
| TAB-K4 | TAB-01 phase 4 | Shift+Click on a parked tab switches to it instead of starting it in place (its menu item names the gesture). **Fixed in code 2026-10-23** (not in a release): the tab's press handler never received a left press (the tab, a ListViewItem, takes it first); it now does, and the Shift+Click keeps its release from selecting the tab. Verified on the test build 0.8.1.1 (run `fixall-0811`: passed) | `test_tab_lifecycle.py::test_shift_click_on_a_parked_tab_starts_it_in_place` (`needs_fix`) |
| TAB-K5 | TAB-10 E4 | With a shell tab in the window, a reopen whose focused tab was a Claude tab other than the last one starts the last Claude tab too (TAB-K2's mechanism) | `test_tab_menu.py::test_known_defect_a_claude_focused_reopen_also_starts_the_last_claude_tab` |
| ROB-K1 | ROB-11 | A reader holding `sessions.json` makes the save fail, with or without delete sharing (measured: the app's replace fails while any handle is open); nothing re-saves until the next change | `test_robustness.py::test_known_defect_a_reader_holding_sessions_json_loses_the_save` |
| STA-K1 | STA-11 E3, STA-01 phase 5 | A claude that ends without its SessionEnd never reads Done; killed mid-turn (or within ~2 s after a turn) the session reads Running for good — its last `busy` heartbeat outlives the process, and the scanner's "work outlived the turn" rule trusts it | `test_session_state.py::test_known_defect_a_killed_claude_keeps_reading_running` |
| STA-K2 | STA-12 E1 | Esc before any output: Claude Code puts the prompt back in the box and writes nothing; the session reads Running ~30 s more (the presence-idle release) | `test_session_state.py::test_known_defect_esc_before_any_output_reads_running_for_half_a_minute` |
| STA-K3 | STA-06 phase 1 | A manual `/compact` reads Running ~30 s after it ended: PostCompact sets Waiting, and 2 ms later the still-`busy` heartbeat promotes Running again | `test_session_state.py::test_known_defect_compact_reads_running_half_a_minute_after_it_ended` |
| STA-K4 | STA-13 phase 2 | A Stop hook that blocks: Claude Code continues the same turn, but the session reads Waiting for you (our Stop hook already ran) until the busy heartbeat outlives the turn by 20 s | `test_session_state.py::test_known_defect_a_blocking_stop_hook_reads_waiting_while_claude_continues` |

TAB-K2 (TAB-10 E1: a reopen whose focused tab was a shell starts a restored Claude tab — the LAST one; "the first" as
filed was measured with one) was filed as a defect on 2026-10-23 and reclassified the same day by the user as a
FEATURE: `test_tab_menu.py::test_a_shell_focused_reopen_starts_the_last_claude_tab` proves it (the last Claude tab
starts, the first stays dormant).

**Checked both ways (2026-10-23).** Each `needs_fix` test fails on 0.8.0 on its defect (runs `nf-080`, `nf-080b`;
TAB-K1's sweep on 6 of its 9 moments) and passes on the test build 0.8.1.1, a purged Release build whose `src/` equals
HEAD 1f4c68678 (runs `fixall-0811`, `fixall-0811b`: every test of these suites passes there, 67 of 67). Each
known-defect test passes on both builds: SP-K2, TCM-K1, ROB-K1, TAB-K5 and A11Y-K2 are still there. Results:
`Q:\agentmaster-e2e\runs\<run>\`.
