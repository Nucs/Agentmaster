#!/usr/bin/env python3
"""The dev identity's icon: Windows Terminal Dev's icon with an ORANGE pill behind "DEV" (Agentmaster).

Agentmaster's two package identities share one branding (WT_BRANDING_DEV), and with it Windows Terminal Dev's icon:
the terminal with a lime-green "DEV" pill (res/terminal/images-Dev, drawn from res/terminal/Terminal_Dev.svg, whose
pill is ``fill:#9bf00b``). The release identity (``Agentmaster``) keeps that set. The dev identity (``AgentmasterDev``)
wears res/terminal/images-AgentmasterDev, which this script derives from it, so the two installs tell apart at a glance
on the taskbar, in Start and in Alt+Tab. CascadiaPackage.wapproj picks the package's folder by
``AgentmasterPackageIdentity`` (its WindowsTerminalAssetSuffix), and WindowsTerminal.rc / WindowsTerminalShim.rc pick
the exes' icon by the ``AGENTMASTER_DEV_IDENTITY`` define that build/rules/Branding.targets sets from the same property.

How the recolour works: the pill is the icon's only colour. Everything else is gray (the terminal's body, its title
bar, the chevron and the cursor bar, their shadows, the black "DEV" letters), so every coloured pixel is a blend
``P = a*GREEN + (1-a)*GRAY`` of the pill with some gray: an anti-aliased edge, a letter's edge, a hand-hinted small
size. Its amount ``a`` is read from the pixel itself: blending with a gray adds the same value to R, G and B, so the
pixel's ``(R-B, G-B)`` is ``a`` times the green's, and ``a`` is their least-squares fit. ``P' = P + a*(ORANGE - GREEN)``
is then the same blend with the orange. A gray pixel has ``a = 0`` and keeps its value, a fully transparent one keeps
its bytes, and the high-contrast variants (which hold no colour at all) are copied byte for byte.

Files are rewritten at the chunk level, so every byte that is not a colour stays the original's: a palette PNG gets a
new PLTE, an RGBA PNG a new IDAT (its gAMA, cHRM, bKGD, pHYs, sBIT and text chunks kept), an ICO's 32-bit DIBs new
colour bytes (their alpha and AND masks kept) and its PNG entry a new IDAT.

Subcommands:
  gen    Write res/terminal/images-AgentmasterDev (every file of images-Dev; a file images-Dev does not have is
         removed) and res/terminal/Terminal_AgentmasterDev.svg (the vector with the orange, for drawing a new size some
         day; no build reads it). Only files whose bytes change are written, so an unchanged file keeps its time stamp
         and a dev build copies nothing for it.
  check  Exit 1, naming the first difference, when the committed folder or SVG is not what ``gen`` would write now.
         Pixels are compared decoded, so another Pillow or zlib (a different IDAT stream for the same pixels) does not
         fail it.

Options: ``--color #RRGGBB`` (default PILL_ORANGE: change the constant to change the committed icon) and ``--out DIR``
(gen only: write ``DIR/images-AgentmasterDev`` and ``DIR/Terminal_AgentmasterDev.svg`` instead, e.g. to preview a
colour). Needs Python 3.9+, Pillow and numpy. After a gen, the next build of the dev identity picks the files up by
itself: the resource compiler's tracker sees the changed .ico, and CopyAgentmasterImagesToLayout copies the changed
PNGs.
"""

from __future__ import annotations

import argparse
import io
import struct
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

#: The repository root (this file is tools/dev-icon/dev_icon.py).
REPO = Path(__file__).resolve().parents[2]
#: Windows Terminal's icon sources and the sets drawn from them.
TERMINAL = REPO / "res" / "terminal"
#: The set the release identity wears, and the source of the dev identity's.
SOURCE_DIR = TERMINAL / "images-Dev"
#: The vector the source set was drawn from.
SOURCE_SVG = TERMINAL / "Terminal_Dev.svg"
#: The dev identity's set: "images" + CascadiaPackage.wapproj's WindowsTerminalAssetSuffix for that identity.
OUT_DIR_NAME = "images-AgentmasterDev"
#: The dev identity's vector, written beside the set.
OUT_SVG_NAME = "Terminal_AgentmasterDev.svg"

