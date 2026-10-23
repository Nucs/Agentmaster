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
| [`guest/`](guest) | What runs in the test desktop: `provision.ps1` (Agentmaster, `claude.exe` 2.1.291, Git, PowerShell 7, the Python venv, hosts, environment, crash dumps) and `run_suite.py` (copy the suite to C:, run pytest, zip the results onto the share). |
| [`host/`](host) | `e2e_run.py`: stage the suite into a desktop, provision, run, collect the results to `Q:\agentmaster-e2e\runs\<run>\`. |

## Running it

```text
# a new desktop, provisioned, the whole suite, deleted afterwards (the 49 tests of 2026-10-23: ~37 min + provisioning)
python e2e/host/e2e_run.py --desktop am-e2e-1 --create --provision --delete-after -- -v

# an existing, provisioned desktop: one area, or the P0 flows only
python e2e/host/e2e_run.py --desktop am-e2e-1 -- -v suites/test_tab_menu.py
python e2e/host/e2e_run.py --desktop am-e2e-1 -- -v -m p0
```

The results folder holds `pytest.log`, `junit.xml`, the mock's request log and, for every failed test, its
evidence: `screen.png`, `uia.txt` (every window of the app), `terminal-<n>.txt`, the profile's logs and records,
`mock.json`, `processes.txt`. A test that passes leaves nothing.

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
test checks the save, then `am.kill(); am.start()` — or `am.restart()`.

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
- A new surface: open it in a probe, `python -m amdriver dump --out Z:\e2e\out\dumps\<name>.txt`, read the names and
  the structure, then add a page object method — never pixel positions.

## What the driver knows (measured in the test desktop, 0.8.0, 2026-10-23)

- **A click is a hand's click.** The pointer rests ~150 ms on the target before the press, and the button is HELD
  ~60 ms. A press on arrival races the target's hover work, and a down and an up sent together are both queued
  before the app handles the press — Agentmaster's one-click Launch over its presets menu reads the button's LIVE
  state (`GetAsyncKeyState`), which is then already "up", so the click is lost.
- **Menus slide into place** for a few hundred ms after they open; a click read before that lands on the item above.
  `Menu` reads the items until two reads agree, and checks that a submenu belongs to the item it hovered.
- **Home ignores a click within the double-click time** of a click that opened the page or jumped back (by
  design); `click_home` spaces separate clicks past it.
- **Every tab close asks**: "Close session?" (Close · ★ Favorite & Close · Cancel) or, for a shell tab, "Close tab?"
  (Close · Cancel). The window's ✕ asks "Close this window?" (Close All Windows · Close Window · Cancel); Close All
  Windows then asks "Do you want to close all windows?", a dialog UI Automation cannot see (Enter answers it).
- **A tooltip opens on a hand's arrival**: `hover(glide=True)` moves in steps; `am.tooltips()` lists what shows (a
  rich session card has no name — read its texts).
- **Read the app's files with delete sharing.** The app saves `sessions.json`, `windows\<id>.json`, `settings.json` …
  by replacing them; a plain `open()` handle (read + write sharing, no delete) makes such a save FAIL —
  `[persist-fail] … atomic replace failed` — and nothing retries it (ROB-K1). Every read in `amdriver` goes through
  `win32.read_bytes_shared`; a test that holds a file with `open()` does it on purpose.
- **Shift+Click on a TAB switches to it** (TAB-K4), with Shift held as long as you like; the tree row's and the board
  card's Shift+Click do start a parked session in place. Wake a parked tab in place with `tabs.activate(tab)` (its
  menu's Activate Tab) or `manager.activate_all()`.
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
| MGR-K1 | MGR-05 | A session launched from the Agent Manager page is not selected on the page: Home offers no way back and the right pane asks for a selection, until a tab switch | `test_manager_page.py::test_known_defect_a_launched_session_is_not_selected_on_the_page` |
| SP-K1 | SP-05 E2 | In the + card, one click on Launch while its presets menu is open does not launch (a second click does); the page's own Launch takes one | `test_side_panel.py::test_known_defect_plus_card_launch_needs_a_second_click` |
| TCM-K1 | TCM-03 E1 | A blank tab rename is refused for the session, but the tab then shows Claude Code's own terminal title (`✳ …`) | `test_tab_menu.py::test_known_defect_blank_rename_shows_claudes_own_title` |
| TCM-K2 | TCM-05 | Tags from the tab menu leaves the keyboard in the terminal: a typed name lands in Claude's box, Esc reaches claude | `test_tab_menu.py::test_known_defect_tags_from_the_tab_menu_leave_the_keys_in_the_terminal` |
| SP-K2 | SP-04 | With Sum off, hovering a side-panel card opens no rich card (the tab's hover does, under the same input) | `test_side_panel.py::test_known_defect_sum_off_card_hover_opens_no_rich_card` |
| TAB-K1 | TAB-10 E2 | A window close can save its Claude tabs as plain shell tabs — seen once; the restart test names it when it recurs | `test_tab_menu.py::test_restart_restores_the_renamed_tab_dormant` (its record check) |
| TAB-K2 | TAB-10 E1 | A reopen whose focused tab was a shell starts the first restored Claude tab | `test_tab_menu.py::test_known_defect_a_shell_focused_reopen_starts_the_first_claude_tab` |
| TAB-K3 | TAB-10 | The quit confirmation is invisible to UI Automation (the driver presses Enter) | — |
| TAB-K4 | TAB-01 phase 4 | Shift+Click on a parked tab switches to it instead of starting it in place (its menu item names the gesture) | `test_tab_lifecycle.py::test_known_defect_shift_click_on_a_parked_tab_switches_to_it` |
| ROB-K1 | ROB-11 | A reader holding `sessions.json` without delete sharing makes the save fail; nothing re-saves until the next change | `test_robustness.py::test_known_defect_a_reader_holding_sessions_json_loses_the_save` |
