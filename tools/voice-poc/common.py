# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Shared plumbing of the Telegram voice POC (doc/agentmaster/VOICE.md §4): where its files live, how a voice note is
decoded, and how a transcript is scored against its reference.

Everything heavy (the models, the venv, the recordings of the owner's voice) lives under ``POC_HOME`` on Q:, never in
the repository: K: is a nearly full SATA SSD (CLAUDE.md *Building FAST* #1) and the repository is PUBLIC, so a
recording committed here would publish the owner's voice.

The scoring is deliberately two-sided, because "close to perfect" means two different things for a dictated prompt:

* ``wer`` - the word error rate after the Whisper English normalizer (case, punctuation, "forty" == "40", British
  spellings). It answers "did the words come through", and forgives formatting choices a reader never notices.
* ``term_hits`` / ``cased_term_hits`` - whether each technical term of the sentence came through as ONE exact token
  sequence ("conpty", not "con pty") and, for the cased variant, with its canonical spelling ("ConPTY", "JSONL").
  These answer "would Claude Code (and the owner reading the echo) see the identifier it was meant to be".
"""

from __future__ import annotations

import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence

# --------------------------------------------------------------------------------------------------------------------
# Locations
# --------------------------------------------------------------------------------------------------------------------

#: The POC's home on the fast NVMe drive: ``venv\``, ``models\``, ``recordings\``, ``synthetic\``, ``results\``.
#: ``AM_VOICE_HOME`` moves it (an e2e guest, another machine); the default is this PC's Q: folder.
POC_HOME = Path(os.environ.get("AM_VOICE_HOME", r"Q:\agentmaster-voice"))
#: Model files (Parakeet ONNX folders, the openai-whisper download root, CTranslate2 conversions).
MODELS_DIR = POC_HOME / "models"
#: The owner's real voice notes, fetched from the bot chat by ``fetch_tg_voice.py`` (never committed: public repo).
RECORDINGS_DIR = POC_HOME / "recordings"
#: Text-to-speech renditions of the same sentences (``synth_fixtures.py``): a pipeline smoke test, not the evaluation.
SYNTHETIC_DIR = POC_HOME / "synthetic"
#: One JSON file per evaluation run (``eval_voice.py``), so runs can be compared after the fact.
RESULTS_DIR = POC_HOME / "results"
#: The folder of this POC's sources (references.json sits beside the scripts).
POC_SRC = Path(__file__).resolve().parent


def add_cuda_dll_dirs() -> None:
    """Puts PyTorch's bundled CUDA 12 / cuDNN 9 DLLs on the DLL search path before CTranslate2 loads.

    CTranslate2's Windows wheel links ``cudnn64_9.dll`` and ``cublas64_12.dll`` but ships neither; this machine's PATH
    reaches an OLDER torch (ChatWithRTX's) first, whose cuDNN 8 makes ``ctranslate2.Translator`` fail with an obscure
    "could not locate cudnn" at the first GPU call. The torch of the POC's base interpreter carries the right ones.
    Safe to call twice; a machine without torch simply keeps the CPU path.
    """
    try:
        import torch  # noqa: F401 - imported for its file location only

        lib = Path(torch.__file__).resolve().parent / "lib"
        if lib.is_dir():
            os.add_dll_directory(str(lib))
            # CTranslate2 also resolves through PATH on some code paths (cuBLAS is loaded lazily by name).
            os.environ["PATH"] = str(lib) + os.pathsep + os.environ.get("PATH", "")
    except Exception as exc:  # torch missing or broken: the engines fall back to CPU
        print(f"[common] no torch CUDA DLLs: {exc}", file=sys.stderr)


# --------------------------------------------------------------------------------------------------------------------
# Audio
# --------------------------------------------------------------------------------------------------------------------


def decode_audio(path: Path | str, sample_rate: int = 16000):
    """Decodes any audio file (a Telegram voice note's Ogg/Opus, WAV, MP3, M4A) to mono float32 PCM.

    PyAV (FFmpeg's libraries, bundled with faster-whisper) does the work, so the POC needs no ffmpeg.exe; the resampler
    is FFmpeg's ``swr`` (a proper low-pass), the same quality the C++ helper must match (VOICE.md §6.3).

    Args:
        path: The audio file.
        sample_rate: The output rate in Hz; every engine here wants 16000.

    Returns:
        A 1-D ``numpy.float32`` array in [-1, 1] at ``sample_rate``.

    Raises:
        FileNotFoundError: The file does not exist.
        av.error.InvalidDataError: The file is not audio PyAV can read.
    """
    import av  # local import: the scoring helpers below must import without PyAV
    import numpy as np

    path = Path(path)
    if not path.is_file():
        raise FileNotFoundError(path)
    chunks: list = []
    with av.open(str(path)) as container:
        stream = next(s for s in container.streams if s.type == "audio")
        resampler = av.AudioResampler(format="flt", layout="mono", rate=sample_rate)
        for frame in container.decode(stream):
            for out in resampler.resample(frame):
                chunks.append(out.to_ndarray().reshape(-1))
        # Flush the resampler: it holds back the filter's tail, which is the last ~10 ms of speech.
        for out in resampler.resample(None):
            chunks.append(out.to_ndarray().reshape(-1))
    if not chunks:
        return np.zeros(0, dtype=np.float32)
    return np.concatenate(chunks).astype(np.float32, copy=False)


def audio_duration_s(samples, sample_rate: int = 16000) -> float:
    """The length of decoded samples in seconds (for the real-time factor of a run).

    Args:
        samples: The decoded mono samples.
        sample_rate: Their rate in Hz.

    Returns:
        The duration in seconds; 0.0 for no samples.
    """
    return float(len(samples)) / float(sample_rate) if sample_rate > 0 else 0.0


# --------------------------------------------------------------------------------------------------------------------
# References and fixtures
# --------------------------------------------------------------------------------------------------------------------


@dataclass
class Reference:
    """One sentence the owner reads aloud: its id, the exact text, and the technical terms that must survive."""

    id: str
    text: str
    terms: list[str] = field(default_factory=list)


def load_references(path: Path | None = None) -> list[Reference]:
    """Reads the evaluation sentences (``references.json`` beside this file by default).

    Args:
        path: Another references file (a new sentence set).

    Returns:
        The sentences in file order.

    Raises:
        FileNotFoundError: The file does not exist.
        json.JSONDecodeError / KeyError: The file is not a references document.
    """
    path = path or (POC_SRC / "references.json")
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    return [Reference(id=s["id"], text=s["text"], terms=list(s.get("terms", []))) for s in doc["sentences"]]


def apply_said(folder: Path, refs: list[Reference]) -> list[Reference]:
    """The references to score a fixture folder against: a recording where the speaker SAID something else than the intended
    sentence ("use" for "run") is scored against what was said, read from ``<folder>/said.json`` ({"said": {id: text}}).

    A correct transcript of the audio must not count as an error because the reader changed a word; the intended sentence
    stays in references.json. Only a change every engine agrees on belongs in said.json (the file says how it was decided).

    Args:
        folder: The fixture folder.
        refs: The intended references.

    Returns:
        New references (the same terms; the text replaced where said.json names the id).
    """
    said_file = Path(folder) / "said.json"
    if not said_file.is_file():
        return refs
    said = json.loads(said_file.read_text(encoding="utf-8")).get("said", {})
    return [Reference(id=r.id, text=said.get(r.id, r.text), terms=list(r.terms)) for r in refs]


def find_fixture(folder: Path, ref_id: str) -> Path | None:
    """Finds the audio file of one sentence in a fixture folder: ``<id>.<any audio extension>``.

    A folder may hold several renditions of the same sentence only under different stems (``s1.oga``, ``s1-zira.oga``);
    the plain ``<id>.*`` one is the fixture, so a stray conversion beside it never changes which file is scored.

    Args:
        folder: The fixture folder (``recordings`` or ``synthetic``).
        ref_id: The sentence id ("s1").

    Returns:
        The file, or None when the folder has no rendition of that sentence.
    """
    for ext in (".oga", ".ogg", ".opus", ".wav", ".mp3", ".m4a", ".flac"):
        candidate = folder / f"{ref_id}{ext}"
        if candidate.is_file():
            return candidate
    return None


# --------------------------------------------------------------------------------------------------------------------
# Scoring
# --------------------------------------------------------------------------------------------------------------------

# Unit spellings an engine may choose for a spoken unit. The Whisper normalizer turns number WORDS into digits but
# leaves unit abbreviations alone, so "16 kHz" and "sixteen kilohertz" would differ by one word without this map.
_UNIT_WORDS = (
    (re.compile(r"(?<=\d)\s*khz\b", re.IGNORECASE), " kilohertz"),
    (re.compile(r"\bkhz\b", re.IGNORECASE), "kilohertz"),
    (re.compile(r"(?<=\d)\s*hz\b", re.IGNORECASE), " hertz"),
    (re.compile(r"(?<=\d)\s*ms\b", re.IGNORECASE), " milliseconds"),
)

_NORMALIZER = None


def normalize_for_wer(text: str) -> str:
    """Normalizes a transcript (or a reference) for word-error-rate scoring.

    Units first (``_UNIT_WORDS``), then OpenAI Whisper's ``EnglishTextNormalizer``: lowercase, punctuation removed,
    number words to digits, British to American spelling, contractions expanded. The same function runs on both sides,
    so the reference may be written the way a person writes it ("sixteen kilohertz", "ConPTY").

    Args:
        text: The text to normalize.

    Returns:
        Space-separated lowercase words; "" for empty input.
    """
    global _NORMALIZER
    if _NORMALIZER is None:
        from whisper.normalizers import EnglishTextNormalizer  # the de facto standard for English WER

        _NORMALIZER = EnglishTextNormalizer()
    for pattern, replacement in _UNIT_WORDS:
        text = pattern.sub(replacement, text)
    return " ".join(_NORMALIZER(text).split())


@dataclass
class Score:
    """The score of one transcript against its reference."""

    wer: float  # word error rate after normalize_for_wer (0.0 = every word right)
    substitutions: int
    deletions: int
    insertions: int
    ref_words: int
    term_hits: int  # terms present as an exact normalized token sequence
    cased_term_hits: int  # terms present with their canonical spelling in the raw transcript
    terms: int  # number of terms of the sentence
    missed_terms: list[str] = field(default_factory=list)  # the terms that did not come through (normalized check)
    miscased_terms: list[str] = field(default_factory=list)  # came through, but not with the canonical spelling


def _contains_sequence(haystack: Sequence[str], needle: Sequence[str]) -> bool:
    """Whether ``needle`` occurs as a contiguous run of ``haystack`` (token lists).

    Args:
        haystack: The transcript's tokens.
        needle: The term's tokens.

    Returns:
        True when the term occurs; False for an empty term (an empty term would match everywhere and hide a bug).
    """
    n = len(needle)
    if n == 0:
        return False
    return any(list(haystack[i : i + n]) == list(needle) for i in range(len(haystack) - n + 1))


def score_transcript(ref: Reference, hypothesis: str) -> Score:
    """Scores one transcript: WER after normalization, plus the term checks of the module docstring.

    Args:
        ref: The sentence that was read.
        hypothesis: What the pipeline produced (raw, as it would be sent to Claude Code).

    Returns:
        The score.
    """
    import jiwer  # local: only scoring needs it

    ref_norm = normalize_for_wer(ref.text)
    hyp_norm = normalize_for_wer(hypothesis)
    # jiwer refuses an empty reference; a sentence file never has one, but an empty HYPOTHESIS is a real outcome (a model
    # that heard nothing) and must score as all-deletions rather than crash the run.
    out = jiwer.process_words(ref_norm, hyp_norm if hyp_norm else "<empty>")
    hyp_tokens = hyp_norm.split()
    missed: list[str] = []
    miscased: list[str] = []
    hits = cased = 0
    for term in ref.terms:
        if _contains_sequence(hyp_tokens, normalize_for_wer(term).split()):
            hits += 1
            # The cased check reads the RAW transcript with word boundaries: "ConPTY" must appear as written, not as
            # "Conpty" or "CONPTY". A term written all in lowercase ("named pipe") is a pair of plain words whose casing
            # is incidental (a sentence start capitalizes it), so it is matched without case; only a term that carries
            # capitals is a SPELLING the transcript must reproduce.
            flags = 0 if any(ch.isupper() for ch in term) else re.IGNORECASE
            if re.search(r"(?<![\w])" + re.escape(term) + r"(?![\w])", hypothesis, flags):
                cased += 1
            else:
                miscased.append(term)
        else:
            missed.append(term)
    return Score(
        wer=float(out.wer),
        substitutions=out.substitutions,
        deletions=out.deletions,
        insertions=out.insertions if hyp_norm else 0,
        ref_words=len(ref_norm.split()),
        term_hits=hits,
        cased_term_hits=cased,
        terms=len(ref.terms),
        missed_terms=missed,
        miscased_terms=miscased,
    )


def aggregate_wer(scores: Iterable[Score]) -> float:
    """The corpus WER of several transcripts: total errors over total reference words (not a mean of per-file rates,
    which would weigh a short sentence as much as a long one).

    Args:
        scores: The per-file scores.

    Returns:
        The corpus WER; 0.0 for no scores.
    """
    scores = list(scores)
    words = sum(s.ref_words for s in scores)
    errors = sum(s.substitutions + s.deletions + s.insertions for s in scores)
    return errors / words if words else 0.0