#: The pill's green: Terminal_Dev.svg's ``fill:#9bf00b``, and the commonest coloured pixel of every source PNG.
GREEN = (0x9B, 0xF0, 0x0B)
#: The dev identity's pill (CSS "darkorange"). Change it here, run ``gen``, commit the folder: the next dev build
#: wears it.
PILL_ORANGE = "#FF8C00"

#: The green's R-B and G-B: the direction every green-over-gray blend lies along.
_GREEN_RB = GREEN[0] - GREEN[2]
_GREEN_GB = GREEN[1] - GREEN[2]

#: The eight bytes every PNG starts with.
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def parse_color(text: str) -> tuple[int, int, int]:
    """Read a ``#RRGGBB`` (or ``RRGGBB``) colour.

    Args:
        text: The colour as six hexadecimal digits, with or without a leading ``#``.

    Returns:
        The colour's (R, G, B), each 0-255.

    Raises:
        argparse.ArgumentTypeError: ``text`` is not six hexadecimal digits (argparse shows it as a usage error).
    """
    digits = text[1:] if text.startswith("#") else text
    if len(digits) != 6 or any(ch not in "0123456789abcdefABCDEF" for ch in digits):
        raise argparse.ArgumentTypeError(f"not a #RRGGBB colour: {text!r}")
    return int(digits[0:2], 16), int(digits[2:4], 16), int(digits[4:6], 16)


def hex_of(color: tuple[int, int, int]) -> str:
    """Spell a colour the way the command line takes it.

    Args:
        color: (R, G, B), each 0-255.

    Returns:
        ``#RRGGBB`` in upper case.
    """
    return "#%02X%02X%02X" % color


def recolor_rgb(rgb: np.ndarray, alpha: np.ndarray | None, orange: tuple[int, int, int]) -> np.ndarray:
    """Turn the pill's green into ``orange`` wherever it is blended into a pixel, keeping its blend with the grays.

    A pixel's green amount is the least-squares fit of its ``(R-B, G-B)`` to the green's, clamped to 0..1: exact for a
    blend of the green with any gray, 0 for a gray, and clamped for the few edge pixels whose straight-alpha colour lost
    its precision (alpha 17 or less in the source set, invisible either way). The pixel moves by that amount of
    ``orange - GREEN`` and is rounded half up.

    Args:
        rgb: ``(..., 3)`` uint8 colours (R, G, B order).
        alpha: The pixels' ``(...)`` uint8 alpha, or None when every pixel is opaque. A pixel of alpha 0 is returned as
            it is: its colour is never seen, and keeping it keeps the file's bytes.
        orange: The pill's new colour.

    Returns:
        A new ``(..., 3)`` uint8 array; ``rgb`` is not modified.
    """
    c = rgb.astype(np.float64)
    amount = ((c[..., 0] - c[..., 2]) * _GREEN_RB + (c[..., 1] - c[..., 2]) * _GREEN_GB) / (
        _GREEN_RB * _GREEN_RB + _GREEN_GB * _GREEN_GB
    )
    amount = np.clip(amount, 0.0, 1.0)
    if alpha is not None:
        amount = np.where(alpha > 0, amount, 0.0)
    shift = np.asarray(orange, np.float64) - np.asarray(GREEN, np.float64)
    return np.clip(np.floor(c + amount[..., None] * shift + 0.5), 0, 255).astype(np.uint8)


def png_chunks(data: bytes) -> list[tuple[bytes, bytes]]:
    """Split a PNG into its chunks, checking each one's CRC.

    Args:
        data: The whole PNG file (or an ICO's PNG entry).

    Returns:
        ``(type, body)`` for every chunk up to and including IEND, in file order.

    Raises:
        ValueError: ``data`` is not a PNG, is cut short, or has a chunk whose CRC does not match.
    """
    if data[:8] != PNG_SIGNATURE:
        raise ValueError("not a PNG")
    chunks: list[tuple[bytes, bytes]] = []
    pos = 8
    while True:
        if pos + 12 > len(data):
            raise ValueError("PNG cut short before IEND")
        (length,) = struct.unpack_from(">I", data, pos)
        kind = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        if len(body) != length or pos + 12 + length > len(data):
            raise ValueError(f"PNG chunk {kind!r} cut short")
        (crc,) = struct.unpack_from(">I", data, pos + 8 + length)
        if crc != zlib.crc32(kind + body) & 0xFFFFFFFF:
            raise ValueError(f"PNG chunk {kind!r} has a bad CRC")
        chunks.append((kind, body))
        pos += 12 + length
        if kind == b"IEND":
            return chunks


