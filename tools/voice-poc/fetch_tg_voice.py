# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Gets the owner's voice notes out of a session's Telegram topic, for the evaluation (doc/agentmaster/VOICE.md §7.1).

The running Agentmaster polls the bot (``getUpdates``), and a build without voice support answers a voice note "not
taken yet" and keeps nothing - not even its ``file_id``. The Bot API has no "read message N", so this tool recovers a
note by its message id, without ever calling ``getUpdates`` (which would steal the live poller's updates):

1. ``forwardMessage`` of message N into the same topic, silently: Telegram answers with the FULL message object of
   the forward, whose ``voice.file_id`` names the same file;
2. ``deleteMessage`` of that forward at once (it shows for well under a second);
3. ``getFile`` + the file download (``https://api.telegram.org/file/bot<token>/<file_path>``) into the recordings
   folder (``common.RECORDINGS_DIR``).

Message ids are found without guessing: ``post`` prints the id of the message it sends (the sentences to read), the
owner's "done" message is logged by Agentmaster as ``[msg] <sid8> queued from topic (message N, …)``, and every voice
note lies between the two. Calls are paced at one per ~1.2 s: the live app spends ~18 calls a minute of the chat's
flood budget, and this tool must not push it into a 429.

The token comes from Windows Credential Manager (``Agentmaster:messaging:telegram:<profile leaf>``) and is never
printed, logged or written anywhere.

Usage::

    python fetch_tg_voice.py post  --session ba1e0620 --text-file sentences.txt
    python fetch_tg_voice.py fetch --session ba1e0620 --after 14010 --before 14025 [--ids s1,s2,s3]
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from common import RECORDINGS_DIR

DEFAULT_PROFILE = Path.home() / ".agentmaster"


# --------------------------------------------------------------------------------------------------------------------
# Credentials and state
# --------------------------------------------------------------------------------------------------------------------


class _CREDENTIALW(ctypes.Structure):
    """wincred.h's CREDENTIALW (only the blob fields are read)."""

    _fields_ = [
        ("Flags", wt.DWORD), ("Type", wt.DWORD), ("TargetName", wt.LPWSTR), ("Comment", wt.LPWSTR),
        ("LastWritten", wt.FILETIME), ("CredentialBlobSize", wt.DWORD), ("CredentialBlob", ctypes.POINTER(ctypes.c_ubyte)),
        ("Persist", wt.DWORD), ("AttributeCount", wt.DWORD), ("Attributes", ctypes.c_void_p),
        ("TargetAlias", wt.LPWSTR), ("UserName", wt.LPWSTR),
    ]


def read_bot_token(profile: Path) -> str:
    """Reads the profile's bot token from Credential Manager, exactly as Messaging.cpp's ReadCredentialToken does.

    Args:
        profile: The Agentmaster profile folder (its leaf names the credential).

    Returns:
        The token.

    Raises:
        RuntimeError: No such credential (messaging never set up for that profile).
    """
    target = f"Agentmaster:messaging:telegram:{profile.name}"
    advapi = ctypes.WinDLL("advapi32", use_last_error=True)
    pcred = ctypes.POINTER(_CREDENTIALW)()
    if not advapi.CredReadW(target, 1, 0, ctypes.byref(pcred)):  # 1 = CRED_TYPE_GENERIC
        raise RuntimeError(f"no credential {target!r} (error {ctypes.get_last_error()})")
    try:
        size = pcred.contents.CredentialBlobSize
        raw = ctypes.string_at(pcred.contents.CredentialBlob, size)
    finally:
        advapi.CredFree(pcred)
    # The app stores the blob as UTF-16 (wchar_t); trailing NULs and line breaks are trimmed like the app does.
    return raw.decode("utf-16-le", "replace").rstrip("\x00 \r\n")


def read_owner_and_thread(profile: Path, session_prefix: str) -> tuple[int, int, str]:
    """The paired owner's chat id and the session topic's thread id, from the profile's settings and messaging state.

    Args:
        profile: The Agentmaster profile folder.
        session_prefix: The session id or its first characters.

    Returns:
        (owner chat id, topic thread id, full session id).

    Raises:
        RuntimeError: No owner is paired, or no topic belongs to that session.
    """
    doc = json.loads((profile / "settings.json").read_text(encoding="utf-8-sig"))
    # Persistence.cpp writes {"version": n, "settings": {...}}; the owner id is a STRING of digits there.
    settings = doc.get("settings", doc)
    owner = int(str(settings.get("messagingTelegramOwnerId") or "0").strip() or 0)
    if not owner:
        raise RuntimeError("no paired Telegram owner in settings.json")
    state = json.loads((profile / "messaging" / "telegram" / "state.json").read_text(encoding="utf-8-sig"))
    for topic in state.get("topics", []):
        if str(topic.get("sessionId", "")).startswith(session_prefix):
            return owner, int(topic["threadId"]), topic["sessionId"]
    raise RuntimeError(f"no topic for session {session_prefix!r}")


# --------------------------------------------------------------------------------------------------------------------
# The Bot API
# --------------------------------------------------------------------------------------------------------------------


class Bot:
    """A minimal Bot API client: JSON POSTs, paced, with 429 handling; the token stays inside it."""

    def __init__(self, token: str, pace_s: float = 1.2) -> None:
        """Creates the client.

        Args:
            token: The bot token.
            pace_s: The least time between two calls (the live app shares the chat's flood budget).
        """
        self._token = token
        self._pace = pace_s
        self._last = 0.0

    def call(self, method: str, **params) -> dict:
        """Calls one method; waits out a 429 once.

        Args:
            method: The Bot API method.
            **params: Its parameters (JSON-serializable).

        Returns:
            Telegram's whole answer (``ok`` / ``result`` / ``description``); a transport error becomes ``ok: false``.
        """
        for attempt in range(2):
            wait = self._last + self._pace - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            self._last = time.monotonic()
            req = urllib.request.Request(
                f"https://api.telegram.org/bot{self._token}/{method}",
                data=json.dumps(params).encode("utf-8"),
                headers={"Content-Type": "application/json"},
            )
            try:
                with urllib.request.urlopen(req, timeout=30) as resp:
                    return json.loads(resp.read().decode("utf-8"))
            except urllib.error.HTTPError as err:
                body = json.loads(err.read().decode("utf-8", "replace") or "{}")
                retry = int((body.get("parameters") or {}).get("retry_after", 0))
                if err.code == 429 and retry and attempt == 0:
                    time.sleep(retry + 1)
                    continue
                return body or {"ok": False, "description": f"HTTP {err.code}"}
            except Exception as exc:  # DNS, TLS, timeout: reported, never raised (the token is in the URL: no repr)
                return {"ok": False, "description": f"transport: {type(exc).__name__}"}
        return {"ok": False, "description": "429 twice"}

    def download(self, file_path: str, dest: Path) -> int:
        """Downloads a file ``getFile`` named.

        Args:
            file_path: The ``file_path`` from getFile.
            dest: Where to write it.

        Returns:
            The number of bytes written.

        Raises:
            urllib.error.URLError: The download failed.
        """
        with urllib.request.urlopen(f"https://api.telegram.org/file/bot{self._token}/{file_path}", timeout=60) as resp:
            data = resp.read()
        dest.write_bytes(data)
        return len(data)


# --------------------------------------------------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------------------------------------------------


def cmd_post(args) -> int:
    """Posts a text into the session's topic (the sentences to read) and prints the new message's id.

    Args:
        args: The parsed command line (profile, session, text_file).

    Returns:
        The exit code (0 = posted).
    """
    owner, thread, sid = read_owner_and_thread(args.profile, args.session)
    bot = Bot(read_bot_token(args.profile))
    text = Path(args.text_file).read_text(encoding="utf-8").strip()
    r = bot.call("sendMessage", chat_id=owner, message_thread_id=thread, text=text,
                 link_preview_options={"is_disabled": True})
    if not r.get("ok"):
        print(f"post failed: {r.get('description')}", file=sys.stderr)
        return 1
    print(f"posted message {r['result']['message_id']} in thread {thread} of {sid[:8]}")
    return 0


def cmd_fetch(args) -> int:
    """Recovers every voice / audio / video note between two message ids (exclusive) and saves them in order.

    Args:
        args: The parsed command line (profile, session, after, before, ids, out).

    Returns:
        The exit code (0 = as many notes saved as ids asked for, or at least one when no ids were given).
    """
    owner, thread, sid = read_owner_and_thread(args.profile, args.session)
    bot = Bot(read_bot_token(args.profile))
    names = [n.strip() for n in args.ids.split(",")] if args.ids else []
    args.out.mkdir(parents=True, exist_ok=True)
    found: list[dict] = []
    for mid in range(args.after + 1, args.before):
        r = bot.call("forwardMessage", chat_id=owner, from_chat_id=owner, message_id=mid,
                     message_thread_id=thread, disable_notification=True)
        if not r.get("ok"):
            continue  # a deleted id, a service message, or one that cannot be forwarded
        msg = r["result"]
        # Delete the forward first, whatever it holds: it must not stay in the topic.
        bot.call("deleteMessage", chat_id=owner, message_id=msg["message_id"])
        media = msg.get("voice") or msg.get("audio") or msg.get("video_note")
        if not media:
            continue
        origin = msg.get("forward_origin") or {}
        found.append({"message_id": mid, "media": media, "date": origin.get("date"),
                      "kind": "voice" if msg.get("voice") else ("audio" if msg.get("audio") else "video_note")})
        print(f"message {mid}: {found[-1]['kind']} {media.get('duration')} s, {media.get('mime_type')}, "
              f"{media.get('file_size')} bytes")
    if not found:
        print("no voice notes in that range", file=sys.stderr)
        return 1
    manifest = []
    for i, item in enumerate(found):
        name = names[i] if i < len(names) else f"note-{item['message_id']}"
        info = bot.call("getFile", file_id=item["media"]["file_id"])
        if not info.get("ok"):
            print(f"getFile {item['message_id']}: {info.get('description')}", file=sys.stderr)
            continue
        ext = ".oga" if item["kind"] == "voice" else Path(info["result"]["file_path"]).suffix or ".bin"
        dest = args.out / f"{name}{ext}"
        size = bot.download(info["result"]["file_path"], dest)
        manifest.append({"id": name, "file": dest.name, "message_id": item["message_id"], "kind": item["kind"],
                         "duration": item["media"].get("duration"), "mime_type": item["media"].get("mime_type"),
                         "bytes": size, "date": item["date"], "session": sid, "thread": thread})
        print(f"saved {dest} ({size} bytes)")
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    return 0 if (not names or len(manifest) >= len(names)) else 1


def main(argv: list[str] | None = None) -> int:
    """The command line (see the module docstring).

    Args:
        argv: The arguments (None = sys.argv).

    Returns:
        The exit code.
    """
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profile", type=Path, default=DEFAULT_PROFILE, help="the Agentmaster profile folder")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("post", help="post a text into the session's topic")
    p.add_argument("--session", required=True)
    p.add_argument("--text-file", required=True)
    f = sub.add_parser("fetch", help="recover the voice notes between two message ids")
    f.add_argument("--session", required=True)
    f.add_argument("--after", type=int, required=True, help="the last message id BEFORE the notes")
    f.add_argument("--before", type=int, required=True, help="the first message id AFTER the notes")
    f.add_argument("--ids", default="", help="names for the notes in order (s1,s2,s3)")
    f.add_argument("--out", type=Path, default=RECORDINGS_DIR)
    for sp in (p, f):
        sp.add_argument("--profile", type=Path, default=DEFAULT_PROFILE, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    return cmd_post(args) if args.cmd == "post" else cmd_fetch(args)


if __name__ == "__main__":
    sys.exit(main())
