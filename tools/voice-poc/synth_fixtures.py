# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Writes text-to-speech renditions of the evaluation sentences as Telegram-like voice notes (doc/agentmaster/VOICE.md
§7.2): Windows' own SAPI voices (David, Zira) -> WAV -> Ogg/Opus, mono, 48 kHz, ~32 kbit/s, the format a Telegram
client records.

They are a PIPELINE smoke test (decode -> engine -> vocabulary -> score runs end to end without the owner), never the
evaluation: a synthetic voice pronounces "ConPTY" the way its letter-to-sound rules guess, not the way a developer says
it, and it has no accent, no room and no microphone.

Usage::

    python synth_fixtures.py [--voice "Microsoft Zira Desktop"] [--out Q:\\agentmaster-voice\\synthetic]
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from common import SYNTHETIC_DIR, load_references


def synthesize_wav(text: str, wav: Path, voice: str) -> None:
    """Speaks ``text`` into a WAV file with a Windows SAPI voice (System.Speech through Windows PowerShell).

    The text and paths reach PowerShell through environment variables, never through the command line, so no quoting
    of the sentence can break the script (a sentence may hold quotes, '$' or backticks).

    Args:
        text: The sentence.
        wav: The WAV file to write.
        voice: The SAPI voice name ("Microsoft David Desktop", "Microsoft Zira Desktop").

    Raises:
        subprocess.CalledProcessError: PowerShell failed (no such voice, no System.Speech).
    """
    import os

    script = (
        "Add-Type -AssemblyName System.Speech; "
        "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
        "$s.SelectVoice($env:AMV_VOICE); $s.Rate = 0; "
        "$s.SetOutputToWaveFile($env:AMV_WAV); $s.Speak($env:AMV_TEXT); $s.Dispose()"
    )
    env = dict(os.environ, AMV_VOICE=voice, AMV_WAV=str(wav), AMV_TEXT=text)
    subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script], env=env, check=True)


def wav_to_voice_note(wav: Path, oga: Path) -> None:
    """Encodes a WAV as a Telegram-like voice note: Opus in Ogg, mono, 48 kHz, 32 kbit/s, VOIP tuning.

    PyAV (bundled FFmpeg with libopus) does it, so no ffmpeg.exe is needed.

    Args:
        wav: The input.
        oga: The ``.oga`` file to write.

    Raises:
        av.error.FFmpegError: The encode failed.
    """
    import av

    with av.open(str(wav)) as src, av.open(str(oga), mode="w", format="ogg") as dst:
        out = dst.add_stream("libopus", rate=48000)
        out.layout = "mono"
        out.bit_rate = 32000
        out.options = {"application": "voip"}
        resampler = av.AudioResampler(format="s16", layout="mono", rate=48000)
        for frame in src.decode(audio=0):
            for rs in resampler.resample(frame):
                for packet in out.encode(rs):
                    dst.mux(packet)
        for rs in resampler.resample(None):
            for packet in out.encode(rs):
                dst.mux(packet)
        for packet in out.encode(None):
            dst.mux(packet)


def main(argv: list[str] | None = None) -> int:
    """Writes ``<out>/<sentence id>.oga`` for every reference sentence.

    Args:
        argv: The command line (None = sys.argv).

    Returns:
        The process exit code (0 = every file written).
    """
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--voice", default="Microsoft David Desktop", help="SAPI voice name")
    ap.add_argument("--out", type=Path, default=SYNTHETIC_DIR, help="output folder")
    args = ap.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for ref in load_references():
            wav = Path(tmp) / f"{ref.id}.wav"
            synthesize_wav(ref.text, wav, args.voice)
            oga = args.out / f"{ref.id}.oga"
            wav_to_voice_note(wav, oga)
            print(f"{ref.id}: {oga} ({oga.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
