// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the summary panel's CONVERSATION LIST: every prompt of a session's whole conversation,
// oldest first, numbered 1..N as ONE conversation, with a session separator after the last prompt of
// each earlier session the current one continues (a fork's parent, a plan-restart parent, a /clear
// predecessor, a /handover origin). The per-tab summary panel, the side panel's summary and the copied
// Summary all build their Messages section from these rows (AgentTabOverlay.Internal.h RenderSummaryBox).
//
// Deliberately LIGHT: plain C++, no WinRT, and only forward declarations of the engine types, because
// AgentTabOverlay.h (included by TerminalPage.cpp and five more TUs) keeps the rows of its current render
// as members and must not drag ProcessInspect.h / TranscriptTurns.h into all of them. The two builders
// are defined in TranscriptTurns.cpp — next to CollectConversationUpstream, whose result they consume —
// which the m5 harness compiles too, so the numbering and ownership rules are unit-tested
// (TestSummaryConversation). So is SummarySeparatorLabel, the text a separator's rule shows, and the two
// header-only display rules every display of the box shares (inline here because the agentmaster CLI links the
// engine's box renderer but not TranscriptTurns.cpp): SummaryCompactionLabel, a compaction line's text, and
// SummaryLineIsListHeader, which sections draw one line per item.

#pragma once

#include <functional>
#include <string>
#include <string_view> // SummaryCompactionLabel's label parsing
#include <vector>

namespace Agentmaster
{
    struct SessionSummary; // ProcessInspect.h — the analyzed current session
    struct ConversationUpstream; // TranscriptTurns.h — the earlier sessions of its chain

    // ONE row of the conversation list — a numbered PROMPT, a SESSION SEPARATOR (sessionId non-empty) drawn
    // after the last prompt of an earlier session the current one continues, or a COMPACTION line
    // (compactions non-empty) where a /compact cut the conversation. RenderSummaryBox renders a prompt row as
    // one " N. " line (N counting 1.. across every session — a compaction line never interrupts the count), a
    // separator as a lone `\x1E<full id>` line and a compaction as a lone `\x1D<label>` line
    // (SummaryCompactionLabel); the panel matches the prompt and separator lines back to their rows, in order,
    // and draws a compaction line from its own text.
    struct SummaryConversationRow
    {
        // Prompt row: the prompt exactly as the summary lists it — unescaped and uncapped (the panel's
        // wrap/truncate rendering applies only to the line it draws); what the row's "Copy Prompt" copies.
        std::wstring text;
        // Prompt row: the prompt's index in the CURRENT session's analyzer list (SessionSummary::userMsgs,
        // the list the ▸ jump resolver and alt+up/down navigate), or -1 when it is not in that list (an
        // earlier session's prompt this session's transcript does not hold, or a part before the current
        // session's last /compact). A row with an index gets the ▸ jump button and the jump highlight.
        int headIndex{ -1 };
        // Separator row: the session whose conversation ends at this point — its FULL id (the panel shows
        // the first 8 characters; a click copies all of it). Empty on a prompt row: the discriminator.
        std::wstring sessionId;
        // Separator row: that session's title ("" when unknown) — for the separator's hover text.
        std::wstring sessionTitle;
        // Separator row: how the next session continued from it — "fork", "plan" (a plan-mode restart),
        // "clear" or "handover" ("" when unknown) — for the separator's hover text, and for its label
        // ("fork" leads the id on the rule — SummarySeparatorLabel).
        std::wstring continuedBy;
        // Compaction row (the summary's compaction lines, 2026-11-20): the label of every /compact that cut the
        // conversation at this point, oldest first — one, or several when no prompt came between them
        // ("compacted · auto · 923k→27k"; ConversationSegment::compactions). Non-empty only on a compaction row:
        // its discriminator (text and sessionId stay empty, headIndex stays -1). The line shows
        // SummaryCompactionLabel(compactions).
        std::vector<std::wstring> compactions;
    };

    // How many compactions' numbers a merged COMPACTION line lists one by one (SummaryCompactionLabel): up to this
    // many, all of them; past it the first and the last around "…" ("917k→23k, …, 930k→26k"). A long autonomous turn
    // can compact ten times with no prompt between (NumSharp's overnight run did), and ten number pairs would run the
    // line far past the summary panel's and the hover card's width — the merged line then still says how many (×10)
    // and the range they spanned. The copied Summary carries the same text.
    constexpr size_t kSummaryCompactionsListed = 3;

