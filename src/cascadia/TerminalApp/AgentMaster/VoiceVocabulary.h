// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — VoiceVocabulary: the session vocabulary of a dictated prompt (VOICE.md §5). Pure C++ (std only, no
// Windows, no WinRT), header-only, harness-tested against the POC's goldens (tests\voice\, tests_voice.cpp).
//
// A Telegram voice note becomes a prompt for ONE session, so the words to get right are that session's: its folder's
// name, the hard names of its CLAUDE.md, a built-in developer glossary and the owner's own dictionary. FluidVoice
// (altic-dev/FluidVoice, GPLv3 — studied, not copied) gets technical words right with three layers, and this header is
// the first two of them, rebuilt for Agentmaster:
//
//   1. the WORD BOOST — the vocabulary handed to the speech engine before it decodes: Whisper's initial prompt
//      (WhisperPrompt) or sherpa-onnx's per-stream hotwords for Parakeet (Hotwords);
//   2. the DICTIONARY PASS — a text pass after the engine (Apply) that respells a window of 1–4 words when it SQUASHES to
//      a distinctive term or one of its spoken forms ("Con PTY", "Agent Master", "Jason L" → ConPTY, Agentmaster, JSONL),
//      and never touches an everyday English word.
//
// (The third, the LLM correction, is optional and lives in the voice worker, VOICE.md §5.3.)
//
// ⚠ THE PYTHON POC IS THE REFERENCE. tools\voice-poc\vocab.py defines every rule here; this header is a line-for-line
// port, and tests\voice\make_goldens.py freezes the reference's answers (with a frozen word list, so the machine's
// dictionary cannot move them) into tests\voice\vocab_goldens.json, which tests_voice.cpp replays. A change of a rule is
// made in vocab.py first, the goldens regenerated, then ported here — the TranscriptTurns discipline.
//
// "Everyday English" is ONE question asked through a WordTest: the app answers it with Windows' own en-US spell checker
// (VoiceSpellWords.h — ISpellChecker, nothing to ship; the POC asks the same API through comtypes), the tests with the
// frozen list. Python's regular expressions are emulated by hand (no std::wregex: MSVC's is slow enough to stall a
// thread on a long document, memory "msvc-wregex-cost"), each emulation documented with the pattern it replaces.

