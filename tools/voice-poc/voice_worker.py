# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""An Agentmaster VOICE WORKER: speech-to-text behind the voice protocol v1 (doc/agentmaster/VOICE.md §6.2), for the
POC's engines (Parakeet on the CPU through sherpa-onnx, Whisper on the GPU through faster-whisper).

Agentmaster starts ONE worker per process when the first voice note arrives (the setting ``voiceWorkerCommand``; the
bundled ``agentmaster-voice.exe`` speaks the same protocol), keeps it while notes keep coming, and ends it after
``voiceWorkerIdleMinutes``. The protocol is JSON lines over stdin / stdout, one request at a time:

    -> {"v":1,"id":1,"op":"hello"}
    <- {"v":1,"id":1,"ok":true,"engine":"whisper-large-v3","languages":["en",...],"prompt":true,"hotwords":false}
    -> {"v":1,"id":2,"op":"transcribe","path":"C:\\...\\voice-14021.oga","language":"en",
        "prompt":"A developer dictates ... Glossary: ...","hotwords":"ConPTY :2.5/..."}
    <- {"v":1,"id":2,"ok":true,"text":"Check why the ConPTY host ...","audio_ms":7480,"decode_ms":512,"engine":"..."}
    -> {"v":1,"id":3,"op":"quit"}

Rules of the protocol (what Agentmaster relies on):

* stdout carries ONLY protocol lines (UTF-8, one JSON object per line); everything else goes to stderr, which the app
  logs - a library that prints to stdout would corrupt the stream, so this worker redirects ``sys.stdout`` to stderr
  for everything but its own answers.
* every request gets exactly one answer with the same ``id``; a failure is ``{"ok":false,"error":"..."}``, never a
  crash: the app reads a dead worker as "the engine failed" for every note in flight and starts a new one next time.
* the worker transcribes; the app owns the vocabulary (it sends ``prompt`` and ``hotwords``) and the dictionary pass
  (it runs on the text the worker returns, VoiceVocabulary.h), so every engine gets the same correction layer.

Usage (the value of ``voiceWorkerCommand``; the configuration that transcribed the owner's three recordings word for word,
VOICE.md section 4.4 - Whisper on the GPU with Parakeet's second hearing for the app's correction layer)::

    "Q:\\agentmaster-voice\\venv\\Scripts\\python.exe" "K:\\source\\Agentmaster\\tools\\voice-poc\\voice_worker.py" --engine whisper-large-v3-turbo+prompt --fuse parakeet-v2-beam+hotwords

