// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — what the side panel's AI strip puts back into its request box, and what its copy button puts on the
// clipboard (SIDE_PANEL.md §2h, "⊘ and the copy button").
//
// The user's two requests of 2026-11-10:
//   - "when we click on circle icon that cancels/clears the session, unless the prompt box has a prompt - copy the prompt
//     that was sent (and was running) to the prompt box". ⊘ (a new conversation) puts the conversation's NEWEST request
//     back into an EMPTY request box, whether it still ran or was already answered (the user's pick of the two), so Enter
//     asks it again in the new conversation, or the user edits it first. A box that holds text keeps it: the rule of
//     2026-10-31 stands (what is typed there is often the next request itself).
//   - "add a copy icon below the circle icon that copies to clipboard the prompt user sent + assistant answers and
//     additionally, if prompt box is empty, copy the prompt sent to prompt box". The copy button under ⊘ copies the WHOLE
//     conversation (every request with the reply to it) and the sessions listed now, each with its folder, id and why (the
//     user's pick of four layouts), as Markdown in Copy ▸ Transcript's style; an EMPTY box gets the newest request back by
//     ⊘'s rule.
//
// This header is the pure half of both, pinned by the harness (TestSidePanelAiCopy, tests_sidepanel.cpp): the refill rule,
// the fold of the conversation's record into requests and replies, and the clipboard text. The view (the ⊘ handler in
// TerminalPage::_SidePanelAiEnsureView, and TerminalPage::_SidePanelAiCopyConversation) reads the agent, the registry and
// the clipboard, and only applies these.
//
// Plain C++, header-only (the SidePanelAiList.h idiom): no WinRT, no XAML, so the m5 harness compiles it without the app.
// The one clock it reads is the CRT's local time in SidePanelAiCopyLocalStamp, which the builder takes as a parameter (a
// test passes a fixed formatter).

#pragma once

#include <algorithm> // (std::max) — SidePanelAiCodeSpan
#include <cstdint>
#include <ctime> // localtime_s — SidePanelAiCopyLocalStamp
#include <cwchar> // std::swprintf — SidePanelAiCopyLocalStamp
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "AgentHarnessCore.h" // HarnessRecord / HarnessItem / HarnessItemKind / kHarnessMaxItems — the conversation's record, read here

namespace Agentmaster
{
    /// <summary>
    /// One request of the AI search's conversation and the reply to it, as the copy writes them (SidePanelAiCopyExchanges
    /// folds the conversation's record into these).
    /// </summary>
    struct SidePanelAiCopyExchange
    {
        /// <summary>The request's turn id (1, 2, ...). A request that a newer one replaced before it started never reaches the
        /// record, so the ids of a conversation can skip one; the copy names each request by its id, never by its position.</summary>
        uint64_t turn{ 0 };
        /// <summary>When the harness saw the request (Unix ms); 0 = unknown (a running request the harness has not recorded
        /// yet): its heading then names no time.</summary>
        int64_t askedAtUnixMs{ 0 };
        /// <summary>The words the user sent, as the record keeps them (cut at kHarnessMaxItemTextChars, ending "...").</summary>
        std::wstring request;
        /// <summary>
        /// The reply: the model's text after the turn's LAST tool call — a text block that a call follows was the model's plan
        /// (the strip's narration), never the reply — several blocks joined by a line break; the error's text for a failed
        /// turn; "" when the turn ended without one (stopped by Esc, or replaced by a newer request).
        /// </summary>
        std::wstring reply;
        /// <summary>The turn ended in an error: `reply` holds the error's text.</summary>
        bool failed{ false };
        /// <summary>The request still runs: no reply yet. What it wrote so far is left out — it may still turn out to be its
        /// plan.</summary>
        bool running{ false };
    };