    // The text of a COMPACTION line: the one compaction's own label, or — several compactions with no prompt
    // between them — one merged label: "compacted · auto ×2 · 917k→23k, 920k→25k" when they share a trigger, else
    // "compacted ×2 · auto · 917k→23k, manual · 30k→5k"; past kSummaryCompactionsListed compactions only the first
    // and the last are spelled out ("compacted · auto ×10 · 917k→23k, …, 930k→26k"). Every display of the line uses
    // it (the summary panel's rule, the rich hover card's, the copied Summary's plain rule), so one compaction can
    // never read two ways. Pure; header-only so every renderer can call it (the engine's box renderer lives in a
    // unit the agentmaster CLI links, which does not link TranscriptTurns.cpp).
    //   labels — the row's compactions, each "compacted[ · <trigger>][ · <pre>→<post>]" (SeFormatCompactionLabel).
    // Returns the line's label; "" for no label. Throws std::bad_alloc only.
    inline std::wstring SummaryCompactionLabel(const std::vector<std::wstring>& labels)
    {
        if (labels.empty())
        {
            return {};
        }
        if (labels.size() == 1)
        {
            return labels.front();
        }
        constexpr std::wstring_view head = L"compacted";
        constexpr std::wstring_view dot = L" \x00B7 "; // " · " — the label's own field separator
        std::vector<std::wstring> triggers; // each label's trigger ("auto", "manual", "" when it carried none)
        std::vector<std::wstring> tokens; // each label's "<pre>→<post>" ("" when it carried none)
        std::vector<std::wstring> rests; // each label without its leading "compacted · " (the mixed-trigger form)
        for (const auto& l : labels)
        {
            std::wstring_view v{ l };
            if (v.substr(0, head.size()) == head)
            {
                v.remove_prefix(head.size());
                if (v.substr(0, dot.size()) == dot)
                {
                    v.remove_prefix(dot.size());
                }
            }
            rests.emplace_back(v);
            // The first field is the trigger only when it is not the token itself: a label whose metadata carried no
            // trigger reads "compacted · 917k→23k", and its numbers must not pose as a trigger ("→" is the token's).
            const std::wstring_view first = v.substr(0, v.find(dot));
            const bool firstIsToken = first.find(L'\x2192') != std::wstring_view::npos;
            triggers.emplace_back(firstIsToken ? std::wstring_view{} : first);
            if (firstIsToken)
            {
                tokens.emplace_back(first);
            }
            else
            {
                const size_t p = v.find(dot);
                tokens.emplace_back(p == std::wstring_view::npos ? std::wstring_view{} : v.substr(p + dot.size()));
            }
        }
        const std::wstring count = std::wstring{ L" \x00D7" } + std::to_wstring(labels.size()); // " ×2"
        // ", "-joined non-empty parts — all of them up to kSummaryCompactionsListed, else the first, "…" and the last.
        const auto join = [](const std::vector<std::wstring>& parts) {
            std::vector<std::wstring_view> shown;
            for (const auto& part : parts)
            {
                if (!part.empty())
                {
                    shown.emplace_back(part);
                }
            }
            if (shown.size() > kSummaryCompactionsListed)
            {
                shown = { shown.front(), std::wstring_view{ L"\x2026" }, shown.back() };
            }
            std::wstring out;
            for (const auto& part : shown)
            {
                if (!out.empty())
                {
                    out += L", ";
                }
                out += part;
            }
            return out;
        };
        bool sameTrigger = !triggers.front().empty();
        for (const auto& t : triggers)
        {
            sameTrigger = sameTrigger && t == triggers.front();
        }
        std::wstring out{ head };
        if (sameTrigger)
        {
            out += std::wstring{ dot } + triggers.front() + count;
            if (const std::wstring t = join(tokens); !t.empty())
            {
                out += std::wstring{ dot } + t;
            }
            return out;
        }
        out += count;
        if (const std::wstring r = join(rests); !r.empty())
        {
            out += std::wstring{ dot } + r;
        }
        return out;
    }

