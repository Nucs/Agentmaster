"""Generate the C++ macro block of the AI search's system prompts (SessionSearchTools.cpp, "the system prompts") from
pieces.txt, and check that the PAGE prompt it composes is EXACTLY a prompt file the eval scored (SESSIONS.md 9.15).

    python -I gen_prompt.py <scored prompt.txt> <out dir>

It writes <out dir>/macros.cpp.txt (the macros, for splice_prompt.py) and the two composed prompts,
<out dir>/rendered_page.txt and <out dir>/rendered_db.txt, and prints whether the page prompt equals the scored file and
whether either prompt holds a non-ASCII character (the source is ASCII; m5 checks the built prompts too). Keep <out dir>
outside the repository. To make a new candidate from edited pieces: run it with the old file, take rendered_page.txt as
the candidate, eval it with harness_live.exe --agent-prompt-file, and when it wins run it again with that file (the
check then passes) and splice.

pieces.txt: "@@NAME" lines start pieces; the blank line(s) before the next "@@" only separate pieces. A piece's text is
its lines + "\\n"; the pieces in BLANK_AFTER also end with a blank line (a paragraph break to what follows), the others
run straight on (a numbered list continued by the next piece). "{{READ_BODY}}" (at the end of a line) becomes the
AM_PROMPT_READ_BODY macro, whose own text ends in "\\n". PAGE / DB are the two prompts' piece orders; ORDER is the order
of the #defines (every piece once). Raises OSError for a missing file and KeyError for a piece the orders name that
pieces.txt lacks.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BS = chr(92)
QUOTE = chr(34)

BLANK_AFTER = {"INTRO", "BEHAVIORS", "PROC_TAIL", "TOOLS_HEAD", "TOOLS_PAGE", "TOOLS_DATABASE", "SHOW_AND_TABS"}
PAGE = ["INTRO", "BEHAVIORS", "PROC_HEAD", "PROC_NET_PAGE", "PROC_TAIL", "TOOLS_HEAD", "TOOLS_PAGE", "SHOW_AND_TABS", "WORKFLOWS_HEAD",
        "WORKFLOW_3_PAGE", "WORKFLOW_4_PAGE", "WORKFLOWS_5_TO_7", "WORKFLOW_8_PAGE", "WORKFLOWS_9_TO_15", "WORKFLOW_16_PAGE", "WORKFLOWS_TAIL"]
DB = ["INTRO", "BEHAVIORS", "PROC_HEAD", "PROC_NET_DATABASE", "PROC_TAIL", "TOOLS_HEAD", "TOOLS_DATABASE", "SHOW_AND_TABS", "WORKFLOWS_HEAD",
      "WORKFLOW_3_DATABASE", "WORKFLOW_4_DATABASE", "WORKFLOWS_5_TO_7", "WORKFLOW_8_DATABASE", "WORKFLOWS_9_TO_15", "WORKFLOW_16_DATABASE", "WORKFLOWS_TAIL"]
ORDER = ["INTRO", "BEHAVIORS", "PROC_HEAD", "PROC_NET_PAGE", "PROC_NET_DATABASE", "PROC_TAIL", "TOOLS_HEAD", "READ_BODY", "TOOLS_PAGE", "TOOLS_DATABASE",
         "SHOW_AND_TABS", "WORKFLOWS_HEAD", "WORKFLOW_3_PAGE", "WORKFLOW_3_DATABASE", "WORKFLOW_4_PAGE", "WORKFLOW_4_DATABASE", "WORKFLOWS_5_TO_7",
         "WORKFLOW_8_PAGE", "WORKFLOW_8_DATABASE", "WORKFLOWS_9_TO_15", "WORKFLOW_16_PAGE", "WORKFLOW_16_DATABASE", "WORKFLOWS_TAIL"]


def read_pieces(path):
    """Read pieces.txt into its named pieces.

    Args:
        path: the pieces file.

    Returns:
        A dict name -> text, each text ending in "\\n" (and "\\n\\n" for a piece in BLANK_AFTER).
    """
    pieces = {}
    name = None
    buf = []

    def close():
        """Store the piece being read: its lines without the separating blank ones, plus its ending."""
        while buf and buf[-1] == "":
            buf.pop()
        text = "\n".join(buf) + "\n"
        if name in BLANK_AFTER:
            text += "\n"
        pieces[name] = text

    for line in open(path, encoding="utf-8").read().split("\n"):
        m = re.match(r"^@@([A-Z0-9_]+)$", line)
        if m:
            if name:
                close()
            name = m.group(1)
            buf = []
            continue
        buf.append(line)
    if name:
        close()
    return pieces


def render(pieces, order):
    """Compose one prompt as the C++ compiles it.

    Args:
        pieces: read_pieces' dict.
        order: PAGE or DB.

    Returns:
        The prompt's text, with {{READ_BODY}} replaced by the READ_BODY piece (its trailing line break stands for the
        line's own end).
    """
    body = pieces["READ_BODY"].rstrip("\n")
    return "".join(pieces[k].replace("{{READ_BODY}}", body) for k in order)


def literal(text):
    """A text's body as a C++ wide string literal (backslashes, quotes and line breaks escaped).

    Args:
        text: the text.

    Returns:
        The escaped text, without the L"" around it.
    """
    return text.replace(BS, BS + BS).replace(QUOTE, BS + QUOTE).replace("\n", BS + "n")


def chunks(text, width=118):
    """Cut a text into pieces of at most about `width` characters, each cut after a space, for one literal per line.

    Args:
        text: the text (one line of a piece, its line break included).
        width: the longest piece wanted.

    Returns:
        The pieces in order; their concatenation is `text`.
    """
    parts = []
    while len(text) > width:
        cut = text.rfind(" ", 0, width)
        if cut <= 0:
            cut = width - 1
        parts.append(text[:cut + 1])
        text = text[cut + 1:]
    if text:
        parts.append(text)
    return parts


def macro(name, text):
    """One piece as its #define AM_PROMPT_<name>: adjacent wide literals, one per line, AM_PROMPT_READ_BODY spliced in.

    Args:
        name: the piece's name.
        text: the piece's text.

    Returns:
        The #define's C++ text, ending in a line break.
    """
    lines = []
    if name == "READ_BODY":
        text = text.rstrip("\n") + "\n"
    for seg in re.split(r"(?<=\n)", text):
        if not seg:
            continue
        if "{{READ_BODY}}" in seg:
            before, after = seg.split("{{READ_BODY}}", 1)
            # The macro's text ends in its own "\n", which stands for this line's end.
            after = after[1:] if after.startswith("\n") else after
            for part in chunks(before)[:-1]:
                lines.append("    L" + QUOTE + literal(part) + QUOTE)
            last = chunks(before)[-1] if before else ""
            lines.append(("    L" + QUOTE + literal(last) + QUOTE + " " if last else "    ") + "AM_PROMPT_READ_BODY")
            if after:
                lines.append("    L" + QUOTE + literal(after) + QUOTE)
            continue
        for part in chunks(seg):
            lines.append("    L" + QUOTE + literal(part) + QUOTE)
    return "#define AM_PROMPT_" + name + " " + BS + "\n" + (" " + BS + "\n").join(lines) + "\n"


def main():
    """Generate, check and write (see the module's docstring).

    Raises:
        SystemExit: on a wrong command line (with the usage line).
        OSError: when a file cannot be read or written.
        KeyError: when an order names a piece that pieces.txt lacks.
    """
    if len(sys.argv) != 3:
        raise SystemExit("usage: python -I gen_prompt.py <scored prompt.txt> <out dir>")
    winning = open(sys.argv[1], encoding="utf-8").read()
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    pieces = read_pieces(os.path.join(HERE, "pieces.txt"))
    page = render(pieces, PAGE)
    if page != winning:
        i = next((k for k in range(min(len(page), len(winning))) if page[k] != winning[k]), min(len(page), len(winning)))
        print("PAGE PROMPT DIFFERS at", i, "of", len(page), "vs", len(winning))
        print("  pieces :", repr(page[max(0, i - 100):i + 100]))
        print("  scored :", repr(winning[max(0, i - 100):i + 100]))
    else:
        print("page prompt == scored prompt:", len(page), "characters")
    db = render(pieces, DB)
    print("database prompt:", len(db), "characters; contains 'deep':", "deep" in db)
    for label, text in (("page", page), ("database", db)):
        bad = [c for c in text if not (c == "\n" or 32 <= ord(c) < 127)]
        print(label, "non-ASCII:", bad[:5])
    out = "".join(macro(k, pieces[k]) + "\n" for k in ORDER)
    open(os.path.join(out_dir, "macros.cpp.txt"), "w", encoding="utf-8", newline="").write(out)
    open(os.path.join(out_dir, "rendered_page.txt"), "w", encoding="utf-8", newline="").write(page)
    open(os.path.join(out_dir, "rendered_db.txt"), "w", encoding="utf-8", newline="").write(db)
    print("wrote", os.path.join(out_dir, "macros.cpp.txt"), len(out), "characters of C++")


main()
