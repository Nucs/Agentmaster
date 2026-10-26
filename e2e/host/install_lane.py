"""The install lane: flows/19's install scenarios, each on a freshly reset test desktop, and one more reset after them.

An install scenario installs Agentmaster FOR REAL — Program Files, HKCU and HKLM, shortcuts, the PATH, Active Setup —
so no two scenarios may share a machine, and no other suite may meet what one leaves (doc/agentmaster/E2E_TESTING.md
§8.5). This script runs them that way:

    for each scenario of e2e/suites/install/install_catalog.py (or the ones --only names):
        reset_desktop            a fresh overlay over the golden disk and a cold boot (the fleet's tool); the persist
                                 share (Z:) survives it, so the suite, the cache and earlier results stay
        provision.ps1 -Lane Install
                                 the venv, PowerShell 7, the kit (the build under test with its `.portable` marker),
                                 the lane marker; no Claude Code, no Git (no scenario starts a session)
        run_suite.py             that one test (pytest), its progress lines printed as they come
        collect                  its results into Q:\\agentmaster-e2e\\runs\\<run>\\<nn>-<test>\\
    then: reset_desktop once more, and provision the Standard lane again (unless --no-reprovision or --delete-after),
    so the desktop is back to what the other suites expect.

    python e2e/host/install_lane.py --desktop am-install-1 --create --delete-after -- -v
    python e2e/host/install_lane.py --desktop am-e2e-1 --only IS-01,IS-40 -- -v      # two scenarios on the shared desktop
    python e2e/host/install_lane.py --desktop am-install-1 --create --isolation clean -- -v   # one desktop for all (faster)
    python e2e/host/install_lane.py --plan                                             # print the plan, run nothing

Isolation (--isolation): "reset" (the default: the user's rule of 2026-10-24 — a reset before EVERY scenario and one
after them all), "clean" (one provisioning, one guest run of every scenario; each test first removes every Agentmaster
with tools/install-qa's Reset-AmQa and checks the machine — minutes faster, for iterating on the suite; the final
reset still runs) or "none" (no resets and no clean-up: a scenario on a machine that is not clean fails at once).

Why a reset and not a snapshot: QEMU's own savevm/loadvm would roll a desktop back in seconds, but these VMs refuse it
(measured 2026-10-24: "State blocked by non-migratable CPU device (invtsc flag)"; behind that, their UEFI variables are a
WRITABLE RAW pflash drive, which QEMU's savevm refuses too). A reset costs 35-70 s; the lane's provisioning ~1-2 min
from the cache.

The exit code: 0 when every scenario passed; 1 when one failed or errored; 97 when a scenario's wait ran out (its guest
run goes on); 98 when the orchestration failed. The summary (also in <run>\\summary.txt) lists each scenario with its
outcome, its reset / provisioning / test times, and the evidence folder of a failure.

⚠ The fleet token is read from the cluster into a variable and never printed (e2e_run.Fleet). A Windows desktop takes
4-8 GB and several cores of this PC: --delete-after, or delete it when done.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import shutil
import sys
import time
import zipfile
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import e2e_run as er  # noqa: E402 - the orchestrator's building blocks (Fleet, stage, provision, launch, follow)

REPO = HERE.parents[1]
CATALOG = REPO / "e2e" / "suites" / "install" / "install_catalog.py"
KIT_FILES = {
    "Install-Agentmaster.ps1": REPO / "tools" / "Install-Agentmaster.ps1",
    "amqa-guest.ps1": REPO / "tools" / "install-qa" / "amqa-guest.ps1",
}
"""The scripts under test and the QA kit's guest helpers, staged into Z:\\e2e\\src\\kit (the guest's C:\\e2e\\src\\kit)."""
READY_TIMEOUT_S = 300.0
"""How long a reset desktop's agent may take to answer its first command."""
PACKAGE_RELEASE = "v0.7.0"
"""The release whose MSIX bundle the "pkg" machine stages: the release package the owner had installed (2026-10-26) when
a portable copy's "Install just for you" reset their profile."""
PACKAGE_ASSETS = ("Agentmaster_0.7.0.0.msixbundle", "Agentmaster.cer")
"""That release's bundle and the certificate it is signed with (CN=Agentmaster, self-signed: the guest trusts it)."""
PACKAGE_CACHE = Path(r"Q:\agentmaster-e2e\cache\pkg-0.7.0")
"""Where the host keeps the downloaded assets (once; never under K:, CLAUDE.md "Every build writes to Q:")."""


