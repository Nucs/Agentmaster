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

# ⚠ The examples name NO word of the evaluation sentences (references.json): a correction prompt that quotes the test's own
# terms would score itself. Keep it that way when editing.
SYSTEM_PROMPT = """You fix speech-recognition errors in a software developer's dictated message to an AI coding agent.
The message was spoken aloud and transcribed by speech models; the glossary lists the technical terms of the
developer's project and tools. The speaker may have an accent, so a word can come out as a similar-sounding word
(for example "dirty" for "thirty", "seat" for "sheet", "kube control" for "kubectl").

Rules:
- Keep the speaker's words, their order and their meaning. Never add content, answer, summarize or rephrase.
- Fix only recognition errors: a word that SOUNDS like a glossary term becomes that term with the glossary's exact
  spelling (for example "duck DB" -> "DuckDB", "win RT" -> "WinRT"); a word that makes no sense in a developer's prompt
  becomes the similar-sounding word that does; identifiers and acronyms get their usual casing; a stray filler or
  article the grammar rejects is dropped; numbers stay as the transcript writes them; punctuation is fixed.
- When SEVERAL transcripts of the same recording are given, they are independent hearings by different engines: where
  they agree, keep it; where they differ, choose for each disputed word the reading the audio most likely held - the
  one that fits the sentence, a developer's prompt and the glossary. Output ONE message.
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


def accept_correction(before: str, after: str) -> bool:
    """Whether a correction is plausible enough to replace the transcript: not empty, and within 0.6x-1.5x of its length
    (plus slack for a short note). A correction never adds or drops content, so a much longer or shorter answer means the
    model ANSWERED the prompt instead of fixing it - the transcript then stands. VoiceCorrect.h's AcceptCorrection is the
    C++ twin (the goldens pin both).

    Args:
        before: The transcript the model was given (the primary engine's, after the dictionary pass).
        after: The model's text.

    Returns:
        True to use the correction.
    """
    a = after.strip()
    if not a:
        return False
    return 0.6 * len(before) - 10 <= len(a) <= 1.5 * len(before) + 20


def build_user_prompt(text: str, glossary: str, alternatives: tuple[str, ...] = ()) -> str:
    """The user message of one correction: the glossary, then the transcript - or, with alternatives, every engine's
    transcript of the same recording labeled A, B, ... (the first is the primary engine's).

    Args:
        text: The primary transcript.
        glossary: The glossary line.
        alternatives: Other engines' transcripts of the same recording (identical ones are dropped).

    Returns:
        The prompt text.
    """
    others = [a for a in alternatives if a and a != text]
    if not others:
        return f"Glossary: {glossary}\n\nTranscript:\n{text}\n"
    lines = [f"{chr(ord('A') + i)}: {t}" for i, t in enumerate([text] + others)]
    return f"Glossary: {glossary}\n\nTranscripts of the same recording by different speech engines:\n" + "\n".join(lines) + "\n"


@lru_cache(maxsize=256)
def _fix_cached(text: str, glossary: str, model: str, effort: str, alternatives: tuple[str, ...] = ()) -> tuple[str, float]:
    """One correction call (cached: the evaluation sends the same transcript from several configurations).

    Args:
        text: The transcript.
        glossary: The glossary line.
        model: The model id ("claude-haiku-5-5").
        effort: The effort level ("low").
        alternatives: Other engines' transcripts of the same recording (multi-hypothesis fusion).

    Returns:
        (the corrected text, the wall seconds).

    Raises:
        RuntimeError: claude failed, timed out or answered with an error.
    """
    with tempfile.TemporaryDirectory() as tmp:
        sp = Path(tmp) / "system.txt"
        sp.write_text(SYSTEM_PROMPT, encoding="utf-8")
        prompt = Path(tmp) / "prompt.txt"
        prompt.write_text(build_user_prompt(text, glossary, alternatives), encoding="utf-8")
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
            max_terms: int = 160, alternatives: tuple[str, ...] = ()) -> tuple[str, float]:
    """Corrects a transcript with a headless Claude Code call, given the session glossary - and, for multi-hypothesis
    FUSION, other engines' transcripts of the same recording (where engines disagree, the audio was unclear: the model
    picks per disputed word, VOICE.md section 5.5).

    Args:
        text: The transcript (after or before the dictionary pass).
        vocab: The session vocabulary (None = the built-in glossary is not sent either; the model fixes by ear alone).
        model: The model id.
        effort: The effort level.
        max_terms: The most glossary terms sent (highest weight first).
        alternatives: Other engines' transcripts (after their own dictionary pass); empty = a single-engine correction.

    Returns:
        (the corrected text, the wall seconds of the call; 0.0 when the result came from the cache).

    Raises:
        RuntimeError: The call failed (the caller keeps the uncorrected text).
    """
    glossary = vocab.glossary_line(max_terms) if vocab else ""
    hits_before = _fix_cached.cache_info().hits
    fixed, seconds = _fix_cached(text, glossary, model, effort, tuple(alternatives))
    from_cache = _fix_cached.cache_info().hits > hits_before
    if not accept_correction(text, fixed):
        # The model answered instead of correcting (or said nothing): the transcript stands, as in the app.
        fixed = text
    return fixed, (0.0 if from_cache else seconds)
