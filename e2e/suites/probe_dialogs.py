"""PROBE (not a test of the suite): what UI Automation sees of each kind of ContentDialog (flows TAB-K3).

Not collected by a full run (its name does not start with `test_`); run it by name, on the build you want to look at:

    python e2e/host/e2e_run.py --desktop <name> -- -v suites/probe_dialogs.py

It asserts only that the app survived; the point is the record it writes into the test's evidence folder (kept for a
passing test too): per case `screen.png` + `uia.txt` (evidence.collect) and one entry of `notes.json` — whether
`Dialog.wait` finds the title and with which buttons, the island's direct children, a 3 x 4 grid of hit tests over the
window's middle, the focused element, the app's popup windows, and the title searched in the window's whole tree.

The cases, one after another (each answered with Escape):
  1. a dialog built in code — the shell tab's "Close tab?" (the visible baseline on every build);
  2. the large-paste warning ("Warning"; > 5 KiB, no newline) — DECLARED in TerminalPage.xaml on 0.8.0;
  3. About, from the command palette — declared on 0.8.0;
  4. the quit confirmation from the command palette ("Quit the Terminal"), with no dialog before it;
  5. the quit confirmation after "Close this window?" → Close All Windows (the path TAB-K3 was found on).

What it is for: on 0.8.0, cases 2–5 should show the TAB-K3 root cause (a dialog declared in XAML is shown by WinUI with
only its layout moved into a popup nested inside the page — TransplantedRootInPopup — and UI Automation does not reach
it there); on a build with the fix (every page dialog built in code) all five should read like case 1.
"""
from __future__ import annotations

import json
import time

from amdriver import evidence, inputs, uia, win32
from amdriver.ui.dialogs import Dialog

SHELL = "Administrator: Windows PowerShell"


def _describe(el) -> dict | None:
    """A UI Automation element as plain data.

    Args:
        el: The element, or None.
    Returns:
        Its role, class, name, automation id and child count (None for None).
    """
    if el is None:
        return None
    try:
        kids = len(el.children())
    except Exception as e:  # noqa: BLE001 - a probe records what it can
        kids = f"error: {e}"
    return {"role": el.role, "class": el.class_name, "name": el.name, "aid": el.automation_id, "children": kids}


def _record(am, w, iso, label: str, title: str, notes: list) -> None:
    """Capture the state while a dialog should be open.

    Args:
        am: The App.
        w: The MainWindow.
        iso: The test's Isolation.
        label: The case's folder name.
        title: The dialog's title.
        notes: The list to append the case's entry to.
    Returns:
        None.
    """
    time.sleep(1.5)  # the dialog's open transition: a probe looks once, after it, instead of waiting for a condition
    entry: dict = {"label": label, "title": title}
    try:
        found = Dialog.wait(am, title=title, timeout_s=3)
        entry["dialog_wait"] = repr(found)
        try:
            entry["dialog_buttons"] = found.buttons()
        except Exception as e:  # noqa: BLE001
            entry["dialog_buttons"] = f"error: {e}"
    except Exception as e:  # noqa: BLE001
        entry["dialog_wait"] = f"NOT FOUND: {type(e).__name__}: {e}"
    try:
        entry["input_site_children"] = [_describe(c) for c in w.input_site().children()]
    except Exception as e:  # noqa: BLE001
        entry["input_site_children"] = f"error: {e}"
    try:
        hits = []
        left, top, width, height = w.rect()
        for fx in (0.35, 0.5, 0.65):
            for fy in (0.3, 0.45, 0.6, 0.72):
                x, y = int(left + width * fx), int(top + height * fy)
                hits.append({"at": [round(fx, 2), round(fy, 2)], "el": _describe(uia.at_point(x, y))})
        entry["hits"] = hits
    except Exception as e:  # noqa: BLE001
        entry["hits"] = f"error: {e}"
    try:
        entry["focused"] = _describe(uia.focused())
    except Exception as e:  # noqa: BLE001
        entry["focused"] = f"error: {e}"
    try:
        entry["popups"] = [{"el": _describe(p), "dump": p.dump(max_depth=4, max_nodes=60)} for p in am.popups()]
    except Exception as e:  # noqa: BLE001
        entry["popups"] = f"error: {e}"
    try:
        # The title anywhere in the window's tree (a transplanted dialog's text may sit under the page instead).
        entry["title_anywhere"] = [_describe(h) for h in w.element().find_all(name=title, max_depth=40)]
    except Exception as e:  # noqa: BLE001
        entry["title_anywhere"] = f"error: {e}"
    evidence.collect(am, iso.out_dir / label, None, label=label)
    notes.append(entry)
    (iso.out_dir / "notes.json").write_text(json.dumps(notes, indent=2, ensure_ascii=False), encoding="utf-8")


def _palette(w, command: str) -> None:
    """Run a command-palette entry by typing its name.

    Args:
        w: The MainWindow.
        command: The entry's name.
    Returns:
        None.
    """
    w.focus()
    w.terminal.focus()
    inputs.keys("ctrl+shift+p")
    time.sleep(1.0)  # the palette's open: a probe types once it shows, it does not assert on it
    inputs.type_text(command)
    time.sleep(1.0)  # the palette's filter settles on the typed name
    inputs.keys("enter")


def test_probe_dialog_uia(am, iso):
    """Open the five dialogs one after another and record what UI Automation sees of each (see the module doc).

    Args:
        am: The App.
        iso: The Isolation.
    Returns:
        None.
    """
    notes: list = []
    w = am.window
    w.focus()
    w.tabs.wait_tab(SHELL)

    # 1. Built in code on every build (the visible baseline): the shell tab's "Close tab?".
    w.tabs.close(SHELL, answer=None)
    _record(am, w, iso, "1-close-tab-code-built", "Close tab?", notes)
    inputs.keys("escape")
    time.sleep(1.0)  # the dialog's close

    # 2. The large-paste warning, opened on its own. One line of 6000 characters: over the 5 KiB threshold, and no
    #    newline, so the multi-line warning cannot come first (in the default Automatic mode it is skipped anyway
    #    while pwsh has bracketed paste on; the large-paste check ignores bracketed paste).
    win32.set_clipboard_text("x" * 6000)
    w.focus()
    w.terminal.focus()
    inputs.keys("ctrl+shift+v")
    _record(am, w, iso, "2-large-paste", "Warning", notes)
    inputs.keys("escape")
    time.sleep(1.0)

    # 3. About (a ContentDialog subclass), from the command palette.
    _palette(w, "Open about dialog")
    _record(am, w, iso, "3-about", "About", notes)
    inputs.keys("escape")
    time.sleep(1.0)

    # 4. The quit confirmation on its own, from the command palette: no dialog was shown before it.
    _palette(w, "Quit the Terminal")
    _record(am, w, iso, "4-quit-from-palette", "Do you want to close all windows?", notes)
    inputs.keys("escape")
    time.sleep(1.0)

    # 5. The quit confirmation after "Close this window?" → Close All Windows: the path TAB-K3 was found on.
    win32.set_foreground(w.hwnd)
    win32.post_close(w.hwnd)
    Dialog.wait(am, title="Close this window?", timeout_s=10).press("Close All Windows")
    _record(am, w, iso, "5-quit-after-close-window", "Do you want to close all windows?", notes)
    inputs.keys("escape")
    time.sleep(1.0)
    assert am.alive(), "the app quit (an Escape did not cancel the quit confirmation)"
