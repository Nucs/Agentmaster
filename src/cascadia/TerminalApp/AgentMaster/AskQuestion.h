// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — AskQuestion: Claude Code's AskUserQuestion dialog, ANSWERED FROM TELEGRAM (doc/agentmaster/INTERACTIVE_TOOLS.md
// §6, MESSAGING.md §21.30). The user's request (2026-10-19): "close the gap, enabling telegram picking a choice, including the
// user manual text input. Use telegram to the best of its ability to produce a managed message that will cover this interaction
// and including custom answer".
//
// How an answer travels:
//   1. claude.exe opens the question dialog at the PC and, beside it, runs the PermissionRequest hook that hooks-settings.json
//      registers for AskUserQuestion (ClaudeSpawn.cpp BuildAskHookScript: <profile>\agentmaster-ask.ps1). The PC dialog stays
//      fully usable; the first answer — from the PC or from the hook — wins (measured on 2.1.291, INTERACTIVE_TOOLS.md §5).
//   2. The helper sends one REQUEST LINE (ParseAskRequestLine) over the ask pipe `\\.\pipe\agentmaster.<pid>.ask` (AskBridge,
//      HooksBridge.h) and waits for one reply line.
//   3. The messaging service shows the open question in the session's topic as ONE MANAGED MESSAGE, the QUESTION CARD
//      (BuildAskCardHtml + BuildAskCardButtons), and edits it as the user picks, toggles, writes an answer of their own or a
//      note, moves between questions, or asks Claude to clarify.
//   4. The answer is composed EXACTLY as the PC dialog composes it (ComposeAskAnswers mirrors 2.1.291's `nJ`; the multi-select
//      join is `Tct`; the notes-only sentinel `_0t`; the picked preview's echo `zet`), so the model reads the very text a PC answer
//      gives it — the probe proved the hook's answer byte for byte (INTERACTIVE_TOOLS.md §5, scenarios D and E).
//   5. The helper prints the decision line (BuildAskAllowLine / BuildAskDenyLine), and Claude Code takes it as the answer.
//
// Header-only and pure (no Win32, no network, no service): the m5 suite tests every function here (tests_ask.cpp). The preview
// rules need Unicode properties, so they live in ClaudeInvisibleChars.cpp (ICU).

#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator> // std::begin / std::end over the verb table
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ClaudeInvisibleChars.h" // ClassifyClaudeQuestionPreview / ClaudeQuestionPreviewEcho / ClaudeCellWidth (ICU)
#include "Json.h"
#include "ScreenSnapshot.h" // the previews as a picture (BuildAskPreviewSheet)
#include "TelegramFormat.h" // TgEscapeHtml, TgClockText

namespace Agentmaster::Msg
{
    // ===== Claude Code's own constants (2.1.291) and this feature's bounds ==================================================

    /// <summary>The answer the PC dialog sends for a NOTE WITHOUT A PICK on a preview question (`_0t`); the model then reads "(no option selected)".</summary>
    inline constexpr std::wstring_view kAskNotesOnlyAnswer{ L"(notes only)" };
    /// <summary>The longest single answer Claude Code's own remote cards admit (admitCardAnswer); Agentmaster holds a phone answer to it too.</summary>
    inline constexpr size_t kAskAnswerMaxChars = 8192;
    /// <summary>All answers of one call together, the same function's bound.</summary>
    inline constexpr size_t kAskAnswersTotalMaxChars = 32768;
    /// <summary>The longest request line the ask pipe takes from the hook helper (the payload holds every option's preview: 16 × 2,000 characters at most).</summary>
    inline constexpr size_t kAskRequestMaxBytes = 1024 * 1024;
    /// <summary>
    /// The prefix Claude Code puts before a rejection's feedback (cancelAndAbort's text) — a hook's `deny` message is used
    /// verbatim, so "Chat about this" from the phone must carry it itself to read like the PC's.
    /// </summary>
    inline constexpr std::wstring_view kAskRejectPrefix{ L"The user doesn't want to proceed with this tool use. The tool use was rejected (eg. if it was a file edit, "
                                                         L"the new_string was NOT written to the file). To tell you how to proceed, the user said:\n" };
    /// <summary>What the dialog shows instead of a WITHHELD preview (`Xge`); the card shows the same words.</summary>
    inline constexpr std::wstring_view kAskWithheldPreviewText{ L"(preview cannot be shown in full \x2014 compare the option labels and descriptions instead)" };
    /// <summary>
    /// How long the mirror's PLAIN question item (the transcript's tool_use) waits for its hook request before it is posted as the
    /// show-only "answer it at the PC" message. The helper starts 0.10–0.25 s after the tool_use line and Windows PowerShell takes
    /// ~0.5 s more, so a card normally pairs within ~1 s; a session whose claude has no ask hook (started before the bot was on,
    /// older than 2.1.139) only waits this long.
    /// </summary>
    inline constexpr int64_t kAskPairWaitMs = 8000;
    /// <summary>
    /// How long a phone answer may wait for the transcript's confirmation (the tool result, written 0.01–0.02 s after the hook
    /// answers on 2.1.291) before the card says it was not confirmed — a refusal of the rewritten input by a deny / ask rule, or
    /// a claude that ignored the hook's answer.
    /// </summary>
    inline constexpr int64_t kAskConfirmTimeoutMs = 60000;
    /// <summary>The card's HTML budget: Telegram's 4,096-character message limit, with room for the entities' own text.</summary>
    inline constexpr size_t kAskCardMaxChars = 3800;
    /// <summary>The longest option label a button shows (Telegram cuts a long button anyway; a cut label stays recognizable).</summary>
    inline constexpr size_t kAskButtonLabelMaxChars = 40;
    /// <summary>The widest preview picture, in columns: wider lines are hard-wrapped, as the PC's preview box does at its width.</summary>
    inline constexpr int kAskSheetMaxCols = 160;

    // ===== the request (from the hook helper) ==================================================================================

    /// <summary>One option of a question, as the model wrote it, with its preview's kind (ClassifyClaudeQuestionPreview).</summary>
    struct AskOption
    {
        std::wstring label; // the answer the PC sends when it is picked
        std::wstring description; // shown under the label (the PC's preview layout hides it; the card shows it)
        std::wstring preview; // "" when the option has none
        ClaudePreviewKind previewKind{ ClaudePreviewKind::None };
    };

    /// <summary>One question of an AskUserQuestion call.</summary>
    struct AskQuestion
    {
        std::wstring question; // the text, also the KEY of its answer and annotation
        std::wstring header; // the short chip ("Layout"); may be empty
        bool multiSelect{ false };
        std::vector<AskOption> options;
        /// <summary>
        /// The PC's PREVIEW layout (`P4e`): single-select and at least one option's preview is Full or Withheld. Only such a question
        /// has a Notes field, and only its answer carries annotations.
        /// </summary>
        bool previewLayout{ false };
    };

    /// <summary>What the hook helper sends: the hook payload's facts and the call's input.</summary>
    struct AskRequest
    {
        std::wstring sessionId; // the payload's session_id: Claude's CURRENT conversation (Agentmaster's registry id)
        std::wstring transcriptPath;
        std::wstring toolName; // "AskUserQuestion" (the helper drops anything else)
        std::wstring agentId; // non-empty for a subagent (the helper answers those at once, so the engine never sees one)
        uint32_t hookPid{ 0 }; // the waiting helper (PowerShell)
        uint32_t claudePid{ 0 }; // its parent claude.exe; 0 from the shipped helper, which looks it up only after the request (~1.7 s)
        json::Value toolInput; // the call's input verbatim: the answer is this object plus `answers` and `annotations`
        std::vector<AskQuestion> questions;
    };

    namespace detail
    {
        /// <summary>
        /// JavaScript's String.prototype.trim: strips the WhiteSpace and LineTerminator sets from both ends (the PC trims a note
        /// with it before it sends it).
        /// </summary>
        /// <param name="s">The text.</param>
        /// <returns>The trimmed text.</returns>
        /// <exception cref="std::bad_alloc">If the copy cannot be allocated.</exception>
        inline std::wstring AskJsTrim(std::wstring_view s)
        {
            const auto isSpace = [](wchar_t c) {
                switch (c)
                {
                case 0x09:
                case 0x0A:
                case 0x0B:
                case 0x0C:
                case 0x0D:
                case 0x20:
                case 0xA0:
                case 0x1680:
                case 0x2028:
                case 0x2029:
                case 0x202F:
                case 0x205F:
                case 0x3000:
                case 0xFEFF:
                    return true;
                default:
                    return c >= 0x2000 && c <= 0x200A;
                }
            };
            size_t a = 0;
            size_t b = s.size();
            while (a < b && isSpace(s[a]))
            {
                ++a;
            }
            while (b > a && isSpace(s[b - 1]))
            {
                --b;
            }
            return std::wstring{ s.substr(a, b - a) };
        }