    /// <summary>The conversation as the copy reads it: its requests, and whether the record's cap took the first ones.</summary>
    struct SidePanelAiCopyConversation
    {
        /// <summary>The requests, in the order they were asked.</summary>
        std::vector<SidePanelAiCopyExchange> exchanges;
        /// <summary>The record is at its cap (kHarnessMaxItems, the oldest entries dropped first) and no longer holds the first
        /// request(s): the copy says so above the first request it has, so request 7 is never read as the conversation's
        /// start.</summary>
        bool earlierDropped{ false };
    };

    /// <summary>One session the AI view lists, as the copy names it. The view fills it from the registry's record, else the
    /// agent's index entry — the rules its result cards follow (TerminalPage::_SidePanelAiResultModelFor).</summary>
    struct SidePanelAiCopyListRow
    {
        /// <summary>The whole session id.</summary>
        std::wstring id;
        /// <summary>The card's title; "" = not known yet (the copy then writes the card's own placeholder,
        /// "session &lt;the id's first 8 characters&gt;").</summary>
        std::wstring title;
        /// <summary>The card's state words: "open · waiting-for-you" for a session with a live tab, "closed" otherwise;
        /// "" = none (the copy leaves the part out).</summary>
        std::wstring state;
        /// <summary>The folder the card names (a known session's effective working dir); "" = not known.</summary>
        std::wstring folder;
        /// <summary>The git branch; "" = none known.</summary>
        std::wstring branch;
        /// <summary>The model's evidence for the session (its "why"); "" = none given.</summary>
        std::wstring why;
    };

    /// <summary>Everything BuildSidePanelAiCopyText writes: the conversation and the list on screen.</summary>
    struct SidePanelAiCopyContent
    {
        /// <summary>The requests and replies (SidePanelAiCopyExchanges).</summary>
        SidePanelAiCopyConversation conversation;
        /// <summary>The model listed sessions in this conversation (the view's "has shown" flag): an EMPTY `listed` then reads
        /// "No session fits this request." (the view's own words); false = no list section at all.</summary>
        bool hasShown{ false };
        /// <summary>The list on screen, in the model's order.</summary>
        std::vector<SidePanelAiCopyListRow> listed;
        /// <summary>A request runs, so the list on screen is the answer BEFORE it (the side panel changes its list only when a
        /// request ends, SidePanelAiList.h): the copy says so under the list's heading, so the list is never read as the
        /// running request's result.</summary>
        bool listIsPrevious{ false };
    };

    namespace detail
    {
        /// <summary>
        /// `text` on ONE line, without the white space at its two ends: every run of line breaks, with the spaces and tabs
        /// around it, becomes one space; the spaces between two words on a line stay as they are.
        /// </summary>
        /// <param name="text">Any text (a request, a session title: a title can hold a lone carriage return).</param>
        /// <returns>The folded text; "" for white space alone.</returns>
        /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
        inline std::wstring SidePanelAiOneLine(std::wstring_view text)
        {
            std::wstring out;
            out.reserve(text.size());
            bool pendingBreak = false; // a run of line breaks was read: one space goes before the next visible character
            for (const wchar_t c : text)
            {
                if (c == L'\r' || c == L'\n')
                {
                    while (!out.empty() && (out.back() == L' ' || out.back() == L'\t'))
                    {
                        out.pop_back(); // the spaces before a line break go with it
                    }
                    pendingBreak = true;
                    continue;
                }
                if ((c == L' ' || c == L'\t') && (out.empty() || pendingBreak))
                {
                    continue; // the white space at the start, and after a line break
                }
                if (pendingBreak && !out.empty())
                {
                    out.push_back(L' ');
                }
                pendingBreak = false;
                out.push_back(c);
            }
            while (!out.empty() && (out.back() == L' ' || out.back() == L'\t'))
            {
                out.pop_back();
            }
            return out;
        }

