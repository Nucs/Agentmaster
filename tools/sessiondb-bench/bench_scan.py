# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Scan benchmarks on the STAGING layout of ingest.py (SESSIONS_DB.md sections 1.2, 6.2 and 14).

Prints the text volume per block role, how many blocks are fork copies, and the latency of the
searches that need no text index (the session list, title / folder / branch terms, touched paths,
typed prompts) plus the brute-force agent / tool text scan an index has to beat. Read-only.

The staging layout interleaves prompts with every other block and folds case per row, which is what
makes its prompt scan slow (186-273 ms); build_layout.py + bench_layout.py measure the clustered,
pre-folded layout the design uses (14-37 ms).

Usage: python bench_scan.py <staging db> [threads, default 4]
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # `python -I` drops the script folder
import bench_common as bc  # noqa: E402


def sizes(con) -> None:
    """Print the per-role block counts and text sizes, fork duplication and session windows.

    Timestamps are converted to epoch milliseconds in SQL: fetching TIMESTAMPTZ values into Python
    needs the pytz module, which a bare venv lacks.

    Args:
        con: the open staging database.
    """
    print("== sizes per role (role, blocks, MB, MB capped at 16 KB, median, p99, max chars)")
    for row in con.execute("""select role, count(*), round(sum(strlen(text))/1e6,1), round(sum(least(strlen(text), 16384))/1e6,1),
                                     round(quantile_cont(strlen(text), 0.5)), round(quantile_cont(strlen(text), 0.99)), max(strlen(text))
                              from doc group by role order by role""").fetchall():
        print("  ", row)
    print("== duplication: blocks with a uuid, distinct (uuid, block)")
    print("  ", con.execute("select count(*), count(distinct (uuid, bi)) from doc where uuid is not null").fetchall())
    print("== sessions (all, active in 30 d, in 90 d)",
          con.execute("""select count(*), count(*) filter (where last_ts >= now() - interval 30 day),
                                count(*) filter (where last_ts >= now() - interval 90 day) from session""").fetchall())


def metadata(con) -> None:
    """Time the T0/T1 searches: the session list, title / folder / branch terms, touched paths.

    Args:
        con: the open staging database.
    """
    print("== browse + metadata (T0, T1)")
    listing = "select sid, coalesce(custom_title, ai_title, summary) t, cwd, branch, epoch_ms(first_ts), epoch_ms(last_ts) from session"
    bc.timed(con, "list 30d sorted", listing + " where last_ts >= now() - interval 30 day order by last_ts desc")
    bc.timed(con, "list all-time sorted", listing + " order by last_ts desc")
    for term in ("agentmaster", "numsharp", "release"):
        bc.timed(con, f"title/cwd/branch contains '{term}' (all time)", """
            select sid from session
            where contains(lower(coalesce(custom_title,'') || ' ' || coalesce(ai_title,'') || ' ' || coalesce(summary,'')), ?)
               or contains(lower(coalesce(cwd,'')), ?) or contains(lower(coalesce(branch,'')), ?)""", [term, term, term])
    bc.timed(con, "paths: contains 'terminalpage' (all time)", "select distinct sid from path where contains(lower(p), 'terminalpage')")


def prompts(con) -> None:
    """Time the T2 typed-prompt scan on the staging layout (interleaved rows, lower() per row).

    Args:
        con: the open staging database.
    """
    print("== typed prompts (T2, role 1, not meta) on the staging layout")
    for term in ("release", "string", "duckdb", "tab drag", "e"):
        bc.timed(con, f"prompts contains '{term}'", "select sid, count(*) from doc where role = 1 and not meta and contains(lower(text), ?) group by sid", [term])
    bc.timed(con, "prompts AND(tab, drag, crash) per message", """select sid, count(*) from doc where role = 1 and not meta
        and contains(lower(text), 'tab') and contains(lower(text), 'drag') and contains(lower(text), 'crash') group by sid""")
    bc.timed(con, "prompts ILIKE '%release%'", "select sid, count(*) from doc where role = 1 and not meta and text ilike '%release%' group by sid")


def agent_text(con) -> None:
    """Time the brute-force T3 scan (roles 2-6): the baseline an index must beat.

    Args:
        con: the open staging database.
    """
    print("== agent + tools, brute force (T3 baseline, roles 2-6)")
    for term in ("release", "duckdb", "rendersessionstable", "0xc000027b"):
        bc.timed(con, f"agent contains '{term}'", "select sid, count(*) from doc where role between 2 and 6 and contains(lower(text), ?) group by sid", [term], reps=2)
    bc.timed(con, "agent ILIKE '%duckdb%'", "select sid, count(*) from doc where role between 2 and 6 and text ilike '%duckdb%' group by sid", reps=2)
    bc.timed(con, "agent contains 'duckdb' (case-sensitive, no lower)", "select sid, count(*) from doc where role between 2 and 6 and contains(text, 'duckdb') group by sid", reps=2)


def main() -> None:
    """Run every section against the database named on the command line.

    Raises:
        SystemExit: no database path was given.
        duckdb.Error: the database is missing or lacks ingest.py's tables.
    """
    bc.utf8_stdout()
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    threads = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    con = bc.connect(sys.argv[1], read_only=True, threads=threads)
    sizes(con)
    metadata(con)
    prompts(con)
    agent_text(con)


if __name__ == "__main__":
    main()