        /// <summary>
        /// Sets an object member, REPLACING one of the same key (json::Value::Set appends, and a duplicate key would be two
        /// members; Claude Code's answers are keyed by question text, so a repeated question text keeps one entry, as a JS object does).
        /// </summary>
        /// <param name="obj">The object (anything else is left alone).</param>
        /// <param name="key">The key.</param>
        /// <param name="value">The value.</param>
        /// <exception cref="std::bad_alloc">If the member cannot be stored.</exception>
        inline void AskSetMember(json::Value& obj, const std::wstring& key, json::Value value)
        {
            if (obj.type != json::Value::Type::Obj)
            {
                return;
            }
            for (auto& [k, v] : obj.members)
            {
                if (k == key)
                {
                    v = std::move(value);
                    return;
                }
            }
            obj.members.emplace_back(key, std::move(value));
        }

        /// <summary>Removes every member with this key from an object (no-op for anything else).</summary>
        /// <param name="obj">The object.</param>
        /// <param name="key">The key.</param>
        inline void AskRemoveMember(json::Value& obj, std::wstring_view key) noexcept
        {
            if (obj.type != json::Value::Type::Obj)
            {
                return;
            }
            obj.members.erase(std::remove_if(obj.members.begin(), obj.members.end(), [key](const auto& m) { return m.first == key; }), obj.members.end());
        }

        /// <summary>Cuts a one-line text for a button or a summary: line breaks become spaces, and a longer text ends in "…".</summary>
        /// <param name="text">The text.</param>
        /// <param name="maxChars">The cap (at least 2).</param>
        /// <returns>The cut text.</returns>
        /// <exception cref="std::bad_alloc">If the copy cannot be allocated.</exception>
        inline std::wstring AskCut(std::wstring_view text, size_t maxChars)
        {
            std::wstring out;
            out.reserve((std::min)(text.size(), maxChars));
            for (const wchar_t c : text)
            {
                out.push_back((c == L'\r' || c == L'\n' || c == L'\t') ? L' ' : c);
            }
            if (out.size() > maxChars)
            {
                out.resize(maxChars > 1 ? maxChars - 1 : 0);
                // Never end on the high half of a surrogate pair: the cut character would be a lone surrogate on the wire.
                if (!out.empty() && out.back() >= 0xD800 && out.back() <= 0xDBFF)
                {
                    out.pop_back();
                }
                out += L"\x2026";
            }
            return out;
        }
    }

    /// <summary>
    /// The questions of an AskUserQuestion input: each question's text, header, multiSelect and options (label, description,
    /// preview and its kind), and whether it gets the PC's preview layout. A malformed question (not an object) or option is left
    /// out, as the mirror's question item leaves it out.
    /// </summary>
    /// <param name="toolInput">The call's input ({"questions": [...]}).</param>
    /// <returns>The questions; empty when the input holds none (the caller then releases the dialog to the PC).</returns>
    /// <exception cref="std::bad_alloc">If the questions cannot be allocated.</exception>
    inline std::vector<AskQuestion> ParseAskQuestions(const json::Value& toolInput)
    {
        std::vector<AskQuestion> out;
        const json::Value* qs = toolInput.type == json::Value::Type::Obj ? toolInput.Find(L"questions") : nullptr;
        if (!qs || qs->type != json::Value::Type::Arr)
        {
            return out;
        }
        for (const auto& q : qs->arr)
        {
            if (q.type != json::Value::Type::Obj)
            {
                continue;
            }
            AskQuestion aq;
            aq.question = q.StrAt(L"question");
            aq.header = q.StrAt(L"header");
            aq.multiSelect = q.BoolAt(L"multiSelect", false);
            if (const auto* opts = q.Find(L"options"); opts && opts->type == json::Value::Type::Arr)
            {
                for (const auto& o : opts->arr)
                {
                    if (o.type != json::Value::Type::Obj)
                    {
                        continue;
                    }
                    AskOption ao;
                    ao.label = o.StrAt(L"label");
                    ao.description = o.StrAt(L"description");
                    if (const auto* pv = o.Find(L"preview"); pv && pv->type == json::Value::Type::Str)
                    {
                        ao.preview = pv->str;
                        ao.previewKind = ClassifyClaudeQuestionPreview(ao.preview);
                    }
                    aq.options.push_back(std::move(ao));
                }
            }
            // `P4e`: single-select, and some option's mapped preview exists (Full or Withheld; a None preview maps to "none").
            aq.previewLayout = !aq.multiSelect && std::any_of(aq.options.begin(), aq.options.end(), [](const AskOption& o) { return o.previewKind != ClaudePreviewKind::None; });
            out.push_back(std::move(aq));
        }
        return out;
    }

    /// <summary>
    /// Parses the hook helper's request line: {"v":1,"hookPid":N,"claudePid":N,"payload":{the PermissionRequest payload}}.
    /// </summary>
    /// <param name="line">The line, without its '\n'.</param>
    /// <returns>The request, or nullopt for anything that is not a version-1 request with an object `tool_input`.</returns>
    /// <exception cref="std::bad_alloc">If the request cannot be allocated.</exception>
    inline std::optional<AskRequest> ParseAskRequestLine(std::wstring_view line)
    {
        auto root = json::Parse(line);
        if (!root || root->type != json::Value::Type::Obj || root->I64At(L"v", 0) != 1)
        {
            return std::nullopt;
        }
        const json::Value* payload = root->Find(L"payload");
        if (!payload || payload->type != json::Value::Type::Obj)
        {
            return std::nullopt;
        }
        const json::Value* input = payload->Find(L"tool_input");
        if (!input || input->type != json::Value::Type::Obj)
        {
            return std::nullopt;
        }
        AskRequest r;
        r.sessionId = payload->StrAt(L"session_id");
        r.transcriptPath = payload->StrAt(L"transcript_path");
        r.toolName = payload->StrAt(L"tool_name");
        r.agentId = payload->StrAt(L"agent_id");
        const int64_t hookPid = root->I64At(L"hookPid", 0);
        const int64_t claudePid = root->I64At(L"claudePid", 0);
        r.hookPid = hookPid > 0 && hookPid <= 0xFFFFFFFFll ? static_cast<uint32_t>(hookPid) : 0;
        r.claudePid = claudePid > 0 && claudePid <= 0xFFFFFFFFll ? static_cast<uint32_t>(claudePid) : 0;
        r.toolInput = *input;
        r.questions = ParseAskQuestions(r.toolInput);
        return r;
    }

    /// <summary>
    /// The questions' CONTENT KEY: each question as "header: question" (or the question alone when it has no header), joined by
    /// "\n" — exactly the text the transcript mirror's question item carries (MessagingModels.h ClassifyTranscriptLine), so a hook
    /// request and the transcript's tool_use of the same dialog find each other (the hook payload has no tool_use id).
    /// </summary>
    /// <param name="questions">The questions.</param>
    /// <returns>The key; "" for no questions.</returns>
    /// <exception cref="std::bad_alloc">If the key cannot be allocated.</exception>
    inline std::wstring AskQuestionsSignature(const std::vector<AskQuestion>& questions)
    {
        std::wstring out;
        for (const auto& q : questions)
        {
            if (!out.empty())
            {
                out += L"\n";
            }
            out += (q.header.empty() ? std::wstring{} : q.header + L": ") + q.question;
        }
        return out;
    }

    // ===== the answers =========================================================================================================

    /// <summary>What the user chose for ONE question so far.</summary>
    struct AskChoice
    {
        std::vector<size_t> picks; // option indexes, ascending (single-select: at most one)
        std::wstring otherText; // an answer of the user's own ("Type something." at the PC); a multi-select one joins the picks
        std::wstring note; // a preview question's note (the PC's Notes field)
    };

    /// <summary>
    /// JavaScript's JSON.stringify of a string: quotes it and escapes `"`, `\`, the C0 controls (\b \f \n \r \t, else \u00xx) and
    /// lone surrogates (\udxxx, ES2019's well-formed stringify); everything else stays as it is. Tct quotes a label with it.
    /// </summary>
    /// <param name="s">The string.</param>
    /// <returns>The quoted string.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring AskJsonQuote(std::wstring_view s)
    {
        static constexpr wchar_t kHex[] = L"0123456789abcdef";
        std::wstring out;
        out.reserve(s.size() + 2);
        out.push_back(L'"');
        for (size_t i = 0; i < s.size(); ++i)
        {
            const wchar_t c = s[i];
            switch (c)
            {
            case L'"':
                out += L"\\\"";
                continue;
            case L'\\':
                out += L"\\\\";
                continue;
            case L'\b':
                out += L"\\b";
                continue;
            case L'\f':
                out += L"\\f";
                continue;
            case L'\n':
                out += L"\\n";
                continue;
            case L'\r':
                out += L"\\r";
                continue;
            case L'\t':
                out += L"\\t";
                continue;
            default:
                break;
            }
            const bool high = c >= 0xD800 && c <= 0xDBFF;
            const bool low = c >= 0xDC00 && c <= 0xDFFF;
            const bool paired = (high && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) || (low && i > 0 && s[i - 1] >= 0xD800 && s[i - 1] <= 0xDBFF);
            if (c < 0x20 || ((high || low) && !paired))
            {
                out += L"\\u";
                out.push_back(kHex[(c >> 12) & 0xF]);
                out.push_back(kHex[(c >> 8) & 0xF]);
                out.push_back(kHex[(c >> 4) & 0xF]);
                out.push_back(kHex[c & 0xF]);
                continue;
            }
            out.push_back(c);
        }
        out.push_back(L'"');
        return out;
    }

