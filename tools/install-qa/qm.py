"""Hardware-level keyboard and screen of a claude-fleet Windows desktop, through QEMU's monitor.

Why: the desktop's own agent injects input with SendInput from inside the guest, which can never
reach the UAC secure desktop (or consent.exe at System integrity). QEMU's human monitor can:
`sendkey` is a key press on the virtual keyboard and `screendump` reads the real framebuffer, so a
UAC consent or credential prompt can be seen and answered like a person would. dockur keeps a
private monitor socket for its own power handling at /run/shm/monitor.sock (dockur/windows 6.05
`power.sh`: `nc -q 1 -w 1 -U "$ACPI_SOCKET"`); this script talks to it through `docker exec`.

Usage:
    python -I qm.py [--container NAME] key <key> [<key> ...]   e.g. key alt-y   key esc   key tab ret
    python -I qm.py [--container NAME] type "<text>"            ASCII only; Shift for capitals/symbols
    python -I qm.py [--container NAME] shot <out.png>           the guest's real screen as a PNG

The container defaults to claude-desktop-win-1. Exit code 0 on success, 1 on any monitor error
(the monitor's own text is printed). Never pass a password on a command line others can read:
`type` takes "@file" to read the text from a file instead (the file is not printed).
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

SOCKET = "/run/shm/monitor.sock"

# QEMU sendkey names for the printable ASCII characters (US layout), with Shift where needed.
_PLAIN = {c: c for c in "abcdefghijklmnopqrstuvwxyz0123456789"}
_PLAIN.update({" ": "spc", "-": "minus", "=": "equal", "[": "bracket_left", "]": "bracket_right",
               ";": "semicolon", "'": "apostrophe", "`": "grave_accent", "\\": "backslash",
               ",": "comma", ".": "dot", "/": "slash", "\t": "tab", "\n": "ret"})
_SHIFTED = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8",
            "(": "9", ")": "0", "_": "minus", "+": "equal", "{": "bracket_left",
            "}": "bracket_right", ":": "semicolon", '"': "apostrophe", "~": "grave_accent",
            "|": "backslash", "<": "comma", ">": "dot", "?": "slash"}


def key_for(ch: str) -> str:
    """QEMU sendkey spec for one ASCII character.

    Args:
        ch: one character.
    Returns:
        The key spec, e.g. "a", "shift-a", "shift-1".
    Raises:
        ValueError: the character has no US-layout key (non-ASCII text cannot be typed this way).
    """
    if ch in _PLAIN:
        return _PLAIN[ch]
    if ch.isupper() and ch.lower() in _PLAIN:
        return "shift-" + ch.lower()
    if ch in _SHIFTED:
        return "shift-" + _SHIFTED[ch]
    raise ValueError("no US-layout key for %r" % ch)


def monitor(container: str, commands: list) -> str:
    """Send human-monitor commands over one connection and return QEMU's text answer.

    Args:
        container: the dockur container name.
        commands: monitor command lines (no newlines inside).
    Returns:
        The monitor's output (its banner and echo included).
    Raises:
        RuntimeError: docker exec failed, or QEMU answered with an error text.
    """
    script = "".join(c + "\n" for c in commands)
    # Through stdin, so no command (or typed text) ever appears on a process command line.
    proc = subprocess.run(["docker", "exec", "-i", container, "sh", "-c",
                           "nc -q 1 -w 2 -U %s" % SOCKET],
                          input=script.encode("ascii"), capture_output=True, timeout=60)
    out = proc.stdout.decode("utf-8", "replace") + proc.stderr.decode("utf-8", "replace")
    if proc.returncode != 0:
        raise RuntimeError("docker exec exit %d: %s" % (proc.returncode, out.strip()))
    low = out.lower()
    for bad in ("unknown command", "unknown key", "invalid parameter", "error"):
        if bad in low:
            raise RuntimeError("monitor: " + out.strip())
    return out


def ppm_to_png(ppm: bytes) -> bytes:
    """Convert a binary PPM (P6, maxval 255) screendump into a PNG, stdlib only.

    Args:
        ppm: the screendump bytes.
    Returns:
        PNG bytes.
    Raises:
        ValueError: not a P6 / 255 image.
    """
    # Header: "P6" <ws> width <ws> height <ws> maxval <single ws>; no comments in QEMU's dumps.
    parts = ppm.split(None, 4)
    if len(parts) < 5 or parts[0] != b"P6" or parts[3] != b"255":
        raise ValueError("not a P6/255 PPM")
    w, h = int(parts[1]), int(parts[2])
    header_len = len(ppm) - len(parts[4])
    data = ppm[header_len:header_len + w * h * 3]
    raw = b"".join(b"\x00" + data[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag: bytes, body: bytes) -> bytes:
        """One PNG chunk with its CRC."""
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main() -> int:
    """Parse the command line and run one action.

    Returns:
        0 on success, 1 on an error (printed), 2 on bad usage.
    """
    args = sys.argv[1:]
    container = "claude-desktop-win-1"
    if len(args) >= 2 and args[0] == "--container":
        container, args = args[1], args[2:]
    if not args:
        print(__doc__)
        return 2
    action, rest = args[0], args[1:]
    try:
        if action == "key" and rest:
            # 120 ms hold, one connection; QEMU queues the presses in order.
            out = monitor(container, ["sendkey %s 120" % k for k in rest])
        elif action == "type" and len(rest) == 1:
            text = rest[0]
            if text.startswith("@"):
                with open(text[1:], "r", encoding="utf-8") as f:
                    text = f.read().rstrip("\r\n")
            out = monitor(container, ["sendkey %s 60" % key_for(c) for c in text])
            out = "(typed %d characters)" % len(text)
        elif action == "shot" and len(rest) == 1:
            monitor(container, ["screendump /run/shm/qm-shot.ppm"])
            # Streamed out with `cat`: /run/shm is a tmpfs, which `docker cp` cannot read.
            dump = subprocess.run(["docker", "exec", container, "sh", "-c",
                                   "cat /run/shm/qm-shot.ppm && rm -f /run/shm/qm-shot.ppm"],
                                  check=True, capture_output=True, timeout=60).stdout
            png = ppm_to_png(dump)
            with open(rest[0], "wb") as f:
                f.write(png)
            out = "wrote %s (%d bytes)" % (rest[0], len(png))
        else:
            print(__doc__)
            return 2
    except (RuntimeError, ValueError, subprocess.SubprocessError, OSError) as e:
        print("qm: %s" % e)
        return 1
    print(out.strip()[-400:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
