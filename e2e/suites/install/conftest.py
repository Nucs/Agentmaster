"""The install lane's fixtures: a clean machine per scenario, its evidence when it fails, and the guard that keeps the
install scenarios out of every other run.

An install scenario installs Agentmaster FOR REAL — Program Files, HKCU and HKLM, shortcuts, the PATH, Active Setup —
so it must start on a machine with no Agentmaster at all and must never run on a desktop that another suite shares
(doc/agentmaster/E2E_TESTING.md §8.5). Three rules make that hold:

1. **Deselected unless the lane prepared this desktop.** Without C:\\e2e\\install-lane.json (written by
   `provision.ps1 -Lane Install`, removed by every Standard provisioning) the scenarios are not even collected: a full
   run of the suite on a shared desktop shows them as "deselected", never runs them.
2. **Listed in the catalog.** Every test here must appear in install_catalog.py — the plan the host follows (one reset
   per scenario). An unlisted test fails at collection.
3. **Clean at the start, checked.** The `machine` fixture reads the machine (amdriver.install.MachineState) before the
   test and fails it at once when any Agentmaster is left: under the "reset" isolation that means the host did not
   reset the desktop; under "clean" it first removes everything (tools/install-qa Reset-AmQa); under "none" it only
   checks.

The evidence of a failed scenario lands in C:\\e2e\\out\\tests\\<test>\\ (amdriver.install.collect_evidence), where
run_suite.py zips it and the host's summary names it.
"""
from __future__ import annotations

import importlib.util
import shutil
from pathlib import Path

import pytest

from amdriver import install
from amdriver.app import E2E_ROOT, _safe_name

HERE = Path(__file__).resolve().parent
LANE = install.Lane.load()
"""The lane's facts, or None on a desktop the install lane did not prepare."""


def _catalog() -> list[dict]:
    """The scenarios of install_catalog.py, loaded by path (the host reads the same file the same way).

    Returns:
        The SCENARIOS list.
    Raises:
        ImportError: When the file cannot be loaded.
    """
    spec = importlib.util.spec_from_file_location("am_install_catalog", HERE / "install_catalog.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return list(module.SCENARIOS)


def pytest_configure(config) -> None:
    """Register the lane's marker (this conftest loads before its tests, so --strict-markers accepts it).

    Args:
        config: pytest's config.
    Returns:
        None.
    """
    config.addinivalue_line("markers", "install(*ids): an install scenario of flows/19's catalog (its IS-nn ids); it "
                                       "runs only on a desktop the install lane prepared (E2E_TESTING.md §8.5)")


def _is_install_item(item) -> bool:
    """Whether a collected test lives in this folder.

    Args:
        item: A pytest item.
    Returns:
        True for an install scenario.
    """
    try:
        return HERE in Path(str(item.fspath)).resolve().parents
    except (OSError, ValueError):
        return False


def pytest_collection_modifyitems(session, config, items) -> None:
    """Deselect the install scenarios outside the lane; inside it, refuse a test the catalog does not list.

    pytest calls this hook of every conftest with ALL the run's items, so it touches only the items of this folder.

    Args:
        session: The session.
        config: The config.
        items: The collected items (edited in place).
    Returns:
        None.
    Raises:
        pytest.UsageError: When an install test is missing from install_catalog.py.
    """
    mine = [it for it in items if _is_install_item(it)]
    if not mine:
        return
    if LANE is None:
        config.hook.pytest_deselected(items=mine)
        items[:] = [it for it in items if not _is_install_item(it)]
        return
    listed = {s["test"] for s in _catalog()}
    unlisted = []
    for it in mine:
        rel = f"{Path(str(it.fspath)).name}::{it.originalname}"
        if rel not in listed:
            unlisted.append(rel)
    if unlisted:
        raise pytest.UsageError(f"install tests missing from suites/install/install_catalog.py (the host plans its "
                                f"resets from it): {', '.join(sorted(unlisted))}")


@pytest.fixture
def lane() -> install.Lane:
    """The install lane's facts.

    Returns:
        The Lane.
    """
    if LANE is None:  # deselected by the hook above; kept for a test started by hand on another desktop
        pytest.skip("not an install lane desktop (no C:\\e2e\\install-lane.json)")
    return LANE


@pytest.fixture
def kit(lane) -> install.Kit:
    """Stamped copies and zips of the build under test.

    Args:
        lane: The lane.
    Returns:
        The Kit.
    """
    return install.Kit(lane)


@pytest.fixture
def machine(request, lane):
    """The machine a scenario starts from: no Agentmaster anywhere, checked; and what it leaves when it fails.

    Before: under the "clean" isolation every install is removed first (Reset-AmQa); then MachineState.read() must show
    nothing — a leftover fails the test at once, naming it (an install scenario on a dirty machine proves nothing).
    After: a failed test's evidence; then every Agentmaster ended, so the next scenario's checks (or the host's reset)
    find no process.

    Args:
        request: pytest's request (the test's outcome, its name).
        lane: The lane.
    Yields:
        The clean MachineState.
    Raises:
        pytest.fail: When the machine is not clean.
    """
    install.RUNS.clear()
    out_dir = E2E_ROOT / "out" / "tests" / _safe_name(request.node.name)
    if out_dir.exists():
        shutil.rmtree(out_dir, ignore_errors=True)
    if lane.isolation == "clean":
        install.end_agentmaster()
        install.reset_machine(lane)
        install.RUNS.clear()  # the clean-up is the lane's, not one of the scenario's runs
    state = install.MachineState.read()
    problems = state.baseline_problems()
    if problems:
        how = {"reset": "the install lane resets the desktop before every scenario; was it skipped?",
               "clean": "Reset-AmQa did not remove them",
               "none": "the lane's isolation is 'none': reset the desktop or run with --isolation clean"}.get(lane.isolation, "")
        pytest.fail(f"the machine is not clean ({lane.isolation} isolation): {'; '.join(problems)} — {how}")
    yield state
    failed = any(getattr(getattr(request.node, f"rep_{when}", None), "failed", False) for when in ("setup", "call"))
    if failed:
        install.collect_evidence(out_dir, label=f"{request.node.nodeid} (isolation {lane.isolation})", lane=lane)
    leftovers = install.end_agentmaster()
    if leftovers:
        out_dir.mkdir(parents=True, exist_ok=True)
        (out_dir / "leftovers.txt").write_text("ConPTY hosts still alive after the drain (then killed):\n"
                                               + "\n".join(leftovers) + "\n", encoding="utf-8")
