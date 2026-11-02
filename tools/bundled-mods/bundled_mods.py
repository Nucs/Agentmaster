#!/usr/bin/env python3
"""Agentmaster's bundled Claude Code mods (doc/agentmaster/BUNDLED_MODS.md): copy them into the repo, embed them.

Agentmaster ships a few Claude Code mods inside TerminalApp.dll and loads them into every Claude tab it launches
(`--plugin-dir`). Their source of truth is the owner's mods folder (``~/.claude/mods``, a git repository of its own);
the repo keeps a vendored copy under ``src/cascadia/TerminalApp/AgentMaster/mods/<name>/``, and the engine embeds that
copy through the generated header ``src/cascadia/TerminalApp/AgentMaster/BundledModsData.h``. The manifest
``AgentMaster/mods/bundled-mods.json`` names the mods, the oldest Claude Code each one runs on, and the line the cog shows.

Subcommands:
  sync   Copy each listed mod from its source folder into the vendored folder, file for file with LF line ends, leaving
         out what Claude Code writes into a plugin folder it loads (``.claude-plugin/types/``) and development leftovers
         (``dev/``, ``__pycache__/``, ``node_modules/``). Files the source no longer has are removed from the vendored copy.
  gen    Write BundledModsData.h from the vendored folders: each mod's RUNTIME files (``.claude-plugin/plugin.json`` and
         every file under ``hooks/`` that is not a test), with its name and version (read from its plugin.json), its
         minimum Claude Code version and summary (from the manifest), and a SHA-256 over the files (the engine names the
         mod's folder after it and re-checks it in the unit tests).
  check  Exit 1, naming the first difference, when BundledModsData.h is not what ``gen`` would write now.

Run it with any Python 3.9+; it reads and writes only the paths above (and the source folder for ``sync``).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path

#: The repository root (this file is tools/bundled-mods/bundled_mods.py).
REPO = Path(__file__).resolve().parents[2]
#: The engine folder the vendored mods and the generated header live in.
ENGINE = REPO / "src" / "cascadia" / "TerminalApp" / "AgentMaster"
#: The vendored mods (one folder per mod) and the manifest.
MODS_DIR = ENGINE / "mods"
MANIFEST = MODS_DIR / "bundled-mods.json"
#: The generated header the engine compiles.
HEADER = ENGINE / "BundledModsData.h"

#: Folder names ``sync`` never copies: what Claude Code writes into a plugin folder it loads (``types`` under
#: ``.claude-plugin``) and development leftovers. Matched against each path component.
SKIP_PARTS = {"__pycache__", "node_modules", "dev"}

#: A literal piece holds at most this many bytes of one file. MSVC refuses a string literal past about 64 KB after
#: concatenation (C1091) and a single literal token past 16,380 bytes (C2026); the header concatenates one short
#: literal per source line into pieces of this size, and splits a source line longer than LINE_SEGMENT bytes.
PIECE_BYTES = 48 * 1024
#: The longest run of one source line inside a single literal token.
LINE_SEGMENT = 2000


@dataclass(frozen=True)
class ModFile:
    """One runtime file of a bundled mod, as the header embeds it.

    Attributes:
        path: Its path under the mod's folder, '/'-separated (``hooks/register.ts``).
        data: Its bytes, UTF-8 with LF line ends (``gen`` turns CRLF into LF, so a checkout's line-end setting never
            changes the header or the hash).
    """

    path: str
    data: bytes


@dataclass(frozen=True)
class Mod:
    """One bundled mod, read from the manifest and its vendored folder.

    Attributes:
        name: Its plugin.json name, which is also its folder name.
        version: Its plugin.json version.
        min_claude_code: The oldest Claude Code version that runs it (``2.1.287``).
        summary: The one line the cog's Claude tab shows for it (ASCII).
        files: Its runtime files, sorted by path.
        content_hash: The SHA-256 (64 hex) over ``files`` in the framing of :func:`content_hash`.
    """

    name: str
    version: str
    min_claude_code: str
    summary: str
    files: tuple[ModFile, ...]
    content_hash: str


def load_manifest() -> dict:
    """Reads the manifest.

    Returns:
        The parsed bundled-mods.json object.

    Raises:
        SystemExit: When the manifest is missing or does not parse, or a listed mod lacks a name, minClaudeCode or
            summary (each with a message saying which).
    """
    try:
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise SystemExit(f"cannot read {MANIFEST}: {exc}") from exc
    for entry in manifest.get("mods", []):
        for key in ("name", "minClaudeCode", "summary"):
            if not isinstance(entry.get(key), str) or not entry[key].strip():
                raise SystemExit(f"{MANIFEST}: a mod entry has no {key}: {entry}")
        if not entry["summary"].isascii():
            raise SystemExit(f"{MANIFEST}: {entry['name']}'s summary must be ASCII (it becomes a wide C++ literal)")
    return manifest


def source_root(manifest: dict, override: str | None) -> Path:
    """Resolves the folder ``sync`` copies from.

    Args:
        manifest: The parsed manifest; its ``source`` may start with ``~``.
        override: A folder given on the command line, which wins.

    Returns:
        The absolute source folder.
    """
    raw = override or manifest.get("source") or "~/.claude/mods"
    return Path(os.path.expanduser(raw)).resolve()


def is_skipped(relative: Path) -> bool:
    """Tells whether ``sync`` leaves a path out.

    Args:
        relative: The path under the mod's folder.

    Returns:
        True for the engine-written ``.claude-plugin/types`` tree and for any path through a folder in SKIP_PARTS.
    """
    parts = relative.parts
    if len(parts) >= 2 and parts[0] == ".claude-plugin" and parts[1] == "types":
        return True
    return any(part in SKIP_PARTS for part in parts)


def sync(manifest: dict, override: str | None) -> int:
    """Copies every listed mod from the source folder into the vendored folder.

    The copy mirrors the source: a file the source no longer has is deleted from the vendored copy (only regular files
    inside ``AgentMaster/mods/<name>/``; the manifest and README beside them are never touched).

    Args:
        manifest: The parsed manifest.
        override: The source folder from the command line, or None for the manifest's.

    Returns:
        0 on success.

    Raises:
        SystemExit: When a listed mod has no folder (or no .claude-plugin/plugin.json) in the source.
    """
    root = source_root(manifest, override)
    for entry in manifest["mods"]:
        name = entry["name"]
        src = root / name
        if not (src / ".claude-plugin" / "plugin.json").is_file():
            raise SystemExit(f"{src} holds no .claude-plugin/plugin.json: is the source folder right?")
        dst = MODS_DIR / name
        wanted: set[Path] = set()
        for path in sorted(src.rglob("*")):
            relative = path.relative_to(src)
            if path.is_dir() or is_skipped(relative):
                continue
            wanted.add(relative)
            target = dst / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            # CRLF -> LF, as `gen` embeds them: the repository commits these files byte for byte (no line-end
            # conversion), so a source checked out with CRLF on one machine and LF on another never churns the copy.
            target.write_bytes(path.read_bytes().replace(b"\r\n", b"\n"))
        if dst.is_dir():
            for path in sorted(dst.rglob("*"), reverse=True):
                relative = path.relative_to(dst)
                if path.is_file() and relative not in wanted:
                    path.unlink()
                elif path.is_dir() and not any(path.iterdir()):
                    path.rmdir()
        print(f"synced {name}: {len(wanted)} files from {src}")
    return 0


def runtime_files(mod_dir: Path) -> tuple[ModFile, ...]:
    """Collects the files of a mod that Claude Code loads, as the header embeds them.

    Args:
        mod_dir: The vendored mod folder.

    Returns:
        ``.claude-plugin/plugin.json`` and every file under ``hooks/`` except tests (``*.test.*``), sorted by path, each
        with CRLF turned into LF.

    Raises:
        SystemExit: When the folder has no plugin.json or no hooks/hooks.json (the mod would load nothing).
    """
    files = []
    plugin = mod_dir / ".claude-plugin" / "plugin.json"
    hooks = mod_dir / "hooks"
    if not plugin.is_file() or not (hooks / "hooks.json").is_file():
        raise SystemExit(f"{mod_dir} lacks .claude-plugin/plugin.json or hooks/hooks.json: run `sync` first")
    candidates = [plugin] + sorted(p for p in hooks.rglob("*") if p.is_file() and ".test." not in p.name)
    for path in candidates:
        relative = path.relative_to(mod_dir).as_posix()
        files.append(ModFile(relative, path.read_bytes().replace(b"\r\n", b"\n")))
    return tuple(sorted(files, key=lambda f: f.path))


def content_hash(files: tuple[ModFile, ...]) -> str:
    """Hashes a mod's runtime files in the framing the engine's unit test recomputes.

    For each file in path order: its path, a LF, its byte count in decimal, a LF, then its bytes.

    Args:
        files: The runtime files, sorted by path.

    Returns:
        The SHA-256 as 64 lowercase hex digits.
    """
    digest = hashlib.sha256()
    for f in files:
        digest.update(f.path.encode("utf-8") + b"\n" + str(len(f.data)).encode("ascii") + b"\n")
        digest.update(f.data)
    return digest.hexdigest()


def load_mods(manifest: dict) -> list[Mod]:
    """Reads every listed mod's vendored folder.

    Args:
        manifest: The parsed manifest.

    Returns:
        The mods in manifest order.

    Raises:
        SystemExit: When a mod's plugin.json does not parse, or names another plugin, or has no version.
    """
    mods = []
    for entry in manifest["mods"]:
        name = entry["name"]
        files = runtime_files(MODS_DIR / name)
        plugin = json.loads(next(f for f in files if f.path == ".claude-plugin/plugin.json").data.decode("utf-8"))
        if plugin.get("name") != name:
            raise SystemExit(f"{name}: its plugin.json names {plugin.get('name')!r}")
        if not isinstance(plugin.get("version"), str) or not plugin["version"]:
            raise SystemExit(f"{name}: its plugin.json has no version")
        mods.append(Mod(name, plugin["version"], entry["minClaudeCode"], entry["summary"], files, content_hash(files)))
    return mods


def escape_byte(b: int) -> str:
    """Spells one byte inside a C++ narrow string literal.

    Octal escapes have at most three digits, so unlike ``\\x`` they never swallow a following digit; ``?`` is escaped
    so no trigraph can form.

    Args:
        b: The byte.

    Returns:
        Its spelling.
    """
    if b == 0x09:
        return "\\t"
    if b == 0x22:
        return '\\"'
    if b == 0x5C:
        return "\\\\"
    if b == 0x3F:
        return "\\?"
    if 0x20 <= b <= 0x7E:
        return chr(b)
    return f"\\{b:03o}"


def literal_lines(data: bytes) -> list[str]:
    """Spells bytes as C++ string literal tokens, one per source line (long lines split).

    Args:
        data: The bytes to spell.

    Returns:
        The tokens, each a quoted literal; their concatenation is ``data``.
    """
    tokens = []
    line = []
    count = 0
    for b in data:
        if b == 0x0A:
            line.append("\\n")
            tokens.append('"' + "".join(line) + '"')
            line, count = [], 0
            continue
        line.append(escape_byte(b))
        count += 1
        if count >= LINE_SEGMENT:
            tokens.append('"' + "".join(line) + '"')
            line, count = [], 0
    if line:
        tokens.append('"' + "".join(line) + '"')
    return tokens


def pieces_of(data: bytes) -> list[bytes]:
    """Splits a file into pieces of at most PIECE_BYTES bytes, at line ends where it can.

    Args:
        data: The file's bytes.

    Returns:
        The pieces in order; one empty piece for an empty file.
    """
    if not data:
        return [b""]
    pieces = []
    start = 0
    while start < len(data):
        end = min(start + PIECE_BYTES, len(data))
        if end < len(data):
            cut = data.rfind(b"\n", start, end)
            if cut > start:
                end = cut + 1
        pieces.append(data[start:end])
        start = end
    return pieces


def ident(*parts: str) -> str:
    """Makes a C++ identifier from names and paths.

    Args:
        parts: The names to join.

    Returns:
        The parts joined with ``_``, every character that cannot be in an identifier turned into ``_``.
    """
    return "_".join("".join(c if c.isalnum() else "_" for c in p) for p in parts)


def render_header(mods: list[Mod]) -> str:
    """Writes the header text.

    Args:
        mods: The mods in manifest order.

    Returns:
        BundledModsData.h's full text (ASCII, LF line ends).
    """
    out = []
    w = out.append
    w("// GENERATED by tools/bundled-mods/bundled_mods.py gen. Do not edit: change the mods under AgentMaster/mods/ (or")
    w("// their source, then `sync`) and run `gen` again. doc/agentmaster/BUNDLED_MODS.md.")
    w("//")
    w("// The Claude Code mods Agentmaster ships inside TerminalApp.dll: every RUNTIME file of each one (its plugin.json and")
    w("// its hooks), byte for byte (UTF-8, LF line ends), with what the engine needs to write and gate it. Only BundledMods.cpp")
    w("// includes this header: the bytes then live in one translation unit.")
    w("//")
    for m in mods:
        w(f"//   {m.name} {m.version} (Claude Code {m.min_claude_code}+): {len(m.files)} files, sha256 {m.content_hash}")
    w("")
    w("#pragma once")
    w("")
    w("#include <cstddef>")
    w("#include <string_view>")
    w("")
    w("namespace Agentmaster::BundledModsData")
    w("{")
    w("    // One runtime file of a bundled mod.")
    w("    struct File")
    w("    {")
    w("        std::wstring_view path; // under the mod's folder, '/'-separated: L\".claude-plugin/plugin.json\"")
    w("        const std::string_view* pieces; // the bytes in order, split so no literal passes MSVC's limits")
    w("        std::size_t pieceCount; // how many pieces")
    w("        std::size_t size; // the file's byte count, the pieces' sizes summed")
    w("    };")
    w("")
    w("    // One bundled mod: its identity, its gate and its files.")
    w("    struct Mod")
    w("    {")
    w("        std::wstring_view name; // its plugin.json name (and folder name)")
    w("        std::wstring_view version; // its plugin.json version")
    w("        std::wstring_view minClaudeCode; // the oldest Claude Code that runs it, \"major.minor.patch\"")
    w("        std::wstring_view summary; // the line the cog's Claude tab shows for it")
    w("        std::wstring_view contentHash; // SHA-256 (64 hex) over the files: per file its path, LF, size, LF, bytes")
    w("        const File* files; // sorted by path")
    w("        std::size_t fileCount; // how many files")
    w("    };")
    w("")
    w("    namespace detail")
    w("    {")
    for m in mods:
        for f in m.files:
            pieces = pieces_of(f.data)
            names = []
            for i, piece in enumerate(pieces):
                var = "k" + ident(m.name, f.path, str(i))
                names.append(var)
                tokens = literal_lines(piece) or ['""']
                # The explicit length: the view never depends on a terminating NUL (a NUL byte in a file, or a
                # literal that is not the whole piece, can then never shorten it).
                w(f"        inline constexpr std::string_view {var}{{")
                for t in tokens[:-1]:
                    w(f"            {t}")
                w(f"            {tokens[-1]}, {len(piece)} }};")
            w(f"        inline constexpr std::string_view k{ident(m.name, f.path)}Pieces[] = {{ {', '.join(names)} }};")
        w(f"        inline constexpr File k{ident(m.name)}Files[] = {{")
        for f in m.files:
            count = len(pieces_of(f.data))
            w(f"            {{ L\"{f.path}\", k{ident(m.name, f.path)}Pieces, {count}, {len(f.data)} }},")
        w("        };")
    w("    }")
    w("")
    w("    // How many mods kMods holds (a manifest listing none still compiles: kMods then holds one empty record).")
    w(f"    inline constexpr std::size_t kModCount = {len(mods)};")
    w("")
    w("    // The bundled mods, in --plugin-dir order; read only the first kModCount.")
    w(f"    inline constexpr Mod kMods[{max(1, len(mods))}] = {{")
    for m in mods:
        w(f"        {{ L\"{m.name}\", L\"{m.version}\", L\"{m.min_claude_code}\", L\"{m.summary}\", L\"{m.content_hash}\", detail::k{ident(m.name)}Files, {len(m.files)} }},")
    if not mods:
        w("        {},")
    w("    };")
    w("}")
    w("")
    return "\n".join(out)


def main(argv: list[str]) -> int:
    """Runs one subcommand.

    Args:
        argv: The command line after the program name.

    Returns:
        The exit code: 0 on success, 1 when ``check`` finds the header stale.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    sync_p = sub.add_parser("sync", help="copy the listed mods from their source folder into the repo")
    sync_p.add_argument("--from", dest="source", help="the source mods folder (default: the manifest's, ~/.claude/mods)")
    sub.add_parser("gen", help="write BundledModsData.h from the vendored mods")
    sub.add_parser("check", help="exit 1 when BundledModsData.h is stale")
    args = parser.parse_args(argv)
    manifest = load_manifest()
    if args.cmd == "sync":
        return sync(manifest, args.source)
    text = render_header(load_mods(manifest))
    if args.cmd == "gen":
        # newline="\n": the header is LF on every platform, so a Windows run writes the bytes a Linux run would.
        with open(HEADER, "w", encoding="ascii", newline="\n") as fh:
            fh.write(text)
        print(f"wrote {HEADER} ({len(text)} bytes)")
        return 0
    current = HEADER.read_text(encoding="ascii") if HEADER.is_file() else ""
    current = current.replace("\r\n", "\n")
    if current == text:
        print("BundledModsData.h is current")
        return 0
    for i, (a, b) in enumerate(zip(current.splitlines(), text.splitlines()), start=1):
        if a != b:
            print(f"BundledModsData.h is stale at line {i}:\n  have: {a[:200]}\n  want: {b[:200]}")
            break
    else:
        print("BundledModsData.h is stale (its length differs)")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
