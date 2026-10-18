"""Put generated prompt macros into the source: replace the system prompts' macro block of SessionSearchTools.cpp (from
"#define AM_PROMPT_INTRO" to the last "#undef AM_PROMPT_WORKFLOWS_TAIL") with gen_prompt.py's macros, the two
compositions (compose.cpp.txt: kSystemPrompt and kDatabaseSystemPrompt) and the #undefs (SESSIONS.md 9.15).

    python -I splice_prompt.py <macros.cpp.txt>

The target is this repository's src/cascadia/TerminalApp/AgentMaster/SessionSearchTools.cpp (two folders up from this
script). The file is read and written as UTF-8 bytes, its line endings kept. Prints the number of #defines and #undefs
it now holds (23 and 23 for the current pieces). Raises SystemExit on a wrong command line or when the block's markers
are not found (the file is then left as it was), and OSError when a file cannot be read or written.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(HERE, "..", "..", "src", "cascadia", "TerminalApp", "AgentMaster", "SessionSearchTools.cpp")


def main():
    """Splice (see the module's docstring).

    Raises:
        SystemExit: on a wrong command line, or when the macro block's start or end marker is missing.
        OSError: when a file cannot be read or written.
    """
    if len(sys.argv) != 2:
        raise SystemExit("usage: python -I splice_prompt.py <macros.cpp.txt>")
    s = open(TARGET, encoding="utf-8", newline="").read()
    a = s.find("#define AM_PROMPT_INTRO " + chr(92))
    end_marker = "#undef AM_PROMPT_WORKFLOWS_TAIL\n"
    b = s.find(end_marker)
    if a <= 0 or b <= a:
        raise SystemExit("the macro block was not found in " + TARGET + " (start " + str(a) + ", end " + str(b) + "): nothing changed")
    macros = open(sys.argv[1], encoding="utf-8").read()
    compose = open(os.path.join(HERE, "compose.cpp.txt"), encoding="utf-8").read()
    s = s[:a] + macros + compose + s[b + len(end_marker):]
    open(TARGET, "w", encoding="utf-8", newline="").write(s)
    print("ok", s.count("#define AM_PROMPT_"), s.count("#undef AM_PROMPT_"))


main()
