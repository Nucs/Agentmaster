#!/usr/bin/env python3
"""The release identity's icon: the README's logo, laid out exactly like Windows Terminal Dev's icon set (Agentmaster).

The production install (package ``Agentmaster``, the release identity) wears the project's logo, the picture at the
top of the README (doc/agentmaster/img/logo.png: Windows Terminal Dev's icon redrawn by Codex with its three title-bar
tabs in the session-state colours; tools/readme-art made it). This script draws that identity's whole icon set,
res/terminal/images-Agentmaster, from the logo. The dev identity keeps its own set (res/terminal/images-AgentmasterDev,
tools/dev-icon/dev_icon.py: Windows Terminal Dev's icon with an orange pill), so the two installs still tell apart.
CascadiaPackage.wapproj picks the package's folder by ``AgentmasterPackageIdentity`` (its WindowsTerminalAssetSuffix:
``-Agentmaster`` for Release), and WindowsTerminal.rc / WindowsTerminalShim.rc give the release exes images-Agentmaster's
terminal.ico (their branch without ``AGENTMASTER_DEV_IDENTITY``).

How the set is drawn: images-Dev (drawn by res/terminal/Generate-TerminalAssets.ps1 from Terminal_Dev.svg) is the
TEMPLATE: the new folder holds exactly its files, under the same names and at the same pixel sizes, so the manifests
and resources.pri need nothing else.
  * A standard-contrast PNG is that file's canvas with the logo in the place the template's icon box takes. The
    generator renders the 48-unit icon into an N x N box and centres the box on the canvas; the icon spans the box's
    whole width (the window starts at x 0, the DEV pill ends at x 48), so the template's own opaque pixels give the
    box: its left edge and width N, and its top from the icon's top edge at 7.125 / 48 of N. N is checked against
    the generator's size table (``expected_box``), so a template that does not match it is refused, not guessed.
    Inside the box the logo fills the icon's content box (x 0..48, y 7.125..43 of 48), as large as its proportions
    allow, centred: the new icon takes the old one's place and size on the taskbar, in Start and on every tile.
  * terminal.ico is rebuilt frame by frame in the template's layout: the same sizes in the same order, a 32-bit DIB
    frame as a 32-bit DIB (its AND mask recomputed from the new alpha), a PNG frame as a PNG.
  * The high-contrast variants (``*_contrast-black*`` / ``*_contrast-white*``: Windows' contrast themes) are copied
    byte for byte. They are one-colour silhouettes of the same window-and-pill shape, which the logo keeps.
Every picture is resampled from the one logo.png (512 px; its art is ~473 px wide, more than the largest box, 384 px)
through premultiplied alpha. Pillow writes the PNGs (no metadata chunk), so ``gen`` is deterministic for a given
Pillow.

Subcommands:
  gen    Write res/terminal/images-Agentmaster (every file of images-Dev; a file images-Dev does not have is removed).
         Only files whose bytes change are written.
  check  Exit 1, naming the first difference, when the committed folder is not what ``gen`` would write now. Pictures
         are compared decoded, so another Pillow or zlib (another byte stream for the same pixels) does not fail it.

Options: ``--out DIR`` (gen only: write ``DIR/images-Agentmaster`` instead, e.g. to preview). Needs Python 3.9+,
Pillow and numpy, and tools/dev-icon/dev_icon.py beside this folder (its PNG and ICO readers are reused). After a
``gen``, the next release-identity build picks the files up by itself (CopyAgentmasterImagesToLayout and the .rc
files' tracker).
"""

from __future__ import annotations

import argparse
import io
import math
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

#: The repository root (this file is tools/release-icon/release_icon.py).
REPO = Path(__file__).resolve().parents[2]

sys.path.insert(0, str(REPO / "tools" / "dev-icon"))
import dev_icon  # noqa: E402  (its PNG chunk and ICO directory readers; importing runs nothing)

#: The logo: the README's picture, the one source of every file this script writes.
LOGO = REPO / "doc" / "agentmaster" / "img" / "logo.png"
#: The template: Windows Terminal Dev's set, whose names, sizes and placements the new set keeps.
TEMPLATE_DIR = REPO / "res" / "terminal" / "images-Dev"
#: The release identity's set: "images" + CascadiaPackage.wapproj's WindowsTerminalAssetSuffix for that identity.
OUT_DIR_NAME = "images-Agentmaster"

#: The icon's content box in Terminal_Dev.svg's 48-unit viewBox: the window spans x 0..45 and y 7.125..40.875, the DEV
#: pill x 19..48 and y 31.05..43 (the window group is scaled by 0.9375 and moved down 1.5).
CONTENT_TOP = 7.125
CONTENT_BOTTOM = 43.0
VIEWBOX = 48.0