def load_catalog() -> tuple[list[dict], frozenset]:
    """The scenarios and the Machine tags the lane prepares, from the suite's catalog (pure data, loaded by path).

    Returns:
        (SCENARIOS, PREPARED).
    Raises:
        ImportError / OSError: When the catalog cannot be loaded.
    """
    spec = importlib.util.spec_from_file_location("am_install_catalog", CATALOG)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return list(module.SCENARIOS), frozenset(getattr(module, "PREPARED", frozenset()))


def select(scenarios: list[dict], only: str) -> list[dict]:
    """The scenarios a --only list names (catalog ids like IS-01, or test function names); all when it is empty.

    Args:
        scenarios: The catalog's scenarios.
        only: A comma-separated list.
    Returns:
        The selected scenarios, in catalog order.
    Raises:
        SystemExit: When a name matches no scenario.
    """
    if not only.strip():
        return scenarios
    wanted = [w.strip() for w in only.split(",") if w.strip()]
    chosen, unknown = [], []
    for w in wanted:
        hits = [s for s in scenarios if w in s["ids"] or s["test"].endswith("::" + w) or s["test"] == w]
        if not hits:
            unknown.append(w)
        for s in hits:
            if s not in chosen:
                chosen.append(s)
    if unknown:
        raise SystemExit(f"--only: no install scenario for {', '.join(unknown)} (see {CATALOG})")
    return [s for s in scenarios if s in chosen]


def stage_kit(persist: Path) -> None:
    """Stage the scripts under test beside the suite (the guest's run_suite.py copies Z:\\e2e\\src to C:\\e2e\\src).

    Args:
        persist: The desktop's persist folder.
    Returns:
        None.
    Raises:
        OSError: When a file cannot be copied.
    """
    kit = persist / "e2e" / "src" / "kit"
    kit.mkdir(parents=True, exist_ok=True)
    for name, source in KIT_FILES.items():
        shutil.copy2(source, kit / name)


def stage_package_kit(persist: Path) -> Path:
    """Stage the "pkg" machine's release package into the kit: PACKAGE_RELEASE's msixbundle and certificate, downloaded
    from GitHub into PACKAGE_CACHE once and copied into Z:\\e2e\\src\\kit\\pkg-0.7.0 (the guest's run copies it to
    C:\\e2e\\src\\kit\\pkg-0.7.0, where the tests look).

    The folder is made new when it is missing and filled before the guest looks: a NEW file the host drops into an
    EXISTING folder of the share stays invisible to the guest for over a minute (the share's directory lease), while a
    new folder shows at once (measured 2026-10-24).

    Args:
        persist: The desktop's persist folder.
    Returns:
        The staged folder.
    Raises:
        OSError / urllib.error.URLError: When an asset cannot be downloaded or copied.
    """
    import urllib.request  # noqa: PLC0415 - only the pkg machine downloads

    PACKAGE_CACHE.mkdir(parents=True, exist_ok=True)
    for name in PACKAGE_ASSETS:
        cached = PACKAGE_CACHE / name
        if not cached.exists():
            url = f"https://github.com/Nucs/Agentmaster/releases/download/{PACKAGE_RELEASE}/{name}"
            partial = cached.with_name(name + ".part")
            urllib.request.urlretrieve(url, partial)  # nosec - a fixed github.com release URL of this repo
            partial.replace(cached)
    target = persist / "e2e" / "src" / "kit" / "pkg-0.7.0"
    target.mkdir(parents=True, exist_ok=True)
    for name in PACKAGE_ASSETS:
        shutil.copy2(PACKAGE_CACHE / name, target / name)
    return target


def wait_ready(fleet: er.Fleet, desktop: str, timeout_s: float = READY_TIMEOUT_S) -> float:
    """Wait until a (reset) desktop's agent runs commands and its persist share is mapped.

    Args:
        fleet: The fleet.
        desktop: The desktop.
        timeout_s: How long.
    Returns:
        The seconds it took.
    Raises:
        TimeoutError: When the desktop never answered.
    """
    begun = time.monotonic()
    last = ""
    while time.monotonic() - begun < timeout_s:
        try:
            out = fleet.desk(desktop, "run_command", {"command": "if (Test-Path Z:\\e2e\\src\\guest\\provision.cmd) "
                                                                 "{ 'ready' } else { 'no-share' }", "timeout_s": 60})
            if "ready" in out:
                return time.monotonic() - begun
            last = out.strip()[-200:]
        except RuntimeError as error:
            last = str(error)[-200:]
        time.sleep(5)  # the agent starts with the guest's sign-in; a pause between two tries, not a guessed wait
    raise TimeoutError(f"{desktop} did not answer within {timeout_s:.0f} s (last: {last})")


