"""Reading Claude Code's Messages API requests: who is asking, for what, and where the turn's script is.

Claude Code sends more than the agent loop's requests: a session-title request after the first prompt and a
compaction request at /compact both QUOTE the conversation, script included, so running the script for them
would answer a title with a tool call (seen in the spike). Every request is therefore classified before any
script runs (E2E_TESTING.md §4.2). The markers are Claude Code 2.1.291's own texts; a later version may move
them, which is what the `unknown` kind and the contract lane are for.

Pure functions over the parsed body; no I/O.
"""
from __future__ import annotations

import hashlib
import json
from typing import Any, Optional

from .script import find_scripts

#: The system-prompt phrase of the session-title side request (2.1.291).
TITLE_MARKER = "naming a coding session"
#: The text that opens the compaction request's instruction (the LAST USER message carries it).
COMPACT_MARKER = "CRITICAL: Respond with TEXT ONLY"


def text_of(content: Any) -> str:
    """The concatenated text blocks of a message's content.

    Args:
        content: A message's `content`: a string, or a list of blocks (only `text` blocks count).
    Returns:
        The text ("" for None or a list without text blocks).
    """
    if isinstance(content, str):
        return content
    if not isinstance(content, list):
        return ""
    return "\n".join(b.get("text", "") for b in content if isinstance(b, dict) and b.get("type") == "text")


def tool_results_of(content: Any) -> list[str]:
    """The texts of the tool_result blocks of a message's content.

    Args:
        content: A message's `content`.
    Returns:
        One string per tool_result block (its string content, or its text blocks joined), in order.
    """
    if not isinstance(content, list):
        return []
    out = []
    for block in content:
        if not isinstance(block, dict) or block.get("type") != "tool_result":
            continue
        inner = block.get("content")
        out.append(inner if isinstance(inner, str) else text_of(inner))
    return out


def system_text(body: dict) -> str:
    """The request's system prompt as one string.

    Args:
        body: The parsed request.
    Returns:
        The `system` text (blocks joined), "" when absent.
    """
    sysp = body.get("system")
    if isinstance(sysp, list):
        return "\n".join(b.get("text", "") for b in sysp if isinstance(b, dict))
    return sysp if isinstance(sysp, str) else ""


def session_of(body: dict) -> str:
    """Claude Code's session id, from the request's metadata.

    Args:
        body: The parsed request.
    Returns:
        metadata.user_id's `session_id` (Claude Code sends user_id as a JSON STRING holding device_id,
        account_uuid and session_id), or "" when missing or unparseable.
    """
    try:
        return json.loads((body.get("metadata") or {}).get("user_id") or "{}").get("session_id", "") or ""
    except (ValueError, AttributeError):
        return ""


def tool_schemas(body: dict) -> dict[str, dict]:
    """The request's tools by name.

    Args:
        body: The parsed request.
    Returns:
        {name: input_schema} (an empty schema for a tool without one).
    """
    out = {}
    for tool in body.get("tools") or []:
        if isinstance(tool, dict) and tool.get("name"):
            out[tool["name"]] = tool.get("input_schema") or {}
    return out


def last_user_index(msgs: list) -> Optional[int]:
    """The index of the last message with role "user".

    Args:
        msgs: The request's messages.
    Returns:
        The index, or None when there is no user message. ⚠ Not simply the last message: 2.1.291 appends
        role "system" messages (reminders, the environment) after the user's own message.
    """
    for idx in range(len(msgs) - 1, -1, -1):
        if isinstance(msgs[idx], dict) and msgs[idx].get("role") == "user":
            return idx
    return None


def scripted_index(msgs: list) -> Optional[int]:
    """The index of the newest USER message whose text holds a script.

    Args:
        msgs: The request's messages.
    Returns:
        The index, or None when no user message holds a script.
    """
    for idx in range(len(msgs) - 1, -1, -1):
        msg = msgs[idx]
        if isinstance(msg, dict) and msg.get("role") == "user" and find_scripts(text_of(msg.get("content"))):
            return idx
    return None


def classify(path: str, body: dict) -> str:
    """The kind of a request.

    Args:
        path: The URL path (/v1/messages, /v1/messages/count_tokens, ...).
        body: The parsed request.
    Returns:
        "count_tokens", "title", "compact", "main" (the agent loop or a subagent: it offers tools), or
        "unknown" (no tools and no known marker — a side request this mock does not know yet; it is
        answered with a short generic text and flagged in the log).
    """
    if "/count_tokens" in path:
        return "count_tokens"
    if TITLE_MARKER in system_text(body):
        return "title"
    msgs = body.get("messages") or []
    idx = last_user_index(msgs)
    if idx is not None and COMPACT_MARKER in text_of(msgs[idx].get("content")):
        return "compact"
    if body.get("tools"):
        return "main"
    return "unknown"


def turn_key(session: str, msgs: list, index: Optional[int]) -> str:
    """A stable key of one turn: the session plus the turn's opening user message.

    Args:
        session: The session id.
        msgs: The request's messages.
        index: The turn's user message (the scripted one, or the last user message for an unscripted turn).
    Returns:
        A short hex key; equal for every request of the same turn (and its retries).
    """
    opening = text_of(msgs[index].get("content")) if index is not None else ""
    digest = hashlib.sha256(f"{session}\x00{index}\x00{opening}".encode("utf-8", "replace")).hexdigest()
    return digest[:16]


def summarize(path: str, body: dict) -> dict:
    """The facts of a request worth logging (never the whole body; never a header).

    Args:
        path: The URL path.
        body: The parsed request.
    Returns:
        A JSON-serializable dict: kind, session, model, stream, thinking, effort, the tool names, the
        message count, the last user message's text and its tool results (each cut to 4,000 characters),
        and the opening text of the scripted message.
    """
    msgs = body.get("messages") or []
    lu = last_user_index(msgs)
    last_user = msgs[lu] if lu is not None else {}
    output_config = body.get("output_config") or {}
    return {
        "kind": classify(path, body),
        "session": session_of(body),
        "model": body.get("model"),
        "stream": bool(body.get("stream")),
        "thinking": body.get("thinking"),
        "effort": output_config.get("effort"),
        "output_format": (output_config.get("format") or {}).get("type"),
        "max_tokens": body.get("max_tokens"),
        "tools": sorted(tool_schemas(body)),
        "n_msgs": len(msgs),
        "user_text": text_of(last_user.get("content"))[:4000],
        "tool_results": [t[:4000] for t in tool_results_of(last_user.get("content"))],
    }
