// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — VOICE.md §5.5: the session vocabulary of a dictated prompt (VoiceVocabulary.h) against the POC's
// reference (tools\voice-poc\vocab.py), through the goldens tests\voice\make_goldens.py wrote:
//   1. the rules per term: the squash key, the case split (Python's regex emulated, backtracking included), the generated
//      aliases, distinctive, hard;
//   2. the context terms mined from a sample CLAUDE.md (every filter: code-only spellings, shouted English, compounds of
//      everyday words, one-off mentions, two-letter acronyms);
//   3. three vocabularies (the glossary; a session's; a session's with the owner's dictionary): their terms, weights,
//      sources and respell flags in order, Whisper's prompt at two budgets, the hotwords at two limits, and the dictionary
//      pass over 15 transcripts — the text AND the respellings it made;
//   4. the owner's dictionary text read like the reference reads it;
//   5. properties the goldens do not spell out: the pass is idempotent, never rewrites an everyday word, keeps the
//      punctuation around a window, and asks the word test about nothing the reference never asked.
// The word test answers from tests\voice\plain_words.txt (the spell checker's answers the reference recorded), so this
// suite does not depend on the machine's dictionary; the live spell checker is VoiceSpellWords.h's (app-only).

#include "m5_tests.h"

#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../Json.h"
#include "../VoiceProtocol.h"
#include "../VoiceVocabulary.h"

using namespace Agentmaster;
using namespace Agentmaster::Voice;

namespace
{
    namespace fs = std::filesystem;

    /// <summary>The folder m5_tests.exe runs from (the fixtures sit in its voice\ subfolder, whatever the current folder).</summary>
    /// <returns>The folder; "." when the module path cannot be read.</returns>
    fs::path VoiceHarnessDir()
    {
        wchar_t buffer[MAX_PATH * 4]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        if (n == 0 || n >= std::size(buffer))
        {
            return fs::path{ L"." };
        }
        return fs::path{ buffer }.parent_path();
    }

    /// <summary>A UTF-8 file as UTF-16 ("" when it cannot be read; the checks that need it then fail visibly).</summary>
    /// <param name="path">The file.</param>
    /// <returns>The text.</returns>
    std::wstring ReadVoiceUtf8(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.empty())
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>((std::max)(n, 0)), L'\0');
        if (n > 0)
        {
            ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
        }
        return out;
    }

    /// <summary>
    /// The frozen dictionary: plain_words.txt's "spelling&lt;TAB&gt;1|0" lines as a WordTest. A spelling the reference never
    /// asked about is recorded in `unknown` (and answered "known": the safe answer), so a parity bug shows as a failed check
    /// rather than as a silently different golden.
    /// </summary>
    struct FrozenWords
    {
        std::map<std::wstring, bool> answers;
        std::set<std::wstring> unknown;

        /// <summary>The WordTest over this table (the table must outlive it).</summary>
        /// <returns>The test.</returns>
        WordTest Test()
        {
            return [this](std::wstring_view spelling) {
                const auto it = answers.find(std::wstring{ spelling });
                if (it == answers.end())
                {
                    unknown.insert(std::wstring{ spelling });
                    return true;
                }
                return it->second;
            };
        }
    };

    /// <summary>Reads plain_words.txt.</summary>
    /// <param name="text">The file's text.</param>
    /// <returns>The table.</returns>
    FrozenWords LoadFrozenWords(const std::wstring& text)
    {
        FrozenWords f;
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find(L'\n', start);
            if (end == std::wstring::npos)
            {
                end = text.size();
            }
            std::wstring line = text.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == L'\r')
            {
                line.pop_back();
            }
            const size_t tab = line.find(L'\t');
            if (tab != std::wstring::npos)
            {
                f.answers[line.substr(0, tab)] = line.substr(tab + 1) == L"1";
            }
        }
        return f;
    }

    /// <summary>A JSON array of strings as a vector.</summary>
    /// <param name="v">The array (anything else gives an empty vector).</param>
    /// <returns>The strings.</returns>
    std::vector<std::wstring> Strings(const json::Value* v)
    {
        std::vector<std::wstring> out;
        if (v && v->type == json::Value::Type::Arr)
        {
            for (const auto& e : v->arr)
            {
                out.push_back(e.AsStr());
            }
        }
        return out;
    }

    /// <summary>Prints a failed comparison's two sides (non-ASCII as \uXXXX, so the console cannot cut the line).</summary>
    /// <param name="what">What was compared.</param>
    /// <param name="expected">The golden.</param>
    /// <param name="actual">The port's value.</param>
    void ReportVoiceDiff(const std::wstring& what, const std::wstring& expected, const std::wstring& actual)
    {
        const auto esc = [](const std::wstring& s) {
            std::wstring out;
            for (const wchar_t c : s)
            {
                if (c < 0x80)
                {
                    out += c;
                }
                else
                {
                    wchar_t buf[8]{};
                    swprintf_s(buf, L"\\u%04X", static_cast<unsigned>(c));
                    out += buf;
                }
            }
            return out;
        };
        std::wprintf(L"    %s\n      expected: %s\n      actual:   %s\n", esc(what).c_str(), esc(expected).c_str(), esc(actual).c_str());
    }
}

