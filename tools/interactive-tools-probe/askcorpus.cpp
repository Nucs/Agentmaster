// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// askcorpus — replays every real AskUserQuestion call of this machine through the Telegram question card's pure core
// (AskQuestion.h, MESSAGING.md §21.30) and checks what a phone would get (a test tool, not part of the product; built by
// build_askdrive.py with the tool name `askcorpus`, from the engine sources the m5 suite compiles).
//
// Input: the export of ask_census.py (`--export <file>`): one JSON object per call — {id, file, line, version, sidechain,
// probe, input, result, isError, toolUseResult}. For each call:
//   RENDER  every tab (each question, and the review of a card of several questions) in several states — fresh; every
//           question answered (the recorded answers when they can be rebuilt, else synthetic ones); waiting for typed text;
//           asking to confirm ✖ Cancel; the longest own answer and note the card shows — and every outcome phase with long
//           answers. Each text must keep the card's budget (kAskCardMaxChars), stay under Telegram's 4,096 characters once the
//           tags are gone, keep its tags balanced and never cut a surrogate pair; each keyboard must keep Telegram's limits
//           (callback data ≤ 64 bytes, ≤ 8 buttons a row, ≤ 100 in all) and every callback must parse back.
//   COMPOSE the user's REAL answer (toolUseResult.answers + annotations) is rebuilt as the card's choices (a label → a pick, a
//           multi-select answer split as Claude Code's parser `H` splits it, anything else → the user's own words,
//           "(notes only)" + the note) and composed by ComposeAskAnswers; the result must equal what the PC recorded, byte for
//           byte — the card's answers are the PC's on every real case, not only on the probe's.
// Nothing is written but stdout.
//
// Usage: askcorpus.exe <calls.jsonl> [--real-only] [--examples N]

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <fcntl.h> // _O_U8TEXT: the report holds the users' own words (Hebrew, emoji), printed as UTF-8
#include <fstream>
#include <io.h>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../../src/cascadia/TerminalApp/AgentMaster/AskQuestion.h"
#include "../../src/cascadia/TerminalApp/AgentMaster/Json.h"

namespace
{
    using namespace Agentmaster;
    using namespace Agentmaster::Msg;

    /// <summary>UTF-8 bytes to UTF-16.</summary>
    std::wstring Wide(const std::string& s)
    {
        if (s.empty())
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
        return out;
    }

