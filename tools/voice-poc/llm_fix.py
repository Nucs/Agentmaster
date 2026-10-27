# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The optional third layer: FluidVoice's "AI Enhancement", as one headless Claude Code call (doc/agentmaster/VOICE.md
§5.3).

The model gets the transcript and the session glossary and may fix ONLY recognition errors (a term the sound matches,
an identifier's casing, digits for quantities, punctuation). It must never answer, add or drop content: the text it
returns is the prompt the session receives. Agentmaster already runs hidden ``claude.exe`` children for ``/status``
(``ClaudeStatusProbe``) and the AI search; the integration would reuse that seam (one more ``AM_HARNESS`` child).

The command line is the verified clean recipe of this PC (memory ``claude-p-headless-on-this-machine``): the parent
session's variables removed, no settings sources, no tools, no MCP, no session file, a bounded retry budget.
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import time
from functools import lru_cache
from pathlib import Path

from vocab import Vocabulary

#: The real claude.exe, never the Agentmaster PATH shim (which adds its hook settings to every call).
CLAUDE_EXE = Path(os.environ.get("AMV_CLAUDE_EXE", str(Path.home() / ".local" / "bin" / "claude.exe")))

SYSTEM_PROMPT = """You fix speech-recognition errors in a software developer's dictated message to an AI coding agent.
The message was spoken aloud and transcribed by a speech model; the glossary lists the technical terms of the
developer's project and tools.

Rules:
- Keep the speaker's words, their order and their meaning. Never add, drop, answer, summarize or rephrase anything.
- Fix only recognition errors: a word that SOUNDS like a glossary term becomes that term with the glossary's exact
  spelling (for example "con PTY" -> "ConPTY", "Jason L" -> "JSONL"); identifiers and acronyms get their usual casing;
  quantities become digits only if the transcript already uses digits elsewhere, otherwise keep them as spoken;
  punctuation is fixed.
- If nothing needs fixing, return the transcript unchanged.
- Output ONLY the corrected message, with no quotes, labels or commentary."""

# The parent session's variables a child must not inherit (it would report the parent's Agentmaster tab, reach its
# messaging socket, or run at the parent's effort level).
_STRIP_ENV = (
    "CLAUDECODE", "CLAUDE_CODE_CHILD_SESSION", "CLAUDE_CODE_SESSION_ID", "CLAUDE_CODE_ENTRYPOINT", "AI_AGENT",
    "CLAUDE_CODE_MESSAGING_SOCKET", "CLAUDE_CODE_MESSAGING_TOKEN", "CLAUDE_CODE_SESSION_ATTENDED",
    "CLAUDE_CODE_EXECPATH", "CLAUDE_PID", "AM_SESSION", "WT_SESSION", "CCMGR_HOOK_PIPE", "CCMGR_SESSION_ID",
    "CLAUDE_CODE_EFFORT_LEVEL", "CLAUDE_EFFORT", "DEBUG", "CLAUDE_CODE_PLUGIN_DIRS",
)


def _child_env() -> dict[str, str]:
    """The environment of the headless child: the user's own, minus the parent session's variables, plus the switches
    that keep a scripted claude from updating itself or writing anything under ``~/.claude``.

    Returns:
        A new environment dictionary.
    """
    env = {k: v for k, v in os.environ.items() if k not in _STRIP_ENV}
    env.update({
        "DISABLE_AUTOUPDATER": "1",
        "CLAUDE_CODE_SKIP_AUTO_UPDATE": "1",
        "CLAUDE_CODE_DISABLE_AUTO_MEMORY": "1",
        "AM_HARNESS": "voice-fix",  # Agentmaster's Fleet Observer and CLI skip a child that carries it
    })
    return env


@lru_cache(maxsize=256)
def _fix_cached(text: str, glossary: str, model: str, effort: str) -> tuple[str, float]:
    """One correction call (cached: the evaluation sends the same transcript from several configurations).

    Args:
        text: The transcript.
        glossary: The glossary line.
        model: The model id ("claude-haiku-5-5").
        effort: The effort level ("low").

    Returns:
        (the corrected text, the wall seconds).

    Raises:
        RuntimeError: claude failed, timed out or answered with an error.
    """
    with tempfile.TemporaryDirectory() as tmp:
        sp = Path(tmp) / "system.txt"
        sp.write_text(SYSTEM_PROMPT, encoding="utf-8")
        prompt = Path(tmp) / "prompt.txt"
        prompt.write_text(f"Glossary: {glossary}\n\nTranscript:\n{text}\n", encoding="utf-8")
        settings = json.dumps({
            "verbose": False,
            "env": {"CLAUDE_CODE_MAX_RETRIES": "2", "CLAUDE_CODE_RETRY_WATCHDOG": "0", "CLAUDE_CODE_EFFORT_LEVEL": effort},
        })
        cmd = [
            str(CLAUDE_EXE), "-p", "--model", model, "--no-session-persistence", "--output-format", "json",
            "--strict-mcp-config", "--mcp-config", '{"mcpServers":{}}', "--system-prompt-file", str(sp),
            "--tools", "", "--permission-mode", "dontAsk", "--setting-sources", "", "--settings", settings,
        ]
        t0 = time.perf_counter()
        with prompt.open("rb") as stdin:
            # The working folder is the temp dir: no CLAUDE.md, no project settings, nothing of the repository loaded.
            proc = subprocess.run(cmd, stdin=stdin, capture_output=True, cwd=tmp, env=_child_env(), timeout=120)
        seconds = time.perf_counter() - t0
    if proc.returncode != 0:
        raise RuntimeError(f"claude exited {proc.returncode}: {proc.stderr.decode('utf-8', 'replace')[:400]}")
    doc = json.loads(proc.stdout.decode("utf-8", "replace"))
    if isinstance(doc, list):  # a verbose configuration prints every message; the result is the last object
        doc = doc[-1]
    if doc.get("is_error"):
        raise RuntimeError(f"claude answered an error: {str(doc.get('result'))[:400]}")
    return str(doc.get("result", "")).strip(), seconds


def llm_fix(text: str, vocab: Vocabulary | None, model: str = "claude-haiku-5-5", effort: str = "low",
            max_terms: int = 160) -> tuple[str, float]:
    """Corrects a transcript with a headless Claude Code call, given the session glossary.

    Args:
        text: The transcript (after or before the dictionary pass).
        vocab: The session vocabulary (None = the built-in glossary is not sent either; the model fixes by ear alone).
        model: The model id.
        effort: The effort level.
        max_terms: The most glossary terms sent (highest weight first).

    Returns:
        (the corrected text, the wall seconds of the call; 0.0 when the result came from the cache).

    Raises:
        RuntimeError: The call failed (the caller keeps the uncorrected text).
    """
    glossary = ", ".join(t.text for t in (vocab.ranked()[:max_terms] if vocab else []))
    hits_before = _fix_cached.cache_info().hits
    fixed, seconds = _fix_cached(text, glossary, model, effort)
    from_cache = _fix_cached.cache_info().hits > hits_before
    return fixed, (0.0 if from_cache else seconds)
