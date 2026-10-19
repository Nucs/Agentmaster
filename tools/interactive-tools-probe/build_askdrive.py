"""Builds askdrive.exe, the live end-to-end driver of MESSAGING.md §21.30 (INTERACTIVE_TOOLS.md §10.4), from the working tree.

The driver links the engine sources the m5 suite compiles (the `..\\*.cpp` items of tests/run-m5-tests.bat's cl line, so
the list never drifts from the suite's), compiled with the suite's flags. Nothing is written into the source tree: the
objects and the exe go to the output folder.

Usage:
    python -I build_askdrive.py <output folder>      # prints the exe's path; exit code = cl's

Needs Visual Studio 2022 (vcvars64.bat) and the Windows SDK's icu.lib, like the m5 suite.
"""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
TESTS = REPO / "src" / "cascadia" / "TerminalApp" / "AgentMaster" / "tests"
VCVARS = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"


def main():
    """Compile and link the driver into the output folder.

    Returns:
        cl's exit code (0 = built).
    Raises:
        SystemExit with a usage line when no output folder is given; StopIteration when the runner has no cl line.
    """
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    out = Path(sys.argv[1]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    runner = (TESTS / "run-m5-tests.bat").read_text(encoding="utf-8", errors="replace")
    cl_line = next(l for l in runner.splitlines() if l.startswith("cl /std:c++20"))
    engine = [t for t in cl_line.split() if t.startswith("..\\") and t.endswith(".cpp")]
    # The engine's relative paths resolve from the tests folder (the suite's own working directory); /Fo sends the objects out.
    cmd = ('cl /std:c++20 /EHsc /nologo /W3 /utf-8 /MP4 /DUNICODE /D_UNICODE /Fo"%s\\\\" /Fe:"%s" "%s" %s '
           "ole32.lib user32.lib oleaut32.lib icu.lib shell32.lib" % (out, out / "askdrive.exe", HERE / "askdrive.cpp", " ".join(engine)))
    bat = out / "build-askdrive.bat"
    with open(bat, "w", encoding="utf-8", newline="\r\n") as f:
        f.write('@echo off\r\ncall "%s" >nul 2>&1\r\ncd /d "%s"\r\n%s\r\n' % (VCVARS, TESTS, cmd))
    r = subprocess.run(["cmd", "/c", str(bat)], capture_output=True)
    text = r.stdout.decode("utf-8", "replace") + r.stderr.decode("utf-8", "replace")
    errors = [l for l in text.splitlines() if " error " in l or "error C" in l or "error LNK" in l]
    print("\n".join(errors[:40]))
    if r.returncode == 0:
        print("built", out / "askdrive.exe")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
