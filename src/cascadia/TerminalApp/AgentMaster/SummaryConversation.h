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
// (TestSummaryConversation). So is SummarySeparatorLabel, the text a separator's rule shows.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Agentmaster
{
    struct SessionSummary; // ProcessInspect.h — the analyzed current session
    struct ConversationUpstream; // TranscriptTurns.h — the earlier sessions of its chain

    // ONE row of the conversation list — a numbered PROMPT, or a SESSION SEPARATOR (sessionId non-empty)
    // drawn after the last prompt of an earlier session the current one continues. RenderSummaryBox renders
    // a prompt row as one " N. " line (N counting 1.. across every session) and a separator as a lone
    // `\x1E<full id>` line; the panel matches those lines back to their rows, in order.
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
    };

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
