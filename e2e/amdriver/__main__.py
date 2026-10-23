"""The locator explorer: dump what UI Automation sees of a running Agentmaster, to write page objects from facts.

Usage (in the test guest, with the suite's venv):
    python -m amdriver windows [--pid N]               list Agentmaster's top-level windows (class, title, size)
    python -m amdriver dump --out FILE [--pid N]       every visible window of the process (main windows AND the
                                                       popup windows that hold menus, flyouts and tooltips),
                                                       indented, one element per line
    python -m amdriver focused                         the element with keyboard focus

Without --pid the newest running Agentmaster.exe is used. A dump is the first step of every new page object:
open the surface by hand (or with a probe), dump, and read the element names, classes and order from the file.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import uia, win32
from .app import find_agentmaster_pids


def _pick_pid(explicit: int | None) -> int:
    """The process to inspect.

    Args:
        explicit: A pid given on the command line, or None.
    Returns:
        The pid.
    Raises:
        SystemExit: When no Agentmaster.exe runs and none was given.
    """
    if explicit:
        return explicit
    pids = find_agentmaster_pids()
    if not pids:
        raise SystemExit("no Agentmaster.exe is running")
    return pids[-1]


def _cmd_windows(args: argparse.Namespace) -> int:
    """Print the process's top-level windows.

    Args:
        args: The parsed command line (pid).
    Returns:
        The exit code.
    """
    pid = _pick_pid(args.pid)
    for w in win32.top_windows(pid):
        x, y, cx, cy = w.rect() if w.alive() else (0, 0, 0, 0)
        print(f"0x{w.hwnd:x} {'visible' if w.visible else 'hidden '} {w.cls:<36} @{x},{y} {cx}x{cy} {w.title!r}")
    return 0


def _cmd_dump(args: argparse.Namespace) -> int:
    """Write every visible window of the process to a file.

    Args:
        args: The parsed command line (pid, out, depth, nodes).
    Returns:
        The exit code.
    """
    pid = _pick_pid(args.pid)
    parts = []
    for el in uia.windows_of(pid, visible_only=True):
        parts.append(f"===== window 0x{el.hwnd:x} cls={el.class_name} {el.name!r}\n"
                     + el.dump(max_depth=args.depth, max_nodes=args.nodes))
    text = "\n\n".join(parts) + "\n"
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(text, encoding="utf-8")
    print(f"wrote {len(text)} chars, {text.count(chr(10))} lines to {args.out}")
    return 0


def _cmd_focused(_args: argparse.Namespace) -> int:
    """Print the focused element and its ancestors.

    Args:
        _args: The parsed command line (unused).
    Returns:
        The exit code.
    """
    el = uia.focused()
    depth = 0
    while el is not None and depth < 30:
        print(el.line(depth))
        el = el.parent()
        depth += 1
    return 0


def main(argv: list[str] | None = None) -> int:
    """The command line.

    Args:
        argv: The arguments (default: sys.argv[1:]).
    Returns:
        The exit code.
    """
    parser = argparse.ArgumentParser(prog="python -m amdriver", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("windows")
    w.add_argument("--pid", type=int)
    d = sub.add_parser("dump")
    d.add_argument("--pid", type=int)
    d.add_argument("--out", required=True)
    d.add_argument("--depth", type=int, default=40)
    d.add_argument("--nodes", type=int, default=6000)
    sub.add_parser("focused")
    args = parser.parse_args(argv)
    return {"windows": _cmd_windows, "dump": _cmd_dump, "focused": _cmd_focused}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
