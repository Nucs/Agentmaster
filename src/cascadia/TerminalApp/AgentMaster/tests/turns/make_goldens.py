#!/usr/bin/env python3
"""make_goldens.py — writes the expected outputs TestTranscriptTurns compares the C++ port against.

TranscriptTurns.{h,cpp} is a C++ port of transcript_turns.py, the "turns view" renderer of the
am-agentmaster-cli skill (~/.claude/skills/am-agentmaster-cli/scripts/transcript_turns.py). That
script is the reference: the port must print exactly what it prints. This generator renders every
fixture in this folder at each of the five detail levels the app's Copy > Transcript submenu offers,
under the version the port implements (--preset v9), in UTC, and writes the results to
expected/<fixture>-<level>.md. The harness (tests_turns.cpp) renders the same fixtures through the
port and compares byte for byte.

The fixtures are copies of the skill's own synthetic test transcripts (scripts/tests/), so the two
test suites cover the same cases:
    fixture_*.jsonl   rendered from their lines alone, as the skill's test renders a transcript
                      without the chain feature (no folder to search, no agents to find);
    chain/, agents/   rendered through load_history, which searches their folder for the sessions
                      they were forked or plan-restarted from and the agents they spawned, with the
                      file creation times births.json pins (a checkout gives every file the same
                      arbitrary one).

Usage:
    python make_goldens.py [--script <path to transcript_turns.py>]

Run it again after the skill's renderer changes the v9 output (or when the port moves to a newer
version), review the diff of expected/, then fix the port until the harness passes.

Exit codes: 0 written · 1 the reference script can't be found or loaded.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXPECTED = HERE / "expected"
DEFAULT_SCRIPT = Path.home() / ".claude" / "skills" / "am-agentmaster-cli" / "scripts" / "transcript_turns.py"

# The version the C++ port implements. Pinned, so a newer skill version can't change the goldens
# silently: moving the port to a new version is a deliberate edit here AND in TranscriptTurns.cpp.
PRESET = "v9"

# The app's menu levels -> the reference script's level flag (Default = the steps level, which has no flag).
LEVELS: dict[str, list[str]] = {
    "minimal": ["--minimal"],
    "compact": ["--compact"],
    "default": [],
    "full": ["--full"],
    "max": ["--max"],
}

# Every fixture the port is checked on: the skill test's ALL_FIXTURES plus the two chain cases only
# its CASES table renders (the tail of a fork family, and a plan child whose parent is gone).
FIXTURES: tuple[str, ...] = (
    "fixture_session.jsonl",
    "fixture_compaction.jsonl",
    "fixture_malformed.jsonl",
    "fixture_questions.jsonl",
    "fixture_preserved.jsonl",
    "chain/a4a4a4a4-0000-4000-8000-000000000004.jsonl",
    "chain/a1a1a1a1-0000-4000-8000-000000000001.jsonl",
    "chain/b2b2b2b2-0000-4000-8000-000000000002.jsonl",
    "chain/b3b3b3b3-0000-4000-8000-000000000003.jsonl",
    "agents/c1c1c1c1-0000-4000-8000-000000000001.jsonl",
    "agents/c2c2c2c2-0000-4000-8000-000000000002.jsonl",
)


def load_reference(script: Path):
    """Imports transcript_turns.py from an explicit path (it is not an installed package).

    Args:
        script: The reference script.

    Returns:
        The loaded module.

    Raises:
        FileNotFoundError: The script doesn't exist.
        ImportError: It can't be loaded as a module.
    """
    if not script.is_file():
        raise FileNotFoundError(f"reference script not found: {script}")
    spec = importlib.util.spec_from_file_location("transcript_turns", script)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {script} as a module")
    module = importlib.util.module_from_spec(spec)
    # Registered BEFORE it runs: @dataclass resolves each class's module through sys.modules while the
    # module body executes, and an unregistered module fails there ('NoneType' has no '__dict__').
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def golden_name(fixture: str, level: str) -> str:
    """The expected-output file name for a fixture at a level: the fixture's stem, its folder (if any)
    as a prefix, and the level — e.g. "chain-a4a4a4a4-default.md".

    Args:
        fixture: The fixture's path relative to this folder.
        level: A LEVELS key.

    Returns:
        The file name (no folder).
    """
    path = Path(fixture)
    stem = path.stem.split("-")[0] if path.parent.name else path.stem  # chain ids shorten to their first block
    prefix = f"{path.parent.name}-" if path.parent.name else ""
    return f"{prefix}{stem}-{level}.md"


def render(tt, fixture: Path, level: str) -> str:
    """Renders one fixture at one level exactly as the skill's own test harness does (render_case).

    Args:
        tt: The reference module.
        fixture: The fixture's full path.
        level: A LEVELS key.

    Returns:
        The Markdown the reference prints.

    Raises:
        KeyError: A chain folder holds a transcript births.json doesn't list.
    """
    ns = tt.build_parser().parse_args([str(fixture), "--utc", "--preset", PRESET, *LEVELS[level]])
    opts = tt.resolve_options(ns)
    births_file = fixture.parent / "births.json"
    if births_file.is_file():
        births = json.loads(births_file.read_text(encoding="utf-8"))

        def born(path: Path) -> float:
            """The pinned creation time of a chain fixture."""
            return tt.parse_ts(births[path.stem]).timestamp()

        return tt.render_history(tt.load_history(fixture, opts, born=born), opts, utc=True)
    return tt.render(tt.load_lines(fixture), opts, utc=True)


def main() -> int:
    """Writes every golden.

    Returns:
        0 when every golden was written, 1 when the reference script can't be loaded.
    """
    parser = argparse.ArgumentParser(description="Write the expected outputs of the TranscriptTurns port.")
    parser.add_argument("--script", type=Path, default=DEFAULT_SCRIPT, help=f"transcript_turns.py (default: {DEFAULT_SCRIPT})")
    args = parser.parse_args()
    try:
        tt = load_reference(args.script)
    except (FileNotFoundError, ImportError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    EXPECTED.mkdir(exist_ok=True)
    for fixture in FIXTURES:
        for level in LEVELS:
            text = render(tt, HERE / fixture, level)
            out = EXPECTED / golden_name(fixture, level)
            out.write_text(text, encoding="utf-8", newline="\n")
            print(f"wrote {out.name} ({len(text.encode('utf-8'))} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