def png_join(chunks: list[tuple[bytes, bytes]]) -> bytes:
    """Write chunks back into a PNG, each with its length and CRC.

    Args:
        chunks: ``(type, body)`` in file order, IHDR first and IEND last.

    Returns:
        The PNG file's bytes.
    """
    parts = [PNG_SIGNATURE]
    for kind, body in chunks:
        crc = zlib.crc32(kind + body) & 0xFFFFFFFF
        parts.append(struct.pack(">I", len(body)) + kind + body + struct.pack(">I", crc))
    return b"".join(parts)


def recolor_png(data: bytes, orange: tuple[int, int, int]) -> bytes:
    """Recolour one PNG's pill, keeping every chunk that holds no colour.

    Gray and gray+alpha PNGs (the high-contrast variants) hold no colour and come back unchanged. A palette PNG gets its
    PLTE entries recoloured (the indices and tRNS stay). An 8-bit RGB or RGBA PNG gets its pixels recoloured and a new
    IDAT from Pillow, spliced in where the old IDAT run was; IHDR and every ancillary chunk stay the original's.

    Args:
        data: The PNG file (or an ICO's PNG entry).
        orange: The pill's new colour.

    Returns:
        The recoloured PNG; ``data`` itself (the same bytes) when no pixel changes.

    Raises:
        ValueError: Not a PNG, a damaged one (``png_chunks``), a colour PNG this does not rewrite (16-bit or
            interlaced), a palette PNG without PLTE, or Pillow encoding a header other than the original's.
    """
    chunks = png_chunks(data)
    if chunks[0][0] != b"IHDR":
        raise ValueError("PNG does not start with IHDR")
    ihdr = chunks[0][1]
    _, _, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", ihdr)
    if color_type in (0, 4):
        return data  # gray / gray+alpha: no colour to change
    if color_type == 3:
        plte = next((body for kind, body in chunks if kind == b"PLTE"), None)
        if plte is None:
            raise ValueError("palette PNG without PLTE")
        trns = next((body for kind, body in chunks if kind == b"tRNS"), b"")
        entries = np.frombuffer(plte, np.uint8).reshape(-1, 3)
        # tRNS gives the first entries' alpha; the rest are opaque. An entry of alpha 0 keeps its colour, exactly as a
        # transparent RGBA pixel does, so a palette file and an RGBA file of the same picture recolour alike.
        alpha = np.full(len(entries), 255, np.uint8)
        given = np.frombuffer(trns[: len(entries)], np.uint8)
        alpha[: len(given)] = given
        recolored = recolor_rgb(entries, alpha, orange)
        if np.array_equal(recolored, entries):
            return data
        return png_join([(kind, recolored.tobytes() if kind == b"PLTE" else body) for kind, body in chunks])
    if color_type not in (2, 6) or depth != 8 or interlace != 0:
        raise ValueError(f"colour PNG this does not rewrite (type {color_type}, depth {depth}, interlace {interlace})")
    with Image.open(io.BytesIO(data)) as image:
        mode = "RGBA" if color_type == 6 else "RGB"
        pixels = np.asarray(image.convert(mode))
    alpha = pixels[..., 3] if color_type == 6 else None
    recolored = pixels.copy()
    recolored[..., :3] = recolor_rgb(pixels[..., :3], alpha, orange)
    if np.array_equal(recolored, pixels):
        return data
    encoded = io.BytesIO()
    # fromarray reads RGBA / RGB from the array's (H, W, 4) / (H, W, 3) uint8 shape (its mode argument is deprecated).
    Image.fromarray(recolored).save(encoded, "PNG", optimize=True)
    new_chunks = png_chunks(encoded.getvalue())
    # The new IDAT is only valid under the original IHDR when Pillow chose the very same header (size, depth, colour
    # type, no interlace); a difference would make a file that decodes to garbage, so it is refused instead.
    if new_chunks[0] != (b"IHDR", ihdr):
        raise ValueError("Pillow wrote another IHDR than the original's")
    new_idat = [chunk for chunk in new_chunks if chunk[0] == b"IDAT"]
    spliced: list[tuple[bytes, bytes]] = []
    for kind, body in chunks:
        if kind != b"IDAT":
            spliced.append((kind, body))
        elif new_idat:
            # PNG keeps its IDAT chunks consecutive: the whole new run takes the place of the first old one, and
            # the rest of the old run is dropped.
            spliced.extend(new_idat)
            new_idat = []
    return png_join(spliced)