    /// <summary>UTF-16 to UTF-8 bytes (callback data is measured in bytes).</summary>
    std::string Utf8(const std::wstring& w)
    {
        if (w.empty())
        {
            return {};
        }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    /// <summary>The text Telegram counts: the HTML without its tags, the four entities decoded (UTF-16 units).</summary>
    size_t PlainLength(const std::wstring& html)
    {
        size_t n = 0;
        bool inTag = false;
        for (size_t i = 0; i < html.size(); ++i)
        {
            const wchar_t c = html[i];
            if (inTag)
            {
                inTag = c != L'>';
                continue;
            }
            if (c == L'<')
            {
                inTag = true;
                continue;
            }
            if (c == L'&')
            {
                const size_t semi = html.find(L';', i);
                if (semi != std::wstring::npos && semi - i <= 6)
                {
                    i = semi;
                }
            }
            ++n;
        }
        return n;
    }

    /// <summary>Count of a substring.</summary>
    size_t Count(const std::wstring& s, std::wstring_view what)
    {
        size_t n = 0;
        for (size_t at = s.find(what); at != std::wstring::npos; at = s.find(what, at + what.size()))
        {
            ++n;
        }
        return n;
    }

    /// <summary>A lone surrogate anywhere in the text (a cut through a pair).</summary>
    bool HasLoneSurrogate(const std::wstring& s)
    {
        for (size_t i = 0; i < s.size(); ++i)
        {
            const wchar_t c = s[i];
            if (c >= 0xD800 && c <= 0xDBFF)
            {
                if (i + 1 >= s.size() || s[i + 1] < 0xDC00 || s[i + 1] > 0xDFFF)
                {
                    return true;
                }
                ++i;
            }
            else if (c >= 0xDC00 && c <= 0xDFFF)
            {
                return true;
            }
        }
        return false;
    }

    /// <summary>Claude Code 2.1.291's multi-select parser `H`: the items of a Tct-joined answer, or nullopt.</summary>
    std::optional<std::vector<std::wstring>> SplitMulti(std::wstring rest)
    {
        std::vector<std::wstring> items;
        for (;;)
        {
            if (!rest.empty() && rest[0] == L'"')
            {
                size_t end = std::wstring::npos;
                bool escaped = false;
                for (size_t i = 1; i < rest.size(); ++i)
                {
                    if (escaped)
                    {
                        escaped = false;
                        continue;
                    }
                    if (rest[i] == L'\\')
                    {
                        escaped = true;
                        continue;
                    }
                    if (rest[i] == L'"')
                    {
                        end = i;
                        break;
                    }
                }
                if (end == std::wstring::npos)
                {
                    return std::nullopt;
                }
                const auto v = json::Parse(rest.substr(0, end + 1));
                if (!v || v->type != json::Value::Type::Str)
                {
                    return std::nullopt;
                }
                items.push_back(v->str);
                rest = rest.substr(end + 1);
            }
            else
            {
                const size_t k = rest.find(L", ");
                const std::wstring piece = k == std::wstring::npos ? rest : rest.substr(0, k);
                if (piece.find(L'"') != std::wstring::npos)
                {
                    return std::nullopt;
                }
                items.push_back(piece);
                rest = k == std::wstring::npos ? std::wstring{} : rest.substr(k);
            }
            if (rest.empty())
            {
                return items;
            }
            if (rest.rfind(L", ", 0) != 0)
            {
                return std::nullopt;
            }
            rest = rest.substr(2);
            if (rest.empty())
            {
                return std::nullopt;
            }
        }
    }

    /// <summary>JSON equality with objects compared as key sets (an absent annotations object equals {}).</summary>
    bool Same(const json::Value& a, const json::Value& b)
    {
        if (a.type != b.type)
        {
            return false;
        }
        switch (a.type)
        {
        case json::Value::Type::Str:
            return a.str == b.str;
        case json::Value::Type::Num:
            return a.num == b.num;
        case json::Value::Type::Bool:
            return a.boolean == b.boolean;
        case json::Value::Type::Arr:
            if (a.arr.size() != b.arr.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.arr.size(); ++i)
            {
                if (!Same(a.arr[i], b.arr[i]))
                {
                    return false;
                }
            }
            return true;
        case json::Value::Type::Obj:
        {
            if (a.members.size() != b.members.size())
            {
                return false;
            }
            for (const auto& [k, v] : a.members)
            {
                const json::Value* o = b.Find(k);
                if (!o || !Same(v, *o))
                {
                    return false;
                }
            }
            return true;
        }
        default:
            return true;
        }
    }

    struct Tally
    {
        std::map<std::wstring, size_t> counts;
        std::map<std::wstring, std::vector<std::wstring>> examples;
        size_t maxExamples{ 5 };

        void Add(const std::wstring& kind, const std::wstring& where)
        {
            ++counts[kind];
            auto& ex = examples[kind];
            if (ex.size() < maxExamples)
            {
                ex.push_back(where);
            }
        }
    };

    /// <summary>Checks one rendered card (text and keyboard) and files any failure.</summary>
    void CheckCard(const AskCardModel& m, const std::wstring& where, Tally& fail, Tally& fit, bool countFit)
    {
        const std::wstring html = BuildAskCardHtml(m, 1792413650000ll);
        if (html.size() > kAskCardMaxChars)
        {
            fail.Add(L"text over the card's budget", where + L" (" + std::to_wstring(html.size()) + L")");
        }
        if (PlainLength(html) > 4096)
        {
            fail.Add(L"text over Telegram's 4,096", where);
        }
        if (Count(html, L"<b>") != Count(html, L"</b>") || Count(html, L"<i>") != Count(html, L"</i>") || Count(html, L"<pre>") != Count(html, L"</pre>"))
        {
            fail.Add(L"unbalanced tags", where);
        }
        if (HasLoneSurrogate(html))
        {
            fail.Add(L"lone surrogate in the text", where);
        }
        if (countFit)
        {
            const bool lastResort = html.find(L"(cut to fit one message)") != std::wstring::npos;
            const bool shortened = html.find(L"(Shortened to fit one message") != std::wstring::npos;
            const bool previews = html.find(L"The previews do not fit in one message") != std::wstring::npos;
            fit.Add(lastResort ? L"labels only (last resort)" : shortened ? L"shortened" : previews ? L"previews left to the picture" : L"in full", where);
        }
        const auto rows = BuildAskCardButtons(m, L"abcdefghij");
        size_t total = 0;
        for (const auto& row : rows)
        {
            if (row.size() > 8)
            {
                fail.Add(L"more than 8 buttons in a row", where);
            }
            total += row.size();
            for (const auto& [label, data] : row)
            {
                if (label.empty() || HasLoneSurrogate(label))
                {
                    fail.Add(L"empty or torn button label", where);
                }
                if (Utf8(data).size() > 64)
                {
                    fail.Add(L"callback data over 64 bytes", where + L" " + data);
                }
                if (!ParseAskCallbackData(data))
                {
                    fail.Add(L"callback data does not parse back", where + L" " + data);
                }
            }
        }
        if (total > 100)
        {
            fail.Add(L"more than 100 buttons", where);
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    // The CRT's default "C" locale stops a wide printf at the first character it cannot convert; the examples hold the
    // users' own answers in any script.
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    if (argc < 2)
    {
        std::fwprintf(stderr, L"usage: askcorpus <calls.jsonl> [--real-only] [--examples N]\n");
        return 2;
    }
    bool realOnly = false;
    size_t examples = 5;
    for (int i = 2; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        if (a == L"--real-only")
        {
            realOnly = true;
        }
        else if (a == L"--examples" && i + 1 < argc)
        {
            examples = static_cast<size_t>(std::stoul(argv[++i]));
        }
    }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f)
    {
        std::fwprintf(stderr, L"cannot open %ls\n", argv[1]);
        return 2;
    }
    Tally fail;
    Tally fit;
    Tally compose;
    fail.maxExamples = fit.maxExamples = compose.maxExamples = examples;
    size_t calls = 0;
    size_t questions = 0;
    size_t renders = 0;
    size_t skippedProbe = 0;
    std::string line;
    while (std::getline(f, line))
    {
        if (line.empty())
        {
            continue;
        }
        const auto rec = json::Parse(Wide(line));
        if (!rec || rec->type != json::Value::Type::Obj)
        {
            continue;
        }
        if (realOnly && rec->BoolAt(L"probe", false))
        {
            ++skippedProbe;
            continue;
        }
        ++calls;
        std::wstring file = rec->StrAt(L"file");
        if (const size_t slash = file.find_last_of(L"\\/"); slash != std::wstring::npos)
        {
            file = file.substr(slash + 1);
        }
        const std::wstring where = file + L":" + std::to_wstring(rec->I64At(L"line", 0)) + L" " + rec->StrAt(L"version");
        const json::Value* input = rec->Find(L"input");
        if (!input || input->type != json::Value::Type::Obj)
        {
            fail.Add(L"(info) input not an object", where);
            continue;
        }
        const std::vector<AskQuestion> qs = ParseAskQuestions(*input);
        if (qs.empty())
        {
            fail.Add(L"(info) no question the card can read (Claude Code refused the call too)", where);
            continue;
        }
        questions += qs.size();
        const size_t n = qs.size();

        // ----- COMPOSE: rebuild the PC's recorded answer as the card's choices -----
        std::vector<AskChoice> recorded(n);
        bool rebuilt = false;
        const json::Value* tur = rec->Find(L"toolUseResult");
        const json::Value* answers = tur && tur->type == json::Value::Type::Obj ? tur->Find(L"answers") : nullptr;
        if (answers && answers->type == json::Value::Type::Obj)
        {
            const json::Value* anns = tur->Find(L"annotations");
            std::wstring why;
            for (size_t i = 0; i < n && why.empty(); ++i)
            {
                const AskQuestion& q = qs[i];
                AskChoice& c = recorded[i];
                std::wstring note;
                if (anns && anns->type == json::Value::Type::Obj)
                {
                    if (const auto* a = anns->Find(q.question); a && a->type == json::Value::Type::Obj)
                    {
                        note = a->StrAt(L"notes");
                    }
                }
                if (!note.empty() && !q.previewLayout)
                {
                    why = L"a note on a question without previews";
                    break;
                }
                c.note = note;
                const json::Value* a = answers->Find(q.question);
                if (!a)
                {
                    continue;
                }
                if (a->type != json::Value::Type::Str)
                {
                    why = L"an array answer";
                    break;
                }
                if (a->str.find(L"(Image attached)") != std::wstring::npos)
                {
                    why = L"an image pasted into the dialog";
                    break;
                }
                if (a->str == kAskNotesOnlyAnswer)
                {
                    continue;
                }
                const auto labelIndex = [&q](const std::wstring& t) -> size_t {
                    for (size_t k = 0; k < q.options.size(); ++k)
                    {
                        if (q.options[k].label == t)
                        {
                            return k;
                        }
                    }
                    return q.options.size();
                };
                if (!q.multiSelect)
                {
                    const size_t k = labelIndex(a->str);
                    if (k < q.options.size())
                    {
                        c.picks = { k };
                    }
                    else
                    {
                        c.otherText = a->str;
                    }
                    continue;
                }
                const auto items = SplitMulti(a->str);
                if (!items)
                {
                    c.otherText = a->str;
                    continue;
                }
                // In the recorded order: the card keeps the order the user ticked (AskChoice::picks), as the PC does.
                for (size_t j = 0; j < items->size(); ++j)
                {
                    const size_t k = labelIndex((*items)[j]);
                    if (k < q.options.size() && std::find(c.picks.begin(), c.picks.end(), k) == c.picks.end())
                    {
                        c.picks.push_back(k);
                    }
                    else if (j + 1 == items->size() && c.otherText.empty())
                    {
                        c.otherText = (*items)[j];
                    }
                    else
                    {
                        // The PC puts an answer of the user's own LAST and quotes an item holding ", " (Tct): a non-label item before
                        // the end is an older Claude Code's unquoted ", " inside a label or own words.
                        why = L"an unquoted \", \" inside an item (an older Claude Code: no Tct quoting)";
                    }
                }
            }
            if (!why.empty())
            {
                compose.Add(L"not rebuildable: " + why, where);
            }
            else
            {
                rebuilt = true;
                json::Value composedAnswers;
                json::Value composedAnnotations;
                ComposeAskAnswers(qs, recorded, composedAnswers, composedAnnotations);
                json::Value recordedAnnotations = json::Value::MkObj();
                if (const json::Value* anns = tur->Find(L"annotations"); anns && anns->type == json::Value::Type::Obj)
                {
                    recordedAnnotations = *anns;
                }
                const bool sameAnswers = Same(composedAnswers, *answers);
                const bool sameAnnotations = Same(composedAnnotations, recordedAnnotations);
                if (sameAnswers && sameAnnotations)
                {
                    compose.Add(L"equal to the PC's answer", where);
                }
                else
                {
                    compose.Add(std::wstring{ L"DIFFERS: " } + (sameAnswers ? L"" : L"answers ") + (sameAnnotations ? L"" : L"annotations"),
                                where + L"\n          ours: " + json::Dump(composedAnswers) + L" " + json::Dump(composedAnnotations) + L"\n          PC:   " + json::Dump(*answers) +
                                    L" " + json::Dump(recordedAnnotations));
                }
            }
        }
        else
        {
            compose.Add(L"(no structured answer: rejected, cut off or still open)", where);
        }

        // ----- RENDER: every tab, several states -----
        std::vector<AskChoice> full(n);
        for (size_t i = 0; i < n; ++i)
        {
            if (rebuilt && AskQuestionAnswered(qs[i], recorded[i]))
            {
                full[i] = recorded[i];
                continue;
            }
            // Synthetic: every option of a multi-select question, else the last option; the longest own words and note.
            if (qs[i].multiSelect)
            {
                for (size_t k = 0; k < qs[i].options.size(); ++k)
                {
                    full[i].picks.push_back(k);
                }
                full[i].otherText = std::wstring(300, L'w');
            }
            else if (!qs[i].options.empty())
            {
                full[i].picks = { qs[i].options.size() - 1 };
            }
            if (qs[i].previewLayout)
            {
                full[i].note = std::wstring(600, L'n');
            }
        }
        const size_t tabs = n > 1 ? n + 1 : 1;
        for (size_t tab = 0; tab < tabs; ++tab)
        {
            const std::wstring at = where + L" tab " + std::to_wstring(tab + 1) + L"/" + std::to_wstring(tabs);
            AskCardModel m = MakeAskCardModel(qs);
            m.current = tab;
            CheckCard(m, at + L" fresh", fail, fit, true);
            m.choices = full;
            CheckCard(m, at + L" answered", fail, fit, false);
            m.confirmCancel = true;
            CheckCard(m, at + L" confirm cancel", fail, fit, false);
            m.confirmCancel = false;
            if (tab < n)
            {
                m.awaiting = qs[tab].previewLayout ? AskTextTarget::Note : AskTextTarget::Answer;
                CheckCard(m, at + L" waiting for text", fail, fit, false);
                m.awaiting = AskTextTarget::None;
                m.choices[tab].otherText = std::wstring(600, L'o');
                m.choices[tab].note = qs[tab].previewLayout ? std::wstring(600, L'n') : std::wstring{};
                CheckCard(m, at + L" longest own answer and note", fail, fit, false);
            }
            // What a plain message is asked about, from this tab.
            m.choices = tab == 0 ? std::vector<AskChoice>(n) : full;
            const std::wstring which = AskWhichHtml(m);
            if (which.size() > kAskCardMaxChars || HasLoneSurrogate(which))
            {
                fail.Add(L"answer-or-prompt question too long or torn", at);
            }
            for (const auto& row : AskWhichButtons(m, L"abcdefghij", 999999999999ll))
            {
                for (const auto& [label, data] : row)
                {
                    if (Utf8(data).size() > 64 || !ParseAskCallbackData(data))
                    {
                        fail.Add(L"answer-or-prompt callback bad", at + L" " + data);
                    }
                }
            }
            renders += 6;
        }
        // Every outcome, with long answers.
        AskCardModel done = MakeAskCardModel(qs);
        done.choices = full;
        done.outcomeAtMs = 1792413600000ll;
        done.atPcAnswer = std::wstring(8000, L'a');
        done.reply = std::wstring(4000, L'r');
        done.timeoutMs = 60000;
        done.closedWhy = L"its session ended";
        done.unconfirmed = true;
        for (const AskCardPhase p : { AskCardPhase::Sending, AskCardPhase::Answered, AskCardPhase::AnsweredAtPc, AskCardPhase::Clarify, AskCardPhase::Cancelled, AskCardPhase::Replied,
                                      AskCardPhase::TimedOut, AskCardPhase::Closed })
        {
            done.phase = p;
            CheckCard(done, where + L" outcome " + std::to_wstring(static_cast<int>(p)), fail, fit, false);
            ++renders;
        }
    }
    std::wprintf(L"calls %zu (probe sessions skipped: %zu), questions %zu, card renders %zu\n\n", calls, skippedProbe, questions, renders);
    const auto print = [](const wchar_t* title, const Tally& t) {
        std::wprintf(L"%ls\n", title);
        if (t.counts.empty())
        {
            std::wprintf(L"   (none)\n");
        }
        for (const auto& [kind, count] : t.counts)
        {
            std::wprintf(L"  %6zu  %ls\n", count, kind.c_str());
            for (const auto& ex : t.examples.at(kind))
            {
                std::wprintf(L"            %ls\n", ex.c_str());
            }
        }
        std::wprintf(L"\n");
    };
    print(L"FAILURES (the card or its keyboard breaks a limit):", fail);
    print(L"HOW EACH FRESH TAB FITS ONE MESSAGE:", fit);
    print(L"THE USER'S REAL ANSWERS, COMPOSED BY THE CARD:", compose);
    size_t hard = 0;
    for (const auto& [kind, count] : fail.counts)
    {
        hard += kind.rfind(L"(info)", 0) == 0 ? 0 : count;
    }
    return hard == 0 ? 0 : 1;
}
