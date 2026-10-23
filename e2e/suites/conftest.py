"""The suite's fixtures: the scripted model, an isolated Agentmaster per test, evidence when a test fails.

A test asks for `am` and gets a running Agentmaster (its first window maximized, on a fresh profile and a fresh
Claude Code config, every session pointed at the mock). Everything else is one call away:

    def test_home_turns_into_the_way_back(am):
        a = am.launch("t1")                    # F-S: a managed session, its first turn done (Waiting for you)
        page = am.window.manager.open()        # Home
        assert page.home_name() == f"Jump back to {a.title}"

After the test the app is ended (the GUI killed, its ConPTY hosts left to end their shells and claudes), and
when the test failed the evidence (amdriver/evidence.py) is written first, into C:\\e2e\\out\\tests\\<test>\\.
A test that ends the app on purpose (App.close / kill / restart) is not reported as a crash; an app that died
on its own is, together with any crash dump Windows Error Reporting wrote (C:\\e2e\\dumps, set up by
provision.ps1).
"""
from __future__ import annotations

import psutil
import pytest

from amdriver import evidence
from amdriver.app import DEFAULT_EXE, E2E_ROOT, App, Isolation, find_agentmaster_pids
from mockllm.client import MockClient
from mockllm.server import serve_in_thread
from mockllm.state import MockState

MOCK_PORT = 8787
"""The port every isolated environment points ANTHROPIC_BASE_URL at."""
DUMPS = E2E_ROOT / "dumps"


@pytest.hookimpl(hookwrapper=True, tryfirst=True)
def pytest_runtest_makereport(item, call):
    """Keep each phase's report on the test item, so fixtures can see in their teardown whether the test failed.

    Args:
        item: The test item.
        call: The phase's call info.
    Yields:
        To the other hooks; the report is attached afterwards as item.rep_<phase>.
    """
    outcome = yield
    report = outcome.get_result()
    setattr(item, f"rep_{report.when}", report)


@pytest.fixture(scope="session")
def mock_server():
    """The scripted model, in this process, for the whole run.

    Yields:
        A MockClient bound to it.
    Raises:
        OSError: When the port is taken (an earlier run's mock still runs).
    """
    out = E2E_ROOT / "out"
    out.mkdir(parents=True, exist_ok=True)
    state = MockState(log_path=str(out / "mock-requests.jsonl"))
    server, _thread = serve_in_thread(state, port=MOCK_PORT)
    client = MockClient(f"http://127.0.0.1:{MOCK_PORT}")
    client.wait_up()
    yield client
    server.shutdown()


@pytest.fixture
def mock(mock_server) -> MockClient:
    """The scripted model, reset for this test (an empty request log, no gates, the default config).

    Args:
        mock_server: The run's mock.
    Returns:
        The MockClient.
    """
    mock_server.reset()
    return mock_server


def _end_strays() -> None:
    """End what an earlier test may have left running: an Agentmaster of the suite's exe, its ConPTY hosts and
    every claude.exe (in the disposable guest every claude belongs to a test).

    Returns:
        None.
    """
    for pid in find_agentmaster_pids(DEFAULT_EXE):
        try:
            psutil.Process(pid).kill()
        except psutil.NoSuchProcess:
            pass
    am_dir = str(DEFAULT_EXE.parent).lower()
    strays = []
    for p in psutil.process_iter(["name", "exe"]):
        try:
            name = (p.info["name"] or "").lower()
            exe = (p.info["exe"] or "").lower()
            if name == "claude.exe" or (name == "openconsole.exe" and exe.startswith(am_dir)):
                p.kill()
                strays.append(p)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    psutil.wait_procs(strays, timeout=10)


def _dump_files() -> set[str]:
    """The crash dumps on disk now.

    Returns:
        Their names.
    """
    return {p.name for p in DUMPS.glob("*.dmp")} if DUMPS.is_dir() else set()


@pytest.fixture
def iso(request) -> Isolation:
    """The test's folders (profile, Claude config, work folders, evidence), emptied and seeded.

    A test marked `@pytest.mark.claude_env(NAME="value", ...)` gets those variables in the app's environment, so every
    claude.exe the app starts inherits them (e.g. CLAUDE_CODE_MAX_RETRIES="1" makes an API error final after one retry).

    Args:
        request: pytest's request (the test's name names the folders; its claude_env marker adds variables).
    Returns:
        The Isolation.
    """
    iso = Isolation(name=request.node.name)
    marker = request.node.get_closest_marker("claude_env")
    if marker is not None:
        iso.extra_env.update({k: str(v) for k, v in marker.kwargs.items()})
    return iso.prepare()


@pytest.fixture
def am(request, iso, mock):
    """A running Agentmaster for this test.

    A test marked `@pytest.mark.am_settings(key=value, ...)` starts the app with those Settings-cog values already in
    its settings.json (the app reads the file at start; e.g. skipPermissions=False for a permission prompt).

    Args:
        request: pytest's request (its am_settings marker seeds settings.json before the start).
        iso: The test's folders.
        mock: The scripted model.
    Yields:
        The started App (first window maximized).
    Raises:
        pytest.fail: In teardown, when the app died during the test without the test ending it.
    """
    _end_strays()
    dumps_before = _dump_files()
    app = App(iso, mock=mock)
    marker = request.node.get_closest_marker("am_settings")
    if marker is not None:
        app.profile.write_settings(dict(marker.kwargs))
    app.start()
    yield app
    failed = any(getattr(getattr(request.node, f"rep_{when}", None), "failed", False) for when in ("setup", "call"))
    died = not app.alive() and not app.ended_by_test
    new_dumps = sorted(_dump_files() - dumps_before)
    if failed or died or new_dumps:
        evidence.collect(app, iso.out_dir, mock, label=f"{request.node.nodeid} failed={failed} died={died} "
                                                       f"dumps={new_dumps}")
    report = app.kill()
    if report.leftovers:
        (iso.out_dir / "leftovers.txt").write_text(
            f"processes still alive {report.seconds:.1f} s after the GUI was killed (then killed): "
            f"{report.leftovers}\n", encoding="utf-8")
    if died:
        pytest.fail(f"Agentmaster died during the test (exit code {app.proc.returncode}); crash dumps: {new_dumps}; "
                    f"evidence in {iso.out_dir}")
    if new_dumps:
        pytest.fail(f"a process of the test crashed: {new_dumps} (in {DUMPS})")


@pytest.fixture
def s1(am):
    """F-S: one managed session in t1, its first turn done.

    Args:
        am: The app.
    Returns:
        The Session.
    """
    return am.launch("t1")


@pytest.fixture
def s2(am, s1):
    """F-S2: sessions in t1 and t2 in one window, both Waiting for you.

    Args:
        am: The app.
        s1: The first session.
    Returns:
        (the t1 session, the t2 session).
    """
    return s1, am.launch("t2")