def recolor_dib(blob: bytes, orange: tuple[int, int, int]) -> bytes:
    """Recolour one 32-bit DIB of an ICO (BITMAPINFOHEADER, BGRA rows, then the AND mask).

    Only the B, G and R bytes change; the header, the alpha bytes and the AND mask stay (the alpha does not change, so
    the mask still matches it).

    Args:
        blob: The icon image as stored in the ICO.
        orange: The pill's new colour.

    Returns:
        The recoloured image; ``blob`` itself when no pixel changes.

    Raises:
        ValueError: Not an uncompressed 32-bit DIB with a 40-byte header and no colour table, or cut short.
    """
    if len(blob) < 40:
        raise ValueError("icon DIB cut short")
    header_size, width, double_height, _, bits, compression = struct.unpack_from("<IiiHHI", blob, 0)
    (colors_used,) = struct.unpack_from("<I", blob, 32)
    if header_size != 40 or bits != 32 or compression != 0 or colors_used != 0 or width <= 0 or double_height <= 0:
        raise ValueError("icon image is not an uncompressed 32-bit DIB")
    height = double_height // 2  # an icon DIB's height counts the colour rows and the AND mask's rows
    count = width * height * 4
    if len(blob) < 40 + count:
        raise ValueError("icon DIB cut short")
    pixels = np.frombuffer(blob, np.uint8, count=count, offset=40).reshape(height, width, 4)
    recolored = pixels.copy()
    # The DIB stores B, G, R, A: hand recolor_rgb R, G, B, and put its answer back in the file's order.
    recolored[..., 2::-1] = recolor_rgb(np.ascontiguousarray(pixels[..., 2::-1]), pixels[..., 3], orange)
    if np.array_equal(recolored, pixels):
        return blob
    return blob[:40] + recolored.tobytes() + blob[40 + count :]


def ico_images(data: bytes) -> tuple[list[list[int]], list[bytes]]:
    """Read an ICO's directory and images.

    Args:
        data: The ICO file.

    Returns:
        The directory entries (each ``[width, height, colours, reserved, planes, bits, size, offset]``) and each
        entry's image bytes, in directory order.

    Raises:
        ValueError: Not an icon file, or its images are not stored back to back in directory order right after the
            directory (the only layout ``recolor_ico`` rebuilds byte for byte).
    """
    if len(data) < 6:
        raise ValueError("not an icon file")
    reserved, kind, count = struct.unpack_from("<HHH", data, 0)
    if reserved != 0 or kind != 1 or count == 0 or len(data) < 6 + 16 * count:
        raise ValueError("not an icon file")
    entries = [list(struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)) for i in range(count)]
    offset = 6 + 16 * count
    for entry in entries:
        if entry[7] != offset:
            raise ValueError("icon images are not stored back to back in directory order")
        offset += entry[6]
    if offset != len(data):
        raise ValueError("icon file has bytes past its last image")
    return entries, [data[entry[7] : entry[7] + entry[6]] for entry in entries]


def recolor_ico(data: bytes, orange: tuple[int, int, int]) -> bytes:
    """Recolour every image of an ICO and lay the file out again.

    Args:
        data: The ICO file.
        orange: The pill's new colour.

    Returns:
        The recoloured ICO: the same directory (sizes and offsets updated) and the images back to back in directory
        order; ``data`` itself when no image changes.

    Raises:
        ValueError: From ``ico_images``, ``recolor_png`` or ``recolor_dib``.
    """
    entries, images = ico_images(data)
    recolored = [
        recolor_png(image, orange) if image[:8] == PNG_SIGNATURE else recolor_dib(image, orange) for image in images
    ]
    if recolored == images:
        return data
    directory = b""
    offset = 6 + 16 * len(entries)
    for entry, image in zip(entries, recolored):
        entry[6], entry[7] = len(image), offset
        directory += struct.pack("<BBBBHHII", *entry)
        offset += len(image)
    return struct.pack("<HHH", 0, 1, len(entries)) + directory + b"".join(recolored)