def reset(fleet: er.Fleet, desktop: str) -> float:
    """Reset the desktop to a clean machine and wait for it to answer.

    Args:
        fleet: The fleet.
        desktop: The desktop.
    Returns:
        The seconds the reset and the wait took.
    Raises:
        RuntimeError / TimeoutError: When the reset failed or the desktop never came back.
    """
    begun = time.monotonic()
    fleet.tool("reset_desktop", {"name": desktop})
    tool_s = time.monotonic() - begun
    ready_s = wait_ready(fleet, desktop)
    # The two parts apart: the fleet's reset (a fresh overlay, a cold boot, the agent up) and the share's mapping.
    er.log(f"  reset_desktop {tool_s:.0f} s, then the agent and Z: ready {ready_s:.0f} s later")
    return time.monotonic() - begun


def provision_lane(fleet: er.Fleet, desktop: str, isolation: str, extra: list[str]) -> float:
    """Provision the install lane (provision.ps1 -Lane Install).

    Args:
        fleet: The fleet.
        desktop: The desktop.
        isolation: "reset", "clean" or "none" (written into the lane file for the tests).
        extra: More provision.ps1 arguments (e.g. -AgentmasterZip <zip>).
    Returns:
        The seconds it took.
    Raises:
        RuntimeError / TimeoutError: When provisioning failed.
    """
    begun = time.monotonic()
    er.provision(fleet, desktop, ["-Lane", "Install", "-Isolation", isolation.capitalize(), *extra])
    return time.monotonic() - begun


def machine_args(scenarios: list[dict]) -> list[str]:
    """The provision.ps1 arguments that prepare what some scenarios' Machine tags ask for.

    Args:
        scenarios: The scenarios one provisioning serves.
    Returns:
        The arguments ("-Accounts" for "std" and "adm2", "-Old" for "old").
    """
    tags = {m for s in scenarios for m in s.get("machine", [])}
    return (["-Accounts"] if tags & {"std", "adm2"} else []) + (["-Old"] if "old" in tags else [])


def collect_into(persist: Path, name: str, target: Path) -> Path:
    """Unpack one guest run's results zip into a folder of the lane's results.

    Args:
        persist: The desktop's persist folder.
        name: The guest run's name.
        target: The folder (replaced).
    Returns:
        The folder.
    Raises:
        OSError / zipfile.BadZipFile: When the zip is missing or broken.
    """
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    with zipfile.ZipFile(er.out_folder(persist) / f"{name}.zip") as z:
        z.extractall(target)
    return target


def guest_run(fleet: er.Fleet, desktop: str, persist: Path, name: str, pytest_args: list[str], wait_s: float,
              progress: str, results: Path) -> tuple[int, dict | None, Path | None]:
    """Run pytest in the guest and collect its results.

    Args:
        fleet: The fleet.
        desktop: The desktop.
        persist: Its persist folder.
        name: The guest run's name (unique per scenario).
        pytest_args: pytest's arguments (the node ids and the user's own).
        wait_s: How long the wait may last.
        progress: Which per-test lines to print ("all", "failures", "none").
        results: Where to collect.
    Returns:
        (exit code, the guest's final status or None, the results folder or None).
    """
    er.launch(fleet, desktop, persist, name, pytest_args)
    status = er.follow(persist, name, wait_s, progress, attach=False)
    if status is None:
        return er.STILL_RUNNING, None, None
    folder = collect_into(persist, name, results)
    return int(status.get("exit", er.ORCHESTRATION_FAILED)), status, folder


def outcome_word(code: int) -> str:
    """A scenario's outcome from its pytest exit code.

    Args:
        code: pytest's exit code (97 still running, 98 orchestration).
    Returns:
        PASS, FAIL, NONE (no test ran: 5), STILL RUNNING or ERROR.
    """
    return {0: "PASS", 1: "FAIL", 5: "NONE", er.STILL_RUNNING: "STILL RUNNING"}.get(code, "ERROR")


