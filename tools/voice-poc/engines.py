# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The speech-to-text engines the POC compares (doc/agentmaster/VOICE.md §4.2).

* ``ParakeetEngine`` - NVIDIA Parakeet TDT 0.6B (v2 English, v3 multilingual) through sherpa-onnx on the CPU: the model
  FluidVoice runs by default, here in the runtime the C++ integration links (sherpa-onnx's C API), so a POC number is
  what Agentmaster would get. Greedy decoding, or modified beam search with per-stream HOTWORDS (the word boost).
* ``FasterWhisperEngine`` - OpenAI Whisper large-v3 / large-v3-turbo through CTranslate2 on the GPU, with the
  vocabulary as ``initial_prompt``: the accuracy reference, and the engine for languages Parakeet lacks (Hebrew).

Every engine takes 16 kHz mono float32 samples and an optional ``Vocabulary``, and returns the raw text the model
wrote (no dictionary pass: that is a separate, measured layer).
"""

from __future__ import annotations

import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from common import MODELS_DIR, add_cuda_dll_dirs
from vocab import Vocabulary


@dataclass
class EngineResult:
    """One engine run: the text and how long it took."""

    text: str
    seconds: float  # wall time of the transcription call alone (model loading excluded)


class Engine:
    """The engine interface: a name for reports and one call."""

    name: str = "engine"

    def transcribe(self, samples, vocab: Optional[Vocabulary] = None) -> EngineResult:
        """Transcribes one utterance.

        Args:
            samples: 16 kHz mono float32 samples.
            vocab: The session vocabulary to bias toward, or None for the model alone.

        Returns:
            The text and the wall time.

        Raises:
            NotImplementedError: Always, on the base class.
        """
        raise NotImplementedError


class ParakeetEngine(Engine):
    """Parakeet TDT 0.6B through sherpa-onnx (CPU, int8)."""

    def __init__(self, version: str = "v2", decoding: str = "greedy_search", threads: int = 8,
                 use_hotwords: bool = False, hotword_score: float = 2.0, provider: str = "cpu") -> None:
        """Loads the model (~1-2 s from a warm disk; ~650 MB of int8 weights).

        Args:
            version: "v2" (English only, the best English Parakeet) or "v3" (25 European languages).
            decoding: "greedy_search" or "modified_beam_search" (required for hotwords).
            threads: ONNX Runtime intra-op threads (the 13900K has 8 P-cores: more threads land on E-cores and slow down).
            use_hotwords: Bias each stream toward the vocabulary (forces modified beam search).
            hotword_score: The score of a weight-1 hotword (Vocabulary.hotwords scales it by weight).
            provider: ONNX Runtime provider: "cpu" (the integration's default), "cuda" or "directml" when built in.

        Raises:
            FileNotFoundError: The model folder (MODELS_DIR / parakeet-tdt-0.6b-<version>-int8) is incomplete.
        """
        import sherpa_onnx

        self.version = version
        self.use_hotwords = use_hotwords
        self.hotword_score = hotword_score
        if use_hotwords:
            decoding = "modified_beam_search"
        self.decoding = decoding
        d = MODELS_DIR / f"parakeet-tdt-0.6b-{version}-int8"
        for f in ("encoder.int8.onnx", "decoder.int8.onnx", "joiner.int8.onnx", "tokens.txt"):
            if not (d / f).is_file():
                raise FileNotFoundError(d / f)
        bpe_vocab = d / "bpe.vocab"
        if not bpe_vocab.is_file():
            # The release ships no bpe.vocab, but hotwords given as WORDS need one to be cut into the model's pieces.
            # sherpa-onnx's own example derives it from tokens.txt with EQUAL scores, which makes its encoder a
            # longest-match tokenizer - close enough to SentencePiece for short phrases.
            lines = [ln.split(" ")[0] + "\t-1.0" for ln in (d / "tokens.txt").read_text(encoding="utf-8").splitlines() if ln.strip()]
            bpe_vocab.write_text("\n".join(lines) + "\n", encoding="utf-8")
        self.recognizer = sherpa_onnx.OfflineRecognizer.from_transducer(
            encoder=str(d / "encoder.int8.onnx"),
            decoder=str(d / "decoder.int8.onnx"),
            joiner=str(d / "joiner.int8.onnx"),
            tokens=str(d / "tokens.txt"),
            num_threads=threads,
            sample_rate=16000,
            feature_dim=80,  # sherpa-onnx reads the real dimension (128) from the model's metadata
            decoding_method=decoding,
            max_active_paths=4,
            hotwords_score=hotword_score,
            modeling_unit="bpe",
            bpe_vocab=str(bpe_vocab),
            model_type="nemo_transducer",
            provider=provider,
        )
        hw = "+hotwords" if use_hotwords else ""
        dec = "greedy" if decoding == "greedy_search" else "beam"
        self.name = f"parakeet-{version}-{dec}{hw}"

    def transcribe(self, samples, vocab: Optional[Vocabulary] = None) -> EngineResult:
        """Transcribes one utterance, with the vocabulary as per-stream hotwords when this engine boosts.

        Args:
            samples: 16 kHz mono float32 samples.
            vocab: The vocabulary (ignored unless the engine was built with ``use_hotwords``).

        Returns:
            The text and the wall time.
        """
        t0 = time.perf_counter()
        hotwords = vocab.hotwords(self.hotword_score) if (self.use_hotwords and vocab and vocab.terms) else None
        stream = self.recognizer.create_stream(hotwords=hotwords) if hotwords else self.recognizer.create_stream()
        stream.accept_waveform(16000, samples)
        self.recognizer.decode_stream(stream)
        text = stream.result.text.strip()
        return EngineResult(text=text, seconds=time.perf_counter() - t0)


class FasterWhisperEngine(Engine):
    """Whisper through CTranslate2 (faster-whisper) on the GPU."""

    def __init__(self, model: str = "large-v3", use_prompt: bool = False, device: str = "cuda",
                 compute_type: str = "float16", beam_size: int = 5, language: str = "en") -> None:
        """Loads the model (from the Hugging Face cache; large-v3 is ~3 GB, turbo ~1.6 GB).

        Args:
            model: A faster-whisper model name ("large-v3", "large-v3-turbo", "distil-large-v3") or a CTranslate2 folder.
            use_prompt: Pass the vocabulary as ``initial_prompt``.
            device: "cuda" or "cpu".
            compute_type: CTranslate2 precision ("float16" on the RTX 4060 Ti; "int8" on a CPU).
            beam_size: Beam width (5 is Whisper's own default for best quality).
            language: Forced language ("en"); None would detect it per file (and can mis-detect a short note).
        """
        add_cuda_dll_dirs()
        from faster_whisper import WhisperModel

        self.model = WhisperModel(model, device=device, compute_type=compute_type, download_root=None)
        self.use_prompt = use_prompt
        self.beam_size = beam_size
        self.language = language
        self.name = f"whisper-{Path(model).name}{'+prompt' if use_prompt else ''}"

    def transcribe(self, samples, vocab: Optional[Vocabulary] = None) -> EngineResult:
        """Transcribes one utterance, with the vocabulary as the decoder's initial prompt when this engine prompts.

        Args:
            samples: 16 kHz mono float32 samples.
            vocab: The vocabulary (ignored unless the engine was built with ``use_prompt``).

        Returns:
            The text (segments joined) and the wall time.
        """
        t0 = time.perf_counter()
        prompt = vocab.whisper_prompt() if (self.use_prompt and vocab and vocab.terms) else None
        segments, _info = self.model.transcribe(
            samples,
            language=self.language,
            beam_size=self.beam_size,
            initial_prompt=prompt,
            # A voice note is one utterance: conditioning a later segment on an earlier one only spreads an early error.
            condition_on_previous_text=False,
            vad_filter=False,  # Telegram notes are short and trimmed by the sender; VAD can clip the first word
            temperature=0.0,
        )
        text = " ".join(s.text.strip() for s in segments).strip()
        return EngineResult(text=text, seconds=time.perf_counter() - t0)


def build_engine(spec: str) -> Engine:
    """Builds an engine from its report name, so a run can be scripted ("parakeet-v2-greedy", "whisper-large-v3+prompt").

    Args:
        spec: One of ``ENGINE_SPECS``.

    Returns:
        The loaded engine.

    Raises:
        ValueError: An unknown spec.
    """
    table = {
        "parakeet-v2-greedy": lambda: ParakeetEngine("v2"),
        "parakeet-v3-greedy": lambda: ParakeetEngine("v3"),
        "parakeet-v2-beam+hotwords": lambda: ParakeetEngine("v2", use_hotwords=True),
        "parakeet-v3-beam+hotwords": lambda: ParakeetEngine("v3", use_hotwords=True),
        "whisper-large-v3": lambda: FasterWhisperEngine("large-v3"),
        "whisper-large-v3+prompt": lambda: FasterWhisperEngine("large-v3", use_prompt=True),
        "whisper-large-v3-turbo": lambda: FasterWhisperEngine("large-v3-turbo"),
        "whisper-large-v3-turbo+prompt": lambda: FasterWhisperEngine("large-v3-turbo", use_prompt=True),
    }
    if spec not in table:
        raise ValueError(f"unknown engine {spec!r}; known: {', '.join(table)}")
    return table[spec]()


#: Every engine spec ``build_engine`` knows, in report order.
ENGINE_SPECS = (
    "parakeet-v2-greedy",
    "parakeet-v3-greedy",
    "parakeet-v2-beam+hotwords",
    "parakeet-v3-beam+hotwords",
    "whisper-large-v3",
    "whisper-large-v3+prompt",
    "whisper-large-v3-turbo",
    "whisper-large-v3-turbo+prompt",
)