#: The opacity a logo pixel needs to count as art when the logo is cropped to it: the README cut leaves a faint
#: anti-aliased rim around the art, which must not make the art look smaller inside the box.
ART_ALPHA = 8

#: Generate-TerminalAssets.ps1's asset table: name -> (canvas width, canvas height, icon box side) at scale 1.
ASSET_TYPES = {
    "LargeTile": (310, 310, 96),
    "LockScreenLogo": (24, 24, 24),
    "SmallTile": (71, 71, 36),
    "SplashScreen": (620, 300, 96),
    "Square44x44Logo": (44, 44, 32),
    "Square150x150Logo": (150, 150, 48),
    "StoreLogo": (50, 50, 36),
    "Wide310x150Logo": (310, 150, 48),
}


def expected_box(name: str) -> tuple[int, int, int]:
    """Read a template file's canvas size and icon box side from its name, as Generate-TerminalAssets.ps1 computes them.

    Args:
        name: A standard-contrast PNG's name, e.g. ``SmallTile.scale-125.png`` or
            ``Square44x44Logo.targetsize-24_altform-unplated.png``.

    Returns:
        (canvas width, canvas height, icon box side N), in pixels.

    Raises:
        ValueError: The name is not one the generator writes (an unknown asset type or qualifier).
    """
    stem = name[:-4] if name.lower().endswith(".png") else name
    kind, _, qualifiers = stem.partition(".")
    if kind not in ASSET_TYPES:
        raise ValueError(f"{name}: not an asset type of the generator")
    qualifier = qualifiers.split("_")[0]
    if qualifier.startswith("targetsize-"):
        n = int(qualifier[len("targetsize-"):])
        return n, n, n
    if qualifier.startswith("scale-"):
        scale = int(qualifier[len("scale-"):]) / 100.0
        width, height, icon = ASSET_TYPES[kind]
        # The canvas: [math]::Round(x, ToPositiveInfinity) rounds to the nearest integer, a half up. The box:
        # CeilToEven takes an [int] (PowerShell's cast rounds a half to even, as Python's round does) and raises an
        # odd side by one.
        box = round(icon * scale)
        box += box % 2
        return math.floor(width * scale + 0.5), math.floor(height * scale + 0.5), box
    raise ValueError(f"{name}: no targetsize or scale qualifier")