def print_plan(scenarios: list[dict], prepared: frozenset, isolation: str) -> None:
    """Print what a run would do.

    Args:
        scenarios: The selected scenarios.
        prepared: The Machine tags the lane prepares.
        isolation: The isolation.
    Returns:
        None.
    """
    minutes = sum(s.get("minutes", 2) for s in scenarios)
    if isolation == "reset":
        # Measured 2026-10-24 (E2E_TESTING.md s.8.5): a reset 35-570 s by the PC's load, the lane's provisioning after
        # it 78 s since Expand-Zip and the Defender exclusion of the test tooling (it was ~6.4 min).
        per = ("a reset + provisioning before each (~2-3 min on a quiet PC, up to ~11 min with three more desktops "
               "running), one reset after all")
    else:
        per = "one provisioning; one guest run" + ("; each test cleans first" if isolation == "clean" else "")
    print(f"install lane: {len(scenarios)} scenario(s), ~{minutes} min of tests; isolation {isolation}: {per}")
    for i, s in enumerate(scenarios, 1):
        missing = [m for m in s.get("machine", []) if m not in prepared]
        note = f"  (needs {', '.join(missing)}: not prepared by the lane yet - skipped)" if missing else ""
        print(f"  {i:2d}. {s['test']:<75} {' '.join(s['ids'])}{note}")