    /// <summary>
    /// A multi-select answer as Claude Code writes it (`Tct`): the items joined with ", ", an item that holds ", " or `"` written
    /// as a JSON string (so the model, and Claude Code's own parse `H`, can split the answer again).
    /// </summary>
    /// <param name="items">The picked labels, then an answer of the user's own when there is one.</param>
    /// <returns>The joined answer; "" for no items.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::wstring AskJoinMultiSelect(const std::vector<std::wstring>& items)
    {
        std::wstring out;
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i)
            {
                out += L", ";
            }
            const std::wstring& t = items[i];
            out += (t.find(L", ") != std::wstring::npos || t.find(L'"') != std::wstring::npos) ? AskJsonQuote(t) : t;
        }
        return out;
    }

    /// <summary>
    /// The ANSWER the PC dialog would send for one question with this choice: the picked label (single-select); the picked labels
    /// plus the user's own text, joined by AskJoinMultiSelect (multi-select); the user's own text; "(notes only)" for a note
    /// without a pick on a preview question; else "" (unanswered — the question is left out of `answers`).
    /// </summary>
    /// <param name="q">The question.</param>
    /// <param name="c">The choice.</param>
    /// <returns>The answer, or "".</returns>
    /// <exception cref="std::bad_alloc">If the answer cannot be allocated.</exception>
    inline std::wstring AskAnswerFor(const AskQuestion& q, const AskChoice& c)
    {
        if (q.multiSelect)
        {
            std::vector<std::wstring> items;
            for (const size_t i : c.picks)
            {
                if (i < q.options.size())
                {
                    items.push_back(q.options[i].label);
                }
            }
            if (!c.otherText.empty())
            {
                items.push_back(c.otherText);
            }
            return AskJoinMultiSelect(items);
        }
        if (!c.picks.empty() && c.picks.front() < q.options.size())
        {
            return q.options[c.picks.front()].label;
        }
        if (!c.otherText.empty())
        {
            return c.otherText;
        }
        if (q.previewLayout && !detail::AskJsTrim(c.note).empty())
        {
            return std::wstring{ kAskNotesOnlyAnswer };
        }
        return {};
    }

    /// <summary>Does the question have an answer yet (AskAnswerFor is not empty)?</summary>
    /// <param name="q">The question.</param>
    /// <param name="c">The choice.</param>
    /// <returns>True when answered.</returns>
    /// <exception cref="std::bad_alloc">If the answer cannot be computed.</exception>
    inline bool AskQuestionAnswered(const AskQuestion& q, const AskChoice& c)
    {
        return !AskAnswerFor(q, c).empty();
    }

    /// <summary>
    /// The `answers` and `annotations` objects of the PC dialog's answer (`nJ`): every answered question's answer under its text;
    /// for a PREVIEW question, an annotation {preview?, notes?} — the picked option's preview as the dialog echoes it
    /// (ClaudeQuestionPreviewEcho, only for a Full preview; the PC sends it with or without a note) and the trimmed note — left
    /// out when it has neither.
    /// </summary>
    /// <param name="questions">The questions.</param>
    /// <param name="choices">One choice per question (missing ones read as empty).</param>
    /// <param name="answers">Out: the answers object.</param>
    /// <param name="annotations">Out: the annotations object ({} when no question has one — the PC sends that too).</param>
    /// <exception cref="std::bad_alloc">If the objects cannot be allocated.</exception>
    inline void ComposeAskAnswers(const std::vector<AskQuestion>& questions, const std::vector<AskChoice>& choices, json::Value& answers, json::Value& annotations)
    {
        answers = json::Value::MkObj();
        annotations = json::Value::MkObj();
        for (size_t i = 0; i < questions.size(); ++i)
        {
            const AskQuestion& q = questions[i];
            const AskChoice c = i < choices.size() ? choices[i] : AskChoice{};
            const std::wstring answer = AskAnswerFor(q, c);
            if (!answer.empty())
            {
                detail::AskSetMember(answers, q.question, json::Value::MkStr(answer));
            }
            if (!q.previewLayout)
            {
                continue;
            }
            json::Value ann = json::Value::MkObj();
            // The preview rides only on a picked option (the PC matches the answer against the option labels: a "(notes only)"
            // or typed answer names no option, so it carries none).
            if (!q.multiSelect && !c.picks.empty() && c.picks.front() < q.options.size())
            {
                const AskOption& o = q.options[c.picks.front()];
                if (o.previewKind == ClaudePreviewKind::Full)
                {
                    ann.Set(L"preview", json::Value::MkStr(ClaudeQuestionPreviewEcho(o.preview)));
                }
            }
            if (std::wstring note = detail::AskJsTrim(c.note); !note.empty())
            {
                ann.Set(L"notes", json::Value::MkStr(std::move(note)));
            }
            if (!ann.members.empty())
            {
                detail::AskSetMember(annotations, q.question, std::move(ann));
            }
        }
    }

    /// <summary>
    /// Do the answers keep Claude Code's remote-answer bounds (each at most kAskAnswerMaxChars, all together at most
    /// kAskAnswersTotalMaxChars, in UTF-16 units as JavaScript counts)? Claude Code checks its own remote cards with them; a hook's
    /// answer is not checked, so Agentmaster applies them before it sends.
    /// </summary>
    /// <param name="answers">The answers object.</param>
    /// <returns>True when within the bounds.</returns>
    inline bool AskAnswersWithinBounds(const json::Value& answers) noexcept
    {
        size_t total = 0;
        for (const auto& [k, v] : answers.members)
        {
            const size_t n = v.type == json::Value::Type::Str ? v.str.size() : 0;
            if (n > kAskAnswerMaxChars)
            {
                return false;
            }
            total += n;
        }
        return total <= kAskAnswersTotalMaxChars;
    }

    /// <summary>
    /// A JSON text (json::Dump's output) as pure ASCII UTF-8: every character from U+007F on becomes \uXXXX. Json::Dump writes such
    /// characters only inside strings, where the escape is valid JSON, so the meaning is unchanged — and the hook helper's stdout
    /// then carries no byte a console code page could garble.
    /// </summary>
    /// <param name="jsonText">The JSON text.</param>
    /// <returns>The ASCII bytes.</returns>
    /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
    inline std::string AskJsonToAsciiUtf8(std::wstring_view jsonText)
    {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(jsonText.size() + 16);
        for (const wchar_t c : jsonText)
        {
            if (c < 0x7F)
            {
                out.push_back(static_cast<char>(c));
                continue;
            }
            out += "\\u";
            out.push_back(kHex[(c >> 12) & 0xF]);
            out.push_back(kHex[(c >> 8) & 0xF]);
            out.push_back(kHex[(c >> 4) & 0xF]);
            out.push_back(kHex[c & 0xF]);
        }
        return out;
    }

    /// <summary>
    /// The hook's ALLOW decision, the line the helper prints: {"hookSpecificOutput":{"hookEventName":"PermissionRequest",
    /// "decision":{"behavior":"allow","updatedInput": the call's input + answers + annotations}}} — the input with the two keys
    /// replaced, as the PC builds it ({...input, answers, annotations}).
    /// </summary>
    /// <param name="toolInput">The call's input as the request carried it.</param>
    /// <param name="answers">ComposeAskAnswers' answers.</param>
    /// <param name="annotations">ComposeAskAnswers' annotations.</param>
    /// <returns>The line (ASCII JSON, no line break).</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::string BuildAskAllowLine(const json::Value& toolInput, const json::Value& answers, const json::Value& annotations)
    {
        json::Value updated = toolInput.type == json::Value::Type::Obj ? toolInput : json::Value::MkObj();
        detail::AskRemoveMember(updated, L"answers");
        detail::AskRemoveMember(updated, L"annotations");
        updated.Set(L"answers", answers);
        updated.Set(L"annotations", annotations);
        json::Value decision = json::Value::MkObj();
        decision.Set(L"behavior", json::Value::MkStr(L"allow"));
        decision.Set(L"updatedInput", std::move(updated));
        json::Value hso = json::Value::MkObj();
        hso.Set(L"hookEventName", json::Value::MkStr(L"PermissionRequest"));
        hso.Set(L"decision", std::move(decision));
        json::Value root = json::Value::MkObj();
        root.Set(L"hookSpecificOutput", std::move(hso));
        return AskJsonToAsciiUtf8(json::Dump(root));
    }

    /// <summary>
    /// The hook's DENY decision: Claude Code makes `message` the tool's result verbatim (measured: the transcript holds exactly the
    /// message, is_error true), and `interrupt` true aborts the turn like Esc.
    /// </summary>
    /// <param name="message">The tool result's text.</param>
    /// <param name="interrupt">End the turn.</param>
    /// <returns>The line (ASCII JSON, no line break).</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::string BuildAskDenyLine(std::wstring_view message, bool interrupt)
    {
        json::Value decision = json::Value::MkObj();
        decision.Set(L"behavior", json::Value::MkStr(L"deny"));
        decision.Set(L"message", json::Value::MkStr(std::wstring{ message }));
        decision.Set(L"interrupt", json::Value::MkBool(interrupt));
        json::Value hso = json::Value::MkObj();
        hso.Set(L"hookEventName", json::Value::MkStr(L"PermissionRequest"));
        hso.Set(L"decision", std::move(decision));
        json::Value root = json::Value::MkObj();
        root.Set(L"hookSpecificOutput", std::move(hso));
        return AskJsonToAsciiUtf8(json::Dump(root));
    }

    /// <summary>The reply that leaves the dialog to the PC (no decision): the helper prints nothing and exits.</summary>
    inline constexpr std::string_view kAskNoDecisionLine{ "{}" };

    /// <summary>
    /// "Chat about this" from the phone: the rejection Claude Code makes of it at the PC (`R4e` + `Dno`), with its rejection
    /// prefix, so the model reads the same request to clarify — each question with its answer so far ("(No answer provided)"
    /// when none) and, for a preview question, the note. The turn goes on: the model asks what to clarify.
    /// </summary>
    /// <param name="questions">The questions.</param>
    /// <param name="choices">The choices so far.</param>
    /// <returns>The deny message.</returns>
    /// <exception cref="std::bad_alloc">If the message cannot be allocated.</exception>
    inline std::wstring AskChatAboutThisMessage(const std::vector<AskQuestion>& questions, const std::vector<AskChoice>& choices)
    {
        // The feedback is a template literal in the bundle: its lines 2-4 and "Questions asked:" keep their 4-space indent.
        std::wstring out{ kAskRejectPrefix };
        out += L"The user wants to clarify these questions.\n"
               L"    This means they may have additional information, context or questions for you.\n"
               L"    Take their response into account and then reformulate the questions if appropriate.\n"
               L"    Start by asking them what they would like to clarify.\n"
               L"\n"
               L"    Questions asked:\n";
        for (size_t i = 0; i < questions.size(); ++i)
        {
            const AskQuestion& q = questions[i];
            const AskChoice c = i < choices.size() ? choices[i] : AskChoice{};
            if (i)
            {
                out += L"\n";
            }
            out += L"- \"" + q.question + L"\"\n";
            const std::wstring answer = AskAnswerFor(q, c);
            out += answer.empty() ? std::wstring{ L"  (No answer provided)" } : L"  Answer: " + answer;
            if (q.previewLayout)
            {
                if (const std::wstring note = detail::AskJsTrim(c.note); !note.empty())
                {
                    out += L"\n  User notes: " + note;
                }
            }
        }
        return out;
    }

    // ===== the card: one managed Telegram message per open dialog ============================================================

    /// <summary>Where the card is in its life.</summary>
    enum class AskCardPhase
    {
        Open, // the dialog waits; the card takes taps and text
        Sending, // the phone's answer is handed to the helper; waiting for the transcript to confirm it
        Answered, // answered from the phone (confirmed, or unconfirmed after kAskConfirmTimeoutMs)
        AnsweredAtPc, // answered at the PC first
        Clarify, // the phone asked Claude to clarify ("Chat about this")
        Closed, // gone without an answer from here: cancelled at the PC, the session ended, the app restarted, 24 h passed
    };

    /// <summary>What the user's NEXT typed message becomes (after ✍️ or 📝 was tapped).</summary>
    enum class AskTextTarget
    {
        None,
        Answer, // the current question's answer of the user's own
        Note, // the current preview question's note
    };

    /// <summary>The card's whole state: the questions, the choices, which question it shows and its phase.</summary>
    struct AskCardModel
    {
        std::vector<AskQuestion> questions;
        std::vector<AskChoice> choices; // one per question
        size_t current{ 0 }; // the question the card shows and its buttons act on
        AskCardPhase phase{ AskCardPhase::Open };
        AskTextTarget awaiting{ AskTextTarget::None };
        int64_t outcomeAtMs{ 0 }; // when the card left Open (the final edit's clock)
        std::wstring atPcAnswer; // AnsweredAtPc: the answer as the transcript recorded it
        std::wstring closedWhy; // Closed: why ("it was cancelled at the PC, or its session ended")
        bool unconfirmed{ false }; // Answered: the transcript never confirmed it in time
    };

    /// <summary>
    /// A card's model for a request: the questions and one empty choice each, showing the first question.
    /// </summary>
    /// <param name="questions">The request's questions.</param>
    /// <returns>The model.</returns>
    /// <exception cref="std::bad_alloc">If the model cannot be allocated.</exception>
    inline AskCardModel MakeAskCardModel(std::vector<AskQuestion> questions)
    {
        AskCardModel m;
        m.questions = std::move(questions);
        m.choices.resize(m.questions.size());
        return m;
    }

    // ----- the buttons' callback data: "q:<card key>:<verb>[:<arg>]" (at most ~34 bytes; Telegram allows 64) -----
    // The key is the card's random id (MakeAskCardKey in the service), so a button from an earlier run never acts on a new card.

    /// <summary>Pick option `arg` (single-select: answer it; multi-select: toggle it).</summary>
    inline constexpr std::wstring_view kAskVerbOption{ L"o" };
    /// <summary>Write an answer of my own: the next message (or a reply to the card) is taken as the current question's answer.</summary>
    inline constexpr std::wstring_view kAskVerbWrite{ L"w" };
    /// <summary>Add (or change) the current preview question's note: the next message is taken as the note.</summary>
    inline constexpr std::wstring_view kAskVerbNote{ L"n" };
    /// <summary>Remove the current question's note.</summary>
    inline constexpr std::wstring_view kAskVerbNoteRemove{ L"nr" };
    /// <summary>Send the note alone as the answer (one question: "(notes only)" + the note).</summary>
    inline constexpr std::wstring_view kAskVerbNoteSend{ L"ns" };
    /// <summary>A multi-select question is done: send it (one question) or go on to the next question.</summary>
    inline constexpr std::wstring_view kAskVerbDone{ L"d" };
    /// <summary>Show the previous question.</summary>
    inline constexpr std::wstring_view kAskVerbPrev{ L"p" };
    /// <summary>Show the next question.</summary>
    inline constexpr std::wstring_view kAskVerbNext{ L"x" };
    /// <summary>Send the answers of every question (several questions).</summary>
    inline constexpr std::wstring_view kAskVerbSend{ L"s" };
    /// <summary>Ask Claude to clarify ("Chat about this").</summary>
    inline constexpr std::wstring_view kAskVerbChat{ L"c" };
    /// <summary>Send the current question's previews as a picture.</summary>
    inline constexpr std::wstring_view kAskVerbPicture{ L"i" };
    /// <summary>Stop waiting for a typed answer or note.</summary>
    inline constexpr std::wstring_view kAskVerbCancelWrite{ L"k" };
    /// <summary>Use the held message `arg` (a message id) as the answer or the note.</summary>
    inline constexpr std::wstring_view kAskVerbUseHeld{ L"u" };
    /// <summary>Queue the held message `arg` as the session's next prompt instead (D30).</summary>
    inline constexpr std::wstring_view kAskVerbQueueHeld{ L"h" };

    /// <summary>A card button's callback data.</summary>
    /// <param name="key">The card's key.</param>
    /// <param name="verb">One of the kAskVerb* verbs.</param>
    /// <param name="arg">The verb's number (an option index, a message id), or -1 for none.</param>
    /// <returns>"q:&lt;key&gt;:&lt;verb&gt;" or "q:&lt;key&gt;:&lt;verb&gt;:&lt;arg&gt;".</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring AskCallbackData(std::wstring_view key, std::wstring_view verb, int64_t arg = -1)
    {
        std::wstring out = L"q:" + std::wstring{ key } + L":" + std::wstring{ verb };
        if (arg >= 0)
        {
            out += L":" + std::to_wstring(arg);
        }
        return out;
    }

    /// <summary>A parsed card button tap.</summary>
    struct AskTap
    {
        std::wstring key;
        std::wstring verb;
        int64_t arg{ -1 };
    };

    /// <summary>
    /// Parses a card button's callback data (AskCallbackData's form): a key of 1-16 characters [0-9a-z], a known verb, an
    /// optional non-negative number of at most 18 digits.
    /// </summary>
    /// <param name="data">The callback data.</param>
    /// <returns>The tap, or nullopt for anything else (another feature's button).</returns>
    /// <exception cref="std::bad_alloc">If the tap cannot be allocated.</exception>
    inline std::optional<AskTap> ParseAskCallbackData(std::wstring_view data)
    {
        if (data.substr(0, 2) != L"q:")
        {
            return std::nullopt;
        }
        data.remove_prefix(2);
        const size_t c1 = data.find(L':');
        if (c1 == std::wstring_view::npos || c1 == 0 || c1 > 16)
        {
            return std::nullopt;
        }
        AskTap tap;
        tap.key = std::wstring{ data.substr(0, c1) };
        for (const wchar_t c : tap.key)
        {
            if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z')))
            {
                return std::nullopt;
            }
        }
        std::wstring_view rest = data.substr(c1 + 1);
        const size_t c2 = rest.find(L':');
        tap.verb = std::wstring{ rest.substr(0, c2) };
        static constexpr std::wstring_view kVerbs[] = { kAskVerbOption, kAskVerbWrite, kAskVerbNote, kAskVerbNoteRemove, kAskVerbNoteSend, kAskVerbDone, kAskVerbPrev,
                                                        kAskVerbNext,   kAskVerbSend,  kAskVerbChat, kAskVerbPicture,    kAskVerbCancelWrite, kAskVerbUseHeld, kAskVerbQueueHeld };
        if (std::find(std::begin(kVerbs), std::end(kVerbs), std::wstring_view{ tap.verb }) == std::end(kVerbs))
        {
            return std::nullopt;
        }
        if (c2 != std::wstring_view::npos)
        {
            const std::wstring_view num = rest.substr(c2 + 1);
            if (num.empty() || num.size() > 18)
            {
                return std::nullopt;
            }
            int64_t v = 0;
            for (const wchar_t c : num)
            {
                if (c < L'0' || c > L'9')
                {
                    return std::nullopt;
                }
                v = v * 10 + (c - L'0');
            }
            tap.arg = v;
        }
        return tap;
    }

    /// <summary>
    /// What a typed message means for the CURRENT question: the note of a preview question (the PC's Notes field is its only free
    /// text), else an answer of the user's own (the PC's "Type something.").
    /// </summary>
    /// <param name="m">The card.</param>
    /// <returns>Note or Answer (None when the card has no question).</returns>
    inline AskTextTarget AskTextTargetFor(const AskCardModel& m) noexcept
    {
        if (m.current >= m.questions.size())
        {
            return AskTextTarget::None;
        }
        return m.questions[m.current].previewLayout ? AskTextTarget::Note : AskTextTarget::Answer;
    }

    /// <summary>How the service routes a plain text message of the topic while a card is there.</summary>
    enum class AskTextRoute
    {
        NotForCard, // the card is not open, or the text is a /command: the topic's usual handling (a queued prompt, D30)
        Take, // the user asked for it (✍️ / 📝) or replied to the card: it becomes the answer or the note
        AskWhich, // an open card and a plain message: ask whether it is the answer or the next prompt
    };

    /// <summary>
    /// Routes a typed message of the topic while a card is there. Commands stay commands; a message the user wrote after tapping
    /// ✍️ or 📝, or as a REPLY to the card, is taken; any other message is asked about (the card's question is still open, and a
    /// queued prompt could not go before it is answered anyway).
    /// </summary>
    /// <param name="m">The card.</param>
    /// <param name="text">The message's text.</param>
    /// <param name="replyToCard">The message replies to the card (or to its write prompt).</param>
    /// <returns>The route.</returns>
    inline AskTextRoute RouteAskText(const AskCardModel& m, std::wstring_view text, bool replyToCard) noexcept
    {
        if (m.phase != AskCardPhase::Open || m.questions.empty())
        {
            return AskTextRoute::NotForCard;
        }
        size_t a = 0;
        while (a < text.size() && (text[a] == L' ' || text[a] == L'\n' || text[a] == L'\r' || text[a] == L'\t'))
        {
            ++a;
        }
        if (a < text.size() && text[a] == L'/')
        {
            return AskTextRoute::NotForCard;
        }
        if (m.awaiting != AskTextTarget::None || replyToCard)
        {
            return AskTextRoute::Take;
        }
        return AskTextRoute::AskWhich;
    }

    /// <summary>What a tap or a typed text did to the card.</summary>
    struct AskStep
    {
        bool send{ false }; // the answer is complete: compose and send it now
        bool changed{ false }; // the card's text or buttons changed: edit it
        std::wstring toast; // the tap's answer (a short line; "" = none)
    };

    namespace detail
    {
        /// <summary>
        /// After a question was answered on a card of several questions: the next UNANSWERED question after the current one
        /// (wrapping around), or the current one when every question is answered (the card then offers 📨 Send).
        /// </summary>
        /// <param name="m">The card.</param>
        inline void AskAdvance(AskCardModel& m)
        {
            const size_t n = m.questions.size();
            for (size_t step = 1; step < n; ++step)
            {
                const size_t i = (m.current + step) % n;
                if (!AskQuestionAnswered(m.questions[i], m.choices[i]))
                {
                    m.current = i;
                    return;
                }
            }
        }
    }

    /// <summary>
    /// A tap on an option. Single-select: the option becomes the answer (an own answer is dropped, a note kept); a one-question
    /// card then SENDS at once (the PC's Enter answers a one-question dialog), a card of several questions moves on to the next
    /// unanswered one. Multi-select: the option is toggled.
    /// </summary>
    /// <param name="m">The card.</param>
    /// <param name="option">The option's index.</param>
    /// <returns>What happened.</returns>
    /// <exception cref="std::bad_alloc">If a choice cannot grow.</exception>
    inline AskStep AskTapOption(AskCardModel& m, size_t option)
    {
        AskStep step;
        if (m.phase != AskCardPhase::Open || m.current >= m.questions.size())
        {
            step.toast = L"This question is not open any more.";
            return step;
        }
        const AskQuestion& q = m.questions[m.current];
        AskChoice& c = m.choices[m.current];
        if (option >= q.options.size())
        {
            return step;
        }
        m.awaiting = AskTextTarget::None;
        step.changed = true;
        if (q.multiSelect)
        {
            const auto it = std::lower_bound(c.picks.begin(), c.picks.end(), option);
            if (it != c.picks.end() && *it == option)
            {
                c.picks.erase(it);
            }
            else
            {
                c.picks.insert(it, option);
            }
            return step;
        }
        c.picks.assign(1, option);
        c.otherText.clear();
        if (m.questions.size() == 1)
        {
            step.send = true;
            return step;
        }
        detail::AskAdvance(m);
        return step;
    }

    /// <summary>
    /// A typed text taken by the card. A NOTE: stored on the current question (trimmed), nothing sent. An ANSWER of the user's own:
    /// single-select — it replaces a pick and a one-question card SENDS at once (the PC's "Type something." + Enter), several
    /// questions move on; multi-select — it joins the picks and the user taps ✅ Done.
    /// </summary>
    /// <param name="m">The card.</param>
    /// <param name="text">The typed text.</param>
    /// <param name="target">What it is (AskTextTargetFor, or what ✍️ / 📝 asked for).</param>
    /// <returns>What happened.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be stored.</exception>
    inline AskStep AskTakeText(AskCardModel& m, std::wstring_view text, AskTextTarget target)
    {
        AskStep step;
        if (m.phase != AskCardPhase::Open || m.current >= m.questions.size() || target == AskTextTarget::None)
        {
            step.toast = L"This question is not open any more.";
            return step;
        }
        std::wstring t = detail::AskJsTrim(text);
        if (t.empty())
        {
            step.toast = L"That message is empty \x2014 nothing changed.";
            return step;
        }
        if (t.size() > kAskAnswerMaxChars)
        {
            step.toast = L"That text is longer than 8,192 characters \x2014 send a shorter one.";
            return step;
        }
        const AskQuestion& q = m.questions[m.current];
        AskChoice& c = m.choices[m.current];
        m.awaiting = AskTextTarget::None;
        step.changed = true;
        if (target == AskTextTarget::Note)
        {
            c.note = std::move(t);
            step.toast = m.questions.size() == 1 ? L"\U0001F4DD Note added \x2014 tap an option, or send the note alone." : L"\U0001F4DD Note added.";
            return step;
        }
        c.otherText = std::move(t);
        if (q.multiSelect)
        {
            step.toast = L"\x270D\xFE0F Added \x2014 tap \x2705 Done when your picks are complete.";
            return step;
        }
        c.picks.clear();
        if (m.questions.size() == 1)
        {
            step.send = true;
            return step;
        }
        detail::AskAdvance(m);
        return step;
    }

    /// <summary>
    /// The answer summary of a card for its final text and the log: per question "&lt;header or Q#&gt; → &lt;answer&gt;" (or the
    /// answer alone for one question), with the note on its own line ("📝 …"); "(no answer)" for an unanswered question.
    /// HTML-escaped, ready for the card.
    /// </summary>
    /// <param name="m">The card.</param>
    /// <param name="answerCut">The longest answer shown (characters; a longer one ends in "…"). The card's final text shortens it
    /// when the whole message would pass kAskCardMaxChars.</param>
    /// <param name="noteCut">The longest note shown.</param>
    /// <returns>The summary lines, joined by "\n".</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring AskAnswerSummaryHtml(const AskCardModel& m, size_t answerCut = 300, size_t noteCut = 600)
    {
        std::wstring out;
        for (size_t i = 0; i < m.questions.size(); ++i)
        {
            const AskQuestion& q = m.questions[i];
            const AskChoice c = i < m.choices.size() ? m.choices[i] : AskChoice{};
            std::wstring answer = AskAnswerFor(q, c);
            if (answer == kAskNotesOnlyAnswer)
            {
                answer.clear(); // "(notes only)" reads as the note line alone
            }
            if (!out.empty())
            {
                out += L"\n";
            }
            const std::wstring name = q.header.empty() ? L"Q" + std::to_wstring(i + 1) : q.header;
            const std::wstring note = q.previewLayout ? detail::AskJsTrim(c.note) : std::wstring{};
            const std::wstring shown = answer.empty() ? (note.empty() ? std::wstring{ L"<i>(no answer)</i>" } : std::wstring{}) : L"<b>" + TgEscapeHtml(detail::AskCut(answer, answerCut)) + L"</b>";
            if (m.questions.size() > 1)
            {
                out += TgEscapeHtml(detail::AskCut(name, 40)) + (shown.empty() ? std::wstring{} : L" \x2192 " + shown);
            }
            else
            {
                out += shown;
            }
            if (!note.empty())
            {
                out += (shown.empty() && m.questions.size() == 1 ? L"" : L"\n") + std::wstring{ L"\U0001F4DD <i>" } + TgEscapeHtml(detail::AskCut(note, noteCut)) + L"</i>";
            }
        }
        return out;
    }

    namespace detail
    {
        /// <summary>
        /// The current question's block of the card: its text, a multi-select hint, and every option (number, label,
        /// description, the picked mark) with its preview in a monospace block when `withPreviews` (a Withheld preview shows
        /// Claude Code's own placeholder; an option without one shows nothing).
        /// </summary>
        /// <param name="m">The card.</param>
        /// <param name="withPreviews">Show the previews inline.</param>
        /// <returns>The HTML.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring AskQuestionBlockHtml(const AskCardModel& m, bool withPreviews)
        {
            const AskQuestion& q = m.questions[m.current];
            const AskChoice& c = m.choices[m.current];
            std::wstring out = L"<b>" + TgEscapeHtml(q.question) + L"</b>";
            if (q.multiSelect)
            {
                out += L"\n<i>Pick one or more.</i>";
            }
            out += L"\n";
            for (size_t i = 0; i < q.options.size(); ++i)
            {
                const AskOption& o = q.options[i];
                const bool picked = std::find(c.picks.begin(), c.picks.end(), i) != c.picks.end();
                out += L"\n" + std::wstring{ picked ? (q.multiSelect ? L"\x2611 " : L"\x2705 ") : L"" } + L"<b>" + std::to_wstring(i + 1) + L". " + TgEscapeHtml(o.label) + L"</b>";
                if (!o.description.empty())
                {
                    out += L" \x2014 " + TgEscapeHtml(o.description);
                }
                if (!withPreviews || q.multiSelect)
                {
                    continue; // the PC never shows a multi-select question's previews
                }
                if (o.previewKind == ClaudePreviewKind::Full)
                {
                    out += L"\n<pre>" + TgEscapeHtml(ClaudeQuestionPreviewEcho(o.preview)) + L"</pre>";
                }
                else if (o.previewKind == ClaudePreviewKind::Withheld)
                {
                    out += L"\n<i>" + TgEscapeHtml(kAskWithheldPreviewText) + L"</i>";
                }
            }
            return out;
        }
    }

    /// <summary>
    /// The card's text (Telegram HTML). Open: "❓ Claude is asking" (with the header, or how many questions), a list of every
    /// question with its answer so far (several questions), the current question with its options and their previews, the note and
    /// the user's own answer, and a hint — "write your answer as your next message" while ✍️ / 📝 waits. Sending: the answer on its
    /// way. Answered / AnsweredAtPc / Clarify / Closed: the outcome with its clock time and the answers, the question kept for the
    /// record. The previews are left out (with a pointer to 🖼) when they would push the text past kAskCardMaxChars.
    /// </summary>
    /// <param name="m">The card.</param>
    /// <param name="nowMs">The clock for the outcome's time ("HH:MM" today, else "D Mon HH:MM").</param>
    /// <returns>The HTML.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildAskCardHtml(const AskCardModel& m, int64_t nowMs)
    {
        if (m.questions.empty())
        {
            return L"\x2753 <b>Claude asked a question</b> <i>(its text could not be read)</i>";
        }
        const size_t n = m.questions.size();
        const auto questionList = [&](size_t cut) {
            std::wstring out;
            for (size_t i = 0; i < n; ++i)
            {
                out += (out.empty() ? L"" : L"\n") + std::wstring{ L"<i>" } + TgEscapeHtml(detail::AskCut(m.questions[i].question, cut)) + L"</i>";
            }
            return out;
        };
        const std::wstring when = m.outcomeAtMs > 0 ? L" \x00B7 " + TgClockText(m.outcomeAtMs, nowMs) : std::wstring{};
        // A finished card keeps the questions and the answers for the record. Four questions with long answers and notes can pass
        // the budget at the full cuts, so each outcome is rendered at the full cuts first, then shorter ones: never a cut tag.
        const auto outcome = [&](size_t questionCut, size_t answerCut, size_t noteCut) -> std::wstring {
            switch (m.phase)
            {
            case AskCardPhase::Answered:
                return L"\x2705 <b>Answered from Telegram</b>" + when + L"\n" + questionList(questionCut) + L"\n" + AskAnswerSummaryHtml(m, answerCut, noteCut) +
                       (m.unconfirmed ? L"\n\x26A0\xFE0F <i>Claude Code did not confirm this answer \x2014 look at the session at the PC.</i>" : L"");
            case AskCardPhase::AnsweredAtPc:
                return L"\x2705 <b>Answered at the PC</b>" + when + L"\n" + questionList(questionCut) +
                       (m.atPcAnswer.empty() ? std::wstring{} : L"\n<b>" + TgEscapeHtml(detail::AskCut(m.atPcAnswer, answerCut * 5)) + L"</b>");
            case AskCardPhase::Clarify:
                return L"\U0001F4AC <b>You asked Claude to clarify</b>" + when + L"\n" + questionList(questionCut);
            case AskCardPhase::Closed:
                return L"\x23F9\xFE0F <b>No longer open here</b>" + when + (m.closedWhy.empty() ? std::wstring{} : L" \x2014 " + TgEscapeHtml(m.closedWhy)) + L"\n" +
                       questionList(questionCut);
            case AskCardPhase::Sending:
                return L"\x23F3 <b>Sending your answer to Claude\x2026</b>\n" + questionList(questionCut) + L"\n" + AskAnswerSummaryHtml(m, answerCut, noteCut);
            case AskCardPhase::Open:
            default:
                return {};
            }
        };
        if (m.phase != AskCardPhase::Open)
        {
            std::wstring text = outcome(300, 300, 600);
            if (text.size() > kAskCardMaxChars)
            {
                text = outcome(120, 120, 200);
            }
            if (text.size() > kAskCardMaxChars)
            {
                text = outcome(60, 60, 80);
            }
            return text;
        }
        const AskQuestion& q = m.questions[m.current];
        const AskChoice& c = m.choices[m.current];
        std::wstring head = L"\x2753 <b>Claude is asking</b>";
        if (n > 1)
        {
            head += L" \x00B7 " + std::to_wstring(n) + L" questions";
        }
        else if (!q.header.empty())
        {
            head += L" \x00B7 <i>" + TgEscapeHtml(q.header) + L"</i>";
        }
        std::wstring list;
        if (n > 1)
        {
            for (size_t i = 0; i < n; ++i)
            {
                const AskQuestion& qi = m.questions[i];
                std::wstring answer = AskAnswerFor(qi, m.choices[i]);
                if (answer == kAskNotesOnlyAnswer)
                {
                    answer = L"\U0001F4DD note only";
                }
                const std::wstring name = qi.header.empty() ? detail::AskCut(qi.question, 40) : qi.header;
                list += L"\n" + std::wstring{ i == m.current ? L"\x25B6 " : L"   " } + L"<b>" + std::to_wstring(i + 1) + L". " + TgEscapeHtml(detail::AskCut(name, 40)) + L"</b> \x2014 " +
                        (answer.empty() ? std::wstring{ L"<i>not answered yet</i>" } : TgEscapeHtml(detail::AskCut(answer, 60)));
            }
            list += L"\n";
        }
        std::wstring tail;
        if (!c.otherText.empty())
        {
            tail += L"\n\x270D\xFE0F <b>Your own answer:</b> <i>" + TgEscapeHtml(detail::AskCut(c.otherText, 600)) + L"</i>";
        }
        if (q.previewLayout && !c.note.empty())
        {
            tail += L"\n\U0001F4DD <b>Note:</b> <i>" + TgEscapeHtml(detail::AskCut(c.note, 600)) + L"</i>";
        }
        std::wstring hint;
        if (m.awaiting == AskTextTarget::Answer)
        {
            hint = L"\x270D\xFE0F <b>Write your answer</b> and send it here \x2014 as your next message, or as a reply to this one.";
        }
        else if (m.awaiting == AskTextTarget::Note)
        {
            hint = L"\U0001F4DD <b>Write your note</b> and send it here \x2014 as your next message, or as a reply to this one.";
        }
        else if (q.multiSelect)
        {
            hint = L"<i>Tap the options you want, then \x2705 Done.</i>";
        }
        else if (q.previewLayout)
        {
            hint = n == 1 ? L"<i>Tap an option to answer. A note goes with the option you tap \x2014 or alone, as your own answer.</i>"
                          : L"<i>Tap an option. A note goes with it \x2014 or alone, as your own answer.</i>";
        }
        else
        {
            hint = n == 1 ? L"<i>Tap an option to answer \x2014 or \x270D\xFE0F write your own.</i>" : L"<i>Tap an option \x2014 or \x270D\xFE0F write your own.</i>";
        }
        if (n > 1)
        {
            hint += L"\n<i>When you are done, \U0001F4E8 Send answers.</i>";
        }
        const std::wstring prefix = head + L"\n" + list + L"\n" + (n > 1 ? L"<b>Question " + std::to_wstring(m.current + 1) + L" of " + std::to_wstring(n) + L"</b>\n" : L"");
        std::wstring withPreviews = prefix + detail::AskQuestionBlockHtml(m, true) + L"\n" + tail + L"\n\n" + hint;
        if (withPreviews.size() <= kAskCardMaxChars)
        {
            return withPreviews;
        }
        const bool anyFull = std::any_of(q.options.begin(), q.options.end(), [](const AskOption& o) { return o.previewKind == ClaudePreviewKind::Full; });
        std::wstring without = prefix + detail::AskQuestionBlockHtml(m, false) + L"\n" + tail +
                               (q.previewLayout && anyFull ? L"\n\U0001F5BC <i>The previews do not fit in one message \x2014 tap \U0001F5BC Previews.</i>" : L"") + L"\n\n" + hint;
        if (without.size() > kAskCardMaxChars)
        {
            // Very long questions or descriptions: Telegram cuts an edit at its limit anyway; cut here, before a tag can be split,
            // by keeping the head and the hint and ending the middle with an ellipsis line.
            const size_t keep = kAskCardMaxChars > hint.size() + 64 ? kAskCardMaxChars - hint.size() - 64 : kAskCardMaxChars / 2;
            std::wstring plain = prefix + L"<b>" + TgEscapeHtml(detail::AskCut(q.question, keep / 2)) + L"</b>";
            for (size_t i = 0; i < q.options.size() && plain.size() < keep; ++i)
            {
                plain += L"\n<b>" + std::to_wstring(i + 1) + L". " + TgEscapeHtml(detail::AskCut(q.options[i].label, 80)) + L"</b>";
            }
            return plain + L"\n<i>(cut to fit one message)</i>\n\n" + hint;
        }
        return without;
    }

    /// <summary>
    /// The card's buttons (an inline keyboard as rows of {label, callback data}); none unless the card is Open. The current
    /// question's options first (✅ marks a picked one; ☐ / ☑ toggles for multi-select; two to a row when every label is short),
    /// then ✍️ write my own (or 📝 the note, ✏️ / ✖ change or remove it, 📨 send it alone) and 🖼 the previews, ✅ Done for a
    /// multi-select question, ◀ / ▶ and 📨 Send answers for several questions, and 💬 Chat about this. While ✍️ / 📝 waits for
    /// text, a ✖ Cancel row comes first.
    /// </summary>
    /// <param name="m">The card.</param>
    /// <param name="key">The card's key (the callback data's second field).</param>
    /// <returns>The rows.</returns>
    /// <exception cref="std::bad_alloc">If the rows cannot be allocated.</exception>
    inline std::vector<std::vector<std::pair<std::wstring, std::wstring>>> BuildAskCardButtons(const AskCardModel& m, std::wstring_view key)
    {
        std::vector<std::vector<std::pair<std::wstring, std::wstring>>> rows;
        if (m.phase != AskCardPhase::Open || m.current >= m.questions.size())
        {
            return rows;
        }
        const AskQuestion& q = m.questions[m.current];
        const AskChoice& c = m.choices[m.current];
        const size_t n = m.questions.size();
        if (m.awaiting != AskTextTarget::None)
        {
            rows.push_back({ { m.awaiting == AskTextTarget::Note ? L"\x2716 Cancel the note" : L"\x2716 Cancel writing", AskCallbackData(key, kAskVerbCancelWrite) } });
        }
        // The options: two to a row when every label is short (a phone row holds two ~18-character buttons), else one each.
        const bool shortLabels = std::all_of(q.options.begin(), q.options.end(), [](const AskOption& o) { return o.label.size() <= 18; });
        std::vector<std::pair<std::wstring, std::wstring>> row;
        for (size_t i = 0; i < q.options.size(); ++i)
        {
            const bool picked = std::find(c.picks.begin(), c.picks.end(), i) != c.picks.end();
            std::wstring mark = q.multiSelect ? (picked ? L"\x2611 " : L"\x2610 ") : (picked ? L"\x2705 " : L"");
            row.emplace_back(mark + std::to_wstring(i + 1) + L" \x00B7 " + detail::AskCut(q.options[i].label, kAskButtonLabelMaxChars), AskCallbackData(key, kAskVerbOption, static_cast<int64_t>(i)));
            if (!shortLabels || row.size() == 2)
            {
                rows.push_back(std::move(row));
                row.clear();
            }
        }
        if (!row.empty())
        {
            rows.push_back(std::move(row));
        }
        if (q.multiSelect)
        {
            const size_t count = c.picks.size() + (c.otherText.empty() ? 0 : 1);
            std::wstring done = n == 1 ? L"\U0001F4E8 Send (" + std::to_wstring(count) + L")" : L"\x2705 Done \x25B6";
            rows.push_back({ { c.otherText.empty() ? L"\x270D\xFE0F Add my own" : L"\x270D\xFE0F Change my own", AskCallbackData(key, kAskVerbWrite) },
                             { std::move(done), AskCallbackData(key, kAskVerbDone) } });
        }
        else if (q.previewLayout)
        {
            if (c.note.empty())
            {
                rows.push_back({ { L"\U0001F4DD Add a note", AskCallbackData(key, kAskVerbNote) } });
            }
            else
            {
                rows.push_back({ { L"\x270F\xFE0F Change the note", AskCallbackData(key, kAskVerbNote) }, { L"\x2716 Remove the note", AskCallbackData(key, kAskVerbNoteRemove) } });
                if (n == 1)
                {
                    rows.push_back({ { L"\U0001F4E8 Send the note alone", AskCallbackData(key, kAskVerbNoteSend) } });
                }
            }
        }
        else
        {
            rows.push_back({ { c.otherText.empty() ? L"\x270D\xFE0F Write my own answer" : L"\x270D\xFE0F Change my own answer", AskCallbackData(key, kAskVerbWrite) } });
        }
        if (q.previewLayout && std::any_of(q.options.begin(), q.options.end(), [](const AskOption& o) { return o.previewKind == ClaudePreviewKind::Full; }))
        {
            rows.push_back({ { L"\U0001F5BC Previews as a picture", AskCallbackData(key, kAskVerbPicture) } });
        }
        if (n > 1)
        {
            std::vector<std::pair<std::wstring, std::wstring>> nav;
            if (m.current > 0)
            {
                nav.emplace_back(L"\x25C0 Question " + std::to_wstring(m.current), AskCallbackData(key, kAskVerbPrev));
            }
            if (m.current + 1 < n)
            {
                nav.emplace_back(L"Question " + std::to_wstring(m.current + 2) + L" \x25B6", AskCallbackData(key, kAskVerbNext));
            }
            if (!nav.empty())
            {
                rows.push_back(std::move(nav));
            }
            size_t answered = 0;
            for (size_t i = 0; i < n; ++i)
            {
                answered += AskQuestionAnswered(m.questions[i], m.choices[i]) ? 1 : 0;
            }
            rows.push_back({ { L"\U0001F4E8 Send answers (" + std::to_wstring(answered) + L" of " + std::to_wstring(n) + L")", AskCallbackData(key, kAskVerbSend) } });
        }
        rows.push_back({ { L"\U0001F4AC Chat about this", AskCallbackData(key, kAskVerbChat) } });
        return rows;
    }

    /// <summary>The words the bot asks with when a plain message arrives while a card is open (AskTextRoute::AskWhich).</summary>
    /// <param name="m">The card.</param>
    /// <returns>The HTML (one line and a question).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring AskWhichHtml(const AskCardModel& m)
    {
        const bool note = AskTextTargetFor(m) == AskTextTarget::Note;
        const std::wstring q = m.current < m.questions.size() ? detail::AskCut(m.questions[m.current].question, 120) : std::wstring{};
        return L"\x2753 Claude's question is still open: <i>" + TgEscapeHtml(q) + L"</i>\n" +
               (note ? std::wstring{ L"Is this message your <b>note</b> on it, or the session's <b>next prompt</b>?" }
                     : std::wstring{ L"Is this message your <b>answer</b> to it, or the session's <b>next prompt</b>?" });
    }

    // ===== the previews as a picture =========================================================================================

    namespace detail
    {
        /// <summary>
        /// Appends one line of text to a snapshot as cells (hard-wrapped at `cols`, like the PC's preview box): one cell per code
        /// point of width 1 or 2 (ClaudeCellWidth), a zero-width code point joined to the cell before it.
        /// </summary>
        /// <param name="lines">The snapshot's rows (rows are appended).</param>
        /// <param name="text">The line (no line break).</param>
        /// <param name="cols">The content width.</param>
        /// <param name="indent">Blank cells before the text on every row it fills.</param>
        /// <param name="fg">The text color (0xRRGGBB).</param>
        /// <param name="bg">The background (0xRRGGBB).</param>
        /// <param name="flags">kScreenCell* style bits.</param>
        /// <param name="frameFg">The frame's color, when `framed` draws a │ on both sides.</param>
        /// <param name="framed">Draw "│ " before and " │" after the text (the row is then cols + 4 wide).</param>
        /// <exception cref="std::bad_alloc">If a row cannot be allocated.</exception>
        inline void AskSheetAppendText(std::vector<std::vector<ScreenCell>>& lines, std::wstring_view text, int cols, int indent, uint32_t fg, uint32_t bg, uint8_t flags, uint32_t frameFg, bool framed)
        {
            const auto openRow = [&]() {
                std::vector<ScreenCell> row;
                for (int i = 0; i < indent; ++i)
                {
                    row.push_back(ScreenCell{ L" ", 1, fg, bg, 0 });
                }
                if (framed)
                {
                    row.push_back(ScreenCell{ L"\x2502", 1, frameFg, bg, 0 });
                    row.push_back(ScreenCell{ L" ", 1, fg, bg, 0 });
                }
                return row;
            };
            const auto closeRow = [&](std::vector<ScreenCell>& row, int used) {
                if (framed)
                {
                    for (int i = used; i < cols; ++i)
                    {
                        row.push_back(ScreenCell{ L" ", 1, fg, bg, 0 });
                    }
                    row.push_back(ScreenCell{ L" ", 1, fg, bg, 0 });
                    row.push_back(ScreenCell{ L"\x2502", 1, frameFg, bg, 0 });
                }
                lines.push_back(std::move(row));
            };
            std::vector<ScreenCell> row = openRow();
            int used = 0;
            for (size_t i = 0; i < text.size();)
            {
                char32_t cp = static_cast<char16_t>(text[i]);
                size_t units = 1;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
                {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<char16_t>(text[i + 1]) - 0xDC00);
                    units = 2;
                }
                const std::wstring_view piece = text.substr(i, units);
                i += units;
                int w = ClaudeCellWidth(cp);
                if (w == 0)
                {
                    // A mark or a zero-width character rides on the cell before it; at a row's start it has nothing to ride on.
                    if (used > 0 && !row.empty() && row.back().text.size() + piece.size() <= kScreenCellMaxChars)
                    {
                        row.back().text.append(piece);
                    }
                    continue;
                }
                if (cp < 0x20)
                {
                    continue; // a stray control (the echo turned them into U+FFFD already; a TAB was expanded)
                }
                if (used + w > cols)
                {
                    closeRow(row, used);
                    row = openRow();
                    used = 0;
                }
                row.push_back(ScreenCell{ std::wstring{ piece }, static_cast<uint8_t>(w), fg, bg, flags });
                used += w;
            }
            closeRow(row, used);
        }
    }

    /// <summary>
    /// The current question's previews as a SNAPSHOT that ScreenImage.h draws into a picture: each option's number, label and
    /// description in bold, then its preview inside a frame like the PC's box (hard-wrapped at kAskSheetMaxCols), "No preview
    /// available" or Claude Code's withheld text where the PC shows them, a blank row between options. A phone shows ASCII
    /// mockups and box drawing intact in a picture, where a monospace text block wraps at the screen's width.
    /// </summary>
    /// <param name="q">The question.</param>
    /// <returns>The snapshot (at most kScreenSnapshotMaxRows rows; a longer one ends with a "…" row).</returns>
    /// <exception cref="std::bad_alloc">If the snapshot cannot be allocated.</exception>
    inline ScreenSnapshot BuildAskPreviewSheet(const AskQuestion& q)
    {
        constexpr uint32_t kBg = 0x0C0C0C;
        constexpr uint32_t kText = 0xCCCCCC;
        constexpr uint32_t kTitle = 0xF2F2F2;
        constexpr uint32_t kDim = 0x767676;
        constexpr uint32_t kFrame = 0x5A5A5A;
        // The content width: the widest preview line (by terminal columns), at least the room for a title, at most the cap.
        int widest = 24;
        std::vector<std::wstring> echoes(q.options.size());
        for (size_t i = 0; i < q.options.size(); ++i)
        {
            const AskOption& o = q.options[i];
            if (o.previewKind != ClaudePreviewKind::Full)
            {
                continue;
            }
            echoes[i] = ClaudeQuestionPreviewEcho(o.preview);
            int lineWidth = 0;
            for (size_t k = 0; k < echoes[i].size(); ++k)
            {
                const wchar_t ch = echoes[i][k];
                if (ch == L'\n')
                {
                    widest = (std::max)(widest, lineWidth);
                    lineWidth = 0;
                    continue;
                }
                if (ch >= 0xDC00 && ch <= 0xDFFF && k > 0 && echoes[i][k - 1] >= 0xD800 && echoes[i][k - 1] <= 0xDBFF)
                {
                    continue; // the low half of a pair: counted with its high half below
                }
                char32_t cp = ch;
                if (ch >= 0xD800 && ch <= 0xDBFF && k + 1 < echoes[i].size() && echoes[i][k + 1] >= 0xDC00 && echoes[i][k + 1] <= 0xDFFF)
                {
                    cp = 0x10000 + ((static_cast<char32_t>(ch) - 0xD800) << 10) + (static_cast<char32_t>(echoes[i][k + 1]) - 0xDC00);
                }
                lineWidth += ClaudeCellWidth(cp);
            }
            widest = (std::max)(widest, lineWidth);
        }
        const int cols = (std::min)(widest, kAskSheetMaxCols);
        std::vector<std::vector<ScreenCell>> lines;
        const auto frameRule = [&](wchar_t left, wchar_t right) {
            std::vector<ScreenCell> row;
            row.push_back(ScreenCell{ std::wstring(1, left), 1, kFrame, kBg, 0 });
            for (int i = 0; i < cols + 2; ++i)
            {
                row.push_back(ScreenCell{ L"\x2500", 1, kFrame, kBg, 0 });
            }
            row.push_back(ScreenCell{ std::wstring(1, right), 1, kFrame, kBg, 0 });
            lines.push_back(std::move(row));
        };
        for (size_t i = 0; i < q.options.size(); ++i)
        {
            const AskOption& o = q.options[i];
            if (i > 0)
            {
                lines.emplace_back();
            }
            std::wstring title = std::to_wstring(i + 1) + L". " + o.label;
            detail::AskSheetAppendText(lines, title, cols + 4, 0, kTitle, kBg, kScreenCellBold, kFrame, false);
            if (!o.description.empty())
            {
                detail::AskSheetAppendText(lines, o.description, cols + 4, 0, kDim, kBg, kScreenCellItalic, kFrame, false);
            }
            frameRule(L'\x250C', L'\x2510');
            if (o.previewKind == ClaudePreviewKind::Full)
            {
                std::wstring_view rest = echoes[i];
                for (;;)
                {
                    const size_t nl = rest.find(L'\n');
                    detail::AskSheetAppendText(lines, rest.substr(0, nl), cols, 0, kText, kBg, 0, kFrame, true);
                    if (nl == std::wstring_view::npos)
                    {
                        break;
                    }
                    rest.remove_prefix(nl + 1);
                }
            }
            else
            {
                detail::AskSheetAppendText(lines, o.previewKind == ClaudePreviewKind::Withheld ? kAskWithheldPreviewText : std::wstring_view{ L"No preview available" }, cols, 0, kDim, kBg,
                                           kScreenCellItalic, kFrame, true);
            }
            frameRule(L'\x2514', L'\x2518');
        }
        if (lines.size() > static_cast<size_t>(kScreenSnapshotMaxRows))
        {
            lines.resize(static_cast<size_t>(kScreenSnapshotMaxRows) - 1);
            std::vector<ScreenCell> more;
            more.push_back(ScreenCell{ L"\x2026", 1, kDim, kBg, 0 });
            lines.push_back(std::move(more));
        }
        ScreenSnapshot snap;
        snap.cols = (std::min)(cols + 4, kScreenSnapshotMaxCols);
        snap.rows = (std::max)(1, static_cast<int>(lines.size()));
        snap.defaultFg = kText;
        snap.defaultBg = kBg;
        snap.lines = std::move(lines);
        if (snap.lines.empty())
        {
            snap.lines.emplace_back();
        }
        return snap;
    }
}
