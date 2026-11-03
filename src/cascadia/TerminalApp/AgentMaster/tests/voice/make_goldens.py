# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Writes the goldens of the voice vocabulary port (VoiceVocabulary.h; VOICE.md §5.5): the Python reference's answers,
for tests_voice.cpp to replay byte for byte.

The reference (tools/voice-poc/vocab.py) asks "is this an everyday English word?" of Windows' spell checker. To keep
the goldens independent of the machine's dictionary, this script RECORDS every answer the reference needed while it
computed the goldens and writes them to plain_words.txt; the C++ test answers from that list (and fails on any word
not in it - a parity bug: the port asked something the reference never did).

Run it after a change of a rule in vocab.py (with the POC's venv, which has comtypes for the spell checker)::

    Q:\\agentmaster-voice\\venv\\Scripts\\python.exe make_goldens.py

Outputs (beside this file): vocab_goldens.json, plain_words.txt.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[5]  # tests\voice -> tests -> AgentMaster -> TerminalApp -> cascadia -> src -> repo
sys.path.insert(0, str(REPO / "tools" / "voice-poc"))

import vocab  # noqa: E402  (the reference, from the POC folder)

#: Terms whose rule answers are pinned one by one (the glossary's hard cases plus the context sample's).
TERMS = [
    "ConPTY", "Agentmaster", "JSONL", "JSON", "PCM", "DuckDB", "WinRT", "C++", "C#", "UTF-8", "k3s", "Node.js", "VS Code",
    "Claude Code", "named pipe", "Telegram", "Parakeet", "SessionState", "PDBs", "x64", "llama.cpp", "Hugging Face",
    "SHA-256", "MSIX", "XAML", "kHz", "regex", "Ogg", "Opus", "NVMe", "cuDNN", "Win32", "ARM64",
]

#: Transcripts the dictionary pass rewrites (or must leave alone), across every vocabulary below.
TRANSCRIPTS = [
    "Check why the Con PTY host in Agent Master drops the hook events from the named pipe after a session resumes.",
    "Check why the ConPTY host in AgentMaster drops the hook events.",
    "Decode the Telegram voice note from OG Opus to sixteen kilohertz mono PCM, then run Parakeet on the GPU.",
    "Decode the Telegram voice note from Agopus to 16 kHz mono pcm.",
    "Add a unit test that feeds the transcript scanner a JSON L file with forty lines.",
    "Add a unit test that feeds the transcript scanner a Jason L file and asserts the session state flips.",
    "the server rejects it and we go on, ON and AND stay as they are",
    "clawed said the gpu and pcm are fine; conpty, agentmaster.",
    "Ask Cloud Code to open (con pty) and [duck db], then \"win rt\".",
    "use c plus plus and c sharp with utf 8 on x 64",
    "run kube control get pods on k three s",
    "the session state flips from running to waiting in the terminal page",
    "",
    "   leading and trailing spaces   ",
    "JSONL JSONL json l Jason l",
    # The property checks of tests_voice.cpp (section 5) replay these too: their words must be in the frozen list.
    "we go on and the server rejects it",
    "(con PTY),",
    "fluid boys said hi",
    # Spoken SEARCH requests (search_vocabulary): the fleet's names respelled, the everyday words around them left alone.
    # ("con pty HOST", not "con pty fix": next to the title term ConPTY-fix the pass is not idempotent - VOICE.md §5.4 - and
    # tests_voice.cpp's second pass would ask a spelling this script never records.)
    "find the agent master sessions about the con pty host and the better clipboard tab",
    "open the fluid voice one, then the num sharp np where port",
]

#: The owner's dictionary text the "+dictionary" vocabularies carry.
DICTIONARY = "# my words\nFluidVoice\nfluid voice, fluid boys => FluidVoice\n\njay son el => JSONL\nRunPod\n"

#: A spoken search's FLEET (search_vocabulary): the folders, most recently active first. Every slot rule is here: a name
#: that joins (Agentmaster, BetterClipboard with its trailing separator, FluidVoice with forward slashes), a generic leaf
#: ("src" of NumSharp\src), a second clone of a name already judged (agentmaster: never asked again), a drive root (no
#: name) and a lowercase name (kvnode, catalog).
SEARCH_FOLDERS = [
    r"K:\source\Agentmaster", r"K:\source\NumSharp\src", r"Q:\tmp\agentmaster", "K:\\", r"K:\source\kvnode",
    "C:\\Users\\me\\BetterClipboard\\", r"K:\source\catalog", "K:/work/FluidVoice",
]

#: ...and the tabs' titles, most recently active first (an empty one adds nothing).
SEARCH_TITLES = ["fluidvoice telegram ConPTY-fix", "Proxmox backup (fork)", "NumSharp np.where port", "Telegram voice notes", ""]


def main() -> int:
    """Computes and writes the goldens.

    Returns:
        The exit code (0 = written).
    """
    asked: dict[str, bool] = {}
    # The real dictionary lookup, held before the recorder replaces it (``_known`` with no test asks the spell checker).
    real = vocab._known.__wrapped__

    def recording_test(spelling: str) -> bool:
        """Answers like the reference (the spell checker) and records the answer.

        Args:
            spelling: One exact spelling the reference asked about.

        Returns:
            The spell checker's answer.
        """
        vocab._WORD_TEST = None  # a direct switch: set_word_test would clear the caches mid-computation
        try:
            answer = bool(real(spelling))
        finally:
            vocab._WORD_TEST = recording_test
        asked[spelling] = answer
        return answer

    vocab.set_word_test(recording_test)
    sample = (HERE / "context_sample.md").read_text(encoding="utf-8")

    goldens: dict = {"version": 1, "terms": [], "context": {}, "vocabularies": {}}
    for t in TERMS:
        goldens["terms"].append({
            "term": t,
            "squash": vocab.squash(t),
            "aliases": sorted(vocab.spoken_aliases(t)),
            "case_split": vocab._CASE_SPLIT.findall(t),
            "distinctive": vocab.is_distinctive(t),
            "hard": vocab.is_hard(t),
        })
    goldens["context"] = {
        "file": "context_sample.md",
        "terms": [[w, c] for w, c in vocab.extract_context_terms(sample)],
    }
    vocabularies = {
        "builtin": vocab.Vocabulary.builtin(),
        "session": vocab.session_vocabulary(workdir=Path(r"K:\source\Agentmaster"), title="fluidvoice telegram ConPTY-fix",
                                            claude_md_text=sample),
        "session+dictionary": vocab.session_vocabulary(workdir=Path(r"K:\source\Agentmaster"), title="",
                                                       claude_md_text=sample,
                                                       dictionary=vocab.parse_dictionary(DICTIONARY)),
        # A spoken search (the Search topic): the fleet's names, at the default caps; with the owner's dictionary; and
        # capped hard (2 names: the folders after BetterClipboard are never read; 1 title).
        "search": vocab.search_vocabulary(SEARCH_FOLDERS, SEARCH_TITLES),
        "search+dictionary": vocab.search_vocabulary(SEARCH_FOLDERS, SEARCH_TITLES,
                                                     dictionary=vocab.parse_dictionary(DICTIONARY)),
        "search-capped": vocab.search_vocabulary(SEARCH_FOLDERS, SEARCH_TITLES, max_folders=2, max_titles=1),
    }
    for name, v in vocabularies.items():
        entry = {
            "terms": [[t.text, t.weight, t.source, t.respell_split] for t in v.terms],
            "whisper_prompt": v.whisper_prompt(),
            "whisper_prompt_200": v.whisper_prompt(200),
            "hotwords": v.hotwords(),
            "hotwords_10": v.hotwords(2.0, 10),
            "apply": [],
        }
        for text in TRANSCRIPTS:
            out, made = v.apply(text)
            entry["apply"].append({"in": text, "out": out, "made": [list(m) for m in made]})
        goldens["vocabularies"][name] = entry
    # The correction layer's prompts (VoiceCorrect.h ports llm_fix.py): the system prompt verbatim, the glossary line of a
    # session vocabulary, and the user prompt with and without other engines' hearings.
    import llm_fix  # noqa: E402  (the reference's correction layer, beside vocab.py)

    session = vocabularies["session"]
    goldens["correction"] = {
        "system_prompt": llm_fix.SYSTEM_PROMPT,
        "glossary_160": session.glossary_line(160),
        "glossary_5": session.glossary_line(5),
        "cases": [
            {"text": t, "alternatives": list(a), "user_prompt": llm_fix.build_user_prompt(t, session.glossary_line(5), tuple(a))}
            for t, a in (
                ("Check why the ConPTY host drops.", ()),
                ("Decode a note with a faulty lines.", ("Decode the note with a 40 lines.",)),
                ("Same text twice.", ("Same text twice.",)),
                ("Three hearings.", ("Second hearing.", "Third hearing.")),
            )
        ],
        "accept": [
            {"before": b, "after": a, "accepted": llm_fix.accept_correction(b, a)}
            for b, a in (
                ("Decode a note with a faulty lines.", "Decode the note with 40 lines."),
                ("Decode a note with a faulty lines.", ""),
                ("Decode a note with a faulty lines.", "   "),
                ("Short.", "Short, fixed."),
                ("Check why the ConPTY host drops the hook events.", "I can't help with that. Here is an explanation of what a ConPTY host is and why hook events could be dropped after a resume, in detail."),
                ("Check why the ConPTY host drops the hook events.", "Check."),
            )
        ],
    }
    goldens["dictionary_text"] = DICTIONARY
    goldens["dictionary"] = [[t, trig] for t, trig in vocab.parse_dictionary(DICTIONARY)]
    goldens["session_args"] = {
        "session": {"workdir": r"K:\source\Agentmaster", "title": "fluidvoice telegram ConPTY-fix", "dictionary": False},
        "session+dictionary": {"workdir": r"K:\source\Agentmaster", "title": "", "dictionary": True},
    }
    # The search vocabularies' inputs. "defaults" means the C++ builds it WITHOUT passing caps, so its own default caps are
    # what is checked; "search_defaults" pins those defaults to the reference's.
    goldens["search_args"] = {
        "folders": SEARCH_FOLDERS,
        "titles": SEARCH_TITLES,
        "search": {"dictionary": False, "defaults": True},
        "search+dictionary": {"dictionary": True, "defaults": True},
        "search-capped": {"dictionary": False, "defaults": False, "max_folders": 2, "max_titles": 1},
    }
    goldens["search_defaults"] = {"max_folders": vocab.SEARCH_MAX_FOLDERS, "max_titles": vocab.SEARCH_MAX_TITLES}

    (HERE / "vocab_goldens.json").write_text(json.dumps(goldens, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    lines = [f"{w}\t{1 if a else 0}" for w, a in sorted(asked.items())]
    (HERE / "plain_words.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"goldens: {len(TERMS)} terms, {len(goldens['context']['terms'])} context terms, "
          f"{len(vocabularies)} vocabularies x {len(TRANSCRIPTS)} transcripts; {len(asked)} words frozen")
    return 0


if __name__ == "__main__":
    sys.exit(main())