        /// <summary>`text` without the spaces, tabs and line breaks at its two ends (a reply's trailing line break would
        /// otherwise become an empty quote line).</summary>
        /// <param name="text">Any text.</param>
        /// <returns>A view into `text` (it must outlive the result).</returns>
        /// <remarks>Never throws: it only narrows the view.</remarks>
        inline std::wstring_view SidePanelAiTrimmed(std::wstring_view text) noexcept
        {
            const auto isSpace = [](wchar_t c) noexcept { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
            while (!text.empty() && isSpace(text.front()))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && isSpace(text.back()))
            {
                text.remove_suffix(1);
            }
            return text;
        }
    }

    /// <summary>
    /// The text ⊘ and the copy button write into the side panel's request box: the conversation's newest request, when the
    /// box holds nothing but white space (the user's "unless the prompt box has a prompt").
    /// </summary>
    /// <param name="boxText">The box's text now.</param>
    /// <param name="newestRequest">The conversation's newest request (SessionSearchSnapshot::question), running or answered —
    /// the user's pick. ⊘ must read it BEFORE SessionSearchAgent::Reset, which clears it.</param>
    /// <returns>The request on ONE line (detail::SidePanelAiOneLine: the box is a single-line TextBox, which would keep only
    /// the first line of several); "" when the box holds text — it keeps it (the rule of 2026-10-31: what is typed there is
    /// often the next request) — or when there is no request.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    [[nodiscard]] inline std::wstring SidePanelAiRefillText(std::wstring_view boxText, std::wstring_view newestRequest)
    {
        if (!detail::SidePanelAiTrimmed(boxText).empty())
        {
            return {}; // the box holds the user's words: never replace them
        }
        return detail::SidePanelAiOneLine(newestRequest);
    }