    // Is this summary-box line a bare "<Label>:" LIST HEADER — the line every "* item" list section of the box opens
    // with ("Skills Loaded:", "Files Created:", "Files Edited:", "Files Read:", and any list section added later)?
    // ASCII letters and spaces, then ONE trailing ':' and nothing after it. Deliberately not one: a numbered prompt that
    // ends in ':' (" 1. do this:" — a leading space and digits, so a wrapped prompt whose continuation lines are
    // Markdown "* " bullets keeps wrapping), "Recap: <text>" / "Tasks:  3 done …" / "Parent: <id>" (text after the
    // colon), and a session separator or compaction line (it opens with its control-character mark, not a letter).
    // The section a list header opens draws its "* " items ONE LINE EACH, cut at the edge, instead of wrapping a long
    // path over several lines. The one rule both displays of the box use — the summary panel
    // (AgentTabOverlay::_SetSummaryContent) and the rich hover card (TtBuildSummaryBody) — so a section can never be a
    // list on one and wrap on the other. Pure; header-only (unit-tested by TestSummaryConversation).
    //   line — one line of the box, without its '\n'.
    // Returns true for a list header. Never throws.
    inline bool SummaryLineIsListHeader(std::wstring_view line) noexcept
    {
        if (line.size() < 2 || line.back() != L':')
        {
            return false;
        }
        const wchar_t first = line.front();
        if (!((first >= L'A' && first <= L'Z') || (first >= L'a' && first <= L'z')))
        {
            return false;
        }
        for (size_t k = 0; k + 1 < line.size(); ++k)
        {
            const wchar_t c = line[k];
            if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L' '))
            {
                return false;
            }
        }
        return true;
    }

    // Builds the conversation list of a Claude session: the earlier sessions it continues (`up`, oldest
    // first, a separator closing each), then its own parts before its last /compact, then its current part.
    // Each prompt is listed ONCE, under the session that typed it first: one a FORK copied from its parent
    // (the same transcript-line uuid, ConversationUpstream::uuids) is listed under the parent and dropped
    // from this session's own parts — yet it keeps its ▸ jump index when `a.userMsgs` holds it (the fork's
    // terminal renders it too, so the jump still lands).
    //   a           — the current session's analysis: previousSegments = its own pre-/compact parts;
    //                 userMsgs (+ userMsgUuids, in lockstep) = the part after its last compaction.
    //   up          — the earlier sessions (TranscriptTurns.h CollectConversationUpstream).
    //   showHistory — the ⏮ toggle (AppSettings::summaryPanelShowHistory). OFF lists `a.userMsgs` alone —
    //                 the classic view (a fork's copies of its parent's prompts included), exactly what the
    //                 panel listed before the chain existed — with each row's headIndex == its position.
    //   titleOf     — the title to show for an earlier session ("" = unknown: the transcript's own title,
    //                 ConversationSegment::sessionTitle, is used instead); may be empty.
    // Returns the rows, in display order. A session of `up` whose segments are all empty still gets its
    // separator (it IS a link of the chain). Throws std::bad_alloc only (titleOf's own throws pass through).
    // COMPACTION rows (2026-11-20): with showHistory ON, a compaction row follows every segment a /compact ended
    // (ConversationSegment::compactions — one row per point, several compactions with no prompt between them in one
    // row), after that segment's prompts and before its session's separator. A compaction this session's transcript
    // only COPIED (a fork's copy of its parent's boundary, by uuid in ConversationUpstream::uuids) is drawn in the
    // parent's part alone. The current part ends at no compaction (nothing has ended it yet), and OFF lists none.
    std::vector<SummaryConversationRow> BuildSummaryConversation(const SessionSummary& a,
                                                                 const ConversationUpstream& up,
                                                                 bool showHistory,
                                                                 const std::function<std::wstring(const std::wstring&)>& titleOf);

    // The conversation rows of a Codex session — its rollout's human prompts, in order. No jump index
    // (Codex prompts have no ▸ jump) and no history (a rollout has no chain the summary follows); the rows
    // exist so "Copy Prompt" works on each numbered line exactly as on a Claude session's.
    //   prompts — the rollout's prompts (CodexRolloutInfo::userPrompts), the list RenderCodexSummary numbers.
    // Returns one prompt row per prompt. Throws std::bad_alloc only.
    std::vector<SummaryConversationRow> CodexSummaryConversation(const std::vector<std::wstring>& prompts);

    // The text a SESSION SEPARATOR's rule shows: the session's id, led by the word "fork" when the session
    // below is a FORK of it ("fork 358db618" — the user's request of 2026-10-12: a fork's separator read
    // like any other, so nothing on the rule said that the conversation below is a fork of this one). A
    // plan-mode restart, a /clear, a /handover and an unknown link keep the bare id; the separator's hover
    // text names every kind of link. The ONE rule behind both renderings of a separator — the panel's
    // clickable rule (AgentTabOverlay::_MakeSessionSeparator, 8 characters) and the copied Summary's
    // plain-text rule (CopySummaryAsync, the whole id) — so the two can never name a link differently.
    // The word is display only: a click on the rule still copies the bare id.
    //   row     — a separator row (sessionId non-empty); a prompt row has no label.
    //   idChars — how many leading characters of the id to show: 8 on the panel's rule; 0 shows the whole
    //             id (the copy, where `claude --resume <id>` needs all of it). An id shorter than idChars
    //             shows whole.
    // Returns the label, or "" for a prompt row. Throws std::bad_alloc only.
    std::wstring SummarySeparatorLabel(const SummaryConversationRow& row, size_t idChars);
}