def recolor_file(name: str, data: bytes, orange: tuple[int, int, int]) -> bytes:
    """Recolour one file of the source set by its kind.

    Args:
        name: The file's name (its extension decides: ``.png`` or ``.ico``).
        data: The file's bytes.
        orange: The pill's new colour.

    Returns:
        The file as the dev identity's set holds it.

    Raises:
        ValueError: Another kind of file (the set holds PNGs and ICOs only), or a damaged / unsupported one.
    """
    lowered = name.lower()
    if lowered.endswith(".png"):
        return recolor_png(data, orange)
    if lowered.endswith(".ico"):
        return recolor_ico(data, orange)
    raise ValueError(f"{name}: not a PNG or an ICO")


def make_svg(orange: tuple[int, int, int]) -> bytes:
    """Write the dev identity's vector: Terminal_Dev.svg with the pill's fill in ``orange``, and a note saying so.

    Args:
        orange: The pill's new colour.

    Returns:
        The SVG file's bytes (Terminal_Dev.svg's line ends kept).

    Raises:
        ValueError: Terminal_Dev.svg does not hold the pill's ``fill:#9bf00b`` exactly once, or has no XML declaration.
        OSError: Terminal_Dev.svg cannot be read.
    """
    data = SOURCE_SVG.read_bytes()
    needle = b"fill:#9bf00b"
    if data.count(needle) != 1:
        raise ValueError(f"{SOURCE_SVG.name} does not hold the pill's {needle.decode()} exactly once")
    hex_color = ("#%02x%02x%02x" % orange).encode("ascii")
    data = data.replace(needle, b"fill:" + hex_color)
    newline = b"\r\n" if b"\r\n" in data else b"\n"
    declaration_end = data.find(b"?>")
    if not data.startswith(b"<?xml") or declaration_end < 0:
        raise ValueError(f"{SOURCE_SVG.name} has no XML declaration")
    # A comment may not precede the XML declaration, so the note goes right after it.
    note = (
        b"<!-- Agentmaster: the dev identity's icon - Terminal_Dev.svg with the DEV pill "
        + hex_color
        + b" instead of #9bf00b. Written by tools/dev-icon/dev_icon.py gen (change its PILL_ORANGE, not this file);"
        + b" the PNG and ICO set beside it, images-AgentmasterDev, is recoloured from images-Dev, not drawn from"
        + b" this file. -->"
    )
    return data[: declaration_end + 2] + newline + note + data[declaration_end + 2 :]


def source_names() -> list[str]:
    """List the source set's files.

    Returns:
        The names of the regular files in images-Dev, sorted.

    Raises:
        OSError: images-Dev cannot be listed.
    """
    return sorted(entry.name for entry in SOURCE_DIR.iterdir() if entry.is_file())


def same_image(name: str, want: bytes, got: bytes) -> bool:
    """Tell whether two encodings of one set file hold the same picture and the same non-pixel chunks.

    Args:
        name: The file's name (``.png`` or ``.ico``).
        want: The bytes ``gen`` would write now.
        got: The committed bytes.

    Returns:
        True when the bytes are equal, or the files differ only in how the same pixels are compressed (another Pillow
        or zlib): same chunk list apart from IDAT and same decoded pixels for a PNG, the same directory apart from
        sizes and offsets and the same images (compared the same way) for an ICO. False for anything else, a file that
        does not parse included.
    """
    if want == got:
        return True
    try:
        if name.lower().endswith(".ico"):
            want_entries, want_images = ico_images(want)
            got_entries, got_images = ico_images(got)
            if [entry[:6] for entry in want_entries] != [entry[:6] for entry in got_entries]:
                return False
            return all(
                same_image(name[:-4] + ".png", a, b) if a[:8] == PNG_SIGNATURE else a == b
                for a, b in zip(want_images, got_images)
            )
        if [chunk for chunk in png_chunks(want) if chunk[0] != b"IDAT"] != [
            chunk for chunk in png_chunks(got) if chunk[0] != b"IDAT"
        ]:
            return False
        with Image.open(io.BytesIO(want)) as a, Image.open(io.BytesIO(got)) as b:
            return np.array_equal(np.asarray(a.convert("RGBA")), np.asarray(b.convert("RGBA")))
    except (ValueError, OSError):
        # A committed file that does not even parse is a difference, never a crash of the check.
        return False


