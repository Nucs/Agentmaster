"""The console guard exactly as the compiler builds it.

Reads WrapPwshScriptInConsoleGuard's two literals (kPrefix, kSuffix) out of ClaudeSpawn.cpp and concatenates their
pieces the way C++ adjacent-literal concatenation does. Only the escapes the literals use are decoded (\\ and \");
any other escape is refused, so a new one cannot silently change the text under test.

    load_template()  -> prefix + "RUN" + suffix   (drive.py / drive_claude.py replace RUN with `& <agent>`)
    python -I guard.py  prints the guard and its size (the size kHandoverPromptEscapedBudget assumes)
"""
import os
import re
import sys

SOURCE = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                                       "src", "cascadia", "TerminalApp", "AgentMaster", "ClaudeSpawn.cpp"))


def _literal(src: str, name: str) -> str:
    """Decode one `static constexpr std::wstring_view <name> = L"..." L"...";` block to the string the compiler builds.

    Args:
        src: the whole ClaudeSpawn.cpp text.
        name: the constant's name (kPrefix or kSuffix).

    Returns:
        The concatenated, unescaped literal pieces.

    Raises:
        SystemExit: the constant is missing, or a piece holds an escape other than \\\\ and \\" (refused rather than
            guessed, so the probe never tests text the compiler would not build).
    """
    match = re.search(r"static constexpr std::wstring_view " + name + r" =(.*?);\n", src, re.S)
    if not match:
        raise SystemExit(f"{name} not found in {SOURCE}")
    out = []
    for piece in re.findall(r'L"((?:[^"\\]|\\.)*)"', match.group(1)):
        i = 0
        while i < len(piece):
            if piece[i] == "\\":
                if piece[i + 1] not in '\\"':
                    raise SystemExit(f"unexpected escape \\{piece[i + 1]} in {name}")
                out.append(piece[i + 1])
                i += 2
            else:
                out.append(piece[i])
                i += 1
    return "".join(out)


def load_template() -> str:
    """Return the guard as the launch will run it, with the agent's statements marked RUN.

    The probes replace RUN with `& <agent>`, so they always test the code as written in ClaudeSpawn.cpp, never a
    hand-copied version of it.

    Returns:
        kPrefix + "RUN" + kSuffix.

    Raises:
        OSError: ClaudeSpawn.cpp cannot be read (the probe must run from a repository checkout).
        SystemExit: a literal is missing or malformed (see _literal), or the guard holds a character outside printable
            ASCII (the launch's payload is meant to stay plain ASCII).
    """
    src = open(SOURCE, encoding="utf-8").read()
    prefix, suffix = _literal(src, "kPrefix"), _literal(src, "kSuffix")
    if any(ord(c) < 0x20 or ord(c) > 0x7E for c in prefix + suffix):
        raise SystemExit("the guard holds a character outside printable ASCII")
    return prefix + "RUN" + suffix


if __name__ == "__main__":
    template = load_template()
    print(template)
    print(f"\n{len(template) - 3} chars of script around the agent's run")