void TestVoiceVocabulary()
{
    wprintf(L"== TestVoiceVocabulary (VOICE.md 5.5: the session vocabulary - rules, context terms, prompt, hotwords, the dictionary pass - vs vocab.py's goldens) ==\n");
    const fs::path dir = VoiceHarnessDir() / L"voice";
    const std::wstring goldensText = ReadVoiceUtf8(dir / L"vocab_goldens.json");
    const std::wstring sample = ReadVoiceUtf8(dir / L"context_sample.md");
    FrozenWords frozen = LoadFrozenWords(ReadVoiceUtf8(dir / L"plain_words.txt"));
    const auto goldens = json::Parse(goldensText);
    CHECK(goldens.has_value() && goldens->type == json::Value::Type::Obj, "voice\\vocab_goldens.json is there and parses");
    CHECK(!sample.empty() && !frozen.answers.empty(), "voice\\context_sample.md and voice\\plain_words.txt are there");
    if (!goldens || goldens->type != json::Value::Type::Obj)
    {
        return;
    }

    // ---- 1. the rules per term ----
    {
        const PlainWords plain{ frozen.Test() };
        int termsChecked = 0;
        if (const auto* terms = goldens->Find(L"terms"); terms && terms->type == json::Value::Type::Arr)
        {
            for (const auto& g : terms->arr)
            {
                const std::wstring term = g.StrAt(L"term");
                const auto aliases = SpokenAliases(term);
                const std::vector<std::wstring> gotAliases(aliases.begin(), aliases.end());
                const bool ok = Squash(term) == g.StrAt(L"squash") && CaseSplit(term) == Strings(g.Find(L"case_split")) &&
                                gotAliases == Strings(g.Find(L"aliases")) && IsDistinctive(term, plain) == g.BoolAt(L"distinctive") &&
                                IsHard(term, plain) == g.BoolAt(L"hard");
                if (!ok)
                {
                    ReportVoiceDiff(L"term " + term, L"(see vocab_goldens.json)", Squash(term) + L" | " + std::to_wstring(gotAliases.size()) + L" aliases | distinctive=" +
                                                                                   std::to_wstring(IsDistinctive(term, plain)) + L" hard=" + std::to_wstring(IsHard(term, plain)));
                }
                CHECK(ok, "a term's squash key, case split, aliases, distinctive and hard answers match the reference");
                ++termsChecked;
            }
        }
        CHECK(termsChecked >= 30, "the goldens pin 30+ terms one by one");
    }

    // ---- 2. the context terms of the sample CLAUDE.md ----
    {
        const PlainWords plain{ frozen.Test() };
        const auto got = ExtractContextTerms(sample, plain);
        std::vector<std::pair<std::wstring, int>> want;
        if (const auto* ctx = goldens->Find(L"context"))
        {
            if (const auto* terms = ctx->Find(L"terms"); terms && terms->type == json::Value::Type::Arr)
            {
                for (const auto& pair : terms->arr)
                {
                    if (pair.arr.size() == 2)
                    {
                        want.emplace_back(pair.arr[0].AsStr(), static_cast<int>(pair.arr[1].AsI64()));
                    }
                }
            }
        }
        if (got != want)
        {
            std::wstring a;
            for (const auto& [w, c] : got)
            {
                a += w + L":" + std::to_wstring(c) + L" ";
            }
            ReportVoiceDiff(L"context terms", L"(see vocab_goldens.json)", a);
        }
        CHECK(got == want, "the hard words mined from the sample CLAUDE.md, with their counts and order, match the reference");
        const auto has = [&](const wchar_t* w) { return std::any_of(got.begin(), got.end(), [&](const auto& p) { return p.first == w; }); };
        CHECK(has(L"ConPTY") && has(L"DuckDB") && has(L"MSIX") && has(L"UI"), "the hard names are kept (ConPTY, DuckDB, MSIX, and UI for being used 8+ times)");
        CHECK(!has(L"SessionState") && !has(L"TerminalPage") && !has(L"Scheduler"), "a compound of everyday words is not a context term");
        CHECK(!has(L"ON") && !has(L"NOT") && !has(L"BEFORE") && !has(L"USERPROFILE") && !has(L"English"), "shouted English, a long all-caps name and a proper noun are not");
        CHECK(!has(L"Get-AppxPackage") && !has(L"Debug-B") && !has(L"C2039") && !has(L"VM") && !has(L"Toolhelp"),
              "a cmdlet, a slot name, an error code, a rare two-letter acronym and a one-off mention are not");
    }

    // ---- 3. the three vocabularies ----
    const std::wstring dictText = goldens->StrAt(L"dictionary_text");
    const auto dictionary = ParseDictionary(dictText);
    const auto build = [&](const std::wstring& name) -> VoiceVocabulary {
        if (name == L"builtin")
        {
            return VoiceVocabulary::Builtin(frozen.Test());
        }
        const json::Value* args = goldens->Find(L"session_args") ? goldens->Find(L"session_args")->Find(name) : nullptr;
        const std::wstring workdir = args ? args->StrAt(L"workdir") : std::wstring{};
        const std::wstring title = args ? args->StrAt(L"title") : std::wstring{};
        const bool withDictionary = args && args->BoolAt(L"dictionary");
        return SessionVocabulary(frozen.Test(), workdir, title, sample, withDictionary ? dictionary : std::vector<DictionaryEntry>{});
    };
    if (const auto* vocabs = goldens->Find(L"vocabularies"); vocabs && vocabs->type == json::Value::Type::Obj)
    {
        for (const auto& [name, g] : vocabs->members)
        {
            const VoiceVocabulary v = build(name);
            // The terms, in order, with weight, source and respell flag.
            bool termsOk = true;
            const auto* terms = g.Find(L"terms");
            if (!terms || terms->arr.size() != v.Terms().size())
            {
                termsOk = false;
            }
            else
            {
                for (size_t i = 0; i < terms->arr.size(); ++i)
                {
                    const auto& t = terms->arr[i];
                    const auto& got = v.Terms()[i];
                    if (t.arr.size() != 4 || t.arr[0].AsStr() != got.text || t.arr[1].AsNum() != got.weight || t.arr[2].AsStr() != got.source ||
                        t.arr[3].AsBool() != got.respellSplit)
                    {
                        termsOk = false;
                        ReportVoiceDiff(name + L" term " + std::to_wstring(i), t.arr.empty() ? L"" : t.arr[0].AsStr(), got.text);
                        break;
                    }
                }
            }
            CHECK(termsOk, "a vocabulary's terms, weights, sources and respell flags match the reference, in order");
            const auto same = [&](const wchar_t* key, const std::wstring& actual) {
                const std::wstring want = g.StrAt(key);
                if (want != actual)
                {
                    ReportVoiceDiff(name + L" " + key, want, actual);
                }
                return want == actual;
            };
            CHECK(same(L"whisper_prompt", v.WhisperPrompt()), "Whisper's prompt matches the reference (highest weight last)");
            CHECK(same(L"whisper_prompt_200", v.WhisperPrompt(200)), "Whisper's prompt cut to 200 characters matches the reference (cut from the start, at a comma)");
            CHECK(same(L"hotwords", v.Hotwords()), "the hotwords match the reference (hard terms only, scores by weight)");
            CHECK(same(L"hotwords_10", v.Hotwords(2.0, 10)), "the hotwords cut to 10 match the reference");
            int applied = 0;
            if (const auto* cases = g.Find(L"apply"); cases && cases->type == json::Value::Type::Arr)
            {
                for (const auto& c : cases->arr)
                {
                    std::vector<VoiceReplacement> made;
                    const std::wstring out = v.Apply(c.StrAt(L"in"), &made);
                    std::vector<std::wstring> madeFlat;
                    for (const auto& m : made)
                    {
                        madeFlat.push_back(m.from + L"=>" + m.to);
                    }
                    std::vector<std::wstring> wantMade;
                    if (const auto* gm = c.Find(L"made"); gm && gm->type == json::Value::Type::Arr)
                    {
                        for (const auto& pair : gm->arr)
                        {
                            if (pair.arr.size() == 2)
                            {
                                wantMade.push_back(pair.arr[0].AsStr() + L"=>" + pair.arr[1].AsStr());
                            }
                        }
                    }
                    const bool ok = out == c.StrAt(L"out") && madeFlat == wantMade;
                    if (!ok)
                    {
                        ReportVoiceDiff(name + L" apply: " + c.StrAt(L"in"), c.StrAt(L"out"), out);
                    }
                    CHECK(ok, "the dictionary pass rewrites the transcript exactly as the reference does, with the same respellings");
                    // Idempotence: a second pass over the result changes nothing.
                    CHECK(v.Apply(out) == out, "the dictionary pass is idempotent");
                    ++applied;
                }
            }
            CHECK(applied >= 15, "every golden transcript was replayed");
        }
    }

    // ---- 4. the owner's dictionary text ----
    {
        std::vector<std::wstring> got;
        for (const auto& e : dictionary)
        {
            std::wstring line = e.term + L"<-";
            for (const auto& t : e.triggers)
            {
                line += t + L"|";
            }
            got.push_back(line);
        }
        std::vector<std::wstring> want;
        if (const auto* d = goldens->Find(L"dictionary"); d && d->type == json::Value::Type::Arr)
        {
            for (const auto& pair : d->arr)
            {
                std::wstring line = (pair.arr.size() == 2 ? pair.arr[0].AsStr() : std::wstring{}) + L"<-";
                if (pair.arr.size() == 2)
                {
                    for (const auto& t : pair.arr[1].arr)
                    {
                        line += t.AsStr() + L"|";
                    }
                }
                want.push_back(line);
            }
        }
        CHECK(got == want, "the owner's dictionary text is read like the reference reads it (terms, '=>' triggers, comments and blanks skipped)");
    }

    // ---- 5. properties ----
    {
        const VoiceVocabulary v = SessionVocabulary(frozen.Test(), L"K:\\source\\Agentmaster", L"", sample, dictionary);
        CHECK(v.Apply(L"we go on and the server rejects it") == L"we go on and the server rejects it", "an everyday sentence passes through untouched");
        CHECK(v.Apply(L"(con PTY),") == L"(ConPTY),", "a window keeps the brackets and punctuation around it");
        CHECK(v.Apply(L"fluid boys said hi") == L"FluidVoice said hi", "the owner's own trigger respells (dictionary line 'fluid voice, fluid boys => FluidVoice')");
        CHECK(v.Apply(L"") == L"" && v.Apply(L"   ") == L"   ", "empty and blank transcripts come back as they were");
        CHECK(SessionVocabulary(frozen.Test(), L"K:\\", L"", L"").Terms().size() == VoiceVocabulary::Builtin(frozen.Test()).Terms().size(),
              "a drive root adds no folder name to the vocabulary");
    }
    if (!frozen.unknown.empty())
    {
        std::wstring list;
        for (const auto& w : frozen.unknown)
        {
            list += w + L" ";
        }
        ReportVoiceDiff(L"the port asked about spellings the reference never asked about", L"(none)", list);
    }
    CHECK(frozen.unknown.empty(), "the port asks the word test about exactly the spellings the reference asked about (no parity drift)");
}