def cmd_gen(out_root: Path, orange: tuple[int, int, int]) -> int:
    """Write the dev identity's set and vector.

    Args:
        out_root: The folder that receives ``images-AgentmasterDev`` and ``Terminal_AgentmasterDev.svg`` (res/terminal
            for the committed set).
        orange: The pill's colour.

    Returns:
        0 (a failure raises).

    Raises:
        ValueError: A source file this cannot recolour (``recolor_file``), or the SVG check of ``make_svg``.
        OSError: A file cannot be read, written or removed.
    """
    out_dir = out_root / OUT_DIR_NAME
    out_dir.mkdir(parents=True, exist_ok=True)
    names = source_names()
    written = 0
    for name in names:
        data = recolor_file(name, (SOURCE_DIR / name).read_bytes(), orange)
        target = out_dir / name
        # Rewriting an unchanged file would only move its time stamp, and the layout copy
        # (SkipUnchangedFiles: size and time) would then copy it again at every build.
        if not target.is_file() or target.read_bytes() != data:
            target.write_bytes(data)
            written += 1
    removed = 0
    for entry in out_dir.iterdir():
        # A file images-Dev no longer has would otherwise ship forever; only files are removed, a folder never.
        if entry.is_file() and entry.name not in names:
            entry.unlink()
            removed += 1
    svg = out_root / OUT_SVG_NAME
    svg_data = make_svg(orange)
    if not svg.is_file() or svg.read_bytes() != svg_data:
        svg.write_bytes(svg_data)
        written += 1
    print(f"{out_dir}: {len(names)} files, {written} written (the SVG counted), {removed} removed; pill {hex_of(orange)}")
    return 0


def cmd_check(orange: tuple[int, int, int]) -> int:
    """Compare the committed set and vector with what ``gen`` would write now.

    Args:
        orange: The pill's colour (PILL_ORANGE unless ``--color`` says otherwise).

    Returns:
        0 when everything matches, 1 after printing the first difference.

    Raises:
        ValueError: A source file this cannot recolour, or the SVG check of ``make_svg``.
        OSError: A source file cannot be read.
    """
    out_dir = TERMINAL / OUT_DIR_NAME
    names = source_names()
    committed = sorted(entry.name for entry in out_dir.iterdir() if entry.is_file()) if out_dir.is_dir() else []
    if committed != names:
        missing = sorted(set(names) - set(committed))
        extra = sorted(set(committed) - set(names))
        print(f"{out_dir}: {len(missing)} files missing {missing[:5]}, {len(extra)} extra {extra[:5]}")
        return 1
    for name in names:
        want = recolor_file(name, (SOURCE_DIR / name).read_bytes(), orange)
        if not same_image(name, want, (out_dir / name).read_bytes()):
            print(f"{out_dir / name}: not what gen writes (run: python tools/dev-icon/dev_icon.py gen)")
            return 1
    svg = TERMINAL / OUT_SVG_NAME
    if not svg.is_file() or svg.read_bytes() != make_svg(orange):
        print(f"{svg}: not what gen writes (run: python tools/dev-icon/dev_icon.py gen)")
        return 1
    print(f"{out_dir}: {len(names)} files match; pill {hex_of(orange)}")
    return 0


def main(argv: list[str] | None = None) -> int:
    """Run the command line.

    Args:
        argv: The arguments after the script's name (None: ``sys.argv[1:]``).

    Returns:
        The process exit code: 0 on success, 1 when ``check`` finds a difference, 2 for a usage error (argparse exits).

    Raises:
        ValueError: From ``cmd_gen`` / ``cmd_check`` (a source file or the SVG this cannot handle).
        OSError: A file cannot be read or written.
    """
    parser = argparse.ArgumentParser(
        description="The dev identity's icon: Windows Terminal Dev's icon with an orange DEV pill."
    )
    commands = parser.add_subparsers(dest="command", required=True)
    gen = commands.add_parser("gen", help="write res/terminal/images-AgentmasterDev and Terminal_AgentmasterDev.svg")
    check = commands.add_parser("check", help="exit 1 when the committed set is not what gen writes")
    for command in (gen, check):
        command.add_argument(
            "--color", type=parse_color, default=parse_color(PILL_ORANGE), help=f"the pill's colour (default {PILL_ORANGE})"
        )
    gen.add_argument("--out", type=Path, default=TERMINAL, help="write into this folder instead of res/terminal")
    args = parser.parse_args(argv)
    if args.command == "gen":
        return cmd_gen(args.out, args.color)
    return cmd_check(args.color)


if __name__ == "__main__":
    sys.exit(main())
