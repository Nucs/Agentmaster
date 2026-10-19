// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.30 / INTERACTIVE_TOOLS.md §6: Claude Code's AskUserQuestion dialogs ANSWERED FROM TELEGRAM.
//   1. the answers exactly as the PC composes them — goldens from the probe of 2026-10-19 (tools/interactive-tools-probe; the
//      hook's answers D and E reproduced the PC's tool result byte for byte): a pick and a note on a preview question (D), a
//      note alone (E: "(notes only)"), a pick alone (G: the preview rides along), a plain question, a multi-select with labels
//      that need Tct's quoting, the user's own answer, a note's trim — and the allow / deny lines (ASCII only, the input kept,
//      the answers and annotations replaced, never doubled);
//   2. the preview rules of 2.1.291 (ClassifyClaudeQuestionPreview / ClaudeQuestionPreviewEcho / ClaudeCellWidth);
//   3. the request line, its signature against the mirror's question item, the transcript's answer item with its NOTES;
//   4. the card: its callback data, the text routing, every step (options, own answers, notes, Done, navigation), its text and
//      buttons in every phase and its budget, the "answer or next prompt?" words, the previews as a picture's sheet;
//   5. "Chat about this" (the PC's rejection with its clarify request);
//   6. the hooks settings' question entry and the question hook's script;
//   7. the ask pipe end to end: AskRegistry + AskBridge on a scratch pipe — a refused request, an answered one, a released one,
//      a helper that leaves, a malformed request, Stop.
// The service's half (a card in a topic, its watch, the taps, the typed answers) needs Telegram and a running claude: its live
// check is MESSAGING.md §21.30 "Check after the build".

#include "m5_tests.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../AskQuestion.h"
#include "../ClaudeSpawn.h" // BuildHooksSettingsJson, BuildAskHookScript, kAskHookTimeoutSeconds
#include "../HooksBridge.h" // AskRegistry, AskBridge
#include "../Json.h"
#include "../MessagingModels.h" // ClassifyTranscriptLine (the mirror's question and answer items)
#include "../ScreenImage.h" // the previews' sheet drawn for real

namespace
{
    namespace am = ::Agentmaster;
    namespace msg = ::Agentmaster::Msg;
    using ::Agentmaster::json::Value;

    // The probe's preview question (its D / E / G scenarios asked it with three questions' texts; the composer only reads the
    // text as a key, so one text serves all three goldens).
    constexpr const wchar_t* kLayoutInput =
        LR"({"questions":[{"question":"Which footer layout do you prefer?","header":"Layout","options":[)"
        LR"({"label":"Left","description":"Logo positioned on the left","preview":"+--------------+\n|[LOGO]        |\n+--------------+"},)"
        LR"({"label":"Center","description":"Logo positioned in the center","preview":"+--------------+\n|   [LOGO]     |\n+--------------+"},)"
        LR"({"label":"Right","description":"Logo positioned on the right","preview":"+--------------+\n|        [LOGO]|\n+--------------+"}],"multiSelect":false}]})";

    // A question without previews (the probe's "Which color do you prefer?").
    constexpr const wchar_t* kColorInput =
        LR"({"questions":[{"question":"Which color do you prefer?","header":"Color","options":[{"label":"Red","description":"The color red"},)"
        LR"({"label":"Blue","description":"The color blue"}],"multiSelect":false}]})";

    /// <summary>Parses a JSON text the test wrote (a failure is a test bug: an empty object then fails the checks that read it).</summary>
    /// <param name="text">The JSON.</param>
    /// <returns>The value.</returns>
    Value ParseOrEmpty(std::wstring_view text)
    {
        auto v = am::json::Parse(text);
        return v ? *v : Value::MkObj();
    }

    /// <summary>JSON equality by the dump (member order included — the composer's order is the PC's: preview, then notes).</summary>
    /// <param name="a">One value.</param>
    /// <param name="bText">The other as JSON text.</param>
    /// <returns>True when both dump the same.</returns>
    bool JsonEquals(const Value& a, std::wstring_view bText)
    {
        const auto b = am::json::Parse(bText);
        return b && am::json::Dump(a) == am::json::Dump(*b);
    }

    /// <summary>A card's model for an input text.</summary>
    /// <param name="input">The tool_input JSON.</param>
    /// <returns>The model (empty questions for a bad input).</returns>
    msg::AskCardModel ModelFor(std::wstring_view input)
    {
        return msg::MakeAskCardModel(msg::ParseAskQuestions(ParseOrEmpty(input)));
    }

    /// <summary>The decision line (UTF-8 bytes, ASCII expected) back as JSON.</summary>
    /// <param name="line">The line.</param>
    /// <returns>The value, or nullopt when it does not parse.</returns>
    std::optional<Value> ParseLine(const std::string& line)
    {
        return am::json::Parse(std::wstring(line.begin(), line.end()));
    }

    /// <summary>True when every byte is 7-bit ASCII (the helper's stdout never meets a code page).</summary>
    /// <param name="s">The bytes.</param>
    /// <returns>True when ASCII.</returns>
    bool IsAscii(const std::string& s)
    {
        for (const unsigned char c : s)
        {
            if (c >= 0x80)
            {
                return false;
            }
        }
        return true;
    }

    /// <summary>Counts an object's members named `key` (a duplicate key would be two members in json::Value).</summary>
    /// <param name="obj">The object.</param>
    /// <param name="key">The key.</param>
    /// <returns>The count.</returns>
    size_t CountMembers(const Value& obj, std::wstring_view key)
    {
        size_t n = 0;
        for (const auto& [k, v] : obj.members)
        {
            n += k == key ? 1 : 0;
        }
        return n;
    }

    // ===== 1. the answers as the PC composes them =====