A ``transcribe`` answer then carries ``"alternatives": [<the second engine's text>]`` beside ``text``.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import traceback

from common import audio_duration_s, decode_audio
from engines import ENGINE_SPECS, build_engine
from vocab import Vocabulary

PROTOCOL_VERSION = 1


class _GivenVocabulary(Vocabulary):
    """A vocabulary whose prompt and hotwords are the strings the app sent (the app built them; the worker only passes
    them to the engine)."""

    def __init__(self, prompt: str, hotwords: str) -> None:
        """Wraps the app's strings.

        Args:
            prompt: Whisper's initial prompt ("" = none).
            hotwords: sherpa-onnx's hotwords ("" = none).
        """
        super().__init__()
        self._prompt = prompt or ""
        self._hotwords = hotwords or ""
        # A non-empty terms list makes the engines use the vocabulary at all (they skip an empty one).
        if self._prompt or self._hotwords:
            self.add("Agentmaster")

    def whisper_prompt(self, max_chars: int = 700) -> str:
        """The app's prompt, verbatim.

        Args:
            max_chars: Ignored (the app already cut it).

        Returns:
            The prompt.
        """
        return self._prompt

    def hotwords(self, base_score: float = 2.0, limit: int = 96) -> str:
        """The app's hotwords, verbatim.

        Args:
            base_score: Ignored (the app already scored them).
            limit: Ignored.

        Returns:
            The hotwords.
        """
        return self._hotwords


def _answer(out, payload: dict) -> None:
    """Writes one protocol line and flushes it (the app waits for the line, never for a buffer).

    Args:
        out: The real stdout (text, UTF-8).
        payload: The answer object.
    """
    payload.setdefault("v", PROTOCOL_VERSION)
    out.write(json.dumps(payload, ensure_ascii=False) + "\n")
    out.flush()


def serve(engine_spec: str, out, inp, fuse_spec: str = "") -> int:
    """The request loop: one engine (and an optional FUSION engine), loaded on the first request that needs them.

    Args:
        engine_spec: The engine (``engines.ENGINE_SPECS``).
        out: The protocol output stream.
        inp: The protocol input stream.
        fuse_spec: A second engine whose transcript of every note is returned as ``alternatives`` (VOICE.md section 5.5:
            the app's correction layer resolves where two independent hearings disagree); "" = none.

    Returns:
        The exit code (0 after "quit" or the end of stdin).
    """
    engine = None
    second = None
    for raw in inp:
        line = raw.strip()
        if not line:
            continue
        req_id = None
        try:
            req = json.loads(line)
            req_id = req.get("id")
            op = req.get("op")
            if op == "quit":
                _answer(out, {"id": req_id, "ok": True})
                return 0
            if engine is None:
                t0 = time.perf_counter()
                engine = build_engine(engine_spec)
                print(f"[voice-worker] {engine.name} loaded in {time.perf_counter() - t0:.1f} s", file=sys.stderr)
            if fuse_spec and second is None:
                t0 = time.perf_counter()
                second = build_engine(fuse_spec)
                print(f"[voice-worker] fusion engine {second.name} loaded in {time.perf_counter() - t0:.1f} s", file=sys.stderr)
            if op == "hello":
                name = engine.name + (f" + {second.name}" if second else "")
                _answer(out, {"id": req_id, "ok": True, "engine": name, "languages": ["en"],
                              "prompt": "+prompt" in engine.name, "hotwords": "+hotwords" in name})
                continue
            if op != "transcribe":
                _answer(out, {"id": req_id, "ok": False, "error": f"unknown op {op!r}"})
                continue
            samples = decode_audio(req["path"])
            vocab = _GivenVocabulary(req.get("prompt", ""), req.get("hotwords", ""))
            result = engine.transcribe(samples, vocab)
            answer = {"id": req_id, "ok": True, "text": result.text, "engine": engine.name,
                      "audio_ms": int(audio_duration_s(samples) * 1000), "decode_ms": int(result.seconds * 1000)}
            if second is not None:
                # The fusion engine's hearing of the same note: a failure of it never fails the note (the primary stands).
                try:
                    other = second.transcribe(samples, vocab)
                    answer["alternatives"] = [other.text]
                    answer["decode_ms"] += int(other.seconds * 1000)
                    answer["engine"] = f"{engine.name} + {second.name}"
                except Exception:
                    traceback.print_exc(file=sys.stderr)
            _answer(out, answer)
        except Exception as exc:  # one failed note never ends the worker
            traceback.print_exc(file=sys.stderr)
            _answer(out, {"id": req_id, "ok": False, "error": f"{type(exc).__name__}: {exc}"})
    return 0


def main(argv: list[str] | None = None) -> int:
    """Starts the worker on stdin / stdout.

    Args:
        argv: The command line (None = sys.argv).

    Returns:
        The exit code.
    """
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", default="whisper-large-v3-turbo+prompt", choices=ENGINE_SPECS)
    ap.add_argument("--fuse", default="", choices=("",) + tuple(ENGINE_SPECS),
                    help="a second engine whose hearing of every note is returned as 'alternatives'")
    args = ap.parse_args(argv)
    # The protocol owns the REAL stdout; anything a library prints goes to stderr instead of corrupting the stream.
    proto_out = open(sys.stdout.fileno(), "w", encoding="utf-8", newline="\n", closefd=False)
    proto_in = open(sys.stdin.fileno(), "r", encoding="utf-8", closefd=False)
    sys.stdout = sys.stderr
    return serve(args.engine, proto_out, proto_in, args.fuse)


if __name__ == "__main__":
    sys.exit(main())