#pragma once

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Agentmaster::Voice
{
    /// <summary>
    /// The dictionary lookup: does the en-US dictionary know this EXACT spelling ("english" no, "English" yes; "pty" no)?
    /// The app answers with the Windows spell checker (case-sensitive like the dictionary itself), tests with a frozen
    /// list. It is only ever asked about a non-empty, all-alphabetic spelling; PlainWords decides which spellings to ask
    /// (the lowercase one, and the title-case one for a word that is not all capitals) and caches every answer.
    /// </summary>
    using WordTest = std::function<bool(std::wstring_view spelling)>;

    // ===== character classes (Python's, for the text this header reads: ASCII-first, Unicode-aware where Python is) ====

    namespace detail
    {
        /// <summary>Python's `[A-Z]`: an ASCII capital.</summary>
        /// <param name="c">The character.</param>
        /// <returns>True for 'A'..'Z'.</returns>
        inline bool IsUpperAZ(wchar_t c) noexcept
        {
            return c >= L'A' && c <= L'Z';
        }
        /// <summary>Python's `[a-z]`.</summary>
        /// <param name="c">The character.</param>
        /// <returns>True for 'a'..'z'.</returns>
        inline bool IsLowerAZ(wchar_t c) noexcept
        {
            return c >= L'a' && c <= L'z';
        }
        /// <summary>Python's ASCII `[0-9]` (every pattern here that needs a digit is about ASCII text).</summary>
        /// <param name="c">The character.</param>
        /// <returns>True for '0'..'9'.</returns>
        inline bool IsDigit09(wchar_t c) noexcept
        {
            return c >= L'0' && c <= L'9';
        }
        /// <summary>Python's `str.isalnum` for one character (Unicode letters and digits).</summary>
        /// <param name="c">The character.</param>
        /// <returns>True for a letter or a digit.</returns>
        inline bool IsAlnum(wchar_t c) noexcept
        {
            return std::iswalnum(static_cast<wint_t>(c)) != 0;
        }
        /// <summary>Python's `str.isalpha` for one character.</summary>
        /// <param name="c">The character.</param>
        /// <returns>True for a letter.</returns>
        inline bool IsAlpha(wchar_t c) noexcept
        {
            return std::iswalpha(static_cast<wint_t>(c)) != 0;
        }
        /// <summary>Python's `\w` (a letter, a digit or the underscore).</summary>
        /// <param name="c">The character.</param>
        /// <returns>True for a word character.</returns>
        inline bool IsWordChar(wchar_t c) noexcept
        {
            return c == L'_' || IsAlnum(c);
        }
        /// <summary>Python's `str.isspace` for one character.</summary>
        /// <param name="c">The character.</param>
        /// <returns>True for white space.</returns>
        inline bool IsSpace(wchar_t c) noexcept
        {
            return std::iswspace(static_cast<wint_t>(c)) != 0;
        }
        /// <summary>Python's `str.lower` (per character; the text here is ASCII-first, and towlower covers the BMP).</summary>
        /// <param name="s">The text.</param>
        /// <returns>The text in lowercase.</returns>
        inline std::wstring Lower(std::wstring_view s)
        {
            std::wstring out{ s };
            for (auto& c : out)
            {
                c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
            }
            return out;
        }
        /// <summary>Python's `str.isupper`: at least one cased character, and none in lowercase.</summary>
        /// <param name="s">The text.</param>
        /// <returns>True for "PCM", "C++", "UTF-8"; false for "8", "Con".</returns>
        inline bool PyIsUpper(std::wstring_view s) noexcept
        {
            bool cased = false;
            for (const wchar_t c : s)
            {
                if (std::iswlower(static_cast<wint_t>(c)))
                {
                    return false;
                }
                if (std::iswupper(static_cast<wint_t>(c)))
                {
                    cased = true;
                }
            }
            return cased;
        }
        /// <summary>Python's `str.islower`: at least one cased character, and none in uppercase.</summary>
        /// <param name="s">The text.</param>
        /// <returns>True for "named pipe"; false for "Telegram", "42".</returns>
        inline bool PyIsLower(std::wstring_view s) noexcept
        {
            bool cased = false;
            for (const wchar_t c : s)
            {
                if (std::iswupper(static_cast<wint_t>(c)))
                {
                    return false;
                }
                if (std::iswlower(static_cast<wint_t>(c)))
                {
                    cased = true;
                }
            }
            return cased;
        }
        /// <summary>Python's `str.isalpha`: non-empty and every character a letter.</summary>
        /// <param name="s">The text.</param>
        /// <returns>True for "Agentmaster"; false for "", "k3s", "VS Code".</returns>
        inline bool PyIsAlpha(std::wstring_view s) noexcept
        {
            return !s.empty() && std::all_of(s.begin(), s.end(), [](wchar_t c) { return IsAlpha(c); });
        }
        /// <summary>Whether any character of `s` is in uppercase (Python's `any(ch.isupper() for ch in s)`).</summary>
        /// <param name="s">The text.</param>
        /// <returns>True when one is.</returns>
        inline bool AnyUpper(std::wstring_view s) noexcept
        {
            return std::any_of(s.begin(), s.end(), [](wchar_t c) { return std::iswupper(static_cast<wint_t>(c)) != 0; });
        }
        /// <summary>Whether any character of `s` is a digit (Python's `any(ch.isdigit() for ch in s)`).</summary>
        /// <param name="s">The text.</param>
        /// <returns>True when one is.</returns>
        inline bool AnyDigit(std::wstring_view s) noexcept
        {
            return std::any_of(s.begin(), s.end(), [](wchar_t c) { return std::iswdigit(static_cast<wint_t>(c)) != 0; });
        }
        /// <summary>Python's `str.split()`: the runs of non-space characters.</summary>
        /// <param name="s">The text.</param>
        /// <returns>The words (no empty one).</returns>
        inline std::vector<std::wstring> SplitWords(std::wstring_view s)
        {
            std::vector<std::wstring> out;
            size_t i = 0;
            while (i < s.size())
            {
                while (i < s.size() && IsSpace(s[i]))
                {
                    ++i;
                }
                const size_t start = i;
                while (i < s.size() && !IsSpace(s[i]))
                {
                    ++i;
                }
                if (i > start)
                {
                    out.emplace_back(s.substr(start, i - start));
                }
            }
            return out;
        }
        /// <summary>Python's `str.strip()` (white space at both ends).</summary>
        /// <param name="s">The text.</param>
        /// <returns>The trimmed text.</returns>
        inline std::wstring Strip(std::wstring_view s)
        {
            size_t a = 0;
            size_t b = s.size();
            while (a < b && IsSpace(s[a]))
            {
                ++a;
            }
            while (b > a && IsSpace(s[b - 1]))
            {
                --b;
            }
            return std::wstring{ s.substr(a, b - a) };
        }
        /// <summary>Python's `str.rstrip(chars)` / `str.lstrip(chars)`.</summary>
        /// <param name="s">The text.</param>
        /// <param name="chars">The characters to drop.</param>
        /// <param name="right">True for rstrip, false for lstrip.</param>
        /// <returns>The trimmed text.</returns>
        inline std::wstring StripChars(std::wstring_view s, std::wstring_view chars, bool right)
        {
            size_t a = 0;
            size_t b = s.size();
            if (right)
            {
                while (b > a && chars.find(s[b - 1]) != std::wstring_view::npos)
                {
                    --b;
                }
            }
            else
            {
                while (a < b && chars.find(s[a]) != std::wstring_view::npos)
                {
                    ++a;
                }
            }
            return std::wstring{ s.substr(a, b - a) };
        }
        /// <summary>Python's `" ".join(word)`: a word spelled letter by letter ("pty" → "p t y").</summary>
        /// <param name="word">The word.</param>
        /// <returns>Its characters separated by single spaces.</returns>
        inline std::wstring SpellOut(std::wstring_view word)
        {
            std::wstring out;
            for (size_t i = 0; i < word.size(); ++i)
            {
                if (i)
                {
                    out += L' ';
                }
                out += word[i];
            }
            return out;
        }
        /// <summary>Joins parts with a separator (Python's `sep.join(parts)`).</summary>
        /// <param name="parts">The parts.</param>
        /// <param name="sep">The separator.</param>
        /// <returns>The joined text.</returns>
        inline std::wstring Join(const std::vector<std::wstring>& parts, std::wstring_view sep)
        {
            std::wstring out;
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (i)
                {
                    out += sep;
                }
                out += parts[i];
            }
            return out;
        }
    }

    // ===== the glossary =================================================================================================

    /// <summary>
    /// The built-in developer glossary: words a developer says that a general speech model writes wrong or in the wrong case.
    /// ⚠ The same list, in the same order, as vocab.py's BUILTIN_GLOSSARY (the goldens catch a drift). ORDER IS PRIORITY:
    /// Whisper's prompt keeps ~100 terms and, when the session's own words take room, cuts the glossary from its END —
    /// measured on the owner's recordings (2026-10-27), with formats first the AI / speech group never reached the prompt.
    /// </summary>
    inline const std::vector<std::wstring>& BuiltinGlossary()
    {
        static const std::vector<std::wstring> k{
            // AI and speech
            L"Claude", L"Claude Code", L"Anthropic", L"OpenAI", L"GPT", L"LLM", L"MCP", L"Whisper", L"Parakeet", L"ONNX", L"PyTorch",
            L"TensorFlow", L"Hugging Face", L"LoRA", L"RAG", L"embeddings", L"tokenizer", L"Sonnet", L"Opus", L"Haiku", L"Codex",
            L"Gemini", L"Ollama", L"llama.cpp",
            // Audio / media
            L"PCM", L"WAV", L"MP3", L"AAC", L"Opus", L"Ogg", L"FLAC", L"FFmpeg", L"codec", L"kHz", L"VAD",
            // Hardware and OS
            L"CPU", L"GPU", L"RAM", L"SSD", L"NVMe", L"CUDA", L"cuDNN", L"RTX", L"NVIDIA", L"x64", L"ARM64", L"Windows", L"Win32", L"WinRT",
            L"ConPTY", L"PowerShell", L"WSL", L"MSIX", L"AppX", L"NuGet", L"vcpkg", L"MSBuild", L"CMake", L"XAML", L"COM", L"DLL", L"PDB",
            L"PID", L"TTY", L"PTY", L"VT100", L"ANSI",
            // Data formats, encodings, protocols
            L"JSON", L"JSONL", L"YAML", L"TOML", L"XML", L"CSV", L"HTML", L"CSS", L"Markdown", L"HTTP", L"HTTPS", L"REST", L"gRPC",
            L"WebSocket", L"OAuth", L"JWT", L"UTF-8", L"UTF-16", L"ASCII", L"Base64", L"SHA-256", L"TLS", L"SSH", L"DNS", L"TCP", L"UDP",
            L"IPv4", L"IPv6", L"URL", L"URI", L"UUID", L"GUID", L"regex", L"stdin", L"stdout", L"stderr",
            // Languages and runtimes
            L"C++", L"C#", L".NET", L"Python", L"TypeScript", L"JavaScript", L"Node.js", L"Rust", L"Golang", L"Kotlin", L"SQL", L"Bash",
            L"WebAssembly",
            // Tools and services
            L"Git", L"GitHub", L"GitLab", L"Docker", L"Kubernetes", L"kubectl", L"k3s", L"Helm", L"npm", L"pnpm", L"Yarn", L"Vite",
            L"Webpack", L"pytest", L"Jest", L"VS Code", L"Visual Studio", L"WinDbg", L"Telegram", L"Slack", L"Discord", L"WhatsApp",
            L"Cloudflare", L"Traefik", L"Proxmox",
            // Databases and queues
            L"PostgreSQL", L"Postgres", L"MySQL", L"SQLite", L"DuckDB", L"Redis", L"MongoDB", L"Kafka", L"RabbitMQ",
            // Generic engineering words a model splits or mis-cases
            L"API", L"SDK", L"CLI", L"GUI", L"UI", L"UX", L"IDE", L"CI", L"PR", L"repo", L"async", L"await", L"mutex", L"nullptr", L"enum",
            L"struct", L"refactor", L"middleware", L"localhost", L"namespace", L"lambda", L"boolean", L"JSON schema", L"OpenAPI",
        };
        return k;
    }

    /// <summary>
    /// The CURATED TRIGGERS: how a term is said (or misheard) when a model does not write it right — FluidVoice's built-in
    /// dictionary works the same way ("clawed" → "Claude"). A trigger is explicit, so it may rewrite even an everyday word
    /// ("jason" → JSON). ⚠ Keyed by the EXACT spelling of the glossary term (vocab.py's dict lookup is case-sensitive); the
    /// same entries as vocab.py's ACRONYM_SAYINGS. Never add an everyday word whose ordinary meaning is common in prompts.
    /// </summary>
    inline const std::map<std::wstring, std::vector<std::wstring>>& CuratedTriggers()
    {
        static const std::map<std::wstring, std::vector<std::wstring>> k{
            { L"Claude", { L"claud", L"clawed" } },
            { L"Claude Code", { L"cloud code", L"clawed code", L"claud code" } },
            { L"JSON", { L"jason", L"j son" } },
            { L"JSONL", { L"jason l", L"json l", L"jason el", L"json el", L"jason line", L"json lines" } },
            { L"SQL", { L"sequel" } },
            { L"SQLite", { L"sequel lite", L"sequel light", L"sql lite", L"sql light", L"s q lite" } },
            { L"MySQL", { L"my sequel", L"my sql" } },
            { L"PostgreSQL", { L"postgres sequel", L"postgres q l", L"postgre sql" } },
            { L"GUI", { L"gooey" } },
            { L"YAML", { L"yamel", L"yammel" } },
            { L"TOML", { L"tom l", L"tommel" } },
            { L"ASCII", { L"askey", L"ask e" } },
            { L"NuGet", { L"new get" } },
            { L"vcpkg", { L"vc package", L"v c package", L"vcpackage" } },
            { L"OAuth", { L"o auth" } },
            { L"regex", { L"reg ex" } },
            { L"Ogg", { L"og" } },
            { L"kubectl", { L"kube control", L"kube c t l", L"kube cuddle", L"cube control" } },
            { L"k3s", { L"k three s", L"k3 s", L"k 3 s" } },
            { L"nullptr", { L"null pointer", L"null ptr" } },
            { L"stdin", { L"standard in", L"std in", L"s t d in" } },
            { L"stdout", { L"standard out", L"std out" } },
            { L"stderr", { L"standard error", L"std err" } },
            { L"ConPTY", { L"con pty", L"con p t y", L"compty", L"con petty" } },
            { L"PTY", { L"pity", L"p t y" } },
            { L"WinRT", { L"win rt", L"win r t", L"windows runtime" } },
            { L"Win32", { L"win 32", L"win thirty two" } },
            { L"CUDA", { L"cooda", L"kuda" } },
            { L"cuDNN", { L"cu dnn", L"cuda nn", L"cuda dnn" } },
            { L"PyTorch", { L"pie torch", L"py torch" } },
            { L"Node.js", { L"node js", L"node j s", L"nodejs" } },
            { L"C++", { L"c plus plus", L"cpp", L"c p p" } },
            { L"C#", { L"c sharp", L"see sharp" } },
            { L".NET", { L"dot net" } },
            { L"UTF-8", { L"utf 8", L"u t f 8", L"utf eight", L"u t f eight" } },
            { L"SHA-256", { L"sha 256", L"shah 256", L"sha two fifty six" } },
            { L"x64", { L"x 64", L"x sixty four" } },
            { L"ARM64", { L"arm 64", L"arm sixty four" } },
            { L"llama.cpp", { L"llama cpp", L"llama c p p", L"llama dot cpp" } },
            { L"MSIX", { L"m s i x", L"msi x" } },
            { L"XAML", { L"zamel", L"zammel", L"xamel" } },
            { L"DuckDB", { L"duck db", L"duck d b" } },
            { L"gRPC", { L"g rpc", L"g r p c" } },
            { L"VS Code", { L"vs code", L"v s code", L"vscode" } },
        };
        return k;
    }

    // ===== the word test, cached ========================================================================================

    /// <summary>
    /// vocab.py's is_plain_english: whether a word is everyday English — one the dictionary pass must never rewrite, and
    /// one that makes a term not "hard". An ALL-CAPITAL word of two or more letters is an acronym, a word only when its
    /// LOWERCASE spelling is known ("ON", "AND" are words; "PTY", "DB", "UI" are not, though "Pty" — as in "Pty Ltd" — is
    /// in the dictionary); any other word is a word when its lowercase OR title-case spelling is known (proper nouns are
    /// in the dictionary capitalized only: "English", "Claude"). One instance belongs to one thread: the cache is not
    /// synchronized (each voice job builds its own vocabulary).
    /// </summary>
    class PlainWords
    {
    public:
        /// <summary>Wraps a dictionary lookup.</summary>
        /// <param name="test">The lookup; an EMPTY one knows every spelling — the safe answer: never rewrite.</param>
        explicit PlainWords(WordTest test) :
            _test(std::move(test))
        {
        }

        /// <summary>Whether `word` is an everyday English word (trimmed; a word with a digit or a symbol is not).</summary>
        /// <param name="word">The candidate, in the casing it was written.</param>
        /// <returns>The answer.</returns>
        /// <exception cref="std::bad_alloc">If the cache cannot grow.</exception>
        bool operator()(std::wstring_view word) const
        {
            const std::wstring w = detail::Strip(word);
            if (!detail::PyIsAlpha(w))
            {
                return false;
            }
            const std::wstring low = detail::Lower(w);
            if (detail::PyIsUpper(w) && w.size() >= 2)
            {
                return Known(low);
            }
            std::wstring title = low;
            title[0] = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(title[0])));
            return Known(low) || Known(title);
        }

    private:
        /// <summary>The cached lookup of one exact spelling.</summary>
        /// <param name="spelling">The spelling.</param>
        /// <returns>Whether the dictionary knows it; a lookup that throws reads as known (never a wrong rewrite).</returns>
        /// <exception cref="std::bad_alloc">If the cache cannot grow.</exception>
        bool Known(const std::wstring& spelling) const
        {
            if (const auto it = _cache.find(spelling); it != _cache.end())
            {
                return it->second;
            }
            bool known = true;
            if (_test)
            {
                try
                {
                    known = _test(spelling);
                }
                catch (...)
                {
                    known = true;
                }
            }
            _cache.emplace(spelling, known);
            return known;
        }

        WordTest _test;
        mutable std::unordered_map<std::wstring, bool> _cache;
    };

    // ===== the rules ===================================================================================================

    /// <summary>
    /// vocab.py's squash: the dictionary pass's comparison key — lowercase, letters and digits only ('+' and '#' kept, so
    /// "C++" and "C#" keep their identity).
    /// </summary>
    /// <param name="text">A term, an alias or a window of transcript words.</param>
    /// <returns>The key ("Con-PTY" → "conpty").</returns>
    /// <exception cref="std::bad_alloc">If the key cannot be allocated.</exception>
    inline std::wstring Squash(std::wstring_view text)
    {
        std::wstring out;
        out.reserve(text.size());
        for (const wchar_t c : text)
        {
            const wchar_t lc = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
            if (detail::IsAlnum(lc) || lc == L'+' || lc == L'#')
            {
                out += lc;
            }
        }
        return out;
    }

    /// <summary>
    /// vocab.py's _CASE_SPLIT.findall — the pattern `[A-Z]+(?![a-z])|[A-Z]?[a-z]+|\d+|[^\W\d_]+`, emulated: "ConPTY" →
    /// Con, PTY; "DuckDB" → Duck, DB; "k3s" → k, 3, s; "UTF-8" → UTF, 8. The first alternative backtracks: a run of capitals
    /// followed by a lowercase letter gives back its last capital ("PDBs" → PD, Bs), exactly as the regex does.
    /// </summary>
    /// <param name="s">A word.</param>
    /// <returns>Its parts, in order.</returns>
    /// <exception cref="std::bad_alloc">If the parts cannot be allocated.</exception>
    inline std::vector<std::wstring> CaseSplit(std::wstring_view s)
    {
        using namespace detail;
        std::vector<std::wstring> out;
        size_t i = 0;
        while (i < s.size())
        {
            const wchar_t c = s[i];
            if (IsUpperAZ(c))
            {
                size_t j = i;
                while (j < s.size() && IsUpperAZ(s[j]))
                {
                    ++j;
                }
                // Alternative 1, greedy: the whole run when no lowercase letter follows it; else one capital less (that
                // shorter match is followed by a capital, never by [a-z]) when the run has two or more.
                if (j >= s.size() || !IsLowerAZ(s[j]))
                {
                    out.emplace_back(s.substr(i, j - i));
                    i = j;
                    continue;
                }
                if (j - i >= 2)
                {
                    out.emplace_back(s.substr(i, j - 1 - i));
                    i = j - 1;
                    continue;
                }
                // Alternative 2: one capital and its lowercase run.
                size_t k = i + 1;
                while (k < s.size() && IsLowerAZ(s[k]))
                {
                    ++k;
                }
                out.emplace_back(s.substr(i, k - i));
                i = k;
                continue;
            }
            if (IsLowerAZ(c))
            {
                size_t k = i;
                while (k < s.size() && IsLowerAZ(s[k]))
                {
                    ++k;
                }
                out.emplace_back(s.substr(i, k - i));
                i = k;
                continue;
            }
            if (std::iswdigit(static_cast<wint_t>(c)))
            {
                size_t k = i;
                while (k < s.size() && std::iswdigit(static_cast<wint_t>(s[k])))
                {
                    ++k;
                }
                out.emplace_back(s.substr(i, k - i));
                i = k;
                continue;
            }
            if (IsAlpha(c))
            {
                // Alternative 4 (`[^\W\d_]+`): a run of letters of any script — it reaches here only from a non-ASCII
                // letter, and like the regex it then also takes the ASCII letters that follow.
                size_t k = i;
                while (k < s.size() && IsAlpha(s[k]) && !std::iswdigit(static_cast<wint_t>(s[k])))
                {
                    ++k;
                }
                out.emplace_back(s.substr(i, k - i));
                i = k;
                continue;
            }
            ++i; // a separator ('-', '.', '+', ...) belongs to no part
        }
        return out;
    }

    /// <summary>
    /// vocab.py's spoken_aliases: the ways a speech model may write `term` after hearing it, generated from the spelling —
    /// its case-split parts ("con pty"), acronym parts spelled out ("con p t y"), and for a long one-word name every split
    /// into two parts of 3+ letters ("agent master"). The curated triggers are NOT included (CuratedTriggers).
    /// </summary>
    /// <param name="term">The canonical spelling.</param>
    /// <returns>Lowercase aliases, the lowercase term included, sorted (a set's order; the uses only test membership).</returns>
    /// <exception cref="std::bad_alloc">If the aliases cannot be allocated.</exception>
    inline std::set<std::wstring> SpokenAliases(std::wstring_view term)
    {
        using namespace detail;
        std::set<std::wstring> aliases{ Lower(term) };
        const auto parts = CaseSplit(term);
        if (parts.size() > 1)
        {
            std::vector<std::wstring> lowered;
            std::vector<std::wstring> spelled;
            for (const auto& p : parts)
            {
                lowered.push_back(Lower(p));
                // Each all-capital part of 2–5 letters is an acronym a speaker may spell; a longer one is a word.
                spelled.push_back(PyIsUpper(p) && p.size() >= 2 && p.size() <= 5 ? SpellOut(Lower(p)) : Lower(p));
            }
            aliases.insert(Join(lowered, L" "));
            aliases.insert(Join(spelled, L" "));
        }
        else if (PyIsUpper(term) && term.size() >= 2 && term.size() <= 6)
        {
            aliases.insert(SpellOut(Lower(term)));
        }
        else if (PyIsAlpha(term) && term.size() >= 7 && term.find(L' ') == std::wstring_view::npos)
        {
            const std::wstring low = Lower(term);
            for (size_t i = 3; i + 2 < low.size(); ++i)
            {
                aliases.insert(low.substr(0, i) + L" " + low.substr(i));
            }
        }
        std::set<std::wstring> out;
        for (const auto& a : aliases)
        {
            if (!Strip(a).empty())
            {
                out.insert(a);
            }
        }
        return out;
    }

    /// <summary>
    /// vocab.py's is_distinctive: whether the dictionary pass may rewrite text INTO this spelling — an inner capital
    /// (ConPTY), an acronym (PCM), a digit or symbol (k3s, C++, Node.js), or a word that is not plain English (Agentmaster).
    /// A lowercase phrase ("named pipe") or a capitalized everyday word ("Telegram") is not.
    /// </summary>
    /// <param name="term">The canonical spelling.</param>
    /// <param name="plain">The plain-English test.</param>
    /// <returns>True when respelling a matching window into `term` is safe.</returns>
    /// <exception cref="std::bad_alloc">If a word list cannot be allocated.</exception>
    inline bool IsDistinctive(std::wstring_view term, const PlainWords& plain)
    {
        using namespace detail;
        if (AnyDigit(term) || term.find_first_of(L"+#.") != std::wstring_view::npos)
        {
            return true;
        }
        const auto words = SplitWords(term);
        for (const auto& w : words)
        {
            if (w.size() > 1 && AnyUpper(std::wstring_view{ w }.substr(1)))
            {
                return true; // an inner capital or an acronym
            }
        }
        if (PyIsLower(term))
        {
            return false;
        }
        return std::any_of(words.begin(), words.end(), [&](const std::wstring& w) { return !plain(w); });
    }

    /// <summary>
    /// vocab.py's is_hard: whether a speech model is likely to get `term` wrong — a digit or a symbol, or a case-split part
    /// that is not plain English (ConPTY's PTY, JSONL, Agentmaster). Only hard terms are worth a hotword or a context slot.
    /// </summary>
    /// <param name="term">The canonical spelling.</param>
    /// <param name="plain">The plain-English test.</param>
    /// <returns>True for a term worth boosting.</returns>
    /// <exception cref="std::bad_alloc">If a part list cannot be allocated.</exception>
    inline bool IsHard(std::wstring_view term, const PlainWords& plain)
    {
        using namespace detail;
        if (AnyDigit(term) || term.find_first_of(L"+#") != std::wstring_view::npos)
        {
            return true;
        }
        std::wstring spaced{ term };
        std::replace(spaced.begin(), spaced.end(), L'.', L' ');
        for (const auto& w : SplitWords(spaced))
        {
            for (const auto& p : CaseSplit(w))
            {
                if (!p.empty() && !plain(p))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // ===== the vocabulary ===============================================================================================

    /// <summary>One vocabulary entry (vocab.py's Term).</summary>
    struct VoiceTerm
    {
        std::wstring text; // the canonical spelling
        std::set<std::wstring> aliases; // generated from the spelling (SpokenAliases); guarded in Apply
        std::vector<std::wstring> triggers; // curated (CuratedTriggers or the user's); never guarded
        double weight{ 1.0 }; // relative priority (prompt order, hotword score): builtin 1, context 2, the owner's 3
        std::wstring source{ L"builtin" }; // "builtin" | "context" | "user"
        // Whether a window of several PLAIN English words may be respelled into this term ("agent master" → Agentmaster):
        // true for curated terms (the glossary, the owner's dictionary, the folder's own name), false for a word mined
        // from CLAUDE.md ("session state" is far more often the phrase than the type SessionState).
        bool respellSplit{ true };
    };

    /// <summary>One respelling the dictionary pass made.</summary>
    struct VoiceReplacement
    {
        std::wstring from; // the words as the engine wrote them (without the punctuation around them)
        std::wstring to; // the canonical term
    };

    /// <summary>
    /// The terms one transcription is biased toward, and their three uses (vocab.py's Vocabulary). Built per voice note
    /// (cheap: a few hundred terms), owned by the thread that builds it.
    /// </summary>
    class VoiceVocabulary
    {
    public:
        /// <summary>An empty vocabulary over a word test.</summary>
        /// <param name="test">The plain-English test (the Windows spell checker in the app).</param>
        explicit VoiceVocabulary(WordTest test) :
            _plain(std::move(test))
        {
        }

        /// <summary>The built-in glossary alone (weight 1, respellable).</summary>
        /// <param name="test">The plain-English test.</param>
        /// <returns>A new vocabulary.</returns>
        /// <exception cref="std::bad_alloc">If the terms cannot be allocated.</exception>
        static VoiceVocabulary Builtin(WordTest test)
        {
            VoiceVocabulary v{ std::move(test) };
            for (const auto& t : BuiltinGlossary())
            {
                v.Add(t, 1.0, L"builtin", true);
            }
            return v;
        }

        /// <summary>
        /// Adds a term, or raises the weight of one already present (case-insensitive identity; the higher weight wins and a
        /// curated re-add makes a mined term respellable).
        /// </summary>
        /// <param name="text">The canonical spelling (trimmed; an empty one is ignored).</param>
        /// <param name="weight">Its priority.</param>
        /// <param name="source">Where it came from (reports, the cog).</param>
        /// <param name="respellSplit">VoiceTerm::respellSplit.</param>
        /// <param name="extraTriggers">The owner's own spoken forms for it (a dictionary line "jay son el => JSONL").</param>
        /// <exception cref="std::bad_alloc">If the term cannot be stored.</exception>
        void Add(std::wstring_view text, double weight, std::wstring_view source, bool respellSplit = true, const std::vector<std::wstring>& extraTriggers = {})
        {
            const std::wstring t = detail::Strip(text);
            if (t.empty())
            {
                return;
            }
            const std::wstring low = detail::Lower(t);
            for (auto& term : _terms)
            {
                if (detail::Lower(term.text) == low)
                {
                    if (weight > term.weight)
                    {
                        term.weight = weight;
                        term.source = std::wstring{ source };
                    }
                    term.respellSplit = term.respellSplit || respellSplit;
                    for (const auto& x : extraTriggers)
                    {
                        if (std::find(term.triggers.begin(), term.triggers.end(), x) == term.triggers.end())
                        {
                            term.triggers.push_back(x);
                        }
                    }
                    return;
                }
            }
            VoiceTerm term;
            term.text = t;
            term.aliases = SpokenAliases(t);
            if (const auto it = CuratedTriggers().find(t); it != CuratedTriggers().end())
            {
                term.triggers = it->second;
            }
            for (const auto& x : extraTriggers)
            {
                term.triggers.push_back(x);
            }
            term.weight = weight;
            term.source = std::wstring{ source };
            term.respellSplit = respellSplit;
            _terms.push_back(std::move(term));
        }

        /// <summary>The terms in the order they were added.</summary>
        /// <returns>The terms.</returns>
        const std::vector<VoiceTerm>& Terms() const noexcept
        {
            return _terms;
        }

        /// <summary>The plain-English test this vocabulary asks (the context extraction shares its cache).</summary>
        /// <returns>The cached test.</returns>
        const PlainWords& Plain() const noexcept
        {
            return _plain;
        }

        /// <summary>
        /// The terms, highest weight first, stable within a weight (the glossary's own order), so every use that truncates
        /// (the prompt's budget, the hotword limit) drops the generic glossary before the session's own words.
        /// </summary>
        /// <returns>Pointers into Terms(), valid until the next Add.</returns>
        /// <exception cref="std::bad_alloc">If the list cannot be allocated.</exception>
        std::vector<const VoiceTerm*> Ranked() const
        {
            std::vector<const VoiceTerm*> out;
            out.reserve(_terms.size());
            for (const auto& t : _terms)
            {
                out.push_back(&t);
            }
            std::stable_sort(out.begin(), out.end(), [](const VoiceTerm* a, const VoiceTerm* b) { return a->weight > b->weight; });
            return out;
        }

        /// <summary>
        /// vocab.py's glossary_line: the glossary the CORRECTION layer gets (VoiceCorrect.h) — the terms, highest weight first,
        /// comma-joined. Unlike Whisper's prompt there is room for every term a session has (a model reads 160 terms easily).
        /// </summary>
        /// <param name="maxTerms">The most terms sent.</param>
        /// <returns>"Agentmaster, ConPTY, …" ("" for an empty vocabulary).</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring GlossaryLine(size_t maxTerms = 160) const
        {
            std::wstring out;
            size_t n = 0;
            for (const VoiceTerm* t : Ranked())
            {
                if (n == maxTerms)
                {
                    break;
                }
                if (n++)
                {
                    out += L", ";
                }
                out += t->text;
            }
            return out;
        }

        /// <summary>
        /// vocab.py's whisper_prompt: Whisper's initial prompt, a short glossary line. Whisper keeps only the LAST ~224 tokens
        /// of a prompt and weighs its end most, so the highest-weight terms go last and the line is cut from its start.
        /// </summary>
        /// <param name="maxChars">The character budget (~700 stays inside 224 tokens for this kind of text).</param>
        /// <returns>The prompt, or "" for an empty vocabulary.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring WhisperPrompt(size_t maxChars = 700) const
        {
            if (_terms.empty())
            {
                return {};
            }
            auto ordered = Ranked();
            std::reverse(ordered.begin(), ordered.end()); // lowest weight first, highest last
            const std::wstring prefix = L"A developer dictates a prompt to Claude Code. Glossary: ";
            std::wstring body;
            for (size_t i = 0; i < ordered.size(); ++i)
            {
                if (i)
                {
                    body += L", ";
                }
                body += ordered[i]->text;
            }
            body += L".";
            const size_t budget = maxChars > prefix.size() ? maxChars - prefix.size() : 0;
            if (body.size() > budget)
            {
                body = body.substr(body.size() - budget);
                // Cut at the first comma so no term is half-shown (Python: body[body.find(",") + 1:].lstrip()).
                if (const size_t comma = body.find(L','); comma != std::wstring::npos)
                {
                    body = body.substr(comma + 1);
                    const size_t first = body.find_first_not_of(L" \t\r\n");
                    body = first == std::wstring::npos ? std::wstring{} : body.substr(first);
                }
            }
            return prefix + body;
        }

        /// <summary>
        /// vocab.py's hotwords: sherpa-onnx's per-stream hotwords for a NeMo transducer — '/'-separated phrases, each with a
        /// score that grows gently with the weight (2.0 / 2.5 / 3.0 for weights 1 / 2 / 3). Only HARD terms are sent: each
        /// extra hotword is one more path the beam can be pulled into (measured: ~480 of them made Parakeet worse than none).
        /// </summary>
        /// <param name="baseScore">The score of a weight-1 term.</param>
        /// <param name="limit">At most this many phrases, highest weight first.</param>
        /// <returns>"Agentmaster :3.0/ConPTY :2.5/…", or "" when no term is hard.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring Hotwords(double baseScore = 2.0, size_t limit = 96) const
        {
            std::vector<std::wstring> phrases;
            for (const VoiceTerm* t : Ranked())
            {
                if (phrases.size() >= limit)
                {
                    break;
                }
                std::wstring phrase = t->text;
                std::replace(phrase.begin(), phrase.end(), L'/', L' ');
                phrase = detail::Strip(phrase);
                if (!phrase.empty() && IsHard(phrase, _plain))
                {
                    wchar_t score[32]{};
                    std::swprintf(score, 32, L"%.1f", baseScore * (1.0 + 0.25 * (t->weight - 1.0)));
                    phrases.push_back(phrase + L" :" + score);
                }
            }
            return detail::Join(phrases, L"/");
        }

        /// <summary>
        /// vocab.py's apply — THE DICTIONARY PASS: rewrites windows of 1–4 words that squash to a distinctive term, one of its
        /// generated aliases or one of its curated triggers. Longest window first, left to right; a window keeps the punctuation
        /// around it ("(con PTY," → "(ConPTY,"); a window that already IS the canonical spelling is left alone (idempotent).
        /// A GENERATED alias never rewrites a single everyday word ("on" never becomes "ON") and never respells several
        /// everyday words into a mined term ("session state" stays); a curated trigger may ("clawed" → "Claude").
        /// </summary>
        /// <param name="text">The transcript.</param>
        /// <param name="made">Receives the respellings in order (may be null).</param>
        /// <returns>The rewritten transcript.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        std::wstring Apply(std::wstring_view text, std::vector<VoiceReplacement>* made = nullptr) const
        {
            using namespace detail;
            // squashed key → (term, explicit). A curated trigger is explicit and wins a key over a generated alias; within one
            // kind, the higher-weight term keeps a shared key (ranked order).
            std::unordered_map<std::wstring, std::pair<const VoiceTerm*, bool>> table;
            for (const VoiceTerm* t : Ranked())
            {
                // A curated trigger is a decision someone made ("clawed" → Claude), so it applies even when the term itself
                // is an everyday word the generated aliases must leave alone (Claude is a known first name).
                for (const auto& tr : t->triggers)
                {
                    const std::wstring key = Squash(tr);
                    if (key.empty())
                    {
                        continue;
                    }
                    const auto it = table.find(key);
                    if (it == table.end() || !it->second.second)
                    {
                        table[key] = { t, true };
                    }
                }
                if (!IsDistinctive(t->text, _plain))
                {
                    continue;
                }
                std::set<std::wstring> keys{ Squash(t->text) };
                for (const auto& a : t->aliases)
                {
                    keys.insert(Squash(a));
                }
                for (const auto& key : keys)
                {
                    if (!key.empty() && table.find(key) == table.end())
                    {
                        table[key] = { t, false };
                    }
                }
            }
            if (table.empty())
            {
                return std::wstring{ text };
            }
            // Python: re.findall(r"\S+|\s+", text) — alternating runs, so the text is rebuilt exactly around a rewrite.
            std::vector<std::wstring> tokens;
            for (size_t i = 0; i < text.size();)
            {
                const bool space = IsSpace(text[i]);
                size_t j = i;
                while (j < text.size() && IsSpace(text[j]) == space)
                {
                    ++j;
                }
                tokens.emplace_back(text.substr(i, j - i));
                i = j;
            }
            std::vector<size_t> words;
            for (size_t i = 0; i < tokens.size(); ++i)
            {
                if (!IsSpace(tokens[i][0]))
                {
                    words.push_back(i);
                }
            }
            std::vector<std::wstring> out = tokens;
            static constexpr std::wstring_view kTrail = L".,;:!?)]}\"'";
            static constexpr std::wstring_view kLead = L"([{\"'";
            size_t k = 0;
            while (k < words.size())
            {
                bool replaced = false;
                for (size_t n = 4; n >= 1; --n)
                {
                    if (k + n > words.size())
                    {
                        continue;
                    }
                    const size_t first = words[k];
                    const size_t last = words[k + n - 1];
                    std::wstring window;
                    for (size_t idx = first; idx <= last; ++idx)
                    {
                        window += tokens[idx];
                    }
                    const std::wstring core = StripChars(window, kTrail, true);
                    const std::wstring trail = window.substr(core.size());
                    const std::wstring leadStrip = StripChars(core, kLead, false);
                    const std::wstring lead = core.substr(0, core.size() - leadStrip.size());
                    const std::wstring key = Squash(leadStrip);
                    const auto hit = key.empty() ? table.end() : table.find(key);
                    if (hit == table.end())
                    {
                        continue;
                    }
                    const VoiceTerm* term = hit->second.first;
                    const bool isExplicit = hit->second.second;
                    if (leadStrip == term->text)
                    {
                        break; // already canonical: nothing to do for this window (and no shorter window either)
                    }
                    // Python: re.findall(r"[^\W_]+", lead_strip) — the runs of letters and digits.
                    std::vector<std::wstring> windowWords;
                    for (size_t i = 0; i < leadStrip.size();)
                    {
                        if (!IsAlnum(leadStrip[i]))
                        {
                            ++i;
                            continue;
                        }
                        size_t j = i;
                        while (j < leadStrip.size() && IsAlnum(leadStrip[j]))
                        {
                            ++j;
                        }
                        windowWords.push_back(leadStrip.substr(i, j - i));
                        i = j;
                    }
                    if (!isExplicit)
                    {
                        // A single everyday word is never rewritten by a GENERATED alias: the pass exists for split or
                        // misheard names, which are never one everyday word.
                        if (n == 1 && windowWords.size() == 1 && _plain(windowWords[0]))
                        {
                            continue;
                        }
                        // Several everyday words respell only into a curated term: "agent master" → Agentmaster, never
                        // "session state" → SessionState.
                        if (windowWords.size() > 1 && !term->respellSplit &&
                            std::all_of(windowWords.begin(), windowWords.end(), [&](const std::wstring& w) { return _plain(w); }))
                        {
                            continue;
                        }
                    }
                    out[first] = lead + term->text + trail;
                    for (size_t idx = first + 1; idx <= last; ++idx)
                    {
                        out[idx].clear();
                    }
                    if (made)
                    {
                        made->push_back({ leadStrip, term->text });
                    }
                    k += n;
                    replaced = true;
                    break;
                }
                if (!replaced)
                {
                    ++k;
                }
            }
            std::wstring result;
            for (const auto& t : out)
            {
                result += t;
            }
            return result;
        }

    private:
        PlainWords _plain;
        std::vector<VoiceTerm> _terms;
    };

    // ===== context: where the session's own words come from =============================================================

    namespace detail
    {
        /// <summary>Whether `c` belongs to the identifier class `[A-Za-z0-9+#.-]` of vocab.py's _IDENT.</summary>
        /// <param name="c">The character.</param>
        /// <returns>True when it does.</returns>
        inline bool IsIdentTail(wchar_t c) noexcept
        {
            return IsUpperAZ(c) || IsLowerAZ(c) || IsDigit09(c) || c == L'+' || c == L'#' || c == L'.' || c == L'-';
        }

        /// <summary>
        /// vocab.py's _IDENT.finditer — `(?&lt;![\w.\\/-])([A-Za-z][A-Za-z0-9+#.-]{1,30})(?![\w\\/])`, emulated with the
        /// regex's backtracking: from a start the lookbehind allows, the longest candidate (up to 31 characters) whose next
        /// character is not `\w`, '\' or '/' wins. Inside a run, only a '+', '#', '.' or '-' can follow a shorter candidate,
        /// so a run like "C++/WinRT" yields "C+" (the regex does exactly that; the filters drop it).
        /// </summary>
        /// <param name="text">The document.</param>
        /// <returns>The matches, in order.</returns>
        /// <exception cref="std::bad_alloc">If the list cannot be allocated.</exception>
        inline std::vector<std::wstring> IdentMatches(std::wstring_view text)
        {
            std::vector<std::wstring> out;
            const auto blockedAfter = [&](size_t end) {
                return end < text.size() && (IsWordChar(text[end]) || text[end] == L'\\' || text[end] == L'/');
            };
            size_t i = 0;
            while (i < text.size())
            {
                const wchar_t c = text[i];
                const bool letter = IsUpperAZ(c) || IsLowerAZ(c);
                const bool lookbehindOk = i == 0 || !(IsWordChar(text[i - 1]) || text[i - 1] == L'.' || text[i - 1] == L'\\' || text[i - 1] == L'/' || text[i - 1] == L'-');
                if (!letter || !lookbehindOk)
                {
                    ++i;
                    continue;
                }
                size_t runEnd = i + 1;
                while (runEnd < text.size() && runEnd - (i + 1) < 30 && IsIdentTail(text[runEnd]))
                {
                    ++runEnd;
                }
                size_t matchEnd = 0;
                for (size_t end = runEnd; end >= i + 2; --end)
                {
                    if (!blockedAfter(end))
                    {
                        matchEnd = end;
                        break;
                    }
                }
                if (matchEnd == 0)
                {
                    ++i;
                    continue;
                }
                out.emplace_back(text.substr(i, matchEnd - i));
                i = matchEnd;
            }
            return out;
        }

        /// <summary>
        /// How often `word` occurs in `text` as a whole word (vocab.py: `len(re.findall(r"(?&lt;![\w])WORD(?![\w])", text))`),
        /// case-sensitive, non-overlapping.
        /// </summary>
        /// <param name="text">The document.</param>
        /// <param name="word">The word.</param>
        /// <returns>The count.</returns>
        inline size_t CountWholeWord(std::wstring_view text, std::wstring_view word) noexcept
        {
            size_t count = 0;
            size_t pos = 0;
            while (!word.empty() && (pos = text.find(word, pos)) != std::wstring_view::npos)
            {
                const size_t end = pos + word.size();
                const bool before = pos == 0 || !IsWordChar(text[pos - 1]);
                const bool after = end >= text.size() || !IsWordChar(text[end]);
                if (before && after)
                {
                    ++count;
                    pos = end;
                }
                else
                {
                    ++pos;
                }
            }
            return count;
        }

        /// <summary>vocab.py's file-name tests: `\.[A-Za-z0-9]{1,4}$` or `\.(?:dll|exe|cpp|h|json|md|ps1|py|log)\b` (any case).</summary>
        /// <param name="word">The candidate.</param>
        /// <returns>True for a file name.</returns>
        inline bool LooksLikeFileName(std::wstring_view word)
        {
            if (const size_t dot = word.rfind(L'.'); dot != std::wstring_view::npos)
            {
                const auto ext = word.substr(dot + 1);
                if (!ext.empty() && ext.size() <= 4 && std::all_of(ext.begin(), ext.end(), [](wchar_t c) { return IsUpperAZ(c) || IsLowerAZ(c) || IsDigit09(c); }))
                {
                    return true;
                }
            }
            static const wchar_t* const kExts[] = { L"dll", L"exe", L"cpp", L"h", L"json", L"md", L"ps1", L"py", L"log" };
            for (size_t p = 0; p < word.size(); ++p)
            {
                if (word[p] != L'.')
                {
                    continue;
                }
                for (const wchar_t* ext : kExts)
                {
                    const std::wstring_view e{ ext };
                    if (p + 1 + e.size() <= word.size() && Lower(word.substr(p + 1, e.size())) == e &&
                        (p + 1 + e.size() == word.size() || !IsWordChar(word[p + 1 + e.size()])))
                    {
                        return true;
                    }
                }
            }
            return false;
        }
    }

    /// <summary>
    /// vocab.py's extract_context_terms: the HARD words of a project document (its CLAUDE.md) — the names a developer of
    /// that project says and a speech model gets wrong (ConPTY, DuckDB, MSIX, Agentmaster). Code-only spellings (an
    /// underscore, "::", a file name, a cmdlet with an inner hyphen, an error code like C2039), shouted English ("ON",
    /// "BEFORE"), compounds of everyday words (SessionState) and one-off mentions are dropped (VOICE.md §5.2).
    /// </summary>
    /// <param name="text">The document.</param>
    /// <param name="plain">The plain-English test.</param>
    /// <param name="limit">At most this many terms, most frequent first.</param>
    /// <param name="minCount">The fewest occurrences a term needs.</param>
    /// <returns>(term, count) pairs, most frequent first, then by spelling.</returns>
    /// <exception cref="std::bad_alloc">If the counts cannot be allocated.</exception>
    inline std::vector<std::pair<std::wstring, int>> ExtractContextTerms(std::wstring_view text, const PlainWords& plain, size_t limit = 60, int minCount = 2)
    {
        using namespace detail;
        std::map<std::wstring, int> counts;
        std::unordered_map<std::wstring, size_t> twoLetterCounts; // CountWholeWord per two-letter word, asked once
        for (const auto& m : IdentMatches(text))
        {
            const std::wstring word = StripChars(m, L".-", true);
            const bool plusEnd = !word.empty() && word.back() == L'+' && !(word.size() >= 2 && word[word.size() - 2] == L'+');
            if (word.size() < 2 || word.find(L'_') != std::wstring::npos || word.find(L"::") != std::wstring::npos || plusEnd)
            {
                continue;
            }
            if (LooksLikeFileName(word))
            {
                continue; // a file name (Engine.cpp, hooks.log, am-update.ps1, Module.dll+0xRVA)
            }
            if (word.size() == 2 && PyIsUpper(word))
            {
                // A two-letter acronym (GB, VM, CR) only earns a slot when the document leans on it (UI, CI).
                auto it = twoLetterCounts.find(word);
                if (it == twoLetterCounts.end())
                {
                    it = twoLetterCounts.emplace(word, CountWholeWord(text, word)).first;
                }
                if (it->second < 8)
                {
                    continue;
                }
            }
            bool hyphenLetter = false; // `-[A-Za-z]`: a cmdlet, a flag or a slot name (Get-AppxPackage, Debug-B)
            for (size_t i = 0; i + 1 < word.size(); ++i)
            {
                if (word[i] == L'-' && (IsUpperAZ(word[i + 1]) || IsLowerAZ(word[i + 1])))
                {
                    hyphenLetter = true;
                    break;
                }
            }
            if (hyphenLetter)
            {
                continue;
            }
            {
                // `[A-Z]+\d{3,}` (a fullmatch): a compiler or HRESULT-like code (C2039).
                size_t i = 0;
                while (i < word.size() && IsUpperAZ(word[i]))
                {
                    ++i;
                }
                size_t d = i;
                while (d < word.size() && IsDigit09(word[d]))
                {
                    ++d;
                }
                if (i >= 1 && d == word.size() && d - i >= 3)
                {
                    continue;
                }
            }
            if (PyIsUpper(word) && PyIsAlpha(word) && (word.size() > 6 || plain(word)))
            {
                continue; // shouted English or a long all-caps name (ON, BEFORE, USERPROFILE)
            }
            // SPEAKABLE names only (measured on the owner's recordings: identifiers nobody dictates filled Whisper's prompt and
            // pushed out the glossary words a note DID hold).
            if (std::iswlower(static_cast<wint_t>(word[0])) && AnyUpper(std::wstring_view{ word }.substr(1)))
            {
                continue; // a lowerCamelCase code identifier (tabToken, kStateSettleMs, launchArgs)
            }
            {
                size_t wordParts = 0;
                for (const auto& p : CaseSplit(word))
                {
                    if (!std::all_of(p.begin(), p.end(), [](wchar_t c) { return std::iswdigit(static_cast<wint_t>(c)) != 0; }))
                    {
                        ++wordParts;
                    }
                }
                if (wordParts > 2)
                {
                    continue; // three or more word parts (WriteAllUtf8, SHGetKnownFolderPath, AgentmasterStateDir)
                }
            }
            if (word.size() == 2 && !PyIsUpper(word))
            {
                continue; // a two-character token (v9, m5, Xs)
            }
            if (!(AnyUpper(word) || AnyDigit(word)))
            {
                continue; // a lowercase word: not a name
            }
            if (!(IsDistinctive(word, plain) && IsHard(word, plain)))
            {
                continue;
            }
            ++counts[word];
        }
        std::vector<std::pair<std::wstring, int>> ranked;
        for (const auto& [w, c] : counts)
        {
            if (c >= minCount)
            {
                ranked.emplace_back(w, c);
            }
        }
        // (-count, word): std::map already iterates by word, so a stable sort by count alone keeps that tie order.
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (ranked.size() > limit)
        {
            ranked.resize(limit);
        }
        return ranked;
    }

    /// <summary>One line of the owner's dictionary (AppSettings::voiceDictionary): a term, and the spoken forms that mean it.</summary>
    struct DictionaryEntry
    {
        std::wstring term; // the spelling to produce ("JSONL")
        std::vector<std::wstring> triggers; // what the engine may write instead ("jay son el"); empty = boost the term only
    };

    /// <summary>
    /// Reads the owner's dictionary text: one entry per line, "Term" (boost it) or "spoken form, other form => Term"
    /// (FluidVoice's trigger → replacement). Blank lines and lines starting with '#' are skipped.
    /// </summary>
    /// <param name="text">The setting's text.</param>
    /// <returns>The entries, in order.</returns>
    /// <exception cref="std::bad_alloc">If the entries cannot be allocated.</exception>
    inline std::vector<DictionaryEntry> ParseDictionary(std::wstring_view text)
    {
        std::vector<DictionaryEntry> out;
        size_t start = 0;
        while (start <= text.size())
        {
            size_t end = text.find(L'\n', start);
            if (end == std::wstring_view::npos)
            {
                end = text.size();
            }
            const std::wstring line = detail::Strip(text.substr(start, end - start));
            start = end + 1;
            if (line.empty() || line[0] == L'#')
            {
                if (end == text.size())
                {
                    break;
                }
                continue;
            }
            DictionaryEntry e;
            if (const size_t arrow = line.find(L"=>"); arrow != std::wstring::npos)
            {
                e.term = detail::Strip(std::wstring_view{ line }.substr(arrow + 2));
                const std::wstring left = line.substr(0, arrow);
                size_t p = 0;
                while (p <= left.size())
                {
                    size_t q = left.find(L',', p);
                    if (q == std::wstring::npos)
                    {
                        q = left.size();
                    }
                    const std::wstring trig = detail::Strip(std::wstring_view{ left }.substr(p, q - p));
                    if (!trig.empty())
                    {
                        e.triggers.push_back(detail::Lower(trig));
                    }
                    p = q + 1;
                }
            }
            else
            {
                e.term = line;
            }
            if (!e.term.empty())
            {
                out.push_back(std::move(e));
            }
            if (end == text.size())
            {
                break;
            }
        }
        return out;
    }

    /// <summary>
    /// vocab.py's session_vocabulary: the vocabulary of one session — the built-in glossary, the working folder's name
    /// (weight 3, respellable), the hard words of the folder's CLAUDE.md (weight 2, mined: never respelled from everyday
    /// words), the hard words of the session title (weight 2), and the owner's dictionary (weight 3, curated).
    /// </summary>
    /// <param name="test">The plain-English test.</param>
    /// <param name="workdir">The session's working folder ("" = none); only its last component is read here.</param>
    /// <param name="title">The session title.</param>
    /// <param name="claudeMd">The folder's CLAUDE.md text ("" = none; the caller reads the file, VoiceContext.h).</param>
    /// <param name="dictionary">The owner's dictionary entries.</param>
    /// <param name="includeBuiltin">False for a context-only vocabulary (an evaluation ablation).</param>
    /// <returns>The vocabulary.</returns>
    /// <exception cref="std::bad_alloc">If the vocabulary cannot be built.</exception>
    inline VoiceVocabulary SessionVocabulary(WordTest test, std::wstring_view workdir, std::wstring_view title, std::wstring_view claudeMd,
                                             const std::vector<DictionaryEntry>& dictionary = {}, bool includeBuiltin = true)
    {
        using namespace detail;
        VoiceVocabulary v = includeBuiltin ? VoiceVocabulary::Builtin(std::move(test)) : VoiceVocabulary{ std::move(test) };
        // Python: Path(workdir).name — the last component, trailing separators ignored ("K:\src\App\" → "App"; "K:\" → "").
        std::wstring dir{ workdir };
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
        {
            dir.pop_back();
        }
        const size_t slash = dir.find_last_of(L"\\/");
        std::wstring leaf = slash == std::wstring::npos ? dir : dir.substr(slash + 1);
        if (leaf.size() == 2 && leaf[1] == L':')
        {
            leaf.clear(); // a drive ("K:") has no name
        }
        if (!leaf.empty() && IsDistinctive(leaf, v.Plain()))
        {
            v.Add(leaf, 3.0, L"context", true);
        }
        if (!claudeMd.empty())
        {
            for (const auto& [word, count] : ExtractContextTerms(claudeMd, v.Plain()))
            {
                (void)count;
                v.Add(word, 2.0, L"context", false);
            }
        }
        // Python: re.findall(r"[A-Za-z][A-Za-z0-9+#.-]+", title) — an ASCII letter and at least one more class character.
        for (size_t i = 0; i < title.size();)
        {
            if (!(IsUpperAZ(title[i]) || IsLowerAZ(title[i])))
            {
                ++i;
                continue;
            }
            size_t j = i + 1;
            while (j < title.size() && IsIdentTail(title[j]))
            {
                ++j;
            }
            if (j - i >= 2)
            {
                const std::wstring word{ title.substr(i, j - i) };
                if (IsDistinctive(word, v.Plain()) && IsHard(word, v.Plain()))
                {
                    v.Add(word, 2.0, L"context", false);
                }
                i = j;
            }
            else
            {
                ++i;
            }
        }
        for (const auto& e : dictionary)
        {
            v.Add(e.term, 3.0, L"user", true, e.triggers);
        }
        return v;
    }
}
