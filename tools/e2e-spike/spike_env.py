"""Spike: a CLEAN environment for a claude.exe under test, pointed at mockllm.py, and a headless runner.

A claude.exe started from inside a Claude Code session (or an Agentmaster tab) would inherit dozens of
variables that change its behavior: this machine's session exports CLAUDE_CODE_MAX_RETRIES=50000, an
effort level, a gateway ANTHROPIC_BASE_URL, CLAUDE_CODE_CHILD_SESSION (which silently turns transcript
saving off), Agentmaster's AM_SESSION / CCMGR_* stamps and more. So the child gets a MINIMAL Windows
environment instead, plus only what the test wants.

⚠ The auto-updater: a clean environment also loses the user's DISABLE_AUTOUPDATER=1, and the very first
interactive launch of the spike (2026-10-22) then REPLACED the user's pinned ~/.local/bin/claude.exe
(2.1.291 -> 2.1.308) within three minutes. It was restored from ~/.local/share/claude/versions/2.1.291.
clean_env() therefore always sets DISABLE_AUTOUPDATER=1, CLAUDE_CODE_SKIP_AUTO_UPDATE=1 and
CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1. Never drop them on a machine whose claude is not disposable.

Usage:
    python -I spike_env.py seed <config dir> <cwd>     pre-answer the first-run screens (seed_config)
    python -I spike_env.py p <port> <config dir> <cwd> "<prompt with [[mock ...]]>" [claude args...]
"""
import os
import subprocess
import sys
import time

#: The Windows variables a native program needs (profile folders, system paths, the processor).
KEEP = ["SystemRoot", "SystemDrive", "windir", "USERPROFILE", "HOMEDRIVE", "HOMEPATH", "APPDATA",
        "LOCALAPPDATA", "TEMP", "TMP", "ComSpec", "PATHEXT", "PROCESSOR_ARCHITECTURE",
        "NUMBER_OF_PROCESSORS", "OS", "USERNAME", "USERDOMAIN", "COMPUTERNAME", "ProgramFiles",
        "ProgramFiles(x86)", "ProgramW6432", "ProgramData", "CommonProgramFiles", "PUBLIC",
        "ALLUSERSPROFILE", "PSModulePath"]

#: The native claude.exe the spike tests (the official installer's location).
CLAUDE = os.path.expanduser(r"~\.local\bin\claude.exe")


def clean_env(port, config_dir, extra=None):
    """Build the child's environment.

    Args:
        port: The mock's port on 127.0.0.1 (becomes ANTHROPIC_BASE_URL).
        config_dir: CLAUDE_CONFIG_DIR — an isolated Claude config folder, so the spike never reads or
            writes the user's ~/.claude (transcripts, presence files, .claude.json).
        extra: Optional variables to add last (they win over everything here).
    Returns:
        A dict: the KEEP variables of this process, a PATH of System32 + Windows PowerShell + Git (Claude
        Code on Windows needs Git Bash or PowerShell 7 for its Bash tool), the mock's URL and a dummy
        ANTHROPIC_AUTH_TOKEN (sent as a Bearer header; ANTHROPIC_API_KEY would raise Claude Code's
        "use this API key?" question instead), the config folder, and the three anti-update variables.
    """
    env = {k: os.environ[k] for k in KEEP if k in os.environ}
    sysroot = env.get("SystemRoot", r"C:\Windows")
    env["PATH"] = ";".join([sysroot + r"\System32", sysroot, sysroot + r"\System32\Wbem",
                            sysroot + r"\System32\WindowsPowerShell\v1.0", r"C:\Program Files\Git\cmd",
                            r"C:\Program Files\Git\bin"])
    env["ANTHROPIC_BASE_URL"] = "http://127.0.0.1:%d" % port
    env["ANTHROPIC_AUTH_TOKEN"] = "mock-token-not-a-secret"
    env["CLAUDE_CONFIG_DIR"] = config_dir
    env["DISABLE_AUTOUPDATER"] = "1"
    env["CLAUDE_CODE_SKIP_AUTO_UPDATE"] = "1"
    env["CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC"] = "1"
    if extra:
        env.update(extra)
    return env


