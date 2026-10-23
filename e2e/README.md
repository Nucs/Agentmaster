# Agentmaster end-to-end suite

Agentmaster driven like a user — real clicks, keys and menus in a disposable Windows 11 desktop — while every
Claude Code it starts talks to a **scripted model** whose answers are written into the prompts. The design is
[`doc/agentmaster/E2E_TESTING.md`](../doc/agentmaster/E2E_TESTING.md); WHAT the suite must prove is the flow catalog,
[`doc/agentmaster/flows/`](../doc/agentmaster/flows/README.md). This folder is the code.

| Folder | What it is |
|---|---|
| [`mockllm/`](mockllm) | The scripted model: an Anthropic Messages API on `127.0.0.1:8787` that plays `[[mock …]]` scripts (say, think, tool calls, holds, errors, drops), classifies Claude Code's side requests, logs every request, and has a control API (`/_mock/*`). Its unit tests run anywhere: `python -m pytest mockllm/tests`. |
| [`amdriver/`](amdriver) | The driver: real input (`inputs`), UI Automation (`uia`), waits, the app's process (`app`), its profile's files and logs (`profile`), Claude Code's config and transcripts (`claude`), a tab's terminal (`terminal`), sessions (`session`), page objects (`ui/`), and failure evidence (`evidence`). `python -m amdriver dump --out FILE` writes what UI Automation sees of a running Agentmaster. |
| [`suites/`](suites) | The tests (pytest), one file per flow area, and the fixtures (`conftest.py`). |
| [`guest/`](guest) | What runs in the test desktop: `provision.ps1` (Agentmaster, `claude.exe` 2.1.291, Git, PowerShell 7, the Python venv, hosts, environment, crash dumps) and `run_suite.py` (copy the suite to C:, run pytest, zip the results onto the share). |
| [`host/`](host) | `e2e_run.py`: stage the suite into a desktop, provision, run, collect the results to `Q:\agentmaster-e2e\runs\<run>\`. |

## Running it

```text
# a new desktop, provisioned, the whole suite, deleted afterwards (~25 min for the 30 tests of 2026-10-23)
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
`s1` plus a session in `t2`). More sessions: `am.launch("t3")`; a git folder first: `am.iso.git_folder("t1",
branch="feature/x")`.

**The page objects** (`am.window` is the first window; `am.windows()` all of them):

| Object | Reads | Does |
|---|---|---|
| `window.tabs` | `titles()`, `items()` (title, selected, index), `selected()`, `find(title)`, `rename_box()` | `select(title)`, `context_menu(title)` → Menu, `rename(title, new)`, `close(title, answer)`, `wait_tab / wait_gone / wait_selected` |
| `Menu` (`amdriver.ui.menus`) | `names()`, `items()` (name, enabled, is_sub, expanded), `has`, `enabled` | `choose(name)`, `submenu(name)` → Menu, `choose_path("Close", "Close other tabs")`, `close()`; `Menu.open_menus(app)`, `Menu.dismiss_all(app)` |
| `window.manager` | `showing()`, `home_name()`, `columns()` / `cards()` / `card(title)`, `tree_rows()` / `tree_row(label)`, `board_scope()`, `tree_scope()`, `right_pane_texts()` | `open()`, `click_home(count)`, `launch(folder)`, `type_folder`, `clear()`, `show_all()`, `toggle_board_scope()`, `show_tab("Prompt Queue")`; a Card: `click`, `double_click`, `right_click` |
| `window.side_panel` | `cards()` / `card(title)` (title, state word), `titles()`, `count_line()`, `scope()`, `sum_on()`, `dock_side()`, `collapsed()`, `summary_title()` / `summary_texts()` | `button(name).click()` ('Sum', 'LOCAL', '⌖', '⇄', '«'), `toggle_scope()`, `set_sum(on)`, `open_launcher()` → LauncherCard (`box`, `folder_list`, `type_folder`, `launch`) |
| `window.terminal` | `text()`, `lines()`, `input_box()` (Claude's box and its draft) | `focus()`, `type(text)`, `submit(prompt)`, `wait_input_box()`, `wait_text(regex)` |
| `Dialog` (`amdriver.ui.dialogs`) | `text()`, `buttons()` | `Dialog.wait(app, title=…)`, `press(name)`; `MessageBox` for Win32 boxes |
| `Session` (`amdriver.session`) | `record()` (sessions.json), `title`, `state`, `claude_pids()`, `tab()` | `select()`, `ask(prompt, wait="turn"/"sent"/"none")`, `wait_state(*states)` |

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
  (Close · Cancel). The window's ✕ asks "Close this window?" (Close All Windows · Close Window · Cancel).
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
