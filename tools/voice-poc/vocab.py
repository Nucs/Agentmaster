# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The session vocabulary: FluidVoice's "word boost" + "Custom Dictionary" layers, rebuilt for a dictated Claude Code
prompt (doc/agentmaster/VOICE.md §5).

FluidVoice (altic-dev/FluidVoice, GPLv3; studied, not copied) gets technical words right with three layers:

1. **Word boost** (``ParakeetVocabularyStore``): a list of terms with spoken aliases and weights that biases the Parakeet
   decoder toward them (FluidAudio's CTC keyword spotting).
2. **Custom Dictionary** (``BuiltInDictionary`` + the user's entries): a text pass after recognition that maps trigger
   spellings to a replacement ("clawed" -> "Claude", "hugging face" -> "Hugging Face").
3. **AI Enhancement**: an optional LLM pass over the text (``llm_fix.py`` here).

Here the same three layers take their words from WHERE THE PROMPT GOES - the session's working folder (its name, its
CLAUDE.md) - plus a built-in developer glossary, so nobody has to maintain a word list per project:

* ``Vocabulary.whisper_prompt`` - Whisper's ``initial_prompt`` (the decoder conditions on it, ~224 tokens at most).
* ``Vocabulary.hotwords`` - sherpa-onnx's per-stream hotwords for Parakeet TDT (modified beam search + an Aho-Corasick
  context graph: the closest thing to FluidAudio's word boost that runs on Windows).
* ``Vocabulary.apply`` - the dictionary pass. It only touches a DISTINCTIVE spelling (an inner capital, an acronym, a
  digit, a word that is not plain English), and only when 1-4 transcript words SQUASH to that term or one of its spoken
  aliases ("Con PTY", "con-pty", "Agent Master", "Jason L" -> "ConPTY", "Agentmaster", "JSONL"). A plain English word
  is never re-cased ("go" stays "go" even with Go in the glossary).

⚠ The evaluation sentences were written by the same author as this glossary. The glossary is a general developer list
(languages, formats, tools, models) and never names a sentence's phrase; the honest test of generality is a NEW
sentence set (VOICE.md §7.4).
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path
from typing import Callable, Iterable, Optional

# --------------------------------------------------------------------------------------------------------------------
# The built-in glossary: words a developer says that a general speech model writes wrong or in the wrong case.
# --------------------------------------------------------------------------------------------------------------------

BUILTIN_GLOSSARY: tuple[str, ...] = (
    # ORDER IS PRIORITY: Whisper's prompt keeps ~100 terms, and when the session's own words take room, the glossary is cut
    # from its END. Measured on the owner's recordings (2026-10-27): with formats first, the AI / speech group never reached
    # the prompt. A prompt to Claude Code names AI tools, models and speech/audio terms most, generic words least.
    # AI and speech
    "Claude", "Claude Code", "Anthropic", "OpenAI", "GPT", "LLM", "MCP", "Whisper", "Parakeet", "ONNX", "PyTorch",
    "TensorFlow", "Hugging Face", "LoRA", "RAG", "embeddings", "tokenizer", "Sonnet", "Opus", "Haiku", "Codex",
    "Gemini", "Ollama", "llama.cpp",
    # Audio / media
    "PCM", "WAV", "MP3", "AAC", "Opus", "Ogg", "FLAC", "FFmpeg", "codec", "kHz", "VAD",
    # Hardware and OS
    "CPU", "GPU", "RAM", "SSD", "NVMe", "CUDA", "cuDNN", "RTX", "NVIDIA", "x64", "ARM64", "Windows", "Win32", "WinRT",
    "ConPTY", "PowerShell", "WSL", "MSIX", "AppX", "NuGet", "vcpkg", "MSBuild", "CMake", "XAML", "COM", "DLL", "PDB",
    "PID", "TTY", "PTY", "VT100", "ANSI",
    # Data formats, encodings, protocols
    "JSON", "JSONL", "YAML", "TOML", "XML", "CSV", "HTML", "CSS", "Markdown", "HTTP", "HTTPS", "REST", "gRPC",
    "WebSocket", "OAuth", "JWT", "UTF-8", "UTF-16", "ASCII", "Base64", "SHA-256", "TLS", "SSH", "DNS", "TCP", "UDP",
    "IPv4", "IPv6", "URL", "URI", "UUID", "GUID", "regex", "stdin", "stdout", "stderr",
    # Languages and runtimes
    "C++", "C#", ".NET", "Python", "TypeScript", "JavaScript", "Node.js", "Rust", "Golang", "Kotlin", "SQL", "Bash",
    "WebAssembly",
    # Tools and services
    "Git", "GitHub", "GitLab", "Docker", "Kubernetes", "kubectl", "k3s", "Helm", "npm", "pnpm", "Yarn", "Vite",
    "Webpack", "pytest", "Jest", "VS Code", "Visual Studio", "WinDbg", "Telegram", "Slack", "Discord", "WhatsApp",
    "Cloudflare", "Traefik", "Proxmox",
    # Databases and queues
    "PostgreSQL", "Postgres", "MySQL", "SQLite", "DuckDB", "Redis", "MongoDB", "Kafka", "RabbitMQ",
    # Generic engineering words a model splits or mis-cases
    "API", "SDK", "CLI", "GUI", "UI", "UX", "IDE", "CI", "PR", "repo", "async", "await", "mutex", "nullptr", "enum",
    "struct", "refactor", "middleware", "localhost", "namespace", "lambda", "boolean", "JSON schema", "OpenAPI",
)

# CURATED TRIGGERS: how a term is SAID (or misheard) when a model does not write it right - FluidVoice's built-in
# dictionary works the same way ("clawed" -> "Claude"). A trigger is explicit, so it may rewrite even an everyday word
# ("jason" -> JSON); keep each one a sound that, in a developer's prompt, means the term. Letter-by-letter spellings
# and case splits are generated for every term automatically (spoken_aliases) and need no entry here. Never add an
# everyday word whose ordinary meaning is common in prompts ("rejects" for regex would break "the server rejects it").
ACRONYM_SAYINGS: dict[str, tuple[str, ...]] = {
    "Claude": ("claud", "clawed"),
    "Claude Code": ("cloud code", "clawed code", "claud code"),
    "JSON": ("jason", "j son"),
    "JSONL": ("jason l", "json l", "jason el", "json el", "jason line", "json lines"),
    "SQL": ("sequel",),
    "SQLite": ("sequel lite", "sequel light", "sql lite", "sql light", "s q lite"),
    "MySQL": ("my sequel", "my sql"),
    "PostgreSQL": ("postgres sequel", "postgres q l", "postgre sql"),
    "GUI": ("gooey",),
    "YAML": ("yamel", "yammel"),
    "TOML": ("tom l", "tommel"),
    "ASCII": ("askey", "ask e"),
    "NuGet": ("new get",),
    "vcpkg": ("vc package", "v c package", "vcpackage"),
    "OAuth": ("o auth",),
    "regex": ("reg ex",),
    "Ogg": ("og",),
    "kubectl": ("kube control", "kube c t l", "kube cuddle", "cube control"),
    "k3s": ("k three s", "k3 s", "k 3 s"),
    "nullptr": ("null pointer", "null ptr"),
    "stdin": ("standard in", "std in", "s t d in"),
    "stdout": ("standard out", "std out"),
    "stderr": ("standard error", "std err"),
    "ConPTY": ("con pty", "con p t y", "compty", "con petty"),
    "PTY": ("pity", "p t y"),
    "WinRT": ("win rt", "win r t", "windows runtime"),
    "Win32": ("win 32", "win thirty two"),
    "CUDA": ("cooda", "kuda"),
    "cuDNN": ("cu dnn", "cuda nn", "cuda dnn"),
    "PyTorch": ("pie torch", "py torch"),
    "Node.js": ("node js", "node j s", "nodejs"),
    "C++": ("c plus plus", "cpp", "c p p"),
    "C#": ("c sharp", "see sharp"),
    ".NET": ("dot net",),
    "UTF-8": ("utf 8", "u t f 8", "utf eight", "u t f eight"),
    "SHA-256": ("sha 256", "shah 256", "sha two fifty six"),
    "x64": ("x 64", "x sixty four"),
    "ARM64": ("arm 64", "arm sixty four"),
    "llama.cpp": ("llama cpp", "llama c p p", "llama dot cpp"),
    "MSIX": ("m s i x", "msi x"),
    "XAML": ("zamel", "zammel", "xamel"),
    "DuckDB": ("duck db", "duck d b"),
    "gRPC": ("g rpc", "g r p c"),
    "VS Code": ("vs code", "v s code", "vscode"),
}

# --------------------------------------------------------------------------------------------------------------------
# Plain-English test
# --------------------------------------------------------------------------------------------------------------------


@lru_cache(maxsize=1)
def _spell_checker():
    """Windows' own en-US spell checker (the Spell Checking API of spellcheck.h, through comtypes), or None.

    It is THE word test of this design because the C++ integration calls the very same API (``ISpellCheckerFactory`` ->
    ``ISpellChecker::Check``, Windows 8 and later, nothing to ship), so the POC and the app agree on every word. Measured
    on this PC: it knows "scheduler", "gotcha", "rejects", "parakeet", "telegram", "opus", "on", "go"; it does not know
    "conpty", "jsonl", "ogg", "og", "agentmaster", "json", "gpu", "claude", "xaml". ⚠ It skips ALL-CAPS words (an
    acronym is never flagged), so callers pass lowercase.

    Returns:
        The ``ISpellChecker`` pointer, or None when comtypes or the API is unavailable.
    """
    try:
        import comtypes.client
        from comtypes import COMMETHOD, GUID, HRESULT, IUnknown
        from ctypes import POINTER, c_int, c_wchar_p

        class ISpellingError(IUnknown):
            _iid_ = GUID("{B7C82D61-FBE8-4B47-9B27-6C0D2E0DE0A3}")

        class IEnumSpellingError(IUnknown):
            _iid_ = GUID("{803E3BD4-2828-4410-8290-418D1D73C762}")

        class ISpellChecker(IUnknown):
            _iid_ = GUID("{B6FD0B71-E2BC-4653-8D05-F197E412770B}")

        class ISpellCheckerFactory(IUnknown):
            _iid_ = GUID("{8E018A9D-2415-4677-BF08-794EA61F94BB}")

        # Only the vtable slots used here, in spellcheck.h's order (a later slot needs every earlier one declared).
        IEnumSpellingError._methods_ = [
            COMMETHOD([], HRESULT, "Next", (["out"], POINTER(POINTER(ISpellingError)), "value")),
        ]
        ISpellChecker._methods_ = [
            COMMETHOD([], HRESULT, "get_LanguageTag", (["out"], POINTER(c_wchar_p), "value")),
            COMMETHOD([], HRESULT, "Check", (["in"], c_wchar_p, "text"), (["out"], POINTER(POINTER(IEnumSpellingError)), "value")),
        ]
        ISpellCheckerFactory._methods_ = [
            COMMETHOD([], HRESULT, "get_SupportedLanguages", (["out"], POINTER(POINTER(IUnknown)), "value")),
            COMMETHOD([], HRESULT, "IsSupported", (["in"], c_wchar_p, "languageTag"), (["out"], POINTER(c_int), "value")),
            COMMETHOD([], HRESULT, "CreateSpellChecker", (["in"], c_wchar_p, "languageTag"), (["out"], POINTER(POINTER(ISpellChecker)), "value")),
        ]
        factory = comtypes.client.CreateObject(GUID("{7AB36653-1796-484B-BDFA-E74F1DB7C1DC}"), interface=ISpellCheckerFactory)
        if not factory.IsSupported("en-US"):
            return None
        return factory.CreateSpellChecker("en-US")
    except Exception:
        return None


#: A replacement dictionary lookup (``set_word_test``): the C++ port's goldens freeze the spell checker's answers into a
#: list, so the port is checked against the same decisions whatever the machine's dictionary holds.
_WORD_TEST: Optional[Callable[[str], bool]] = None


def set_word_test(test: Optional[Callable[[str], bool]]) -> None:
    """Replaces the dictionary lookup (None restores the spell checker) and forgets every cached answer.

    Args:
        test: A function of ONE exact spelling ("pty", "Pty", "english", "English"), True when the dictionary knows that
            spelling. ``is_plain_english`` decides which spellings to ask.
    """
    global _WORD_TEST
    _WORD_TEST = test
    is_plain_english.cache_clear()
    _known.cache_clear()


@lru_cache(maxsize=32768)
def _known(spelling: str) -> bool:
    """Whether the dictionary knows this exact spelling (the spell checker is case-sensitive: "english" is flagged,
    "English" is not).

    Args:
        spelling: One alphabetic spelling.

    Returns:
        True when known (also when no dictionary is available at all: the safe answer is "plain", never rewrite).
    """
    if _WORD_TEST is not None:
        return bool(_WORD_TEST(spelling))
    sc = _spell_checker()
    if sc is not None:
        try:
            # Check returns an enumerator of errors: an empty one (Next gives NULL) means the spelling is right.
            return not bool(sc.Check(spelling).Next())
        except Exception:
            pass
    try:
        from wordfreq import zipf_frequency

        return zipf_frequency(spelling.lower(), "en") >= 2.5
    except Exception:
        return True


@lru_cache(maxsize=16384)
def is_plain_english(word: str) -> bool:
    """Whether ``word`` is an everyday English word: one the dictionary pass must never rewrite, and one that makes a
    term not "hard" (``is_hard``).

    The test is dictionary membership (``_known``: Windows' en-US spell checker, the same API the C++ port asks):

    * an ALL-CAPITAL word of two or more letters is an acronym, a word only when its LOWERCASE spelling is known: "ON",
      "AND" are words; "PTY", "DB", "UI" are not (even though "Pty" - as in "Pty Ltd" - is in the dictionary);
    * any other word is a word when its lowercase OR its title-case spelling is known: proper nouns are in the
      dictionary capitalized only ("English", "Claude", "Kubernetes").

    Args:
        word: The candidate word, in the casing it was written.

    Returns:
        True for an everyday English word; False for a word with a digit or a symbol.
    """
    w = word.strip()
    if not w.isalpha():
        return False
    low = w.lower()
    if w.isupper() and len(w) >= 2:
        return _known(low)
    return _known(low) or _known(low[:1].upper() + low[1:])


# --------------------------------------------------------------------------------------------------------------------
# Terms and aliases
# --------------------------------------------------------------------------------------------------------------------

_CASE_SPLIT = re.compile(r"[A-Z]+(?![a-z])|[A-Z]?[a-z]+|\d+|[^\W\d_]+")


def squash(text: str) -> str:
    """The comparison key of the dictionary pass: lowercase, letters and digits only ('+' and '#' kept, so "C++" and
    "C#" keep their identity).

    Args:
        text: A term, an alias or a window of transcript words.

    Returns:
        The key ("Con-PTY" -> "conpty", "C++" -> "c++").
    """
    return "".join(ch for ch in text.lower() if ch.isalnum() or ch in "+#")


def spoken_aliases(term: str) -> set[str]:
    """The ways a speech model may write ``term`` after hearing it said, generated from the spelling alone: its
    case-split parts ("ConPTY" -> "con pty") and acronym parts spelled letter by letter ("PCM" -> "p c m"). The curated
    ``ACRONYM_SAYINGS`` are NOT included: they are explicit triggers (``Term.triggers``) with their own rules.

    A one-word name with no inner capital ("Agentmaster") also gets every split into two parts of 3+ letters
    ("agent master", "agen tmaster", ...): only the real one ever occurs in a transcript, and the pass compares whole
    squashed windows, so the others cost a dictionary entry each and nothing else.

    Args:
        term: The canonical spelling.

    Returns:
        Lowercase aliases (the canonical lowercase form included).
    """
    aliases: set[str] = {term.lower()}
    parts = _CASE_SPLIT.findall(term)
    if len(parts) > 1:
        aliases.add(" ".join(p.lower() for p in parts))
        # Each all-capital part may be spelled out ("con p t y"); a part of 2-5 capitals is an acronym, longer is a word.
        spelled = [" ".join(p.lower()) if p.isupper() and 2 <= len(p) <= 5 else p.lower() for p in parts]
        aliases.add(" ".join(spelled))
    elif term.isupper() and 2 <= len(term) <= 6:
        aliases.add(" ".join(term.lower()))
    elif term.isalpha() and len(term) >= 7 and " " not in term:
        low = term.lower()
        for i in range(3, len(low) - 2):
            aliases.add(low[:i] + " " + low[i:])
    return {a for a in aliases if a.strip()}


def is_distinctive(term: str) -> bool:
    """Whether the dictionary pass may rewrite text INTO this term's spelling.

    Distinctive: an inner capital (ConPTY, DuckDB), an acronym (PCM, JSONL), a digit or symbol (k3s, C++, UTF-8), or a
    single word that is not plain English (Agentmaster). Not distinctive: lowercase phrases ("named pipe": nothing to
    fix) and capitalized everyday words (Telegram stays boosted, but "whisper" is never forced to "Whisper").

    Args:
        term: The canonical spelling.

    Returns:
        True when re-spelling a matching window into ``term`` is safe.
    """
    if any(ch.isdigit() for ch in term) or any(ch in "+#." for ch in term):
        return True
    words = term.split()
    if any(any(ch.isupper() for ch in w[1:]) for w in words):
        return True  # an inner capital or an acronym (PCM, ConPTY, VS Code)
    if term.islower():
        return False  # a lowercase phrase: the model writes it right or wrong, but casing is not the problem
    return any(not is_plain_english(w) for w in words)


def is_hard(term: str) -> bool:
    """Whether a speech model is likely to get ``term`` wrong: at least one of its case-split parts is not plain English
    (ConPTY's "PTY", Agentmaster as one word, JSONL, DuckDB). ``SessionState`` is not hard - "session state" comes out
    right, it is only a spelling choice - and neither is "named pipe".

    Only hard terms are worth a hotword: every hotword is another path the beam search can be pulled into, and measured
    on the synthetic set, ~480 hotwords (every glossary and CLAUDE.md word) made Parakeet WORSE than none.

    Args:
        term: The canonical spelling.

    Returns:
        True for a term worth boosting.
    """
    if any(ch.isdigit() for ch in term) or any(ch in "+#" for ch in term):
        return True
    parts = [p for w in term.replace(".", " ").split() for p in _CASE_SPLIT.findall(w)]
    return any(not is_plain_english(p) for p in parts if p)


@dataclass
class Term:
    """One vocabulary entry: the canonical spelling, its spoken aliases, a weight and where it came from."""

    text: str
    aliases: set[str] = field(default_factory=set)  # generated from the spelling; guarded in Vocabulary.apply
    triggers: set[str] = field(default_factory=set)  # curated (ACRONYM_SAYINGS or the user's); never guarded
    weight: float = 1.0  # relative priority (prompt order, hotword score)
    source: str = "builtin"  # "builtin" | "context" | "user"
    # Whether a window of several PLAIN English words may be respelled into this term ("agent master" -> Agentmaster).
    # True for curated terms (the glossary, the user's dictionary, the folder's own name); False for an identifier
    # mined from CLAUDE.md, where "session state" is far more often the phrase than the type SessionState.
    respell_split: bool = True


@dataclass
class Vocabulary:
    """The terms one transcription is biased toward, and the three ways they are used (module docstring)."""

    terms: list[Term] = field(default_factory=list)

    # ---- building -----------------------------------------------------------------------------------------------

    def add(self, text: str, weight: float = 1.0, source: str = "builtin", respell_split: bool = True,
            triggers: Iterable[str] = ()) -> None:
        """Adds a term, or raises the weight of one already present (case-insensitive identity).

        Args:
            text: The canonical spelling.
            weight: Its priority; the higher of two weights wins.
            source: Where it came from (shown in reports).
            respell_split: Whether windows of plain English words may be respelled into it (``Term.respell_split``);
                a curated source that re-adds a mined term makes it respellable.
            triggers: The owner's own spoken forms for it (a dictionary line "jay son el => JSONL"); merged into an
                existing term's triggers.
        """
        text = text.strip()
        if not text:
            return
        for t in self.terms:
            if t.text.lower() == text.lower():
                if weight > t.weight:
                    t.weight, t.source = weight, source
                t.respell_split = t.respell_split or respell_split
                t.triggers |= set(triggers)
                return
        self.terms.append(Term(text=text, aliases=spoken_aliases(text),
                               triggers=set(ACRONYM_SAYINGS.get(text, ())) | set(triggers),
                               weight=weight, source=source, respell_split=respell_split))

    @staticmethod
    def builtin() -> "Vocabulary":
        """The built-in developer glossary alone (weight 1).

        Returns:
            A new vocabulary.
        """
        v = Vocabulary()
        for t in BUILTIN_GLOSSARY:
            v.add(t, 1.0, "builtin")
        return v

    # ---- the three uses -----------------------------------------------------------------------------------------

    def ranked(self) -> list[Term]:
        """The terms, highest weight first (stable within a weight: the glossary's own order), so every use that
        truncates (the prompt's character budget, the hotword limit) drops the generic glossary before the session's
        own words.

        Returns:
            A new list; the vocabulary itself is not reordered.
        """
        return sorted(self.terms, key=lambda t: -t.weight)

    def glossary_line(self, max_terms: int = 160) -> str:
        """The glossary the CORRECTION layer gets (llm_fix.py; VoiceCorrect.h): the terms, highest weight first, comma-joined.
        Unlike Whisper's prompt there is room for every term a session has (a model reads 160 terms easily).

        Args:
            max_terms: The most terms sent.

        Returns:
            "Agentmaster, ConPTY, …" ("" for an empty vocabulary).
        """
        return ", ".join(t.text for t in self.ranked()[:max_terms])

    def whisper_prompt(self, max_chars: int = 700) -> str:
        """Whisper's ``initial_prompt``: a short glossary line the decoder conditions on.

        Whisper keeps only the LAST ~224 tokens of a prompt and weighs the end most, so the highest-weight terms go LAST
        and the line is cut from its start. The wording is a plain sentence frame: a bare list invites Whisper to
        continue listing words when the audio is quiet.

        Args:
            max_chars: The character budget (~700 characters stays inside 224 tokens for this kind of text).

        Returns:
            The prompt text, or "" for an empty vocabulary.
        """
        if not self.terms:
            return ""
        ordered = list(reversed(self.ranked()))  # lowest weight first, highest last
        prefix = "A developer dictates a prompt to Claude Code. Glossary: "
        body = ", ".join(t.text for t in ordered) + "."
        budget = max_chars - len(prefix)
        if len(body) > budget:
            body = body[len(body) - budget :]
            body = body[body.find(",") + 1 :].lstrip() if "," in body else body
        return prefix + body

    def hotwords(self, base_score: float = 2.0, limit: int = 96) -> str:
        """sherpa-onnx's per-stream hotwords for a NeMo transducer: '/'-separated phrases, each with its own score.

        Only HARD terms (``is_hard``) are sent: the model already writes "named pipe" and "Telegram", and each extra
        hotword is one more path the beam can be pulled into. The canonical spelling is the phrase (the model's BPE
        pieces are cased). The score grows gently with the weight (2.0 / 2.5 / 3.0 for weights 1 / 2 / 3): a score of
        6 made the decoder insert terms that were never said.

        Args:
            base_score: The score of a weight-1 term (sherpa's default is 1.5; its Parakeet example uses 2.0).
            limit: At most this many phrases, highest weight first.

        Returns:
            The hotwords string ("Agentmaster :3.0/ConPTY :2.5/JSONL :2.0"); "" when no term is hard.
        """
        phrases = []
        for t in self.ranked():
            if len(phrases) >= limit:
                break
            phrase = t.text.replace("/", " ").strip()
            if phrase and is_hard(phrase):
                phrases.append(f"{phrase} :{base_score * (1.0 + 0.25 * (t.weight - 1.0)):.1f}")
        return "/".join(phrases)

    def apply(self, text: str) -> tuple[str, list[tuple[str, str]]]:
        """The dictionary pass: rewrites windows of 1-4 words that squash to a distinctive term or one of its aliases.

        Longest window first, left to right; a window keeps the punctuation that trailed its last word ("PTY," ->
        "ConPTY,"). A window that already IS the canonical spelling is left alone, so the pass is idempotent.

        Args:
            text: The transcript.

        Returns:
            (the rewritten text, the (from, to) replacements made, in order).
        """
        # squashed key -> (term, explicit). A curated trigger is explicit and wins a key over a generated alias; within
        # one kind, the higher-weight term keeps a shared key (ranked order).
        table: dict[str, tuple[Term, bool]] = {}
        for t in self.ranked():
            # A curated trigger is a decision someone made ("clawed" -> Claude), so it applies even when the term itself is
            # an everyday word the generated aliases must leave alone (Claude is a known first name).
            for key in (squash(tr) for tr in t.triggers):
                if key and (key not in table or not table[key][1]):
                    table[key] = (t, True)
            if not is_distinctive(t.text):
                continue
            for key in {squash(t.text)} | {squash(a) for a in t.aliases}:
                if key and key not in table:
                    table[key] = (t, False)
        if not table:
            return text, []
        # Tokens with the separators kept, so the text can be rebuilt exactly around the replaced windows.
        tokens = re.findall(r"\S+|\s+", text)
        words = [i for i, tok in enumerate(tokens) if not tok.isspace()]
        out_tokens = list(tokens)
        made: list[tuple[str, str]] = []
        k = 0
        while k < len(words):
            replaced = False
            for n in (4, 3, 2, 1):
                if k + n > len(words):
                    continue
                first, last = words[k], words[k + n - 1]
                window = "".join(tokens[first : last + 1])
                core = window.rstrip(".,;:!?)]}\"'")
                trail = window[len(core) :]
                lead_strip = core.lstrip("([{\"'")
                lead = core[: len(core) - len(lead_strip)]
                key = squash(lead_strip)
                hit = table.get(key) if key else None
                if hit is None:
                    continue
                term, explicit = hit
                if lead_strip == term.text:
                    break  # already canonical: nothing to do for this window (and no shorter window either)
                window_words = re.findall(r"[^\W_]+", lead_strip)
                if not explicit:
                    # A single plain-English word is never rewritten by a GENERATED alias, in any case: "go" stays "go",
                    # "on" never becomes "ON". The pass exists for SPLIT or MISHEARD names, which are never one
                    # everyday word - except a curated trigger, which says exactly that ("clawed" -> Claude).
                    if n == 1 and len(window_words) == 1 and is_plain_english(window_words[0]):
                        continue
                    # Several plain English words respell only into a curated term: "agent master" -> Agentmaster (the
                    # folder's own name), never "session state" -> SessionState (a type CLAUDE.md happens to mention).
                    if (len(window_words) > 1 and not term.respell_split
                            and all(is_plain_english(w) for w in window_words)):
                        continue
                out_tokens[first] = lead + term.text + trail
                for idx in range(first + 1, last + 1):
                    out_tokens[idx] = ""
                made.append((lead_strip, term.text))
                k += n
                replaced = True
                break
            if not replaced:
                k += 1
        return "".join(out_tokens), made


# --------------------------------------------------------------------------------------------------------------------
# Context: where the session's own words come from
# --------------------------------------------------------------------------------------------------------------------

_IDENT = re.compile(r"(?<![\w.\\/-])([A-Za-z][A-Za-z0-9+#.-]{1,30})(?![\w\\/])")


def extract_context_terms(text: str, limit: int = 60, min_count: int = 2) -> list[tuple[str, int]]:
    """The HARD words of a project document (its CLAUDE.md): the names a developer of that project says and a speech
    model gets wrong.

    Kept: a word with an inner capital, an acronym or a digit whose parts are not all plain English (ConPTY, DuckDB,
    MSIX, WinRT, x64), or a capitalized word that is not plain English (Agentmaster, Codex). Dropped:

    * a code-only spelling nobody dictates: an underscore, "::", a path separator, a cmdlet or flag with an inner
      hyphen (``Get-AppxPackage``, ``Debug-B``), a file name (``Engine.cpp``, ``am-update.ps1``), an error code
      (``C2039``), a trailing "+" left by a "C++/WinRT" boundary;
    * SHOUTED English ("ON", "NOT", "BEFORE", "USERPROFILE"): an all-capital word is an acronym only when it is short
      and not an English word;
    * a compound of plain English words (``SessionState``, ``TerminalPage``): the model writes "session state" right;
      respelling it into the type would change what was said (measured: it broke a correct transcript);
    * a word seen fewer than ``min_count`` times (a one-off mention in a 250 KB document is noise).

    Args:
        text: The document.
        limit: At most this many terms, most frequent first.
        min_count: The fewest occurrences a term needs.

    Returns:
        (term, count) pairs, most frequent first.
    """
    counts: dict[str, int] = {}
    for m in _IDENT.finditer(text):
        word = m.group(1).rstrip(".-")
        if len(word) < 2 or "_" in word or "::" in word or word.endswith("+") and not word.endswith("++"):
            continue
        if re.search(r"\.[A-Za-z0-9]{1,4}$", word) or re.search(r"\.(?:dll|exe|cpp|h|json|md|ps1|py|log)\b", word, re.I):
            continue  # a file name (Engine.cpp, hooks.log, am-update.ps1, Module.dll+0xRVA)
        if len(word) == 2 and word.isupper():
            # A two-letter acronym (GB, VM, CR, HH) is only worth a slot when the document leans on it (UI, CI): as a
            # hotword or prompt word it mostly adds a path for "a session" to become "AS" (measured with ASCII).
            if len(re.findall(r"(?<![\w])" + re.escape(word) + r"(?![\w])", text)) < 8:
                continue
        if re.search(r"-[A-Za-z]", word):
            continue  # a cmdlet, a flag or a slot name (Get-AppxPackage, Debug-B, API-error)
        if re.fullmatch(r"[A-Z]+\d{3,}", word):
            continue  # a compiler or HRESULT-like code (C2039)
        if word.isupper() and word.isalpha() and (len(word) > 6 or is_plain_english(word)):
            continue  # shouted English or a long all-caps name (ON, BEFORE, USERPROFILE)
        # SPEAKABLE names only (measured on the owner's recordings: identifiers nobody dictates filled Whisper's ~700-character
        # prompt and pushed out the glossary words a note DID hold):
        if word[0].islower() and any(ch.isupper() for ch in word[1:]):
            continue  # a lowerCamelCase code identifier (tabToken, kStateSettleMs, launchArgs)
        if len([p for p in _CASE_SPLIT.findall(word) if not p.isdigit()]) > 2:
            continue  # three or more word parts (WriteAllUtf8, SHGetKnownFolderPath, AgentmasterStateDir)
        if len(word) == 2 and not word.isupper():
            continue  # a two-character token (v9, m5, Xs)
        if not (any(ch.isupper() for ch in word) or any(ch.isdigit() for ch in word)):
            continue  # a lowercase word: not a name
        if not (is_distinctive(word) and is_hard(word)):
            continue
        counts[word] = counts.get(word, 0) + 1
    ranked = sorted(((w, c) for w, c in counts.items() if c >= min_count), key=lambda wc: (-wc[1], wc[0]))
    return ranked[:limit]


def parse_dictionary(text: str) -> list[tuple[str, list[str]]]:
    """Reads the owner's dictionary (Agentmaster's ``voiceDictionary`` setting): one entry per line, ``Term`` (boost it)
    or ``spoken form, other form => Term`` (FluidVoice's trigger -> replacement). Blank and ``#`` lines are skipped.
    VoiceVocabulary.h's ParseDictionary is the C++ twin.

    Args:
        text: The setting's text.

    Returns:
        (term, lowercase triggers) pairs, in order.
    """
    out: list[tuple[str, list[str]]] = []
    for raw in text.split("\n"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=>" in line:
            left, right = line.split("=>", 1)
            term = right.strip()
            triggers = [t.strip().lower() for t in left.split(",") if t.strip()]
        else:
            term, triggers = line, []
        if term:
            out.append((term, triggers))
    return out


def session_vocabulary(workdir: Path | None = None, title: str = "", claude_md_text: str | None = None,
                       include_builtin: bool = True, extra: Iterable[str] = (),
                       dictionary: Iterable[tuple[str, list[str]]] = ()) -> Vocabulary:
    """The vocabulary of one session: the built-in glossary, the working folder's name, the session title's distinctive
    words, and the distinctive words of the folder's CLAUDE.md (weight 2: the project's own words pull harder).

    Args:
        workdir: The session's working folder (its leaf name and CLAUDE.md are read).
        title: The session title (its distinctive words join at weight 2).
        claude_md_text: The CLAUDE.md text to use instead of reading ``workdir / "CLAUDE.md"`` (the evaluation pins a
            historical version, so the test does not grow easier as this feature's own docs land in CLAUDE.md).
        include_builtin: False for a context-only vocabulary (an ablation in the evaluation).
        extra: User terms (weight 3: the owner's own dictionary wins).
        dictionary: The owner's dictionary entries (``parse_dictionary``), weight 3 with their triggers.

    Returns:
        The vocabulary.
    """
    v = Vocabulary.builtin() if include_builtin else Vocabulary()
    if workdir is not None:
        leaf = Path(workdir).name
        if leaf and is_distinctive(leaf):
            v.add(leaf, 3.0, "context")
        if claude_md_text is None:
            md = Path(workdir) / "CLAUDE.md"
            claude_md_text = md.read_text(encoding="utf-8", errors="replace") if md.is_file() else ""
    if claude_md_text:
        for word, _count in extract_context_terms(claude_md_text):
            # Mined, not curated: a split of plain English words never respells into it (Term.respell_split).
            v.add(word, 2.0, "context", respell_split=False)
    for word in re.findall(r"[A-Za-z][A-Za-z0-9+#.-]+", title or ""):
        if is_distinctive(word) and is_hard(word):
            v.add(word, 2.0, "context", respell_split=False)
    for word in extra:
        v.add(word, 3.0, "user")
    for term, triggers in dictionary:
        v.add(term, 3.0, "user", True, triggers)
    return v