def seed_config(config_dir, cwd):
    """Pre-answer Claude Code's first-run screens in an isolated config folder.

    Measured on 2.1.291 with an empty CLAUDE_CONFIG_DIR: the TUI first shows the theme picker, then (with
    --dangerously-skip-permissions) the "Bypass Permissions mode" warning, then the workspace-trust
    question. Three seeds remove all three:
      - <config>/.claude.json: hasCompletedOnboarding + theme (the picker), and
        projects.<cwd>.hasTrustDialogAccepted (trust; the key is the folder with "/" separators);
      - <config>/settings.json: skipDangerousModePermissionPrompt (the warning). ⚠ The older
        .claude.json key bypassPermissionsModeAccepted no longer suppresses it on 2.1.291.

    Args:
        config_dir: The CLAUDE_CONFIG_DIR to seed (created when missing; existing keys are kept).
        cwd: The working folder the session will run in.
    Returns:
        None.
    Raises:
        OSError: When a file cannot be read or written.
        ValueError: When an existing file is not valid JSON (it is never overwritten blind).
    """
    import json
    os.makedirs(config_dir, exist_ok=True)

    def merge(path, update):
        """Read-modify-write one JSON file, keeping every key the update does not touch.

        Args:
            path: The file (created when missing).
            update: A function that edits the parsed dict in place.
        Returns:
            None.
        Raises:
            OSError: When the file cannot be read or written.
            ValueError: When the existing file is not JSON — raised BEFORE anything is written, so a
                malformed file the user may care about is never replaced blind.
        """
        data = {}
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
        update(data)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)

    def global_config(d):
        """The .claude.json seeds: onboarding done, a theme, and trust for `cwd`.

        Args:
            d: The parsed .claude.json (edited in place).
        Returns:
            None.
        """
        d["hasCompletedOnboarding"] = True
        d["theme"] = "dark"
        d.setdefault("projects", {}).setdefault(cwd.replace("\\", "/"), {})["hasTrustDialogAccepted"] = True

    merge(os.path.join(config_dir, ".claude.json"), global_config)
    merge(os.path.join(config_dir, "settings.json"), lambda d: d.__setitem__("skipDangerousModePermissionPrompt", True))


def run_headless(port, config_dir, cwd, prompt, args):
    """Run one `claude -p` turn against the mock and print what came back.

    Args:
        port: The mock's port.
        config_dir: The isolated CLAUDE_CONFIG_DIR.
        cwd: The working folder. ⚠ Keep it OUTSIDE the user's profile folder: Claude Code walks up from
            the cwd reading CLAUDE.md files, and with an isolated config dir the user's own
            ~/.claude/CLAUDE.md would then read as a project file (its @imports raise a dialog in the TUI).
        prompt: The prompt, normally holding a `[[mock ...]]` script.
        args: Extra claude arguments.
    Returns:
        claude's exit code.
    Raises:
        subprocess.TimeoutExpired: When the run takes over 180 s (a modal or a dead mock).
        OSError: When claude.exe cannot be started.
    """
    t0 = time.time()
    r = subprocess.run([CLAUDE, "-p", prompt, *args], cwd=cwd, env=clean_env(port, config_dir),
                       capture_output=True, timeout=180, stdin=subprocess.DEVNULL)
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    print("exit=%d  wall=%.2fs" % (r.returncode, time.time() - t0))
    print("--- stdout\n" + r.stdout.decode("utf-8", "replace")[:3000])
    print("--- stderr\n" + r.stderr.decode("utf-8", "replace")[:3000])
    return r.returncode


def main(argv):
    """Command line: `seed <config dir> <cwd>` or `p <port> <config dir> <cwd> <prompt> [claude args...]`.

    Args:
        argv: The arguments after the script name.
    Returns:
        The process exit code (0 for seed, claude's for p).
    Raises:
        SystemExit: On a bad command line.
        Everything seed_config / run_headless raise.
    """
    if len(argv) == 3 and argv[0] == "seed":
        seed_config(argv[1], argv[2])
        print("seeded " + argv[1])
        return 0
    if len(argv) < 5 or argv[0] != "p":
        raise SystemExit(__doc__)
    return run_headless(int(argv[1]), argv[2], argv[3], argv[4], argv[5:])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
