"""Search the JavaScript bundled inside claude.exe (a Bun single-file binary) for literal strings.

Claude Code ships as one executable whose JS chunks are plain text from about byte 200,000,000 on (the
earlier hits of a string are usually its copy in a bytecode string table, which is unreadable). Minified
names are local to each chunk and change with every release, so the way back to a function after an
upgrade is a quoted string it holds — e.g. "executePermissionRequestHooks called for tool", "Exit plan
mode?", "Your questions have been answered". doc/agentmaster/INTERACTIVE_TOOLS.md §10 lists the strings and
the 2.1.291 offsets.

Read-only: the binary is only opened for reading.

Usage:
    python -I grepbin.py <claude.exe> <before> <after> <max-hits> <needle> [<needle> ...]
    (prints each hit's byte offset and <before>/<after> bytes of context around it)
"""
import sys


def show_hits(data, needle, before, after, max_hits):
    """Print up to `max_hits` occurrences of `needle` in `data`, each with its offset and context.

    Args:
        data: the whole binary, as bytes.
        needle: the literal to find, as bytes.
        before: bytes of context to print before each hit.
        after: bytes of context to print after each hit.
        max_hits: how many hits to print; the total count is printed either way.
    Returns:
        The total number of occurrences (printed hits may be fewer).
    """
    print(f"===== {needle.decode('utf-8')!r}")
    hits = 0
    start = 0
    while hits < max_hits:
        at = data.find(needle, start)
        if at < 0:
            break
        lo = max(0, at - before)
        hi = min(len(data), at + len(needle) + after)
        print(f"--- @{at}")
        # Undecodable bytes (a bytecode table, a neighbouring binary blob) print as U+FFFD instead of failing.
        print(data[lo:hi].decode("utf-8", "replace"))
        hits += 1
        start = at + 1
    total = data.count(needle)
    print(f"(shown {hits} of {total})")
    return total


def main():
    """Parse the arguments, read the binary once, and print the hits of every needle.

    Returns:
        0 when every needle was found at least once, 1 otherwise (so a script can tell a renamed string).
    Raises:
        OSError when the binary cannot be read; ValueError on non-numeric sizes.
    """
    # -I ignores PYTHONIOENCODING, and a Windows console code page cannot print the bundle's Unicode.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if len(sys.argv) < 6:
        print(__doc__)
        return 1
    exe, before, after, max_hits = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    with open(exe, "rb") as fh:
        data = fh.read()
    missing = 0
    for needle in sys.argv[5:]:
        if show_hits(data, needle.encode("utf-8"), before, after, max_hits) == 0:
            missing += 1
    return 0 if missing == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
