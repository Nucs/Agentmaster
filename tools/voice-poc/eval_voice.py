# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The voice evaluation: every engine x vocabulary context x post-layer over the recorded sentences, scored
(doc/agentmaster/VOICE.md §7).

A configuration is ``<engine> | ctx=<context> | <layers>``:

* engine - ``engines.ENGINE_SPECS`` (an engine whose name carries "+hotwords" / "+prompt" biases its decoding with the
  context's vocabulary; the others transcribe the same audio once whatever the context).
* context - ``none`` (no vocabulary at all), ``builtin`` (the developer glossary), ``session`` (glossary + the session's
  folder name + its CLAUDE.md, pinned at ``--claude-md-rev`` so the test cannot grow easier as this very feature's docs
  land in CLAUDE.md).
* layers - ``raw`` (the model's text), ``dict`` (the dictionary pass), ``dict+llm`` (then the Claude correction).

Usage::

    python eval_voice.py                                   # the owner's recordings, every engine, raw + dict
    python eval_voice.py --synthetic --engines parakeet-v2-greedy,whisper-large-v3+prompt
    python eval_voice.py --llm                             # also the Claude correction layer
    python eval_voice.py --expect "parakeet-v2-beam+hotwords | ctx=session | dict" --max-wer 0.05 --min-cased 1.0

``--expect`` turns the run into a TEST: the exit code is 1 when that configuration misses ``--max-wer`` (corpus WER) or
``--min-cased`` (the share of terms with their exact spelling), 2 when the fixtures are missing, 0 otherwise.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import subprocess
import sys
from dataclasses import asdict
from pathlib import Path

from common import (POC_SRC, RECORDINGS_DIR, RESULTS_DIR, SYNTHETIC_DIR, aggregate_wer, audio_duration_s,
                    decode_audio, find_fixture, load_references, score_transcript)
from engines import ENGINE_SPECS, build_engine
from vocab import Vocabulary, session_vocabulary

#: The commit whose CLAUDE.md is the "session" context: the repository as it stood when the sentences were written
#: (before this feature's own documentation mentioned Parakeet, Opus or voice notes).
DEFAULT_CLAUDE_MD_REV = "a935ca604"


def pinned_claude_md(rev: str, repo: Path) -> str:
    """The repository's CLAUDE.md at ``rev`` (``git show``), or "" when git cannot produce it.

    Args:
        rev: A commit-ish.
        repo: The repository folder.

    Returns:
        The document text.
    """
    try:
        out = subprocess.run(["git", "-C", str(repo), "show", f"{rev}:CLAUDE.md"], capture_output=True, check=True)
        return out.stdout.decode("utf-8", "replace")
    except Exception as exc:  # no git, a shallow clone without the commit: the context degrades to the folder name
        print(f"[eval] CLAUDE.md at {rev} unavailable ({exc}); the session context has no project words", file=sys.stderr)
        return ""


def build_contexts(names: list[str], workdir: Path, title: str, claude_md: str) -> dict[str, Vocabulary | None]:
    """The vocabulary of each requested context.

    Args:
        names: Context names ("none", "builtin", "session").
        workdir: The session's working folder (its leaf name joins the session context).
        title: The session title.
        claude_md: The pinned CLAUDE.md text.

    Returns:
        name -> vocabulary (None for "none").

    Raises:
        ValueError: An unknown context name.
    """
    out: dict[str, Vocabulary | None] = {}
    for name in names:
        if name == "none":
            out[name] = None
        elif name == "builtin":
            out[name] = Vocabulary.builtin()
        elif name == "session":
            out[name] = session_vocabulary(workdir=workdir, title=title, claude_md_text=claude_md)
        else:
            raise ValueError(f"unknown context {name!r}")
    return out


def main(argv: list[str] | None = None) -> int:
    """Runs the evaluation (see the module docstring).

    Args:
        argv: The command line (None = sys.argv).

    Returns:
        The exit code: 0 pass (or no expectation), 1 the expected configuration missed its bar, 2 missing fixtures.
    """
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fixtures", type=Path, default=None, help="fixture folder (default: the owner's recordings)")
    ap.add_argument("--synthetic", action="store_true", help="use the TTS fixtures instead")
    ap.add_argument("--engines", default=",".join(ENGINE_SPECS), help="comma-separated engine specs")
    ap.add_argument("--contexts", default="none,builtin,session", help="comma-separated contexts")
    ap.add_argument("--workdir", type=Path, default=POC_SRC.parent.parent, help="the session's working folder")
    ap.add_argument("--title", default="fluidvoice telegram", help="the session title")
    ap.add_argument("--claude-md-rev", default=DEFAULT_CLAUDE_MD_REV, help="commit whose CLAUDE.md is the context")
    ap.add_argument("--llm", action="store_true", help="also run the Claude correction layer")
    ap.add_argument("--llm-model", default="claude-haiku-5-5")
    ap.add_argument("--expect", default=None, help="the configuration a TEST run asserts")
    ap.add_argument("--max-wer", type=float, default=0.05)
    ap.add_argument("--min-cased", type=float, default=1.0)
    ap.add_argument("--show", type=int, default=6, help="print the hypotheses of the N best configurations")
    args = ap.parse_args(argv)

    folder = args.fixtures or (SYNTHETIC_DIR if args.synthetic else RECORDINGS_DIR)
    refs = load_references()
    fixtures = [(r, find_fixture(folder, r.id)) for r in refs]
    missing = [r.id for r, f in fixtures if f is None]
    if missing:
        print(f"[eval] missing fixtures in {folder}: {', '.join(missing)}", file=sys.stderr)
        return 2
    audio = {r.id: decode_audio(f) for r, f in fixtures}
    claude_md = pinned_claude_md(args.claude_md_rev, args.workdir)
    contexts = build_contexts([c.strip() for c in args.contexts.split(",") if c.strip()], args.workdir, args.title, claude_md)
    if contexts.get("session"):
        print(f"[eval] session vocabulary: {len(contexts['session'].terms)} terms "
              f"({sum(1 for t in contexts['session'].terms if t.source == 'context')} from the session)")

    rows: list[dict] = []
    for spec in [s.strip() for s in args.engines.split(",") if s.strip()]:
        try:
            engine = build_engine(spec)
        except Exception as exc:  # a model that is not downloaded: report it and go on with the others
            print(f"[eval] {spec}: cannot load ({exc})", file=sys.stderr)
            continue
        biased = "+hotwords" in engine.name or "+prompt" in engine.name
        unbiased_cache: dict[str, tuple[str, float]] = {}
        for ctx_name, vocab in contexts.items():
            if biased and vocab is None:
                continue  # a biasing engine without a vocabulary is the same as its unbiased twin
            layers: dict[str, list[dict]] = {"raw": [], "dict": [], "dict+llm": []}
            for ref, _f in fixtures:
                samples = audio[ref.id]
                if biased or ref.id not in unbiased_cache:
                    res = engine.transcribe(samples, vocab)
                    if not biased:
                        unbiased_cache[ref.id] = (res.text, res.seconds)
                    text, secs = res.text, res.seconds
                else:
                    text, secs = unbiased_cache[ref.id]
                dur = audio_duration_s(samples)
                layers["raw"].append({"id": ref.id, "text": text, "seconds": secs, "duration": dur})
                if vocab is not None:
                    fixed, made = vocab.apply(text)
                    layers["dict"].append({"id": ref.id, "text": fixed, "seconds": secs, "duration": dur,
                                           "replacements": made})
                    if args.llm:
                        from llm_fix import llm_fix

                        try:
                            corrected, llm_secs = llm_fix(fixed, vocab, model=args.llm_model)
                        except Exception as exc:  # a failed call keeps the dictionary text, as the app would
                            print(f"[eval] llm: {exc}", file=sys.stderr)
                            corrected, llm_secs = fixed, 0.0
                        layers["dict+llm"].append({"id": ref.id, "text": corrected, "seconds": secs + llm_secs,
                                                   "duration": dur, "llm_seconds": llm_secs})
            for layer, items in layers.items():
                if not items:
                    continue
                scored = []
                for item in items:
                    ref = next(r for r in refs if r.id == item["id"])
                    s = score_transcript(ref, item["text"])
                    scored.append((item, s))
                name = f"{engine.name} | ctx={ctx_name} | {layer}"
                terms = sum(s.terms for _i, s in scored)
                rows.append({
                    "config": name,
                    "wer": aggregate_wer(s for _i, s in scored),
                    "term_rate": sum(s.term_hits for _i, s in scored) / terms if terms else 1.0,
                    "cased_rate": sum(s.cased_term_hits for _i, s in scored) / terms if terms else 1.0,
                    "rtf": sum(i["seconds"] for i, _s in scored) / max(1e-9, sum(i["duration"] for i, _s in scored)),
                    "files": [dict(item, score=asdict(s)) for item, s in scored],
                })
        del engine

    rows.sort(key=lambda r: (r["wer"], -r["cased_rate"], r["rtf"]))
    print()
    print(f"{'configuration':58} {'WER':>6} {'terms':>6} {'cased':>6} {'RTF':>6}")
    for r in rows:
        print(f"{r['config']:58} {r['wer']*100:5.1f}% {r['term_rate']*100:5.0f}% {r['cased_rate']*100:5.0f}% {r['rtf']:6.3f}")
    for r in rows[: args.show]:
        print(f"\n== {r['config']}")
        for f in r["files"]:
            sc = f["score"]
            flags = []
            if sc["missed_terms"]:
                flags.append("missed: " + ", ".join(sc["missed_terms"]))
            if sc["miscased_terms"]:
                flags.append("miscased: " + ", ".join(sc["miscased_terms"]))
            print(f"  {f['id']} wer={sc['wer']*100:4.1f}%  {f['text']}" + (f"   [{'; '.join(flags)}]" if flags else ""))

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = RESULTS_DIR / f"eval-{stamp}-{folder.name}.json"
    out.write_text(json.dumps({"fixtures": str(folder), "claude_md_rev": args.claude_md_rev, "rows": rows}, indent=1),
                   encoding="utf-8")
    print(f"\n[eval] results: {out}")

    if args.expect:
        row = next((r for r in rows if r["config"] == args.expect), None)
        if row is None:
            print(f"[eval] FAIL: no configuration named {args.expect!r}", file=sys.stderr)
            return 1
        ok = row["wer"] <= args.max_wer and row["cased_rate"] >= args.min_cased
        verdict = "PASS" if ok else "FAIL"
        print(f"[eval] {verdict}: {args.expect}: WER {row['wer']*100:.1f}% (max {args.max_wer*100:.1f}%), "
              f"cased terms {row['cased_rate']*100:.0f}% (min {args.min_cased*100:.0f}%)")
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
