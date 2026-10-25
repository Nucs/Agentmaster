"""Converts a binary PPM (P6, what QEMU's `screendump` writes) to a PNG, with the standard library only.

Usage: python3 ppm2png.py <in.ppm> <out.png>
The dockur image has python3 but no imaging library; the build server's measurements read a VM's screen this way
(`screendump` through the monitor, then this). Raises ValueError for anything but an 8-bit P6 file.
"""
import struct
import sys
import zlib


def read_token(data: bytes, pos: int) -> tuple[bytes, int]:
    """Reads one whitespace-separated header token of a PPM, skipping '#' comments.

    Args:
        data: the whole file.
        pos: where to start reading.

    Returns:
        The token and the position just after it.
    """
    while True:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        break
    start = pos
    while not data[pos:pos + 1].isspace():
        pos += 1
    return data[start:pos], pos


def ppm_to_png(source: str, target: str) -> None:
    """Writes the PNG of a P6 PPM.

    Args:
        source: the PPM's path.
        target: the PNG's path.

    Raises:
        ValueError: the file is not an 8-bit P6 PPM.
    """
    data = open(source, "rb").read()
    magic, pos = read_token(data, 0)
    if magic != b"P6":
        raise ValueError("not a P6 PPM")
    width_text, pos = read_token(data, pos)
    height_text, pos = read_token(data, pos)
    maxval_text, pos = read_token(data, pos)
    width, height, maxval = int(width_text), int(height_text), int(maxval_text)
    if maxval != 255:
        raise ValueError("only 8-bit PPMs are supported")
    pixels = data[pos + 1:pos + 1 + width * height * 3]
    rows = b"".join(b"\x00" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        """One PNG chunk: length, type, payload, CRC of type + payload."""
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
    open(target, "wb").write(png)


if __name__ == "__main__":
    ppm_to_png(sys.argv[1], sys.argv[2])