    /// <summary>
    /// Fold the conversation's RECORD (SessionSearchAgent::Record) into its requests and the replies to them — what the copy
    /// button copies (the user's pick: the whole conversation, not only the newest exchange).
    /// </summary>
    /// <param name="record">The record: every request (User), the model's text blocks (Assistant), its tool calls and their
    /// results, the harness's notices — capped at kHarnessMaxItems entries, the oldest dropped first.</param>
    /// <param name="newestTurn">The newest request's turn (SessionSearchSnapshot::turns).</param>
    /// <param name="newestRequest">The newest request's words (SessionSearchSnapshot::question): the copy's words for a RUNNING
    /// request whose turn has not started in the harness yet (it records the request when the turn starts — after a process
    /// start, or after the interrupt of the request before it).</param>
    /// <param name="newestRunning">SessionSearchSnapshot::busy: the newest request still runs.</param>
    /// <returns>One exchange per request the record holds, in the order asked, plus a running newest request it does not hold
    /// yet; `earlierDropped` when the cap took the first ones.</returns>
    /// <exception cref="std::bad_alloc">If the copies cannot be allocated.</exception>
    /// <remarks>
    /// The REPLY follows the agent's own rule (SessionSearchAgent's TurnCompleted handling): a text block that a tool call
    /// follows was the model's plan; the text after the turn's last call is its answer. An error notice of the turn makes it a
    /// failure with that notice's text as the reply. Items of no turn (0), the other notices (a compaction) and the tool
    /// results are skipped, and so is every item of a turn whose request the cap dropped (its reply without its request would
    /// answer nothing the copy shows).
    /// </remarks>
    [[nodiscard]] inline SidePanelAiCopyConversation SidePanelAiCopyExchanges(const HarnessRecord& record, uint64_t newestTurn, std::wstring_view newestRequest, bool newestRunning)
    {
        SidePanelAiCopyConversation out;
        std::unordered_map<uint64_t, size_t> indexOf; // turn -> its exchange in out.exchanges
        std::unordered_map<uint64_t, std::wstring> tail; // turn -> its text since its last tool call (the reply when the turn ends with it)
        std::unordered_map<uint64_t, std::wstring> failure; // turn -> its error notice's text
        bool orphan = false; // an item of a turn whose request is not in the record: the cap dropped that request
        for (const auto& item : record.items)
        {
            if (item.turn == 0)
            {
                continue; // outside any request: a notice of the process itself
            }
            const bool known = indexOf.find(item.turn) != indexOf.end();
            if (item.kind == HarnessItemKind::User)
            {
                if (!known)
                {
                    indexOf.emplace(item.turn, out.exchanges.size());
                    SidePanelAiCopyExchange exchange;
                    exchange.turn = item.turn;
                    exchange.askedAtUnixMs = item.atUnixMs;
                    exchange.request = item.text;
                    out.exchanges.push_back(std::move(exchange));
                }
                continue; // a second request item of one turn is never written; the first one is the request
            }
            if (!known)
            {
                orphan = true; // the request of this turn is gone (the cap): its reply is left out with it
                continue;
            }
            switch (item.kind)
            {
            case HarnessItemKind::Assistant:
            {
                auto& text = tail[item.turn];
                if (!text.empty())
                {
                    text += L"\n";
                }
                text += item.text;
                break;
            }
            case HarnessItemKind::ToolCall:
                tail[item.turn].clear(); // what the model wrote before a call was its plan, not its reply
                break;
            case HarnessItemKind::Notice:
                if (item.isError)
                {
                    failure[item.turn] = item.text; // FinishTurn's error notice: the turn failed
                }
                break;
            case HarnessItemKind::ToolResult:
            case HarnessItemKind::User:
            default:
                break; // a tool's answer is the model's input, not its reply
            }
        }
        for (auto& exchange : out.exchanges)
        {
            if (const auto f = failure.find(exchange.turn); f != failure.end())
            {
                exchange.failed = true;
                exchange.reply = f->second;
            }
            else if (const auto t = tail.find(exchange.turn); t != tail.end())
            {
                exchange.reply = t->second;
            }
            if (newestRunning && exchange.turn == newestTurn)
            {
                // Still running: whatever it wrote so far may yet be followed by a call (then it was the plan).
                exchange.running = true;
                exchange.failed = false;
                exchange.reply.clear();
            }
        }
        if (newestRunning && newestTurn != 0 && indexOf.find(newestTurn) == indexOf.end())
        {
            // Asked, but its turn has not started in the harness yet (the process starts, or the request before it is being
            // interrupted): the snapshot's words stand in for the record's.
            std::wstring words{ detail::SidePanelAiTrimmed(newestRequest) };
            if (!words.empty())
            {
                SidePanelAiCopyExchange exchange;
                exchange.turn = newestTurn;
                exchange.request = std::move(words);
                exchange.running = true;
                out.exchanges.push_back(std::move(exchange));
            }
        }
        // Only a record at its cap can have lost requests: below it, a conversation that starts at request 2 lost request 1 to a
        // newer one before it started (it never reached the record), which the copy must not blame on the cap.
        const bool atCap = record.items.size() >= kHarnessMaxItems;
        out.earlierDropped = atCap && (orphan || (!out.exchanges.empty() && out.exchanges.front().turn > 1));
        return out;
    }