    void TestAskComposeGoldens()
    {
        // D (the hook's answer reproduced the PC's tool result byte for byte): "Center" picked, a note.
        {
            auto m = ModelFor(kLayoutInput);
            CHECK(m.questions.size() == 1 && m.questions[0].previewLayout && m.questions[0].options.size() == 3, "ask: the layout question parses as a PREVIEW question of 3 options");
            m.choices[0].picks = { 1 };
            m.choices[0].note = L"use rounded corners";
            Value answers;
            Value annotations;
            msg::ComposeAskAnswers(m.questions, m.choices, answers, annotations);
            CHECK(JsonEquals(answers, LR"({"Which footer layout do you prefer?":"Center"})"), "ask golden D: answers = the picked label");
            CHECK(JsonEquals(annotations, LR"({"Which footer layout do you prefer?":{"preview":"+--------------+\n|   [LOGO]     |\n+--------------+","notes":"use rounded corners"}})"),
                  "ask golden D: the annotation carries the picked preview's echo, then the note (the PC's order)");
        }
        // E: a note without a pick -> "(notes only)" (`_0t`); no preview (no option named).
        {
            auto m = ModelFor(kLayoutInput);
            m.choices[0].note = L"none of these, make it a tab strip";
            Value answers;
            Value annotations;
            msg::ComposeAskAnswers(m.questions, m.choices, answers, annotations);
            CHECK(JsonEquals(answers, LR"J({"Which footer layout do you prefer?":"(notes only)"})J"), "ask golden E: a note alone answers \"(notes only)\"");
            CHECK(JsonEquals(annotations, LR"({"Which footer layout do you prefer?":{"notes":"none of these, make it a tab strip"}})"), "ask golden E: the note alone, no preview");
        }
        // G: a pick without a note -> the preview rides along anyway (the PC sends it with or without a note).
        {
            auto m = ModelFor(kLayoutInput);
            m.choices[0].picks = { 0 };
            Value answers;
            Value annotations;
            msg::ComposeAskAnswers(m.questions, m.choices, answers, annotations);
            CHECK(JsonEquals(answers, LR"({"Which footer layout do you prefer?":"Left"})"), "ask golden G: answers = the picked label");
            CHECK(JsonEquals(annotations, LR"({"Which footer layout do you prefer?":{"preview":"+--------------+\n|[LOGO]        |\n+--------------+"}})"),
                  "ask golden G: the preview without a note");
        }
        // A plain question: the label; annotations an EMPTY object (the PC sends {} too).
        {
            auto m = ModelFor(kColorInput);
            CHECK(m.questions.size() == 1 && !m.questions[0].previewLayout, "ask: a question without previews has no preview layout");
            m.choices[0].picks = { 1 };
            Value answers;
            Value annotations;
            msg::ComposeAskAnswers(m.questions, m.choices, answers, annotations);
            CHECK(JsonEquals(answers, LR"({"Which color do you prefer?":"Blue"})"), "ask: a plain pick answers its label");
            CHECK(annotations.type == Value::Type::Obj && annotations.members.empty(), "ask: no annotations for a plain question (an empty object)");
            // A note on a plain question is never sent (only a preview question has the Notes field).
            m.choices[0].note = L"ignored";
            msg::ComposeAskAnswers(m.questions, m.choices, answers, annotations);
            CHECK(annotations.members.empty(), "ask: a note on a question without the Notes field is not sent");
        }
        // The user's own answer (the PC's "Type something."): the text itself; a pick is dropped for it by AskTakeText.
        {
            auto m = ModelFor(kColorInput);
            m.choices[0].otherText = L"Purple, please";
            Value answers;
            Value annotations;
            msg::ComposeAskAnswers(m.questions, m.choices, answers, annotations);
            CHECK(JsonEquals(answers, LR"({"Which color do you prefer?":"Purple, please"})"), "ask: an own answer of a single-select question is the text as typed (no quoting)");
        }
        // Multi-select: Tct joins with ", " and JSON-quotes a label holding ", " or a double quote; the own text comes last.
        {
            const std::wstring input = LR"({"questions":[{"question":"Which ones?","header":"Pick","multiSelect":true,"options":[)"
                                       LR"({"label":"A, b","description":""},{"label":"Say \"hi\"","description":""},{"label":"Plain","description":""}]}]})";
            auto m = ModelFor(input);
            CHECK(m.questions.size() == 1 && m.questions[0].multiSelect && !m.questions[0].previewLayout, "ask: a multi-select question never has the preview layout");
            m.choices[0].picks = { 0, 1, 2 };
            m.choices[0].otherText = L"mine";
            CHECK(msg::AskAnswerFor(m.questions[0], m.choices[0]) == L"\"A, b\", \"Say \\\"hi\\\"\", Plain, mine", "ask: Tct's join (quoted where it must be), the own text last");
            CHECK(msg::AskJoinMultiSelect({}) == L"", "ask: an empty multi-select joins to nothing");
            CHECK(msg::AskJsonQuote(L"a\tb\x0001\"\\") == L"\"a\\tb\\u0001\\\"\\\\\"", "ask: JSON.stringify's escapes (\\t, \\u0001, \\\", \\\\)");
            CHECK(msg::AskJsonQuote(std::wstring(1, static_cast<wchar_t>(0xD800))) == L"\"\\ud800\"", "ask: a lone surrogate is escaped (ES2019's well-formed stringify)");
        }
        // The note is trimmed like JavaScript's trim (the PC trims before it sends); an all-blank note is no note.
        {
            auto m = ModelFor(kLayoutInput);
            m.choices[0].picks = { 2 };
            m.choices[0].note = L" \u3000 hi there\n\t";
            Value answers;
            Value annotations;
            msg::ComposeAskAnswers(m.questions, m.choices, answers, annotations);
            const Value* a = annotations.Find(L"Which footer layout do you prefer?");
            CHECK(a && a->StrAt(L"notes") == L"hi there", "ask: the note is trimmed (JS WhiteSpace + LineTerminator, U+3000 included)");
            m.choices[0].note = L"   ";
            msg::ComposeAskAnswers(m.questions, m.choices, answers, annotations);
            a = annotations.Find(L"Which footer layout do you prefer?");
            CHECK(a && !a->Find(L"notes") && a->Find(L"preview"), "ask: a blank note is not sent (the pick's preview still is)");
        }
        // An unanswered question is left out of `answers` (the PC sends only what was answered).
        {
            const std::wstring input = LR"({"questions":[{"question":"One?","header":"A","options":[{"label":"x"},{"label":"y"}]},)"
                                       LR"({"question":"Two?","header":"B","options":[{"label":"p"},{"label":"q"}]}]})";
            auto m = ModelFor(input);
            m.choices[1].picks = { 0 };
            Value answers;
            Value annotations;
            msg::ComposeAskAnswers(m.questions, m.choices, answers, annotations);
            CHECK(JsonEquals(answers, LR"({"Two?":"p"})"), "ask: an unanswered question is not in the answers");
        }
        // The bounds Claude Code keeps for a remote answer.
        {
            Value answers = Value::MkObj();
            answers.Set(L"q", Value::MkStr(std::wstring(msg::kAskAnswerMaxChars, L'x')));
            CHECK(msg::AskAnswersWithinBounds(answers), "ask: an answer of exactly 8,192 characters is within the bounds");
            answers.members[0].second.str.push_back(L'x');
            CHECK(!msg::AskAnswersWithinBounds(answers), "ask: 8,193 characters is not");
            Value many = Value::MkObj();
            for (int i = 0; i < 5; ++i)
            {
                many.Set(L"q" + std::to_wstring(i), Value::MkStr(std::wstring(7000, L'y')));
            }
            CHECK(!msg::AskAnswersWithinBounds(many), "ask: 35,000 characters in all is past the 32,768 bound");
        }
    }

    void TestAskDecisionLines()
    {
        auto m = ModelFor(kLayoutInput);
        m.choices[0].picks = { 1 };
        m.choices[0].note = L"\u05E9\u05DC\u05D5\u05DD \U0001F600"; // Hebrew and an emoji: the line must stay ASCII
        Value answers;
        Value annotations;
        msg::ComposeAskAnswers(m.questions, m.choices, answers, annotations);
        Value input = ParseOrEmpty(kLayoutInput);
        // An input that carries answers already (a model's own) gets them REPLACED, never a second member.
        input.Set(L"answers", ParseOrEmpty(LR"({"stale":"x"})"));
        input.Set(L"annotations", ParseOrEmpty(LR"({"stale":{}})"));
        const std::string line = msg::BuildAskAllowLine(input, answers, annotations);
        CHECK(IsAscii(line), "ask: the allow line is pure ASCII (every non-ASCII character escaped)");
        CHECK(line.find('\n') == std::string::npos, "ask: the allow line is one line");
        const auto parsed = ParseLine(line);
        const Value* hso = parsed ? parsed->Find(L"hookSpecificOutput") : nullptr;
        const Value* decision = hso ? hso->Find(L"decision") : nullptr;
        const Value* updated = decision ? decision->Find(L"updatedInput") : nullptr;
        CHECK(hso && hso->StrAt(L"hookEventName") == L"PermissionRequest", "ask: hookEventName is PermissionRequest");
        CHECK(decision && decision->StrAt(L"behavior") == L"allow", "ask: the behavior is allow");
        CHECK(updated && updated->Find(L"questions") && am::json::Dump(*updated->Find(L"questions")) == am::json::Dump(*ParseOrEmpty(kLayoutInput).Find(L"questions")),
              "ask: updatedInput keeps the questions exactly as the request carried them");
        CHECK(updated && CountMembers(*updated, L"answers") == 1 && CountMembers(*updated, L"annotations") == 1, "ask: answers and annotations appear ONCE (replaced)");
        CHECK(updated && updated->Find(L"answers") && am::json::Dump(*updated->Find(L"answers")) == am::json::Dump(answers), "ask: updatedInput.answers = the composed answers");
        const Value* ann = updated && updated->Find(L"annotations") ? updated->Find(L"annotations")->Find(L"Which footer layout do you prefer?") : nullptr;
        CHECK(ann && ann->StrAt(L"notes") == L"\u05E9\u05DC\u05D5\u05DD \U0001F600", "ask: the Hebrew + emoji note survives the ASCII escape");

        const std::wstring message = L"The user doesn't want to proceed \u2014 because";
        const std::string deny = msg::BuildAskDenyLine(message, false);
        const auto denyParsed = ParseLine(deny);
        const Value* dd = denyParsed && denyParsed->Find(L"hookSpecificOutput") ? denyParsed->Find(L"hookSpecificOutput")->Find(L"decision") : nullptr;
        CHECK(IsAscii(deny) && dd && dd->StrAt(L"behavior") == L"deny" && dd->StrAt(L"message") == message && !dd->BoolAt(L"interrupt", true),
              "ask: the deny line: behavior deny, the message verbatim, interrupt false");
        CHECK(msg::kAskNoDecisionLine == "{}", "ask: the release line is {}");
    }

    // ===== 2. the preview rules =====

    void TestAskPreviewRules()
    {
        using am::ClaudePreviewKind;
        CHECK(am::ClassifyClaudeQuestionPreview(L"") == ClaudePreviewKind::None, "ask preview: empty = None");
        CHECK(am::ClassifyClaudeQuestionPreview(L"  \n\t \u3000") == ClaudePreviewKind::None, "ask preview: whitespace only = None");
        CHECK(am::ClassifyClaudeQuestionPreview(L"\u200B\u200D\u0301") == ClaudePreviewKind::None, "ask preview: zero-width and marks only = None (nothing visible)");
        CHECK(am::ClassifyClaudeQuestionPreview(L"+--+\n|ab|\n+--+") == ClaudePreviewKind::Full, "ask preview: a mockup = Full");
        CHECK(am::ClassifyClaudeQuestionPreview(std::wstring(2000, L'x')) == ClaudePreviewKind::Full, "ask preview: 2,000 UTF-16 units = Full");
        CHECK(am::ClassifyClaudeQuestionPreview(std::wstring(2001, L'x')) == ClaudePreviewKind::Withheld, "ask preview: 2,001 units = Withheld (the PC's bound)");
        CHECK(am::ClassifyClaudeQuestionPreview(L"abc\u2026 [+12 graphemes]") == ClaudePreviewKind::Withheld, "ask preview: Claude Code's truncation marker = Withheld");
        CHECK(am::ClassifyClaudeQuestionPreview(L"abc\u2026 [+ graphemes]") == ClaudePreviewKind::Full, "ask preview: a marker without digits is plain text");
        CHECK(am::ClassifyClaudeQuestionPreview(L"a" + std::wstring(9, static_cast<wchar_t>(0x0301))) == ClaudePreviewKind::Withheld, "ask preview: 9 combining marks in a row = Withheld (the Zalgo guard)");
        CHECK(am::ClassifyClaudeQuestionPreview(L"a" + std::wstring(8, static_cast<wchar_t>(0x0301))) == ClaudePreviewKind::Full, "ask preview: 8 marks is still Full");

        CHECK(am::ClaudeQuestionPreviewEcho(L"a\tb") == L"a       b", "ask echo: a TAB fills to the next multiple of 8 columns");
        CHECK(am::ClaudeQuestionPreviewEcho(L"\u4E2D\tx") == L"\u4E2D      x", "ask echo: a wide character counts two columns before a TAB");
        CHECK(am::ClaudeQuestionPreviewEcho(L"ab\n\tc") == L"ab\n        c", "ask echo: the column count starts again after a LF");
        CHECK(am::ClaudeQuestionPreviewEcho(L"x\r\ny") == L"x\uFFFD\ny", "ask echo: a CR becomes U+FFFD (the PC never draws it)");
        CHECK(am::ClaudeQuestionPreviewEcho(L"\u26A0\uFE0F ok") == L"\u26A0 ok", "ask echo: VS16 is dropped, not replaced");
        CHECK(am::ClaudeQuestionPreviewEcho(L"\u202Ex") == L"\uFFFDx", "ask echo: a bidi override becomes U+FFFD");
        CHECK(am::ClaudeQuestionPreviewEcho(L"\u200Bx") == L"\uFFFDx", "ask echo: a zero-width space (not kept) becomes U+FFFD");
        CHECK(am::ClaudeQuestionPreviewEcho(L"a\u200Db") == L"a\u200Db", "ask echo: ZWJ is one of the kept invisibles");
        CHECK(am::ClaudeQuestionPreviewEcho(std::wstring{ L'x', static_cast<wchar_t>(0xD800), L'y' }) == L"x\uFFFDy", "ask echo: a lone surrogate becomes U+FFFD");
        CHECK(am::ClaudeQuestionPreviewEcho(L"\x001B[31mred") == L"\uFFFD[31mred", "ask echo: ESC becomes U+FFFD (an escape sequence cannot reach the PC's screen)");
        CHECK(am::ClaudeQuestionPreviewEcho(L"\U0001F600 fine") == L"\U0001F600 fine", "ask echo: an emoji (a surrogate PAIR) is kept");

        CHECK(am::ClaudeCellWidth(U'a') == 1 && am::ClaudeCellWidth(0x4E2D) == 2 && am::ClaudeCellWidth(0xFF21) == 2, "ask width: ASCII 1, CJK 2, fullwidth 2");
        CHECK(am::ClaudeCellWidth(0x0301) == 0 && am::ClaudeCellWidth(0x200D) == 0 && am::ClaudeCellWidth(0x0007) == 0, "ask width: a mark, ZWJ, a control = 0");
        CHECK(am::ClaudeCellWidth(0x1F600) == 2, "ask width: an emoji is wide");
    }

    // ===== 3. the request, the signature, the transcript's items =====

    void TestAskRequestAndTranscript()
    {
        const std::wstring line = std::wstring{ LR"({"v":1,"hookPid":123,"claudePid":456,"payload":{"session_id":"sid-1","transcript_path":"C:\\t\\x.jsonl",)" } +
                                  LR"("tool_name":"AskUserQuestion","hook_event_name":"PermissionRequest","tool_input":)" + kLayoutInput + L"}}";
        const auto req = msg::ParseAskRequestLine(line);
        CHECK(req && req->sessionId == L"sid-1" && req->transcriptPath == L"C:\\t\\x.jsonl" && req->toolName == L"AskUserQuestion", "ask request: the payload's facts");
        CHECK(req && req->hookPid == 123 && req->claudePid == 456 && req->agentId.empty(), "ask request: the helper's pids");
        CHECK(req && req->questions.size() == 1 && req->questions[0].options.size() == 3 && req->questions[0].options[1].previewKind == am::ClaudePreviewKind::Full,
              "ask request: the questions with their previews' kinds");
        CHECK(!msg::ParseAskRequestLine(L"{\"v\":2,\"payload\":{\"tool_input\":{}}}"), "ask request: another version is refused");
        CHECK(!msg::ParseAskRequestLine(L"{\"v\":1}"), "ask request: no payload is refused");
        CHECK(!msg::ParseAskRequestLine(L"{\"v\":1,\"payload\":{\"tool_input\":[]}}"), "ask request: a tool_input that is not an object is refused");
        CHECK(!msg::ParseAskRequestLine(L"not json"), "ask request: garbage is refused");
        const auto noPids = msg::ParseAskRequestLine(L"{\"v\":1,\"hookPid\":-5,\"payload\":{\"tool_input\":{}}}");
        CHECK(noPids && noPids->hookPid == 0 && noPids->claudePid == 0 && noPids->questions.empty(), "ask request: a negative or missing pid reads 0; no questions = empty");

        // The signature IS the mirror's question item text, so a hook request and the transcript's tool_use find each other.
        const std::wstring two = LR"({"questions":[{"question":"First?","header":"One","options":[{"label":"a"}]},{"question":"Second?","options":[{"label":"b"}]}]})";
        const std::wstring assistant = std::wstring{ LR"({"type":"assistant","timestamp":"2026-10-19T10:00:00.000Z","message":{"id":"m1","role":"assistant","stop_reason":"tool_use","content":[)" } +
                                       LR"({"type":"tool_use","id":"toolu_42","name":"AskUserQuestion","input":)" + two + L"}]}}";
        const auto items = msg::ClassifyTranscriptLine(ParseOrEmpty(assistant));
        const auto questions = msg::ParseAskQuestions(ParseOrEmpty(two));
        CHECK(items.size() == 1 && items[0].kind == msg::TopicItemKind::Question && items[0].toolUseId == L"toolu_42", "ask transcript: the tool_use is a question item with its id");
        CHECK(!items.empty() && items[0].text == msg::AskQuestionsSignature(questions) && items[0].text == L"One: First?\nSecond?",
              "ask transcript: the question item's text == AskQuestionsSignature (\"header: question\" lines, a header-less one alone)");

        // The answer item: §21.30 carries the NOTES (the mirror dropped them before).
        const auto answerLine = [](std::wstring_view answers, std::wstring_view annotations) {
            return std::wstring{ LR"({"type":"user","timestamp":"2026-10-19T10:00:05.000Z","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"toolu_42","content":"x"}]},)" } +
                   LR"("toolUseResult":{"questions":[{"question":"First?","header":"One"},{"question":"Second?","header":"Two"}],"answers":)" + std::wstring{ answers } +
                   L",\"annotations\":" + std::wstring{ annotations } + L"}}";
        };
        const auto a1 = msg::ClassifyTranscriptLine(ParseOrEmpty(answerLine(LR"J({"First?":"(notes only)"})J", LR"({"First?":{"notes":"none fit"}})")));
        CHECK(a1.size() == 1 && a1[0].kind == msg::TopicItemKind::Answer && a1[0].text == L"\U0001F4DD none fit", "ask transcript: \"(notes only)\" reads as the note alone");
        const auto a2 = msg::ClassifyTranscriptLine(ParseOrEmpty(answerLine(LR"({"First?":"a"})", LR"({"First?":{"preview":"p","notes":"round it"}})")));
        CHECK(a2.size() == 1 && a2[0].text == L"a\n\U0001F4DD round it", "ask transcript: a pick with a note: the label, then the note");
        const auto a3 = msg::ClassifyTranscriptLine(ParseOrEmpty(answerLine(LR"({"First?":"a","Second?":"b"})", LR"({"Second?":{"notes":"n2"}})")));
        CHECK(a3.size() == 1 && a3[0].text == L"One: a\nTwo: b\n\U0001F4DD n2", "ask transcript: several answers as header lines, a note under its answer");
        const auto a4 = msg::ClassifyTranscriptLine(ParseOrEmpty(answerLine(LR"({"First?":"a"})", L"{}")));
        CHECK(a4.size() == 1 && a4[0].kind == msg::TopicItemKind::Answer && a4[0].text == L"a" && a4[0].toolUseId == L"toolu_42",
              "ask transcript: no note = the answer as before, with its tool_use id");
    }

    // ===== 4. the card =====

    void TestAskCallbackAndRouting()
    {
        CHECK(msg::AskCallbackData(L"abc123defg", msg::kAskVerbOption, 2) == L"q:abc123defg:o:2", "ask data: q:<key>:<verb>:<arg>");
        CHECK(msg::AskCallbackData(L"abc123defg", msg::kAskVerbSend) == L"q:abc123defg:s", "ask data: no arg");
        const auto t = msg::ParseAskCallbackData(L"q:abc123defg:o:2");
        CHECK(t && t->key == L"abc123defg" && t->verb == L"o" && t->arg == 2, "ask data: parsed back");
        const auto t2 = msg::ParseAskCallbackData(L"q:k1:nr");
        CHECK(t2 && t2->verb == L"nr" && t2->arg == -1, "ask data: a verb without an arg reads -1");
        CHECK(!msg::ParseAskCallbackData(L"q:ABC:o:1"), "ask data: an upper-case key is refused");
        CHECK(!msg::ParseAskCallbackData(L"q::o"), "ask data: an empty key is refused");
        CHECK(!msg::ParseAskCallbackData(L"q:abc:zz"), "ask data: an unknown verb is refused");
        CHECK(!msg::ParseAskCallbackData(L"q:abc:o:"), "ask data: an empty number is refused");
        CHECK(!msg::ParseAskCallbackData(L"q:abc:o:12a"), "ask data: a number with a letter is refused");
        CHECK(!msg::ParseAskCallbackData(L"q:01234567890123456:o"), "ask data: a key past 16 characters is refused");
        CHECK(!msg::ParseAskCallbackData(L"c:runner:sid"), "ask data: another feature's button is not the card's");
        const std::wstring longest = msg::AskCallbackData(L"0123456789", msg::kAskVerbQueueHeld, 9223372036854775807ll);
        CHECK(longest.size() <= 64, "ask data: the longest button data fits Telegram's 64 bytes");

        auto m = ModelFor(kColorInput);
        CHECK(msg::RouteAskText(m, L"blue it is", false) == msg::AskTextRoute::AskWhich, "ask route: a plain message while the card is open is asked about");
        CHECK(msg::RouteAskText(m, L"/status", false) == msg::AskTextRoute::NotForCard, "ask route: a /command stays a command");
        CHECK(msg::RouteAskText(m, L"  \n/queue", true) == msg::AskTextRoute::NotForCard, "ask route: a /command after blanks too, even as a reply");
        CHECK(msg::RouteAskText(m, L"blue", true) == msg::AskTextRoute::Take, "ask route: a reply to the card is taken");
        m.awaiting = msg::AskTextTarget::Answer;
        CHECK(msg::RouteAskText(m, L"blue", false) == msg::AskTextRoute::Take, "ask route: after ✍️ the next message is taken");
        m.phase = msg::AskCardPhase::Sending;
        CHECK(msg::RouteAskText(m, L"blue", true) == msg::AskTextRoute::NotForCard, "ask route: a card that is not open takes nothing");
        CHECK(msg::AskTextTargetFor(ModelFor(kLayoutInput)) == msg::AskTextTarget::Note && msg::AskTextTargetFor(ModelFor(kColorInput)) == msg::AskTextTarget::Answer,
              "ask route: typed text is the NOTE of a preview question, the own ANSWER of any other");
    }

    void TestAskSteps()
    {
        // One single-select question: a tap SENDS at once (the PC's Enter).
        {
            auto m = ModelFor(kColorInput);
            const auto s = msg::AskTapOption(m, 1);
            CHECK(s.send && s.changed && m.choices[0].picks == std::vector<size_t>{ 1 }, "ask step: one question, a tap answers and sends");
            const auto bad = msg::AskTapOption(m, 7);
            CHECK(!bad.send && !bad.changed, "ask step: an option that is not there does nothing");
        }
        // Two questions: a tap moves on to the next UNANSWERED one; never sends by itself.
        {
            const std::wstring input = LR"({"questions":[{"question":"One?","header":"A","options":[{"label":"x"},{"label":"y"}]},)"
                                       LR"({"question":"Two?","header":"B","options":[{"label":"p"},{"label":"q"}]}]})";
            auto m = ModelFor(input);
            const auto s1 = msg::AskTapOption(m, 0);
            CHECK(!s1.send && m.current == 1, "ask step: two questions: the first tap moves on to question 2");
            const auto s2 = msg::AskTapOption(m, 1);
            CHECK(!s2.send && m.current == 1, "ask step: every question answered: the card stays (📨 Send answers sends)");
            m.current = 0;
            msg::AskTapOption(m, 1);
            CHECK(m.choices[0].picks == std::vector<size_t>{ 1 } && m.current == 0, "ask step: a re-pick replaces the answer; every question answered, so the card stays on the re-picked one");
        }
        // Multi-select: toggles, never sends.
        {
            const std::wstring input = LR"({"questions":[{"question":"Which?","header":"M","multiSelect":true,"options":[{"label":"a"},{"label":"b"},{"label":"c"}]}]})";
            auto m = ModelFor(input);
            msg::AskTapOption(m, 0);
            msg::AskTapOption(m, 2);
            CHECK(m.choices[0].picks == (std::vector<size_t>{ 0, 2 }), "ask step: multi-select toggles on, kept in order");
            const auto s = msg::AskTapOption(m, 0);
            CHECK(!s.send && m.choices[0].picks == std::vector<size_t>{ 2 }, "ask step: a second tap toggles off; nothing is sent");
            const auto t = msg::AskTakeText(m, L"  my own  ", msg::AskTextTarget::Answer);
            CHECK(!t.send && m.choices[0].otherText == L"my own" && m.choices[0].picks == std::vector<size_t>{ 2 }, "ask step: an own answer JOINS the picks (trimmed)");
        }
        // Typed text: the own answer of a single question sends; a note never does.
        {
            auto m = ModelFor(kColorInput);
            m.choices[0].picks = { 0 };
            const auto t = msg::AskTakeText(m, L"Green", msg::AskTextTarget::Answer);
            CHECK(t.send && m.choices[0].otherText == L"Green" && m.choices[0].picks.empty(), "ask step: an own answer replaces the pick and sends");
            auto p = ModelFor(kLayoutInput);
            p.awaiting = msg::AskTextTarget::Note;
            const auto n = msg::AskTakeText(p, L" round corners ", msg::AskTextTarget::Note);
            CHECK(!n.send && n.changed && p.choices[0].note == L"round corners" && p.awaiting == msg::AskTextTarget::None, "ask step: a note is stored (trimmed), nothing sent");
            const auto empty = msg::AskTakeText(p, L"   ", msg::AskTextTarget::Note);
            CHECK(!empty.changed && !empty.toast.empty() && p.choices[0].note == L"round corners", "ask step: an empty text changes nothing and says so");
            const auto tooLong = msg::AskTakeText(p, std::wstring(msg::kAskAnswerMaxChars + 1, L'z'), msg::AskTextTarget::Note);
            CHECK(!tooLong.changed && !tooLong.toast.empty(), "ask step: a text past 8,192 characters is refused with a toast");
            p.phase = msg::AskCardPhase::Answered;
            const auto closed = msg::AskTakeText(p, L"late", msg::AskTextTarget::Note);
            CHECK(!closed.changed && !closed.send, "ask step: a finished card takes nothing");
        }
    }

    void TestAskCardText()
    {
        constexpr int64_t now = 1760000000000;
        // Open, one preview question: its text, its options with their previews in <pre>, the hint.
        {
            auto m = ModelFor(kLayoutInput);
            const std::wstring html = msg::BuildAskCardHtml(m, now);
            CHECK(html.find(L"Claude is asking") != std::wstring::npos && html.find(L"Layout") != std::wstring::npos, "ask card: the head names the header");
            CHECK(html.find(L"Which footer layout do you prefer?") != std::wstring::npos && html.find(L"<b>2. Center</b>") != std::wstring::npos, "ask card: the question and numbered options");
            CHECK(html.find(L"<pre>+--------------+\n|   [LOGO]     |") != std::wstring::npos, "ask card: each preview in a monospace block");
            CHECK(html.find(L"Tap an option to answer") != std::wstring::npos, "ask card: the hint");
            const auto rows = msg::BuildAskCardButtons(m, L"k1");
            // 3 short labels -> 2 + 1, then 📝, 🖼, 💬.
            CHECK(rows.size() == 5 && rows[0].size() == 2 && rows[1].size() == 1, "ask card: short labels two to a row");
            CHECK(rows.size() == 5 && rows[2][0].second == L"q:k1:n" && rows[3][0].second == L"q:k1:i" && rows[4][0].second == L"q:k1:c",
                  "ask card: 📝 a note, 🖼 the previews, 💬 Chat about this");
            bool allParse = true;
            for (const auto& row : rows)
            {
                for (const auto& [label, data] : row)
                {
                    allParse = allParse && msg::ParseAskCallbackData(data).has_value() && data.size() <= 64 && !label.empty();
                }
            }
            CHECK(allParse, "ask card: every button's data parses back and fits");
            m.choices[0].note = L"rounded";
            const auto withNote = msg::BuildAskCardButtons(m, L"k1");
            CHECK(withNote.size() == 6 && withNote[2].size() == 2 && withNote[2][1].second == L"q:k1:nr" && withNote[3][0].second == L"q:k1:ns",
                  "ask card: with a note: ✏️ change / ✖ remove, 📨 send the note alone");
            CHECK(msg::BuildAskCardHtml(m, now).find(L"Note:</b> <i>rounded</i>") != std::wstring::npos, "ask card: the note shows");
            m.awaiting = msg::AskTextTarget::Note;
            const auto waiting = msg::BuildAskCardButtons(m, L"k1");
            CHECK(!waiting.empty() && waiting[0][0].second == L"q:k1:k", "ask card: while a note is awaited, ✖ Cancel comes first");
            CHECK(msg::BuildAskCardHtml(m, now).find(L"Write your note") != std::wstring::npos, "ask card: the hint asks for the note");
        }
        // A plain question: ✍️ write my own; an own answer shows.
        {
            auto m = ModelFor(kColorInput);
            const auto rows = msg::BuildAskCardButtons(m, L"k2");
            CHECK(rows.size() == 3 && rows[1][0].second == L"q:k2:w", "ask card: a plain question offers ✍️ write my own (no 🖼, no 📝)");
            m.choices[0].otherText = L"<b>&";
            CHECK(msg::BuildAskCardHtml(m, now).find(L"&lt;b&gt;&amp;") != std::wstring::npos, "ask card: the user's text is escaped");
        }
        // Two questions: the list, "Question 1 of 2", ▶ and 📨 Send answers (0 of 2).
        {
            const std::wstring input = LR"({"questions":[{"question":"One?","header":"A","options":[{"label":"x"},{"label":"y"}]},)"
                                       LR"({"question":"Two?","header":"B","options":[{"label":"p"},{"label":"q"}]}]})";
            auto m = ModelFor(input);
            const std::wstring html = msg::BuildAskCardHtml(m, now);
            CHECK(html.find(L"2 questions") != std::wstring::npos && html.find(L"Question 1 of 2") != std::wstring::npos && html.find(L"not answered yet") != std::wstring::npos,
                  "ask card: several questions: the list and the position");
            const auto rows = msg::BuildAskCardButtons(m, L"k3");
            bool next = false;
            bool send = false;
            for (const auto& row : rows)
            {
                for (const auto& [label, data] : row)
                {
                    next = next || data == L"q:k3:x";
                    send = send || (data == L"q:k3:s" && label.find(L"(0 of 2)") != std::wstring::npos);
                }
            }
            CHECK(next && send, "ask card: ▶ the next question and 📨 Send answers (0 of 2)");
        }
        // Every phase's text; no buttons once it is not open.
        {
            auto m = ModelFor(kLayoutInput);
            m.choices[0].picks = { 1 };
            m.choices[0].note = L"rounded";
            m.outcomeAtMs = now;
            m.phase = msg::AskCardPhase::Sending;
            CHECK(msg::BuildAskCardHtml(m, now).find(L"Sending your answer") != std::wstring::npos && msg::BuildAskCardButtons(m, L"k").empty(), "ask card: Sending, no buttons");
            m.phase = msg::AskCardPhase::Answered;
            const std::wstring answered = msg::BuildAskCardHtml(m, now);
            CHECK(answered.find(L"Answered from Telegram") != std::wstring::npos && answered.find(L"<b>Center</b>") != std::wstring::npos && answered.find(L"rounded") != std::wstring::npos,
                  "ask card: Answered: the answer and the note");
            m.unconfirmed = true;
            CHECK(msg::BuildAskCardHtml(m, now).find(L"did not confirm") != std::wstring::npos, "ask card: an unconfirmed answer says so");
            m.phase = msg::AskCardPhase::AnsweredAtPc;
            m.atPcAnswer = L"Right";
            CHECK(msg::BuildAskCardHtml(m, now).find(L"Answered at the PC") != std::wstring::npos && msg::BuildAskCardHtml(m, now).find(L"<b>Right</b>") != std::wstring::npos,
                  "ask card: answered at the PC, with the PC's answer");
            m.phase = msg::AskCardPhase::Clarify;
            CHECK(msg::BuildAskCardHtml(m, now).find(L"clarify") != std::wstring::npos, "ask card: Chat about this");
            m.phase = msg::AskCardPhase::Closed;
            m.closedWhy = L"it was closed at the PC";
            CHECK(msg::BuildAskCardHtml(m, now).find(L"No longer open here") != std::wstring::npos && msg::BuildAskCardHtml(m, now).find(L"closed at the PC") != std::wstring::npos,
                  "ask card: Closed, with why");
        }
        // The budget: previews too long for one message are left out (a pointer to 🖼); a huge finished card stays in the budget.
        {
            std::wstring input = LR"({"questions":[{"question":"Which?","header":"Big","options":[)";
            for (int i = 0; i < 3; ++i)
            {
                input += (i ? L"," : L"") + std::wstring{ L"{\"label\":\"L" } + std::to_wstring(i) + L"\",\"preview\":\"" + std::wstring(1900, L'#') + L"\"}";
            }
            input += L"]}]}";
            auto m = ModelFor(input);
            const std::wstring html = msg::BuildAskCardHtml(m, now);
            CHECK(html.size() <= msg::kAskCardMaxChars && html.find(L"do not fit") != std::wstring::npos && html.find(L"<pre>") == std::wstring::npos,
                  "ask card: previews past the budget are left out, with a pointer to the picture");
            std::wstring four = LR"({"questions":[)";
            for (int i = 0; i < 4; ++i)
            {
                four += (i ? L"," : L"") + std::wstring{ L"{\"question\":\"" } + std::wstring(900, static_cast<wchar_t>(L'a' + i)) + L"\",\"header\":\"H" + std::to_wstring(i) +
                        L"\",\"options\":[{\"label\":\"x\"}]}";
            }
            four += L"]}";
            auto big = ModelFor(four);
            for (auto& c : big.choices)
            {
                c.otherText = std::wstring(4000, L'z');
            }
            big.phase = msg::AskCardPhase::Answered;
            CHECK(msg::BuildAskCardHtml(big, now).size() <= msg::kAskCardMaxChars, "ask card: a finished card of four long questions and answers stays in the budget");
            big.phase = msg::AskCardPhase::Open;
            CHECK(msg::BuildAskCardHtml(big, now).size() <= msg::kAskCardMaxChars, "ask card: so does the open card");
        }
        // The "answer or next prompt?" question's words.
        CHECK(msg::AskWhichHtml(ModelFor(kLayoutInput)).find(L"note") != std::wstring::npos && msg::AskWhichHtml(ModelFor(kColorInput)).find(L"answer") != std::wstring::npos,
              "ask card: the question asks about the note (preview) or the answer");
    }

    void TestAskPreviewSheet()
    {
        auto m = ModelFor(kLayoutInput);
        const am::ScreenSnapshot sheet = msg::BuildAskPreviewSheet(m.questions[0]);
        // Per option: title, description, top rule, 3 preview rows, bottom rule (7) + a blank row between options — and the Center
        // option's description ("Logo positioned in the center", 29 columns) wraps at the sheet's 28: one row more.
        CHECK(sheet.rows == 24 && static_cast<int>(sheet.lines.size()) == sheet.rows, "ask sheet: 3 options x 7 rows + 2 blank rows + 1 wrapped description row");
        CHECK(sheet.cols == 28, "ask sheet: the width = the widest preview (at least 24 columns) + the frame's 4");
        CHECK(!sheet.lines.empty() && sheet.lines[0].size() >= 2 && sheet.lines[0][0].text == L"1" && (sheet.lines[0][0].flags & am::kScreenCellBold) != 0,
              "ask sheet: the first row is the option's number and label, bold");
        CHECK(sheet.lines.size() > 3 && !sheet.lines[2].empty() && sheet.lines[2][0].text == L"\x250C" && sheet.lines[3][0].text == L"\x2502",
              "ask sheet: the preview sits in a frame");
        bool fits = true;
        for (const auto& row : sheet.lines)
        {
            int width = 0;
            for (const auto& cell : row)
            {
                width += cell.width;
            }
            fits = fits && width <= sheet.cols;
        }
        CHECK(fits, "ask sheet: no row is wider than the sheet");
        const auto back = am::ParseScreenSnapshot(am::SerializeScreenSnapshot(sheet));
        CHECK(back && back->rows == sheet.rows && back->cols == sheet.cols, "ask sheet: the snapshot's own format reads it back");
        const am::ScreenImage image = am::RenderScreenSnapshotPng(sheet);
        CHECK(!image.png.empty() && image.png.size() > 8 && static_cast<unsigned char>(image.png[1]) == 'P' && image.png[2] == 'N' && image.png[3] == 'G',
              "ask sheet: drawn for real into a PNG (Direct2D + WIC)");
        // A long line is wrapped at the cap; a wide character never splits.
        const std::wstring wide = LR"({"questions":[{"question":"W?","options":[{"label":"a","preview":")" + std::wstring(400, L'\x4E2D') + L"\"},{\"label\":\"b\"}]}]})";
        const auto wq = msg::ParseAskQuestions(ParseOrEmpty(wide));
        const am::ScreenSnapshot ws = msg::BuildAskPreviewSheet(wq[0]);
        bool wideFits = true;
        for (const auto& row : ws.lines)
        {
            int width = 0;
            for (const auto& cell : row)
            {
                width += cell.width;
            }
            wideFits = wideFits && width <= ws.cols;
        }
        CHECK(ws.cols == msg::kAskSheetMaxCols + 4 && wideFits, "ask sheet: a 800-column preview wraps at the cap, wide cells whole");
    }

    // ===== 5. Chat about this =====

    void TestAskChatAboutThis()
    {
        auto m = ModelFor(kLayoutInput);
        m.choices[0].picks = { 1 };
        m.choices[0].note = L" rounded ";
        const std::wstring text = msg::AskChatAboutThisMessage(m.questions, m.choices);
        CHECK(text.rfind(std::wstring{ msg::kAskRejectPrefix }, 0) == 0, "ask chat: starts with Claude Code's rejection prefix");
        CHECK(text.find(L"The user wants to clarify these questions.\n    This means") != std::wstring::npos, "ask chat: the clarify request with its template indent");
        CHECK(text.find(L"- \"Which footer layout do you prefer?\"\n  Answer: Center\n  User notes: rounded") != std::wstring::npos, "ask chat: each question with its answer and note");
        auto u = ModelFor(kColorInput);
        CHECK(msg::AskChatAboutThisMessage(u.questions, u.choices).find(L"(No answer provided)") != std::wstring::npos, "ask chat: an unanswered question says so");
    }

    // ===== 6. the hooks settings and the script =====

    void TestAskHookWiring()
    {
        const std::wstring host = L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
        const std::wstring fwd = L"C:/p/agentmaster-hook.ps1";
        const std::wstring ask = L"C:/p q/agentmaster-ask.ps1";
        const auto with = am::json::Parse(am::BuildHooksSettingsJson(fwd, L"", true, true, host, ask));
        const Value* hooks = with ? with->Find(L"hooks") : nullptr;
        CHECK(hooks && hooks->members.size() == 9, "ask settings: 8 events + PermissionRequest");
        const Value* pr = hooks ? hooks->Find(L"PermissionRequest") : nullptr;
        const Value* entry = pr && pr->type == Value::Type::Arr && !pr->arr.empty() ? &pr->arr[0] : nullptr;
        const Value* inner = entry && entry->Find(L"hooks") && !entry->Find(L"hooks")->arr.empty() ? &entry->Find(L"hooks")->arr[0] : nullptr;
        CHECK(entry && entry->StrAt(L"matcher") == L"AskUserQuestion", "ask settings: matched to AskUserQuestion only");
        const Value* args = inner ? inner->Find(L"args") : nullptr;
        CHECK(inner && inner->StrAt(L"type") == L"command" && inner->StrAt(L"command") == host && args && args->arr.size() == 6 && args->arr[4].str == L"-File" &&
                  args->arr[5].str == ask,
              "ask settings: exec form — PowerShell spawned directly with -File <the ask script> (its space verbatim)");
        CHECK(inner && inner->Find(L"timeout") && inner->Find(L"timeout")->num == am::kAskHookTimeoutSeconds && am::kAskHookTimeoutSeconds == 86400, "ask settings: a day's timeout");
        const auto without = am::json::Parse(am::BuildHooksSettingsJson(fwd, L"", true, true, host));
        CHECK(without && without->Find(L"hooks") && without->Find(L"hooks")->members.size() == 8 && !without->Find(L"hooks")->Find(L"PermissionRequest"),
              "ask settings: no ask script (the bot is off) = the classic 8 events");
        const auto shell = am::json::Parse(am::BuildHooksSettingsJson(fwd, L"", true, true, L"", ask));
        CHECK(shell && shell->Find(L"hooks") && !shell->Find(L"hooks")->Find(L"PermissionRequest"), "ask settings: shell form (an old claude) gets no question hook");

        const std::wstring script = am::BuildAskHookScript(L"C:\\Users\\Some User\\.agentmaster");
        CHECK(script.find(L"'C:\\Users\\Some User\\.agentmaster\\bridge.json'") != std::wstring::npos, "ask script: the profile's bridge.json, single-quoted");
        CHECK(script.find(L"{{AM_") == std::wstring::npos, "ask script: every token replaced");
        CHECK(script.find(L"\"AskUserQuestion\"") != std::wstring::npos && script.find(L"agent_id") != std::wstring::npos && script.find(L".ask") != std::wstring::npos,
              "ask script: AskUserQuestion only, subagents answered at once, the .ask pipe");
        CHECK(script.find(L"UTF8Encoding") != std::wstring::npos && script.find(L"OpenStandardInput") != std::wstring::npos, "ask script: stdin decoded as UTF-8 from the raw stream");
        bool ascii = true;
        for (const wchar_t c : script)
        {
            ascii = ascii && c < 0x80;
        }
        CHECK(ascii, "ask script: pure ASCII (written with a BOM for a non-ASCII profile path)");
    }

    // ===== 7. the ask pipe end to end =====

    /// <summary>What a fake hook helper saw on the pipe.</summary>
    struct AskClientResult
    {
        bool connected{ false };
        std::string reply; // the reply line without its '\n' ("" = none before the pipe closed or the time ran out)
    };

    /// <summary>
    /// A fake question hook: connects (waiting up to 5 s for an instance), writes `line` + "\n", and reads one reply line within
    /// `readMs` — or, with `leave`, closes at once without reading (the helper going away).
    /// </summary>
    /// <param name="pipe">The pipe's name.</param>
    /// <param name="line">The request line (UTF-8).</param>
    /// <param name="readMs">The read's bound.</param>
    /// <param name="leave">Close right after the write.</param>
    /// <returns>What it saw.</returns>
    AskClientResult AskClient(const std::wstring& pipe, const std::string& line, DWORD readMs, bool leave = false)
    {
        AskClientResult r;
        HANDLE h = INVALID_HANDLE_VALUE;
        for (int i = 0; i < 100 && h == INVALID_HANDLE_VALUE; ++i)
        {
            h = ::CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                ::WaitNamedPipeW(pipe.c_str(), 50);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (h == INVALID_HANDLE_VALUE)
        {
            return r;
        }
        r.connected = true;
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const std::string out = line + "\n";
        DWORD n = 0;
        if (!::WriteFile(h, out.data(), static_cast<DWORD>(out.size()), nullptr, &ov) && ::GetLastError() == ERROR_IO_PENDING)
        {
            ::WaitForSingleObject(ov.hEvent, 2000);
        }
        ::GetOverlappedResult(h, &ov, &n, FALSE);
        if (!leave)
        {
            const ULONGLONG deadline = ::GetTickCount64() + readMs;
            std::string got;
            char buf[512];
            while (got.find('\n') == std::string::npos && ::GetTickCount64() < deadline)
            {
                ::ResetEvent(ov.hEvent);
                DWORD read = 0;
                BOOL ok = ::ReadFile(h, buf, sizeof(buf), nullptr, &ov);
                if (!ok && ::GetLastError() == ERROR_IO_PENDING)
                {
                    const ULONGLONG now = ::GetTickCount64();
                    if (::WaitForSingleObject(ov.hEvent, static_cast<DWORD>(deadline > now ? deadline - now : 0)) != WAIT_OBJECT_0)
                    {
                        ::CancelIoEx(h, &ov);
                        ::GetOverlappedResult(h, &ov, &read, TRUE);
                        break;
                    }
                    ok = TRUE;
                }
                if (!ok || !::GetOverlappedResult(h, &ov, &read, FALSE) || read == 0)
                {
                    break; // the pipe broke (the bridge disconnected)
                }
                got.append(buf, read);
            }
            if (const size_t nl = got.find('\n'); nl != std::string::npos)
            {
                r.reply = got.substr(0, nl);
            }
        }
        ::CloseHandle(ov.hEvent);
        ::CloseHandle(h);
        return r;
    }

    /// <summary>A request line as the hook writes it, for a session and a tool.</summary>
    /// <param name="session">The payload's session_id.</param>
    /// <param name="tool">The payload's tool_name.</param>
    /// <param name="agent">The payload's agent_id ("" = none).</param>
    /// <returns>The UTF-8 line.</returns>
    std::string AskRequestUtf8(const std::string& session, const std::string& tool = "AskUserQuestion", const std::string& agent = "")
    {
        return std::string{ "{\"v\":1,\"hookPid\":11,\"claudePid\":22,\"payload\":{\"session_id\":\"" } + session + "\",\"tool_name\":\"" + tool + "\"" +
               (agent.empty() ? std::string{} : ",\"agent_id\":\"" + agent + "\"") + ",\"tool_input\":{\"questions\":[{\"question\":\"Q?\",\"options\":[{\"label\":\"a\"}]}]}}}";
    }

    /// <summary>Waits (up to 5 s) for the registry to hold an entry of `session` in `state`.</summary>
    /// <param name="reg">The registry.</param>
    /// <param name="session">The session.</param>
    /// <param name="state">The state.</param>
    /// <returns>The entry, or nullopt when it did not come.</returns>
    std::optional<am::AskEntryInfo> WaitForEntry(const am::AskRegistry& reg, const std::wstring& session, am::AskEntryState state)
    {
        for (int i = 0; i < 500; ++i)
        {
            for (const auto& e : reg.Snapshot())
            {
                if (e.sessionId == session && e.state == state)
                {
                    return e;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return std::nullopt;
    }

    void TestAskPipe()
    {
        const std::wstring pipe = L"\\\\.\\pipe\\agentmaster-m5-ask-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
        auto reg = std::make_shared<am::AskRegistry>();
        std::atomic<int> changes{ 0 };
        reg->SetListener([&changes]() { ++changes; });
        reg->SetAcceptor([](const std::wstring& sid) { return sid.rfind(L"take", 0) == 0; });
        am::AskBridge bridge{ pipe, reg };
        CHECK(bridge.Start(), "ask pipe: the bridge starts");

        // Refused at the door: the session is not one the acceptor takes -> "{}" (or a closed pipe), no entry.
        {
            const auto r = AskClient(pipe, AskRequestUtf8("nope"), 3000);
            CHECK(r.connected && (r.reply == "{}" || r.reply.empty()), "ask pipe: a refused request hears {} at once");
            CHECK(reg->Snapshot().empty(), "ask pipe: a refused request leaves no entry");
        }
        // Malformed / another tool / a subagent: refused without asking the acceptor.
        {
            CHECK(AskClient(pipe, "not json at all", 3000).reply != "x", "ask pipe: garbage is answered (no hang)");
            CHECK(AskClient(pipe, AskRequestUtf8("take-1", "Bash"), 3000).connected && reg->Snapshot().empty(), "ask pipe: another tool's request is refused");
            CHECK(AskClient(pipe, AskRequestUtf8("take-2", "AskUserQuestion", "agent-7"), 3000).connected && reg->Snapshot().empty(), "ask pipe: a subagent's request is refused");
        }
        // Answered: the decision line reaches the helper; the entry reads Delivered.
        {
            AskClientResult r;
            std::thread client([&]() { r = AskClient(pipe, AskRequestUtf8("take-a"), 8000); });
            const auto e = WaitForEntry(*reg, L"take-a", am::AskEntryState::Open);
            CHECK(e.has_value() && e->requestLine.find(L"\"take-a\"") != std::wstring::npos, "ask pipe: an accepted request is OPEN in the registry, its line decoded");
            const bool answered = e && reg->Answer(e->id, "{\"hookSpecificOutput\":{}}");
            client.join();
            CHECK(answered && r.reply == "{\"hookSpecificOutput\":{}}", "ask pipe: the decision line reaches the helper");
            CHECK(e && WaitForEntry(*reg, L"take-a", am::AskEntryState::Delivered).has_value(), "ask pipe: the entry reads Delivered");
            CHECK(e && !reg->Answer(e->id, "again") && !reg->Release(e->id, L"late"), "ask pipe: a delivered request takes no second decision or release");
            if (e)
            {
                reg->Forget(e->id);
            }
        }
        // Released: "{}" reaches the helper; the entry reads Released with its why.
        {
            AskClientResult r;
            std::thread client([&]() { r = AskClient(pipe, AskRequestUtf8("take-r"), 8000); });
            const auto e = WaitForEntry(*reg, L"take-r", am::AskEntryState::Open);
            const bool released = e && reg->Release(e->id, L"answered at the PC");
            client.join();
            CHECK(released && r.reply == "{}", "ask pipe: a release reaches the helper as {}");
            const auto done = WaitForEntry(*reg, L"take-r", am::AskEntryState::Released);
            CHECK(done && done->why == L"answered at the PC", "ask pipe: the entry reads Released, with why");
            if (e)
            {
                reg->Forget(e->id);
            }
        }
        // The helper leaves (Esc at the PC ends the hook; claude exits): the entry reads Gone, and a late answer is refused.
        {
            const auto r = AskClient(pipe, AskRequestUtf8("take-g"), 0, true);
            CHECK(r.connected, "ask pipe: the leaving helper connected");
            const auto gone = WaitForEntry(*reg, L"take-g", am::AskEntryState::Gone);
            CHECK(gone.has_value(), "ask pipe: a helper that leaves makes its request Gone");
            CHECK(gone && !reg->Answer(gone->id, "late"), "ask pipe: an answer to a gone request is refused (the card then says the PC was first)");
            if (gone)
            {
                reg->Forget(gone->id);
            }
        }
        // Forget keeps an open request; the listener heard every change.
        {
            AskClientResult r;
            std::thread client([&]() { r = AskClient(pipe, AskRequestUtf8("take-f"), 8000); });
            const auto e = WaitForEntry(*reg, L"take-f", am::AskEntryState::Open);
            if (e)
            {
                reg->Forget(e->id);
            }
            CHECK(e && WaitForEntry(*reg, L"take-f", am::AskEntryState::Open).has_value(), "ask pipe: Forget leaves an OPEN request alone");
            CHECK(changes.load() >= 6, "ask pipe: the listener heard the changes (opened, answered, written, released, gone, ...)");
            // Stop with a helper waiting: it hears {} (or a closed pipe) and the entry is Gone ("Agentmaster closed").
            bridge.Stop();
            client.join();
            CHECK(r.reply == "{}" || r.reply.empty(), "ask pipe: Stop releases a waiting helper");
            const auto closed = WaitForEntry(*reg, L"take-f", am::AskEntryState::Gone);
            CHECK(closed && closed->why == L"Agentmaster closed", "ask pipe: after Stop the request is Gone (Agentmaster closed)");
        }
        // Without an acceptor every request is refused (the bot is off).
        {
            am::AskRegistry bare;
            CHECK(bare.Open(L"take-x", L"{}", nullptr) == 0, "ask pipe: no acceptor = refused");
        }
    }
}

void TestAskQuestion()
{
    std::wprintf(L"[ask] Claude Code's AskUserQuestion answered from Telegram (MESSAGING.md \u00A721.30)\n");
    TestAskComposeGoldens();
    TestAskDecisionLines();
    TestAskPreviewRules();
    TestAskRequestAndTranscript();
    TestAskCallbackAndRouting();
    TestAskSteps();
    TestAskCardText();
    TestAskPreviewSheet();
    TestAskChatAboutThis();
    TestAskHookWiring();
    TestAskPipe();
}