def load_art() -> np.ndarray:
    """Read the logo and crop it to its art.

    Returns:
        The art as an ``(h, w, 4)`` float64 RGBA array (0-255), straight alpha.

    Raises:
        OSError: The logo cannot be read.
        ValueError: The logo has no pixel above ART_ALPHA.
    """
    rgba = np.asarray(Image.open(LOGO).convert("RGBA")).astype(np.float64)
    ys, xs = np.nonzero(rgba[..., 3] > ART_ALPHA)
    if ys.size == 0:
        raise ValueError(f"{LOGO} holds no art")
    return rgba[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def art_icon(art: np.ndarray, n: int) -> np.ndarray:
    """Draw the logo into an N x N icon box, where Windows Terminal's icon draws itself.

    The art is scaled to fit the icon's content box (the whole width, from 7.125 to 43 of 48 down) and centred in it,
    so the new icon has the old one's footprint.

    Args:
        art: The cropped logo (``load_art``).
        n: The box side in pixels (1 or more).

    Returns:
        An ``(n, n, 4)`` uint8 RGBA array, transparent outside the art.
    """
    h, w = art.shape[:2]
    box_h = n * (CONTENT_BOTTOM - CONTENT_TOP) / VIEWBOX
    scale = min(n / w, box_h / h)
    tw, th = max(1, round(w * scale)), max(1, round(h * scale))
    source = Image.fromarray(np.clip(art, 0, 255).round().astype(np.uint8))
    # Resample premultiplied ("RGBa"): the colours of transparent pixels must not bleed into the edge.
    small = source.convert("RGBa").resize((tw, th), Image.LANCZOS).convert("RGBA")
    canvas = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    centre_y = n * (CONTENT_TOP + CONTENT_BOTTOM) / 2.0 / VIEWBOX
    canvas.paste(small, ((n - tw) // 2, int(round(centre_y - th / 2.0))))
    return np.asarray(canvas)


def template_box(name: str, data: bytes) -> tuple[int, int, int, int, int]:
    """Find where the template file draws its icon box, and check it against the generator's table.

    Args:
        name: The template file's name.
        data: Its bytes (a standard-contrast PNG).

    Returns:
        (canvas width, canvas height, box x, box y, box side N).

    Raises:
        ValueError: The file is fully transparent, its icon is not as wide as the generator's box for that name, or its
            canvas is not the generator's size for it (the template does not match Generate-TerminalAssets.ps1, so a
            placement would be a guess).
    """
    with Image.open(io.BytesIO(data)) as image:
        alpha = np.asarray(image.convert("RGBA"))[..., 3]
    want_w, want_h, want_n = expected_box(name)
    if alpha.shape != (want_h, want_w):
        raise ValueError(f"{name}: canvas {alpha.shape[1]}x{alpha.shape[0]}, the generator writes {want_w}x{want_h}")
    ys, xs = np.nonzero(alpha)
    if ys.size == 0:
        raise ValueError(f"{name}: fully transparent")
    n = int(xs.max() + 1 - xs.min())
    if n != want_n:
        raise ValueError(f"{name}: its icon is {n} px wide, the generator's box is {want_n}")
    # The icon's top edge sits at 7.125/48 of the box; its first row with any coverage is that edge's pixel row.
    box_y = int(ys.min()) - int(math.floor(CONTENT_TOP * n / VIEWBOX + 1e-9))
    return want_w, want_h, int(xs.min()), box_y, n


def png_bytes(rgba: np.ndarray) -> bytes:
    """Encode an RGBA array as a PNG with no metadata chunk.

    Args:
        rgba: An ``(h, w, 4)`` uint8 array.

    Returns:
        The PNG file's bytes.
    """
    out = io.BytesIO()
    Image.fromarray(rgba).save(out, "PNG", optimize=True)
    return out.getvalue()


def draw_png(name: str, data: bytes, art: np.ndarray) -> bytes:
    """Draw one standard-contrast PNG of the set: the template's canvas with the logo in its icon box.

    Args:
        name: The template file's name.
        data: The template file's bytes.
        art: The cropped logo.

    Returns:
        The new PNG's bytes.

    Raises:
        ValueError: From ``template_box``.
    """
    width, height, box_x, box_y, n = template_box(name, data)
    canvas = np.zeros((height, width, 4), np.uint8)
    canvas[box_y:box_y + n, box_x:box_x + n] = art_icon(art, n)
    return png_bytes(canvas)


def dib_bytes(header: bytes, rgba: np.ndarray) -> bytes:
    """Build a 32-bit icon DIB: the template frame's header, BGRA rows bottom-up, then the AND mask.

    Args:
        header: The template frame's 40-byte BITMAPINFOHEADER (same size, so it stays valid).
        rgba: The frame's ``(n, n, 4)`` uint8 RGBA pixels, top row first.

    Returns:
        The DIB as an ICO stores it.
    """
    n = rgba.shape[0]
    bgra = rgba[::-1, :, [2, 1, 0, 3]]  # a DIB stores its rows bottom-up, each pixel as B, G, R, A
    # The AND mask: 1 bit per pixel, set where the pixel is transparent, each row padded to 32 bits, bottom-up. A
    # 32-bit icon is drawn from its alpha; the mask still serves the old blitters and hit tests, so it follows the alpha.
    row_bytes = ((n + 31) // 32) * 4
    mask = np.zeros((n, row_bytes * 8), np.uint8)
    mask[:, :n] = (rgba[::-1, :, 3] == 0).astype(np.uint8)
    return header + np.ascontiguousarray(bgra).tobytes() + np.packbits(mask, axis=1).tobytes()


def draw_ico(data: bytes, art: np.ndarray) -> bytes:
    """Rebuild the template's terminal.ico with the logo, frame by frame in the template's layout.

    Args:
        data: The template ICO.
        art: The cropped logo.

    Returns:
        The new ICO: the template's frame sizes and order, each frame in the template frame's format.

    Raises:
        ValueError: From ``dev_icon.ico_images`` (not an icon file, or an unusual layout), or a DIB frame that is not a
            40-byte-header, 32-bit, uncompressed, square one.
    """
    entries, images = dev_icon.ico_images(data)
    frames = []
    for entry, image in zip(entries, images):
        n = entry[0] or 256  # a directory width of 0 means 256
        pixels = art_icon(art, n)
        if image[:8] == dev_icon.PNG_SIGNATURE:
            frames.append(png_bytes(pixels))
            continue
        header_size, width, double_height, _, bits, compression = struct.unpack_from("<IiiHHI", image, 0)
        if header_size != 40 or bits != 32 or compression != 0 or width != n or double_height != 2 * n:
            raise ValueError(f"icon frame {n}: not a square 32-bit uncompressed DIB")
        frames.append(dib_bytes(image[:40], pixels))
    directory = b""
    offset = 6 + 16 * len(entries)
    for entry, frame in zip(entries, frames):
        entry[6], entry[7] = len(frame), offset
        directory += struct.pack("<BBBBHHII", *entry)
        offset += len(frame)
    return struct.pack("<HHH", 0, 1, len(entries)) + directory + b"".join(frames)


def is_high_contrast(name: str) -> bool:
    """Tell a high-contrast variant (copied as it is) from a standard picture (drawn from the logo).

    Args:
        name: A template file's name.

    Returns:
        True for ``*_contrast-black*`` / ``*_contrast-white*`` files.
    """
    return "_contrast-" in name


def build(art: np.ndarray) -> dict[str, bytes]:
    """Draw the whole set in memory.

    Args:
        art: The cropped logo.

    Returns:
        File name -> bytes, for every file of the template.

    Raises:
        OSError: The template cannot be read.
        ValueError: From ``draw_png`` / ``draw_ico``, or a template file that is neither a PNG nor an ICO.
    """
    files = {}
    for name in dev_icon.source_names():
        data = (TEMPLATE_DIR / name).read_bytes()
        lowered = name.lower()
        if is_high_contrast(name):
            files[name] = data
        elif lowered.endswith(".png"):
            files[name] = draw_png(name, data, art)
        elif lowered.endswith(".ico"):
            files[name] = draw_ico(data, art)
        else:
            raise ValueError(f"{name}: not a PNG or an ICO")
    return files


def decoded(name: str, data: bytes) -> list[np.ndarray]:
    """Decode a set file into its pictures, for a comparison that ignores the encoder.

    Args:
        name: The file's name (``.png`` or ``.ico``).
        data: Its bytes.

    Returns:
        The PNG's RGBA pixels, or every ICO frame's RGBA pixels in directory order (a DIB frame read top row first).

    Raises:
        ValueError: From ``dev_icon.ico_images``.
    """
    if name.lower().endswith(".png"):
        with Image.open(io.BytesIO(data)) as image:
            return [np.asarray(image.convert("RGBA"))]
    entries, images = dev_icon.ico_images(data)
    pictures = []
    for entry, image in zip(entries, images):
        if image[:8] == dev_icon.PNG_SIGNATURE:
            with Image.open(io.BytesIO(image)) as frame:
                pictures.append(np.asarray(frame.convert("RGBA")))
        else:
            n = entry[0] or 256
            bgra = np.frombuffer(image, np.uint8, count=n * n * 4, offset=40).reshape(n, n, 4)[::-1]
            pictures.append(bgra[..., [2, 1, 0, 3]])
    return pictures


def cmd_gen(out_root: Path) -> int:
    """Write the set into ``out_root/images-Agentmaster``, touching only files whose bytes change.

    Args:
        out_root: The folder that holds the set (res/terminal by default).

    Returns:
        0.

    Raises:
        OSError / ValueError: From ``build`` or the writes.
    """
    out_dir = out_root / OUT_DIR_NAME
    out_dir.mkdir(parents=True, exist_ok=True)
    files = build(load_art())
    written = 0
    for name, data in files.items():
        target = out_dir / name
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)
            written += 1
    removed = 0
    for stale in sorted(p for p in out_dir.iterdir() if p.is_file() and p.name not in files):
        stale.unlink()  # a file the template no longer has would ship as a picture nothing names
        removed += 1
    print(f"{out_dir}: {len(files)} files, {written} written, {removed} removed")
    return 0


def cmd_check() -> int:
    """Compare the committed set with what ``gen`` would write now.

    Returns:
        0 when they match, 1 at the first difference (printed).

    Raises:
        OSError / ValueError: From ``build``.
    """
    committed_dir = REPO / "res" / "terminal" / OUT_DIR_NAME
    want = build(load_art())
    have = sorted(p.name for p in committed_dir.iterdir() if p.is_file()) if committed_dir.is_dir() else []
    if have != sorted(want):
        missing = sorted(set(want) - set(have))
        extra = sorted(set(have) - set(want))
        print(f"{committed_dir}: missing {missing[:5]}, extra {extra[:5]}")
        return 1
    for name, data in want.items():
        got = (committed_dir / name).read_bytes()
        if is_high_contrast(name):
            same = got == data
        else:
            a, b = decoded(name, data), decoded(name, got)
            same = len(a) == len(b) and all(x.shape == y.shape and np.array_equal(x, y) for x, y in zip(a, b))
        if not same:
            print(f"{name}: differs from what gen writes")
            return 1
    print(f"{committed_dir}: {len(want)} files match")
    return 0


def main(argv: list[str] | None = None) -> int:
    """Run ``gen`` or ``check``.

    Args:
        argv: The arguments after the script name (sys.argv's when None).

    Returns:
        The process exit code.

    Raises:
        SystemExit: From argparse on a usage error.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("gen", help="write res/terminal/images-Agentmaster from the README's logo")
    gen.add_argument("--out", type=Path, default=REPO / "res" / "terminal",
                     help="the folder to write images-Agentmaster into (default: res/terminal)")
    sub.add_parser("check", help="exit 1 when the committed set is not what gen writes")
    args = parser.parse_args(argv)
    if args.command == "gen":
        return cmd_gen(args.out)
    return cmd_check()


if __name__ == "__main__":
    sys.exit(main())