void TestVoiceProtocol()
{
    wprintf(L"== TestVoiceProtocol (VOICE.md 6.2: the voice worker protocol - the request lines, the answer reader, the prompt text) ==\n");

    // ---- the request lines ----
    {
        const std::wstring hello = BuildHelloRequest(1);
        const auto doc = json::Parse(hello);
        CHECK(doc && doc->StrAt(L"op") == L"hello" && doc->I64At(L"id") == 1 && doc->I64At(L"v") == kVoiceProtocolVersion, "hello: op, id and version");
        const auto quit = json::Parse(BuildQuitRequest(9));
        CHECK(quit && quit->StrAt(L"op") == L"quit" && quit->I64At(L"id") == 9, "quit: op and id");

        VoiceWorkerRequest r;
        r.path = L"C:\\Users\\me\\.agentmaster\\messaging\\telegram\\voice\\14021.oga";
        r.language = L"en";
        r.prompt = L"A developer dictates.\nGlossary: \"ConPTY\", JSONL.";
        r.hotwords = L"ConPTY :2.5/JSONL :2.0";
        const std::wstring line = BuildTranscribeRequest(2, r);
        CHECK(line.find(L'\n') == std::wstring::npos && line.find(L'\r') == std::wstring::npos,
              "a transcribe request is ONE line even when its prompt holds a line break (the protocol is line-framed)");
        const auto t = json::Parse(line);
        CHECK(t && t->StrAt(L"op") == L"transcribe" && t->StrAt(L"path") == r.path && t->StrAt(L"prompt") == r.prompt &&
                  t->StrAt(L"hotwords") == r.hotwords && t->StrAt(L"language") == L"en",
              "transcribe: the path's backslashes, the prompt's quotes and line break and the hotwords round-trip through the line");
    }

    // ---- the answer reader ----
    {
        auto a = ParseWorkerAnswer(L"{\"v\":1,\"id\":2,\"ok\":true,\"text\":\"Check why the ConPTY host drops.\",\"audio_ms\":7480,\"decode_ms\":512,\"engine\":\"whisper-large-v3+prompt\"}");
        CHECK(a && a->ok && a->id == 2 && a->text == L"Check why the ConPTY host drops." && a->audioMs == 7480 && a->decodeMs == 512 &&
                  a->engine == L"whisper-large-v3+prompt",
              "a transcript answer: id, text, timings, engine");
        a = ParseWorkerAnswer(L"{\"v\":1,\"id\":1,\"ok\":true,\"engine\":\"parakeet-v2-beam+hotwords\",\"prompt\":false,\"hotwords\":true}");
        CHECK(a && a->ok && !a->takesPrompt && a->takesHotwords, "a hello answer: what the engine takes");
        a = ParseWorkerAnswer(L"{\"v\":1,\"id\":3,\"ok\":false,\"error\":\"FileNotFoundError: x.oga\"}");
        CHECK(a && !a->ok && a->error == L"FileNotFoundError: x.oga", "a failure keeps the worker's reason");
        a = ParseWorkerAnswer(L"{\"id\":4,\"ok\":false}");
        CHECK(a && !a->ok && !a->error.empty(), "a failure without a reason still says something (never an empty error)");
        a = ParseWorkerAnswer(L"{\"ok\":false,\"error\":\"JSONDecodeError\"}");
        CHECK(a && a->id == 0 && !a->ok, "an answer without an id (a request the worker could not read) is read with id 0");
        CHECK(!ParseWorkerAnswer(L"Downloading model: 45%"), "a progress line is not an answer");
        CHECK(!ParseWorkerAnswer(L"{\"progress\":0.45}"), "a JSON object without 'ok' is not an answer");
        CHECK(!ParseWorkerAnswer(L"[1,2,3]") && !ParseWorkerAnswer(L""), "an array or an empty line is not an answer");
    }

    // ---- the prompt text ----
    {
        CHECK(NormalizeTranscriptForPrompt(L"  Check why\nthe  ConPTY\r\nhost\tdrops.  ") == L"Check why the ConPTY host drops.",
              "a transcript becomes one paragraph: trimmed, every run of white space one space");
        CHECK(NormalizeTranscriptForPrompt(L" \n\t ") == L"" && NormalizeTranscriptForPrompt(L"") == L"", "nothing heard stays empty");
        CHECK(NormalizeTranscriptForPrompt(L"a\x00A0" L"b") == L"a b", "a no-break space folds too (an engine's segment joint)");
        CHECK(NormalizeTranscriptForPrompt(L"\x05D1\x05D3\x05D9\x05E7\x05D4 \x05D0\x05D7\x05EA") == L"\x05D1\x05D3\x05D9\x05E7\x05D4 \x05D0\x05D7\x05EA",
              "a Hebrew transcript passes through unchanged");
    }
}
