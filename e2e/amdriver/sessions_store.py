"""The fleet as the app saves it: <profile>\\sessions.db (SESSIONS_STORE.md), read the way sessions.json was.

Since the fleet store, Agentmaster keeps every session in a SQLite database (WAL mode, 512-byte pages) instead of
sessions.json: a session's members in one row of `sessions` (its JSON doc without "queue"), each queued prompt's members
in `prompt_state` (without "text", ordered by `ord`) and its text in `prompt_text` (NULL when the prompt had none).
`load_fleet` puts the rows back together into exactly the dicts `json.load(sessions.json)["sessions"]` gave, so no reader
changes its assertions.

The read is READ-ONLY (`mode=ro`) and short: WAL lets it read a consistent snapshot while the app writes, it never blocks
a commit, and it holds no handle on any file after it returns. A profile that has not been migrated yet (no sessions.db)
reads sessions.json as before.
"""
from __future__ import annotations

import json
import sqlite3
import urllib.request
from pathlib import Path
from typing import Any, Optional

from .win32 import read_bytes_shared


def _uri(path: Path) -> str:
    """The database's read-only SQLite URI: `mode=ro` makes the connection unable to write anything, so a test can never
    change what the app saved; the characters a URI gives meaning to (`?`, `#`, `%`, a space) are escaped by pathname2url.

    Args:
        path: The database file.
    Returns:
        The URI to pass to sqlite3.connect(..., uri=True).
    """
    return "file:" + urllib.request.pathname2url(str(path)) + "?mode=ro"


def read_store(db_path: Path) -> list[dict[str, Any]]:
    """Every session of a sessions.db as the dict sessions.json held for it, in the order they were first saved.

    A row this reader cannot parse is skipped, as the app's own load skips it (it stays in the database untouched), and so
    are prompt rows whose session row is missing.

    Args:
        db_path: The database file.
    Returns:
        The sessions.
    Raises:
        sqlite3.Error: When the database cannot be opened or read (a missing file, a damaged one).
    """
    con = sqlite3.connect(_uri(db_path), uri=True, timeout=5.0)
    try:
        texts: dict[tuple[str, str], Optional[str]] = {
            (sid, key): text for sid, key, text in con.execute("SELECT session_id, prompt_key, text FROM prompt_text")
        }
        prompts: dict[str, list[tuple[int, dict[str, Any] | Any, Optional[str]]]] = {}
        for sid, key, ord_, state in con.execute("SELECT session_id, prompt_key, ord, state FROM prompt_state"):
            try:
                parsed = json.loads(state)
            except ValueError:
                parsed = None
            prompts.setdefault(sid, []).append((ord_, parsed, texts.get((sid, key))))
        out: list[dict[str, Any]] = []
        for sid, has_queue, doc in con.execute("SELECT id, has_queue, doc FROM sessions ORDER BY ord, rowid"):
            try:
                record = json.loads(doc)
            except ValueError:
                continue
            if not isinstance(record, dict):
                continue
            if has_queue:
                queue = []
                rows = sorted(prompts.get(sid, []), key=lambda r: r[0])
                if any(state is None for _, state, _ in rows):
                    continue  # a prompt row that does not parse: the app skips the whole session, so does this reader
                for _, state, text in rows:
                    if isinstance(state, dict) and text is not None:
                        state = dict(state)
                        state["text"] = text
                    queue.append(state)
                record["queue"] = queue
            out.append(record)
        return out
    finally:
        con.close()


def load_fleet(profile_dir: Path) -> list[dict[str, Any]]:
    """The profile's sessions: sessions.db when it exists, else sessions.json (a profile from before the store).

    Args:
        profile_dir: The Agentmaster profile folder.
    Returns:
        The session dicts (empty when there is neither file).
    Raises:
        sqlite3.Error: When sessions.db exists but cannot be read.
        ValueError: When sessions.json exists but does not parse.
    """
    db = profile_dir / "sessions.db"
    if db.exists():
        return read_store(db)
    js = profile_dir / "sessions.json"
    if not js.exists():
        return []
    data = json.loads(read_bytes_shared(js).decode("utf-8-sig"))
    return list(data.get("sessions", [])) if isinstance(data, dict) else []
