#!/usr/bin/env python3
"""The README's art: the logo and the twelve heading icons, cut from what Codex's image tool drew (Agentmaster).

The README (doc/agentmaster/img/logo.png, doc/agentmaster/img/icons/*.png) wears art that Codex CLI generated with its
built-in ``image_gen`` tool (model gpt-6-astra, reasoning effort xhigh), from the prompts in ``prompts/`` and the icon
the release identity wears (res/terminal/images-Dev) as the reference image. ``image_gen`` returns flat RGB PNGs, so
every source carries a background, and this script turns them into the transparent assets the README links:

* **The tile sheet** (``prompts/icons.md``): twelve charcoal rounded tiles in a 4 x 3 grid on WHITE. A tile is found
  as one connected region darker than the page (luminance < 200: its body and rim are far below, the soft drop shadow
  the model paints on the page stays above, so the shadow goes with the page), its holes filled (a glyph can be
  lighter than the threshold) and eroded by one pixel (the rim's white-blended anti-aliasing never survives). That
  binary shape is the tile's alpha; downscaling the premultiplied result to ICON pixels gives the edge its
  anti-aliasing back.
* **The logo** (``prompts/logo.md``): one mark on pure MAGENTA, keyed by each pixel's magenta "spill"
  ``s = min(R, B) - G``, which is exactly ``(1 - alpha) * 255`` for any neutral foreground mixed with #FF00FF; every
  pixel is then un-mixed (``fg = (P - (1 - alpha) * MAGENTA) / alpha``), so no pink rim stays on an edge. The prompt
  forbids magenta, pink and purple inside the art, so keying the whole canvas is safe. The model also paints a faint
  drop shadow on the magenta despite the prompt; keyed, it would be a near-invisible black smear that pulls the
  centering down, so every pixel farther than a few pixels from the solid art is cleared before cropping.

Subcommands:
  ref    Write the reference image Codex is given (``--image``): the release icon's 400 % large tile, cropped to the
         icon, squared, upscaled to 1024 px and flattened on white.
  tiles  Cut a tile sheet into ``<out dir>/<slug>.png`` (ICON x ICON), in the sheet's reading order, named by SLUGS.
  logo   Key, clean, crop and center a magenta-background logo into one LOGO x LOGO PNG.

Regenerating (see README.md beside this script for the whole recipe): run ``ref``, give Codex the prompt on stdin and
the reference with ``--image``, take the PNG from ``%USERPROFILE%\\.codex\\generated_images\\<session id>\\`` (Codex
saves there; its sandboxed file tools may fail to copy it out), then run ``tiles`` or ``logo`` on it. Needs Python 3.9+,
Pillow, numpy and scipy.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

#: The repository root (this file is tools/readme-art/readme_art.py).
REPO = Path(__file__).resolve().parents[2]

#: The release identity's 400 % large tile: the largest raster of the icon in the repository (its icon is ~384 px
#: wide; the 256 px Square44x44 target is smaller).
REFERENCE_TILE = REPO / "res" / "terminal" / "images-Dev" / "LargeTile.scale-400.png"

#: The twelve heading icons in the tile sheet's reading order (row by row, left to right), each named for the README
#: heading it stands before. The sheet's prompt lists the glyphs in this same order.
SLUGS = [
    "features",   # What it does for you
    "fleet",      # One page to run the whole fleet
    "restore",    # Never lose a session - ever
    "observer",   # Manages every agent
    "attention",  # Know who needs you - at a glance
    "lens",       # Observability everywhere
    "pocket",     # Your fleet in your pocket
    "status",     # Status
    "install",    # Install
    "docs",       # Docs & building
    "upstream",   # Relationship to Windows Terminal & License
    "more",       # Additional features
]

#: An icon's side in pixels: over 4x the 30 px the README shows its largest (h2) heading icon at, so every heading
#: size (30 / 24 / 20 px) stays sharp on dense screens.
ICON = 128

#: The logo's side in pixels: 3x the 168 px the README shows it at.
LOGO = 512

#: The keyed background of the logo prompts.
MAGENTA = np.array([255.0, 0.0, 255.0])


def downscale(rgba: np.ndarray, side: int) -> Image.Image:
    """Resize a square RGBA float array (0-255) to ``side`` x ``side`` through premultiplied alpha.

    :param rgba: an N x N x 4 float array.
    :param side: the output side in pixels.
    :returns: the resized RGBA image.
    """
    img = Image.fromarray(np.clip(rgba, 0, 255).round().astype(np.uint8))
    # "RGBa" is Pillow's premultiplied mode: resampling there keeps the meaningless colours of transparent pixels
    # from bleeding a dark or pink halo into the anti-aliased edge.
    return img.convert("RGBa").resize((side, side), Image.LANCZOS).convert("RGBA")


def pad_square(rgba: np.ndarray, pad_frac: float) -> np.ndarray:
    """Center an RGBA array on a transparent square canvas with a uniform margin.

    :param rgba: an H x W x 4 float array cropped to its content.
    :param pad_frac: the margin on each side, as a fraction of the content's larger side.
    :returns: the S x S x 4 array, S = max(H, W) * (1 + 2 * pad_frac), the content centered.
    """
    h, w = rgba.shape[:2]
    side = int(round(max(h, w) * (1.0 + 2.0 * pad_frac)))
    out = np.zeros((side, side, 4), dtype=np.float64)
    y, x = (side - h) // 2, (side - w) // 2
    out[y:y + h, x:x + w] = rgba
    return out


def make_reference(target: Path) -> None:
    """Write the 1024 px reference image Codex gets with ``--image``.

    :param target: the PNG to write (its folder must exist).
    :raises FileNotFoundError: when REFERENCE_TILE is missing.
    :raises ValueError: when the tile has no opaque pixel.
    """
    tile = Image.open(REFERENCE_TILE).convert("RGBA")
    box = tile.getchannel("A").getbbox()
    if box is None:
        raise ValueError(f"{REFERENCE_TILE} has no opaque pixel")
    left, top, right, bottom = box
    # A square around the icon with a 20 px margin, so the model sees the whole silhouette with air around it.
    side = max(right - left, bottom - top) + 40
    cx, cy = (left + right) // 2, (top + bottom) // 2
    crop = tile.crop((cx - side // 2, cy - side // 2, cx - side // 2 + side, cy - side // 2 + side))
    big = crop.resize((1024, 1024), Image.LANCZOS)
    flat = Image.new("RGBA", big.size, (255, 255, 255, 255))
    flat.alpha_composite(big)
    flat.convert("RGB").save(target)


def cut_tiles(sheet: Path, out_dir: Path) -> list[Path]:
    """Cut the twelve rounded tiles of a white-background sheet into transparent square icons.

    :param sheet: the generated tile sheet (white background).
    :param out_dir: the folder the ``<slug>.png`` icons are written to (created when missing).
    :returns: the written files, in SLUGS order.
    :raises RuntimeError: when the sheet does not hold exactly twelve tile-sized dark regions (the model drew a
        different grid: generate again rather than guess which tile is which).
    """
    rgb = np.asarray(Image.open(sheet).convert("RGB")).astype(np.float64)
    lum = rgb @ np.array([0.299, 0.587, 0.114])
    labels, _ = ndimage.label(lum < 200)
    tiles = []
    for index, region in enumerate(ndimage.find_objects(labels), start=1):
        # Glyph fragments and specks are small; a tile spans a good part of its grid cell.
        min_side = min(rgb.shape[0] / 3.0, rgb.shape[1] / 4.0) * 0.5
        if region[0].stop - region[0].start > min_side and region[1].stop - region[1].start > min_side:
            tiles.append((region, index))
    if len(tiles) != len(SLUGS):
        raise RuntimeError(f"{sheet}: expected {len(SLUGS)} tiles, found {len(tiles)}")
    # Reading order: tiles of one row start within a few pixels of each other, so bucket by a third of a row.
    row_bucket = rgb.shape[0] / 6.0
    tiles.sort(key=lambda item: (round(item[0][0].start / row_bucket), item[0][1].start))
    out_dir.mkdir(parents=True, exist_ok=True)
    written = []
    for slug, ((rows, cols), index) in zip(SLUGS, tiles):
        solid = ndimage.binary_erosion(ndimage.binary_fill_holes(labels[rows, cols] == index), iterations=1)
        rgba = np.dstack([rgb[rows, cols], solid.astype(np.float64) * 255.0])
        target = out_dir / f"{slug}.png"
        downscale(pad_square(rgba, 0.0), ICON).save(target, optimize=True)
        written.append(target)
    return written


def key_magenta(rgb: np.ndarray) -> np.ndarray:
    """Turn a magenta-background RGB array into a de-spilled RGBA array.

    :param rgb: an H x W x 3 float array (0-255) whose background is #FF00FF.
    :returns: the H x W x 4 float array (alpha 0-255); background pixels have alpha 0.
    """
    spill = np.clip(np.minimum(rgb[..., 0], rgb[..., 2]) - rgb[..., 1], 0.0, 255.0)
    # Below 24 the spill is noise in a foreground colour; above 232 it is background with the generator's grain. In
    # between the alpha follows the mix linearly.
    alpha = np.clip((232.0 - spill) / (232.0 - 24.0), 0.0, 1.0)
    safe = np.maximum(alpha, 1e-6)[..., None]
    fg = (rgb - (1.0 - alpha)[..., None] * MAGENTA) / safe
    fg = np.where(alpha[..., None] > 0, np.clip(fg, 0.0, 255.0), 0.0)
    return np.dstack([fg, alpha * 255.0])


def cut_logo(source: Path, target: Path, side: int = LOGO) -> None:
    """Key a magenta-background logo, clear its stray shadow, crop it to the art and center it on a square.

    :param source: the generated logo (magenta background).
    :param target: the PNG to write (its folder is created when missing).
    :param side: the output side in pixels.
    :raises ValueError: when nothing solid survives the key (a wrong background colour).
    """
    rgba = key_magenta(np.asarray(Image.open(source).convert("RGB")).astype(np.float64))
    solid = rgba[..., 3] > 128
    if not solid.any():
        raise ValueError(f"{source}: no solid pixel after keying")
    # Keep the art and its anti-aliased rim (a few pixels around the solid shape); everything farther out is the
    # model's faint drop shadow or grain, which would only drag the centering.
    rgba[~ndimage.binary_dilation(solid, iterations=4), 3] = 0.0
    ys, xs = np.nonzero(rgba[..., 3] > 8)
    crop = rgba[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    target.parent.mkdir(parents=True, exist_ok=True)
    downscale(pad_square(crop, 0.04), side).save(target, optimize=True)


def main(argv: list[str]) -> int:
    """Run one subcommand.

    :param argv: the arguments after the script name.
    :returns: the process exit code (0 on success).
    :raises SystemExit: from argparse on a usage error.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    ref = sub.add_parser("ref", help="write the reference image Codex gets with --image")
    ref.add_argument("target", type=Path)
    tiles = sub.add_parser("tiles", help="cut a tile sheet into <out dir>/<slug>.png")
    tiles.add_argument("sheet", type=Path)
    tiles.add_argument("out_dir", type=Path, nargs="?", default=REPO / "doc" / "agentmaster" / "img" / "icons")
    logo = sub.add_parser("logo", help="key, crop and center a magenta-background logo")
    logo.add_argument("source", type=Path)
    logo.add_argument("target", type=Path, nargs="?", default=REPO / "doc" / "agentmaster" / "img" / "logo.png")
    logo.add_argument("--size", type=int, default=LOGO)
    args = parser.parse_args(argv)

    if args.command == "ref":
        make_reference(args.target)
        print(args.target)
    elif args.command == "tiles":
        for path in cut_tiles(args.sheet, args.out_dir):
            print(path)
    else:
        cut_logo(args.source, args.target, args.size)
        print(args.target)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
