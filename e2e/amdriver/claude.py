"""Claude Code's side of a test: its isolated config folder (the seeds) and its transcripts (the [TR] oracle).

Agentmaster honors CLAUDE_CONFIG_DIR for everything it reads of Claude Code's (.claude.json, projects\\,
paste-cache\\), so each test gets a fresh one. The seeds make a fresh Claude Code start straight into its input
box: onboarding done, a theme chosen, and the one-time "Bypass Permissions mode" acceptance pre-answered
(Agentmaster launches with --dangerously-skip-permissions). Workspace trust is NOT seeded: Agentmaster seeds it
itself before each launch (`[trust] pre-trusted …` in hooks.log), and a test checks that it does.

A transcript is `<config>\\projects\\<encoded cwd>\\<session id>.jsonl`, one JSON object per line; the encoded
folder is the cwd with every character outside [A-Za-z0-9] replaced by '-', but the session id alone is unique,
so lookups glob for it.
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Callable, Optional

from .waits import wait_until
from .win32 import read_bytes_shared

CLAUDE_JSON_SEED = {
    "hasCompletedOnboarding": True,
    "theme": "dark",
    "autoUpdates": False,
}
"""~\\.claude.json in the isolated config folder: no first-run wizard, no theme picker, no updater."""

SETTINGS_SEED = {
    "skipDangerousModePermissionPrompt": True,
}
"""settings.json: the bypass-mode acceptance answered (the flag alone does not skip it)."""


class ClaudeConfig:
    """An isolated CLAUDE_CONFIG_DIR.

    Attributes:
        dir: The folder.
    """

    def __init__(self, path: Path) -> None:
        """Bind to a config folder.

        Args:
            path: The folder.
        Returns:
            None.
        """
        self.dir = Path(path)

    @property
    def projects(self) -> Path:
        """The transcripts' root.

        Returns:
            <config>\\projects.
        """
        return self.dir / "projects"

    def seed(self, claude_json: Optional[dict] = None, settings: Optional[dict] = None) -> None:
        """Write the seed files (overwriting).

        Args:
            claude_json: More .claude.json keys (merged over CLAUDE_JSON_SEED).
            settings: More settings.json keys (merged over SETTINGS_SEED).
        Returns:
            None.
        """
        self.dir.mkdir(parents=True, exist_ok=True)
        (self.dir / ".claude.json").write_text(json.dumps({**CLAUDE_JSON_SEED, **(claude_json or {})}, indent=2),
                                              encoding="utf-8")
        (self.dir / "settings.json").write_text(json.dumps({**SETTINGS_SEED, **(settings or {})}, indent=2),
                                               encoding="utf-8")

    def config(self) -> dict:
        """The current .claude.json (Claude Code and Agentmaster's trust seed both write it).

        Returns:
            The parsed object (empty when missing or torn).
        """
        try:
            return json.loads(read_bytes_shared(self.dir / ".claude.json").decode("utf-8"))
        except (FileNotFoundError, ValueError):
            return {}

    def transcript_path(self, session_id: str) -> Optional[Path]:
        """Where a session's transcript is.

        Args:
            session_id: The full conversation id.
        Returns:
            The path, or None while Claude Code has not written it (a session writes it at its first prompt).
        """
        if not self.projects.is_dir():
            return None
        return next(iter(self.projects.glob(f"*/{session_id}.jsonl")), None)

    def transcript(self, session_id: str) -> list[dict]:
        """A session's transcript lines.

        Args:
            session_id: The full conversation id.
        Returns:
            The parsed lines (a torn last line skipped); empty when there is no transcript.
        """
        path = self.transcript_path(session_id)
        if path is None:
            return []
        out = []
        for line in read_bytes_shared(path).decode("utf-8", errors="replace").splitlines():
            try:
                out.append(json.loads(line))
            except ValueError:
                continue
        return out

    def user_prompts(self, session_id: str) -> list[str]:
        """The texts of the user's typed prompts in a transcript (strings or text blocks of `user` lines,
        tool results and meta lines left out).

        Args:
            session_id: The full conversation id.
        Returns:
            The prompts, in order.
        """
        prompts = []
        for line in self.transcript(session_id):
            if line.get("type") != "user" or line.get("isMeta") or line.get("isCompactSummary"):
                continue
            content = (line.get("message") or {}).get("content")
            if isinstance(content, str):
                prompts.append(content)
            elif isinstance(content, list):
                texts = [b.get("text", "") for b in content if isinstance(b, dict) and b.get("type") == "text"]
                if texts:
                    prompts.append("\n".join(texts))
        return prompts

    def wait_transcript(self, session_id: str, pred: Callable[[list[dict]], bool], timeout_s: float = 30.0,
                        what: str = "a transcript condition") -> list[dict]:
        """Wait until a session's transcript satisfies a predicate.

        Args:
            session_id: The full conversation id.
            pred: The condition on the parsed lines.
            timeout_s: How long.
            what: A description for the timeout.
        Returns:
            The lines.
        Raises:
            WaitTimeout: When it never held.
        """
        return wait_until(lambda: (lambda lines: lines if lines and pred(lines) else None)(self.transcript(session_id)),
                          timeout_s, what=what)
