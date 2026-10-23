"""Evidence for a failing test: what the screen showed, what UI Automation saw, what the app and the mock logged.

Collected the moment a test fails, BEFORE the app is ended, into the test's own folder (Isolation.out_dir):
    screen.png         the whole desktop
    uia.txt            every window of the app (main windows and popups), as `python -m amdriver dump` writes it
    terminal-<n>.txt   the text of each window's visible terminal
    profile\\          hooks.log, autorunner.log, scanner.log, sessions.json, settings.json, windows\\*.json,
                       open-windows.json, forwarder-errors.log
    mock.json          the mock's request log (the last 300 entries) and its expectation results
    processes.txt      the app's process tree (image, pid, command line)
Every part is best effort: a part that fails writes its error into evidence-errors.txt and the rest go on, so
one broken collector never hides the others.
"""
from __future__ import annotations

import json
import shutil
import traceback
from pathlib import Path
from typing import Callable

from . import uia


def collect(app, out_dir: Path, mock=None, label: str = "") -> Path:
    """Write the evidence of the app's current state.

    Args:
        app: The App (alive or not).
        out_dir: The folder to write into (created).
        mock: The MockClient, or None.
        label: A note written into evidence-errors.txt's header (the test's name and outcome).
    Returns:
        The folder.
    """
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    errors: list[str] = []

    def part(name: str, fn: Callable[[], None]) -> None:
        """Run one collector, recording its failure instead of raising.

        Args:
            name: The collector's name.
            fn: The collector.
        Returns:
            None.
        """
        try:
            fn()
        except Exception:  # noqa: BLE001 - evidence is best effort; the error is kept beside it
            errors.append(f"--- {name}\n{traceback.format_exc()}")

    def screen() -> None:
        """The desktop's screenshot.

        Returns:
            None.
        """
        from PIL import ImageGrab
        ImageGrab.grab(all_screens=True).save(out_dir / "screen.png")

    def tree() -> None:
        """The UI Automation dump of every window of the app.

        Returns:
            None.
        """
        parts = []
        for el in uia.windows_of(app.pid, visible_only=True):
            parts.append(f"===== window 0x{el.hwnd:x} cls={el.class_name} {el.name!r}\n" + el.dump(40, 6000))
        (out_dir / "uia.txt").write_text("\n\n".join(parts), encoding="utf-8")

    def terminals() -> None:
        """Each window's visible terminal text.

        Returns:
            None.
        """
        for i, w in enumerate(app.windows()):
            try:
                text = w.terminal.text()
            except LookupError:
                continue
            (out_dir / f"terminal-{i}.txt").write_text(text, encoding="utf-8")

    def profile() -> None:
        """Copies of the profile's logs and records.

        Returns:
            None.
        """
        dst = out_dir / "profile"
        dst.mkdir(exist_ok=True)
        for name in ("hooks.log", "autorunner.log", "scanner.log", "sessions.json", "settings.json",
                     "open-windows.json", "forwarder-errors.log", "dir-colors.json"):
            src = app.profile.dir / name
            if src.exists():
                shutil.copy2(src, dst / name)
        windows = app.profile.dir / "windows"
        if windows.is_dir():
            shutil.copytree(windows, dst / "windows", dirs_exist_ok=True)

    def mock_log() -> None:
        """The mock's request log and expectations.

        Returns:
            None.
        """
        if mock is None:
            return
        data = {"requests": mock.requests()[-300:], "expectations": mock.expectations()}
        (out_dir / "mock.json").write_text(json.dumps(data, indent=1, default=str), encoding="utf-8")

    def processes() -> None:
        """The app's process tree.

        Returns:
            None.
        """
        lines = []
        for p in app.children():
            try:
                lines.append(f"{p.pid:6d} {p.name():<20} {' '.join(p.cmdline())[:400]}")
            except Exception as error:  # noqa: BLE001 - a process gone between listing and reading
                lines.append(f"{p.pid:6d} ? ({error})")
        (out_dir / "processes.txt").write_text("\n".join(lines), encoding="utf-8")

    if app.alive():
        part("screen", screen)
        part("uia", tree)
        part("terminals", terminals)
        part("processes", processes)
    part("profile", profile)
    part("mock", mock_log)
    header = f"evidence for {label}\n" if label else ""
    if errors or header:
        (out_dir / "evidence-errors.txt").write_text(header + "\n".join(errors), encoding="utf-8")
    return out_dir