    /// <summary>
    /// A request's time as the copy's heading writes it: this machine's wall clock, "Nov 10 14:02" — Copy ▸ Transcript's
    /// "%b %d %H:%M" (English month, two-digit day, 24-hour time; no year, like the transcript's turn headings).
    /// </summary>
    /// <param name="unixMs">The moment (Unix ms).</param>
    /// <returns>The text; "" for 0 or a negative moment, or one the CRT cannot convert (the heading then names no time).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring SidePanelAiCopyLocalStamp(int64_t unixMs)
    {
        if (unixMs <= 0)
        {
            return {};
        }
        const time_t seconds = static_cast<time_t>(unixMs / 1000);
        std::tm local{};
        if (localtime_s(&local, &seconds) != 0)
        {
            return {}; // the CRT's thread-safe form writes nothing on failure
        }
        static constexpr const wchar_t* kMonths[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
        const int month = (local.tm_mon >= 0 && local.tm_mon < 12) ? local.tm_mon : 0;
        wchar_t buffer[32]{};
        std::swprintf(buffer, 32, L"%ls %02d %02d:%02d", kMonths[month], local.tm_mday, local.tm_hour, local.tm_min);
        return buffer;
    }

    /// <summary>
    /// `text` as a Markdown CODE SPAN, so a Windows path's backslashes and a branch's underscores read as they are (outside a
    /// span, "\_" is an escape and "_x_" emphasis).
    /// </summary>
    /// <param name="text">One line of text.</param>
    /// <returns>The span: a fence one backtick longer than the longest run of backticks inside the text (CommonMark's rule, so
    /// no run inside closes it), the text padded by one space on each side when it starts or ends with a backtick (a renderer
    /// strips that padding).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring SidePanelAiCodeSpan(std::wstring_view text)
    {
        size_t longest = 0;
        size_t run = 0;
        for (const wchar_t c : text)
        {
            run = (c == L'`') ? run + 1 : 0;
            longest = (std::max)(longest, run);
        }
        const std::wstring fence(longest + 1, L'`');
        const bool pad = !text.empty() && (text.front() == L'`' || text.back() == L'`');
        std::wstring out = fence;
        if (pad)
        {
            out += L" ";
        }
        out += text;
        if (pad)
        {
            out += L" ";
        }
        out += fence;
        return out;
    }

    /// <summary>
    /// Append `text` to `out` as a Markdown BLOCK QUOTE: every line behind `indent` + "> ", an empty line as `indent` + ">" (so
    /// a reply of several paragraphs stays one quote), the white space at the text's two ends left out.
    /// </summary>
    /// <param name="out">The text being built; one line break ends every quoted line.</param>
    /// <param name="text">The text to quote (a request, a reply, a why); a carriage return before a line break is dropped.</param>
    /// <param name="indent">What goes before each "&gt;": "" at the top level, a list item's width of spaces inside one.</param>
    /// <exception cref="std::bad_alloc">If `out` cannot grow.</exception>
    inline void AppendSidePanelAiQuote(std::wstring& out, std::wstring_view text, std::wstring_view indent)
    {
        text = detail::SidePanelAiTrimmed(text);
        size_t start = 0;
        for (;;)
        {
            const size_t lineBreak = text.find(L'\n', start);
            std::wstring_view line = text.substr(start, lineBreak == std::wstring_view::npos ? std::wstring_view::npos : lineBreak - start);
            if (!line.empty() && line.back() == L'\r')
            {
                line.remove_suffix(1);
            }
            out += indent;
            out += line.empty() ? L">" : L"> ";
            out += line;
            out += L"\n";
            if (lineBreak == std::wstring_view::npos)
            {
                break;
            }
            start = lineBreak + 1;
        }
    }

    /// <summary>
    /// The CLIPBOARD TEXT of the side panel's copy button: the whole conversation and the list on screen, as Markdown in Copy ▸
    /// Transcript's style (the user's approved preview of 2026-11-10).
    /// </summary>
    /// <param name="content">The conversation (SidePanelAiCopyExchanges) and the list the view shows.</param>
    /// <param name="stampOf">A request's time for its heading — the app passes SidePanelAiCopyLocalStamp (this machine's wall
    /// clock), a test a fixed formatter; an empty function, or "" from it, leaves the time out.</param>
    /// <returns>
    /// The text, "" when there is nothing to copy (no request and no list). Its parts, each separated by an empty line:
    /// "## AI search · N requests"; a note when the record's cap took the first requests; per request "### Request &lt;id&gt; ·
    /// &lt;time&gt;", "**❯ User**" over the request as a quote, "**● Claude**" over the reply as a quote ("· failed" after
    /// the label when it is an error; an italic note while it runs or when it ended without a reply); then, when the model
    /// listed sessions, "### Listed sessions · N" with one numbered item per session — its title in bold, its state, its
    /// folder and branch and its id as code spans, its why as a quote inside the item — or "No session fits this request.".
    /// Lines end in a line feed alone, like every other Agentmaster copy.
    /// </returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated; whatever `stampOf` throws.</exception>
    [[nodiscard]] inline std::wstring BuildSidePanelAiCopyText(const SidePanelAiCopyContent& content, const std::function<std::wstring(int64_t)>& stampOf)
    {
        const auto& exchanges = content.conversation.exchanges;
        if (exchanges.empty() && !content.hasShown)
        {
            return {}; // nothing asked, nothing listed: there is nothing to copy
        }
        std::wstring out = L"## AI search \x00B7 " + std::to_wstring(exchanges.size()) + (exchanges.size() == 1 ? L" request\n" : L" requests\n");
        if (content.conversation.earlierDropped)
        {
            out += L"\n_(The first requests of this conversation are no longer in its record, which keeps its newest " + std::to_wstring(kHarnessMaxItems) + L" entries.)_\n";
        }
        for (const auto& exchange : exchanges)
        {
            out += L"\n### Request " + std::to_wstring(exchange.turn);
            const std::wstring stamp = (stampOf && exchange.askedAtUnixMs > 0) ? stampOf(exchange.askedAtUnixMs) : std::wstring{};
            if (!stamp.empty())
            {
                out += L" \x00B7 " + stamp;
            }
            out += L"\n\n**\x276F User**\n";
            AppendSidePanelAiQuote(out, exchange.request, L"");
            out += L"\n**\x25CF Claude**";
            if (exchange.running)
            {
                out += L"\n> _(still running \x2014 no reply yet)_\n";
            }
            else if (exchange.failed)
            {
                out += L" \x00B7 failed\n";
                AppendSidePanelAiQuote(out, exchange.reply, L"");
            }
            else if (detail::SidePanelAiTrimmed(exchange.reply).empty())
            {
                out += L"\n> _(no reply \x2014 the request was stopped before Claude answered)_\n";
            }
            else
            {
                out += L"\n";
                AppendSidePanelAiQuote(out, exchange.reply, L"");
            }
        }
        if (content.hasShown)
        {
            out += L"\n### Listed sessions \x00B7 " + std::to_wstring(content.listed.size()) + L"\n";
            if (content.listIsPrevious)
            {
                out += L"\n_(The list of the answer before the running request: it changes when that request ends.)_\n";
            }
            if (content.listed.empty())
            {
                out += L"\nNo session fits this request.\n";
            }
            else
            {
                out += L"\n";
                for (size_t i = 0; i < content.listed.size(); ++i)
                {
                    const auto& row = content.listed[i];
                    // A list item's later lines are indented by its marker's width ("1. " = 3, "10. " = 4), or Markdown ends
                    // the item there.
                    const std::wstring marker = std::to_wstring(i + 1) + L". ";
                    const std::wstring indent(marker.size(), L' ');
                    const std::wstring title = detail::SidePanelAiOneLine(row.title);
                    out += marker + L"**" + (title.empty() ? (L"session " + row.id.substr(0, 8)) : title) + L"**";
                    const std::wstring state = detail::SidePanelAiOneLine(row.state);
                    if (!state.empty())
                    {
                        out += L" \x00B7 " + state;
                    }
                    out += L"\n";
                    const std::wstring folder = detail::SidePanelAiOneLine(row.folder);
                    const std::wstring branch = detail::SidePanelAiOneLine(row.branch);
                    if (!folder.empty() || !branch.empty())
                    {
                        out += indent;
                        if (!folder.empty())
                        {
                            out += SidePanelAiCodeSpan(folder);
                        }
                        if (!folder.empty() && !branch.empty())
                        {
                            out += L" \x00B7 ";
                        }
                        if (!branch.empty())
                        {
                            out += SidePanelAiCodeSpan(branch);
                        }
                        out += L"\n";
                    }
                    if (!row.id.empty())
                    {
                        out += indent + L"id " + SidePanelAiCodeSpan(row.id) + L"\n";
                    }
                    if (!detail::SidePanelAiTrimmed(row.why).empty())
                    {
                        AppendSidePanelAiQuote(out, row.why, indent);
                    }
                }
            }
        }
        return out;
    }
}
