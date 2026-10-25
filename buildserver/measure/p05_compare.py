"""Proof P0.5's comparison (DESIGN.md §13): the same e2e tests on Tiny11 Core and on a full Windows 11, side by side.

Reads two JUnit reports (a junit.xml, or a run's results zip that holds one) - one from an `amb e2e` run on the
`am-e2e` image (Tiny11 Core), one from `e2e/host/e2e_run.py` on a claude-fleet desktop (Tiny11 25H2, the full
desktop) - and prints every test's outcome and time on both, then the tests whose outcomes differ. A result file of
the same facts is written with --out.

    python buildserver/measure/p05_compare.py <core junit.xml|zip> <full junit.xml|zip> [--out result.json]

Exit codes: 0 every test has the same outcome on both; 1 some differ (the output lists them); 2 a report is missing.
"""
from __future__ import annotations

import argparse
import json
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path


def read_report(path: Path) -> dict[str, dict]:
    """Reads a JUnit report into one entry per test.

    Args:
        path: a junit.xml, or a zip whose member junit.xml is the report (run_suite.py's results zip).

    Returns:
        {test id: {"outcome": "passed|failed|error|skipped", "seconds": float}}; the id is "<classname>::<name>".

    Raises:
        FileNotFoundError: the file, or the zip's junit.xml, is missing.
        ET.ParseError: the report is not XML.
    """
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as archive:
            names = [n for n in archive.namelist() if n.endswith("junit.xml")]
            if not names:
                raise FileNotFoundError(f"{path} holds no junit.xml")
            root = ET.fromstring(archive.read(names[0]))
    else:
        root = ET.parse(path).getroot()
    tests: dict[str, dict] = {}
    for case in root.iter("testcase"):
        outcome = "passed"
        for child, name in (("failure", "failed"), ("error", "error"), ("skipped", "skipped")):
            if case.find(child) is not None:
                outcome = name
                break
        test_id = f"{case.get('classname', '')}::{case.get('name', '')}"
        tests[test_id] = {"outcome": outcome, "seconds": float(case.get("time") or 0.0)}
    return tests


def main(argv: list[str]) -> int:
    """Compares the two reports and prints the table.

    Args:
        argv: the arguments after the program name.

    Returns:
        The exit code.
    """
    parser = argparse.ArgumentParser(description="P0.5: e2e on Tiny11 Core against a full Windows 11.")
    parser.add_argument("core", type=Path)
    parser.add_argument("full", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args(argv)
    try:
        core, full = read_report(args.core), read_report(args.full)
    except (FileNotFoundError, ET.ParseError, zipfile.BadZipFile) as exc:
        print(f"p05_compare: {exc}", file=sys.stderr)
        return 2
    ids = sorted(set(core) | set(full))
    rows, differ = [], []
    print(f"{'test':<92} {'core':>8} {'s':>6}   {'full':>8} {'s':>6}")
    for test_id in ids:
        c, f = core.get(test_id), full.get(test_id)
        co, fo = (c or {}).get("outcome", "absent"), (f or {}).get("outcome", "absent")
        cs, fs = (c or {}).get("seconds", 0.0), (f or {}).get("seconds", 0.0)
        rows.append({"test": test_id, "core": co, "coreSeconds": cs, "full": fo, "fullSeconds": fs})
        if co != fo:
            differ.append(test_id)
        print(f"{test_id[-92:]:<92} {co:>8} {cs:6.1f}   {fo:>8} {fs:6.1f}{'   <- differs' if co != fo else ''}")
    totals = {side: {o: sum(1 for r in rows if r[side] == o) for o in ("passed", "failed", "error", "skipped", "absent")}
              for side in ("core", "full")}
    core_s = sum(r["coreSeconds"] for r in rows)
    full_s = sum(r["fullSeconds"] for r in rows)
    print(f"\ncore: {totals['core']}  {core_s / 60:.1f} min of tests")
    print(f"full: {totals['full']}  {full_s / 60:.1f} min of tests")
    print(f"same outcome on both: {len(ids) - len(differ)} of {len(ids)}" + (f"; differ: {', '.join(differ)}" if differ else ""))
    if args.out:
        args.out.write_text(json.dumps({"tests": rows, "totals": totals, "coreSeconds": core_s, "fullSeconds": full_s,
                                        "differ": differ}, indent=2), encoding="utf-8")
    return 0 if not differ else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