def main(argv: list[str]) -> int:
    """The command line.

    Args:
        argv: The arguments; everything after "--" goes to pytest (added to every scenario's run).
    Returns:
        0 all passed, 1 a scenario failed, 97 a wait ran out, 98 the orchestration failed.
    Raises:
        SystemExit: On a usage error (an unknown --only name, a missing desktop without --create).
    """
    pytest_args: list[str] = []
    if "--" in argv:
        i = argv.index("--")
        argv, pytest_args = argv[:i], argv[i + 1:]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--desktop", default="am-install-1")
    parser.add_argument("--create", action="store_true", help="create the desktop when it does not exist")
    parser.add_argument("--memory-gb", type=int, default=6)
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--isolation", choices=("reset", "clean", "none"), default="reset")
    parser.add_argument("--only", default="", help="catalog ids (IS-01) or test names, comma-separated")
    parser.add_argument("--provision-arg", action="append", default=[], help="an extra provision.ps1 argument")
    parser.add_argument("--run", default="", help="the run's name (default: inst-<time stamp>)")
    parser.add_argument("--scenario-max-min", type=float, default=20, help="how long one scenario's guest run may take")
    parser.add_argument("--progress", choices=("all", "failures", "none"), default="all")
    parser.add_argument("--stop-on-fail", action="store_true",
                        help="stop after the first failed scenario (by default the lane goes on: each scenario has a machine of its own)")
    parser.add_argument("--no-final-reset", action="store_true", help="leave the last scenario's machine as it is")
    parser.add_argument("--no-reprovision", action="store_true",
                        help="after the final reset, do not provision the Standard lane again")
    parser.add_argument("--delete-after", action="store_true", help="delete the desktop when done")
    parser.add_argument("--plan", action="store_true", help="print the plan and exit")
    args = parser.parse_args(argv)

    scenarios, prepared = load_catalog()
    chosen = select(scenarios, args.only)
    print_plan(chosen, prepared, args.isolation)
    if args.plan:
        return 0
    runnable = [s for s in chosen if all(m in prepared for m in s.get("machine", []))]
    run_name = args.run or f"inst-{datetime.now():%Y%m%d-%H%M%S}"
    lane_dir = er.RUNS / run_name
    lane_dir.mkdir(parents=True, exist_ok=True)
    persist = er.PERSIST_ROOT / args.desktop / "persist"
    fleet = er.Fleet()
    rows: list[dict] = []
    code = er.ORCHESTRATION_FAILED
    created = False
    try:
        if not fleet.exists(args.desktop):
            if not args.create:
                raise SystemExit(f"desktop {args.desktop} does not exist (pass --create)")
            er.log(f"creating {args.desktop} ({args.memory_gb} GB, {args.cpus} CPUs)")
            fleet.tool("create_desktop", {"name": args.desktop, "os": "windows", "width": 1920, "height": 1080,
                                          "memory_gb": args.memory_gb, "cpus": args.cpus, "wait": True})
            created = True
        er.log(f"staging the suite and the scripts under test into {persist}")
        er.stage(persist)
        stage_kit(persist)
        if any("pkg" in s.get("machine", []) for s in runnable):
            er.log(f"staging the {PACKAGE_RELEASE} release package (the pkg machine) into {stage_package_kit(persist)}")
        wait_s = args.scenario_max_min * 60
        if args.isolation == "reset":
            for n, s in enumerate(runnable, 1):
                test = s["test"].split("::", 1)[1]
                row = {"n": n, "test": s["test"], "ids": s["ids"], "reset_s": None, "provision_s": None}
                rows.append(row)
                # A desktop created by this run IS a clean machine: its first scenario needs no reset.
                if n > 1 or not created:
                    er.log(f"[{n}/{len(runnable)}] resetting {args.desktop}")
                    row["reset_s"] = round(reset(fleet, args.desktop), 1)
                    er.log(f"[{n}/{len(runnable)}] reset in {row['reset_s']:.0f} s")
                elif created:
                    wait_ready(fleet, args.desktop)
                er.log(f"[{n}/{len(runnable)}] provisioning the install lane")
                row["provision_s"] = round(provision_lane(fleet, args.desktop, "reset",
                                                          args.provision_arg + machine_args([s])), 1)
                name = f"{run_name}-{n:02d}"
                er.log(f"[{n}/{len(runnable)}] {s['test']} ({' '.join(s['ids'])})")
                begun = time.monotonic()
                result, status, folder = guest_run(fleet, args.desktop, persist, name,
                                                   [f"suites/install/{s['test']}", *pytest_args], wait_s, args.progress,
                                                   lane_dir / f"{n:02d}-{test}")
                row.update(code=result, seconds=round(time.monotonic() - begun, 1), results=str(folder or ""),
                           status=status or {})
                er.log(f"[{n}/{len(runnable)}] {outcome_word(result)} in {row['seconds']:.0f} s")
                if result == er.STILL_RUNNING or (args.stop_on_fail and result != 0):
                    break
        else:
            rows.append({"n": 1, "test": f"{len(runnable)} scenario(s) in one run", "ids": [i for s in runnable for i in s["ids"]],
                         "reset_s": None, "provision_s": None})
            if not created:
                er.log(f"isolation {args.isolation}: no reset before the run")
            else:
                wait_ready(fleet, args.desktop)
            rows[0]["provision_s"] = round(provision_lane(fleet, args.desktop, args.isolation,
                                                          args.provision_arg + machine_args(runnable)), 1)
            begun = time.monotonic()
            node_ids = [f"suites/install/{s['test']}" for s in runnable]
            result, status, folder = guest_run(fleet, args.desktop, persist, f"{run_name}-01", node_ids + pytest_args,
                                               wait_s * max(1, len(runnable)), args.progress, lane_dir / "01-all")
            rows[0].update(code=result, seconds=round(time.monotonic() - begun, 1), results=str(folder or ""),
                           status=status or {})
        failed = [r for r in rows if r.get("code") not in (0, 5)]
        code = 0 if not failed else (er.STILL_RUNNING if any(r.get("code") == er.STILL_RUNNING for r in failed) else 1)
        if not args.no_final_reset and not args.delete_after and args.isolation != "none":
            er.log(f"final reset of {args.desktop} (the install scenarios leave nothing behind)")
            reset(fleet, args.desktop)
            if not args.no_reprovision:
                er.log("provisioning the Standard lane again (the other suites expect it)")
                er.provision(fleet, args.desktop, [])
    finally:
        lines = summarize(rows, chosen, runnable)
        (lane_dir / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        (lane_dir / "lane.json").write_text(json.dumps({"isolation": args.isolation, "desktop": args.desktop,
                                                        "rows": rows}, indent=1, default=str), encoding="utf-8")
        for line in lines:
            print(line if line.startswith("  ") else f"[{datetime.now():%H:%M:%S}] {line}", flush=True)
        if args.delete_after:
            er.log(f"deleting {args.desktop}")
            fleet.tool("delete_desktop", {"name": args.desktop})
    return code


def summarize(rows: list[dict], chosen: list[dict], runnable: list[dict]) -> list[str]:
    """The lane's end in a few lines.

    Args:
        rows: One per scenario run (or one for a clean / none run).
        chosen: The selected scenarios.
        runnable: Those the lane could prepare.
    Returns:
        The lines (without time stamps).
    """
    passed = sum(1 for r in rows if r.get("code") == 0)
    lines = [f"install lane: {passed} of {len(rows)} run(s) passed ({len(chosen) - len(runnable)} scenario(s) skipped: "
             f"their machine is not prepared yet)"]
    for r in rows:
        times = ", ".join(f"{k} {r[k]:.0f} s" for k in ("reset_s", "provision_s", "seconds") if r.get(k) is not None)
        lines.append(f"  {outcome_word(r.get('code', er.ORCHESTRATION_FAILED)):<6} {r['n']:2d}. {r['test']} "
                     f"[{' '.join(r['ids'])}] ({times})")
        status = r.get("status") or {}
        results = Path(r["results"]) if r.get("results") else None
        if results is not None and r.get("code") not in (0, 5):
            for line in er.summary_lines(results, status)[1:]:
                lines.append("    " + line.strip())
    return lines


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
