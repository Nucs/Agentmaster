"""The `agentmaster` CLI (doc/agentmaster/flows/22-cli-and-session-database.md, CLI).

The CLI is the console program beside the app — `agentmaster-cli.exe` in the install folder (`C:\\e2e\\am` in the
guest) — that reads the profile's files and the processes, app up or down, and answers in text or in a JSON envelope
`{schemaVersion: 2, profile, …}`. These tests run the CLI of the build under test directly, as a script or an agent
would, with the app's own environment (CLAUDE_CONFIG_DIR, PATH, the mock) and with or without the test's
AGENTMASTER_PROFILE.
"""
import json
import os
import subprocess
from pathlib import Path

import pytest


def _cli(am, *args: str, inherit_profile: bool = True, timeout_s: float = 60.0) -> subprocess.CompletedProcess:
    """Run the build's agentmaster-cli.exe in the app's environment.

    Args:
        am: The app: its exe names the install folder, its isolation the environment and the working folder.
        *args: The CLI's arguments (e.g. "sessions", "--json").
        inherit_profile: Keep the test's AGENTMASTER_PROFILE, as a shell inside the app has it; False runs the CLI
            the way a plain shell outside any Agentmaster does (no profile in the environment).
        timeout_s: How long the CLI may take.
    Returns:
        The finished process, stdout and stderr as bytes.
    Raises:
        subprocess.TimeoutExpired: When the CLI did not end in time.
        FileNotFoundError: When the install folder holds no agentmaster-cli.exe.
    """
    env = am.iso.env()
    if not inherit_profile:
        env.pop("AGENTMASTER_PROFILE", None)
    return subprocess.run([str(am.exe.with_name("agentmaster-cli.exe")), *args], env=env, capture_output=True,
                          timeout=timeout_s, cwd=str(am.iso.work_dir))


def _same_path(a: Path, b: Path) -> bool:
    """Whether two Windows paths name the same folder (case and separators folded, no file system access).

    Args:
        a: One path.
        b: The other.
    Returns:
        True when they are equal after normalization.
    """
    return os.path.normcase(os.path.normpath(str(a))) == os.path.normcase(os.path.normpath(str(b)))


@pytest.mark.p2
@pytest.mark.flow("CLI-02")
@pytest.mark.needs_fix("CLI-K2")
def test_a_plain_shell_reads_the_installs_own_profile(am):
    """CLI-02 / CLI-K2's fix (3b042ea32, 2026-10-23): the CLI beside an unpackaged RELEASE install, run from a plain
    shell — no AGENTMASTER_PROFILE inherited — answers from the profile the app beside it resolves (ResolveProfileDir:
    here no `profile.path` beside the exe, no portable marker, and no saved choice in `~/.agentmaster.profiles`, so
    `%USERPROFILE%\\.agentmaster`), never from `%USERPROFILE%\\.agentmaster-dev`. On 0.8.0 every configuration of the
    CLI carried the dev brand (AGENTMASTER_DEV), so a release CLI called like this read the DEV profile, and created it.

    The call runs while the app is up (on the test's own profile); the CLI's answer is the empty fleet of the profile
    it reads, which is all this test needs: the envelope's `profile` names the folder it read.

    Args:
        am: The app (its install folder holds the CLI under test).
    """
    home = Path(os.environ.get("USERPROFILE", r"C:\Users\Docker"))
    out = _cli(am, "sessions", "--json", inherit_profile=False)
    stderr = out.stderr.decode("utf-8", "replace")
    assert out.returncode == 0, f"exit {out.returncode}: {stderr}"
    reply = json.loads(out.stdout.decode("utf-8-sig"))
    assert reply.get("schemaVersion") == 2, reply
    profile = Path(reply["profile"])
    assert not _same_path(profile, home / ".agentmaster-dev"), f"the release CLI read the DEV profile: {profile}"
    if not (home / ".agentmaster.profiles").exists():
        assert _same_path(profile, home / ".agentmaster"), f"not the install's own profile: {profile}"
