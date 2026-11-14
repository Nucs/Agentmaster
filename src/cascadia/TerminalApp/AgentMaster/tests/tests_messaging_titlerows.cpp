// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.54: EVERY message in a session's topic names its session. The user's rule of 2026-11-14: "I want
// the title to be in all messages at the first line, or at the second line if there's info like in the bottom pinned one, and this
// way we'll always have a jump icon that people can click and go to the topic" — with the ⤴ LIFT beside the chip ("is chip
// clickable like we planned with lift? if no then add lift": on Telegram for Android a link cannot open a bot chat's topic). The
// pure halves:
//   1. the token's lift (TelegramFormat.h): TgTitleToken's `lift` (the upper-case form letter), TgParseTitleToken reading it, the
//      CLASSIC render unchanged by it (so no card hash moves), the RICH render's chip + ⤴ (TgTopicLiftButtonHtml, the directory dot's
//      data), TgTitleRowRichInline, and the row marker dropped by every renderer;
//   2. the placement (MessagingModels.h): TopicTitleRow / UserLineTitleRow, TextNamesTopic, WithTopicTitleRow (line 1, a marker's
//      place, or nothing), WithTopicTitleRowAfterHeader (line 2), the Markdown pair MarkdownTitleRowAfterHeader +
//      WithTopicTitleRowMarkdown through the real TgPrepareRichMarkdown, and TitledCaptionHtml (a caption's row and its cut);
//   3. the builders: the sticky card's title line (the lift in its rich form only, the hashed render unchanged) and the history
//      replay (RenderHistory: the day line and the conversation-only plan / interrupt / API error carry the row on line 2; nothing
//      else is changed there — the sender puts the row first).
// The send paths themselves (MessagingService::_sendText / _sendRich / _htmlItems / the captions / the question and /add-dir cards)
// need Telegram: the dev bot is their live check (MESSAGING.md §21.54.5). What Telegram keeps of an inline <tg-button> in a rich
// MARKDOWN message was measured on the dev bot before this was written (§21.54.1).

#include "m5_tests.h"

#include <algorithm> // std::count / std::any_of
#include <string>
#include <vector>

#include "../MessagingDirectory.h" // kDirectoryFrontCallbackPrefix, ParseDirectoryFrontCallbackData (the lift sends the dot's data)
#include "../MessagingModels.h"
#include "../TelegramFormat.h"

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    /// <summary>How often `needle` occurs in `hay` (non-overlapping) — the tests count rows and buttons with it.</summary>
    /// <param name="hay">The text searched.</param>
    /// <param name="needle">The text counted (not empty).</param>
    /// <returns>The number of occurrences.</returns>
    size_t TrCount(const std::wstring& hay, const std::wstring& needle)
    {
        size_t n = 0;
        for (size_t p = hay.find(needle); p != std::wstring::npos; p = hay.find(needle, p + needle.size()))
        {
            ++n;
        }
        return n;
    }

    /// <summary>A transcript item of the given kind, text and time (RenderHistory's input), for the replay checks.</summary>
    /// <param name="kind">The item's kind.</param>
    /// <param name="text">Its text.</param>
    /// <param name="tsMs">Its transcript time (0 = none).</param>
    /// <returns>The item.</returns>
    TopicItem TrItem(TopicItemKind kind, std::wstring text, int64_t tsMs = 0)
    {
        TopicItem it;
        it.kind = kind;
        it.text = std::move(text);
        it.tsMs = tsMs;
        return it;
    }
}

void TestMessagingTitleRows()
{
    wprintf(L"== TestMessagingTitleRows (MESSAGING.md 21.54: every message in a topic names its session: the row, its place, the lift) ==\n");
    const std::wstring blue = L"\U0001F7E6";
    const std::wstring url42 = L"https://t.me/bot/42?topic=42";
    const std::wstring lift42 = L"<tg-button type=\"callback_data\" data=\"d:front:42\">\x2934</tg-button>";
    const std::wstring marker{ kTgTitleRowMarker };

    // ===== 1. the token's lift ===================================================================================================
    {
        const std::wstring plain = TgTitleToken(TgTitleForm::Plain, blue, L"docs 2", 42);
        const std::wstring lifted = TgTitleToken(TgTitleForm::Plain, blue, L"docs 2", 42, true);
        CHECK(plain.size() == lifted.size() && plain[1] == L'p' && lifted[1] == L'P' && plain.substr(2) == lifted.substr(2),
              "the lift is the form letter upper-cased: the rest of the token is byte for byte the same");
        CHECK(TgTitleToken(TgTitleForm::Bold, blue, L"t", 42, true)[1] == L'B', "a Bold token lifts as 'B' (the card's title line)");
        CHECK(TgTitleToken(TgTitleForm::Plain, blue, L"t", 0, true) == TgTitleToken(TgTitleForm::Plain, blue, L"t", 0),
              "no thread, no lift: a token that names no topic has nothing to lift");
        CHECK(TgTitleToken(TgTitleForm::Quoted, blue, L"t", 42, true) == TgTitleToken(TgTitleForm::Quoted, blue, L"t", 42) &&
                  TgTitleToken(TgTitleForm::Square, blue, L"t", 42, true) == TgTitleToken(TgTitleForm::Square, blue, L"t", 42),
              "only Plain and Bold take the lift: the change line's forms stay as they are");

        TgTitleForm form{ TgTitleForm::Quoted };
        std::wstring square;
        std::wstring title;
        int64_t thread = 0;
        bool lift = false;
        CHECK(Msg::detail::TgParseTitleToken(lifted, 0, form, square, title, thread, lift) == lifted.size() && form == TgTitleForm::Plain && lift && thread == 42 &&
                  square == blue && title == L"docs 2",
              "the parse reads 'P' back: Plain, with the lift, its square, title and thread");
        CHECK(Msg::detail::TgParseTitleToken(plain, 0, form, square, title, thread, lift) == plain.size() && !lift, "a token without the lift reads lift = false");
        const std::wstring liftedBold = TgTitleToken(TgTitleForm::Bold, blue, L"t", 7, true);
        CHECK(Msg::detail::TgParseTitleToken(liftedBold, 0, form, square, title, thread) == liftedBold.size() && form == TgTitleForm::Bold && thread == 7,
              "the parse without the lift argument reads 'B' as Bold (the classic render's overload)");

        // The CLASSIC render never shows the lift (no in-text button in classic HTML): byte for byte the old render, with and
        // without the bot's username — so a card's hash, taken over it, does not move.
        CHECK(TgRenderTitleTokensClassic(lifted) == TgRenderTitleTokensClassic(plain) && TgRenderTitleTokensClassic(lifted, L"bot") == TgRenderTitleTokensClassic(plain, L"bot"),
              "classic: the lift changes nothing");
        CHECK(TgRenderTitleTokensClassic(lifted, L"bot") == blue + L" <a href=\"" + url42 + L"\">docs 2</a>", "classic: the square and the linked title");
        CHECK(TgRenderTitleTokensClassic(TgTitleToken(TgTitleForm::Bold, blue, L"t", 42, true), L"bot") == blue + L" <b><a href=\"" + url42 + L"\">t</a></b>",
              "classic: a lifted Bold token reads as the card's line always did");

        // The RICH render: the chip, a space, the lift.
        CHECK(TgTopicLiftButtonHtml(42) == lift42, "the lift: a plain callback button with the dot's data and the arrow");
        CHECK(TgTopicLiftButtonHtml(0).empty() && TgTopicLiftButtonHtml(-3).empty(), "no thread: no lift button");
        const std::wstring rich = TgRichHtmlFromClassic(lifted, L"bot");
        CHECK(rich == L"<p><tg-button type=\"url\" style=\"primary\" url=\"" + url42 + L"\">" + blue + L" docs 2</tg-button> " + lift42 + L"</p>",
              "rich: the URL chip, then the lift, in one paragraph");
        CHECK(TgRichHtmlFromClassic(plain, L"bot").find(L"callback_data") == std::wstring::npos, "rich: a token without the lift draws none");
        CHECK(TgRichHtmlFromClassic(lifted).find(L"type=\"copy_text\"") != std::wstring::npos && TgRichHtmlFromClassic(lifted).find(lift42) != std::wstring::npos,
              "rich before the username is known: a copy chip, and the lift all the same (a callback needs no link)");
        const std::wstring inBold = TgRichHtmlFromClassic(L"<b>x " + lifted + L" y</b>", L"bot");
        CHECK(inBold.find(L"</b><tg-button type=\"url\"") != std::wstring::npos && inBold.find(lift42 + L"<b> y</b>") != std::wstring::npos,
              "rich: the chip and the lift both sit outside the bold around them");
        CHECK(TgTitleRowRichInline(lifted, L"bot") == L"<tg-button type=\"url\" style=\"primary\" url=\"" + url42 + L"\">" + blue + L" docs 2</tg-button> " + lift42,
              "the row as INLINE rich HTML: no paragraph around it (a Markdown message takes it as an inline tag)");
        CHECK(TgTitleRowRichInline(L"", L"bot").empty(), "no row: nothing inline");

        // The lift sends the directory dot's own data: _handleCallback answers both with _bringTopicToFront.
        CHECK(kDirectoryFrontCallbackPrefix == kTgTopicLiftCallbackPrefix && ParseDirectoryFrontCallbackData(L"d:front:42") == 42,
              "the lift's data is the dot's, and the dot's parser reads it");

        // The row marker never reaches Telegram: every renderer drops it, and a title that holds one loses it.
        CHECK(TgRenderTitleTokensClassic(L"a" + marker + L"b") == L"ab", "classic: a stray marker is dropped");
        CHECK(TgRichHtmlFromClassic(L"a" + marker + L"b") == L"<p>ab</p>", "rich: a stray marker is dropped");
        CHECK(TgTitleToken(TgTitleForm::Plain, L"", L"x" + marker + L"y", 3).find(kTgTitleRowMarker) == std::wstring::npos, "a title that holds a marker loses it");
    }

    // ===== 2. the row and its place ==============================================================================================
    {
        const std::wstring row = TopicTitleRow(L"my tab", L"12345678-aaaa-bbbb-cccc-dddddddddddd", blue, 42);
        CHECK(row == TgTitleToken(TgTitleForm::Plain, blue, L"my tab", 42, true), "TopicTitleRow: the Plain token naming the topic, with the lift");
        CHECK(UserLineTitleRow(L"my tab", L"12345678-aaaa-bbbb-cccc-dddddddddddd", blue, 42) == row, "the You lines draw the very same row");
        CHECK(TopicTitleRow(L"", L"12345678-aaaa", L"", 42) == TgTitleToken(TgTitleForm::Plain, L"", L"12345678", 42, true), "no title: the short id");
        CHECK(TopicTitleRow(L"a\rb", L"x", L"", 5) == TgTitleToken(TgTitleForm::Plain, L"", L"a b", 5, true), "a two-line tab title on one line");
        const std::wstring longTitle(300, L'w');
        CHECK(TopicTitleRow(longTitle, L"x", L"", 5) == TgTitleToken(TgTitleForm::Plain, L"", std::wstring(119, L'w') + L"\x2026", 5, true), "a long title cut at 120");

        // TextNamesTopic: the message names ITS topic (any form, with or without the lift).
        CHECK(TextNamesTopic(L"hi " + row, 42) && TextNamesTopic(TgTitleToken(TgTitleForm::Quoted, blue, L"t", 42), 42), "a token naming the thread names it");
        CHECK(!TextNamesTopic(L"hi " + row, 43) && !TextNamesTopic(L"hi " + TgTitleToken(TgTitleForm::Plain, blue, L"t"), 42), "another thread, or none: it does not");
        CHECK(!TextNamesTopic(L"plain text", 42) && !TextNamesTopic(row, 0), "no token, or thread 0: never");
        CHECK(!TextNamesTopic(std::wstring{ kTgTitleTokenOpen } + L"p" + kTgTitleTokenThread + L"42", 42), "a token cut short names nothing");

        // WithTopicTitleRow: line 1, a marker's place, or nothing.
        CHECK(WithTopicTitleRow(L"Done. The tests pass.", row, 42) == row + L"\nDone. The tests pass.", "a message gets the row as its first line");
        const std::wstring you = BuildPromptLineHtml(0, PromptSource::Pc, L"hello", PromptTurnStatus::Running, false, row);
        CHECK(WithTopicTitleRow(you, row, 42) == you, "a \"You\" line names its topic already: no second row");
        CHECK(WithTopicTitleRow(L"\n \t", row, 42) == L"\n \t" && WithTopicTitleRow(L"x", L"", 42) == L"x" && WithTopicTitleRow(L"x", row, 0) == L"x",
              "a blank text, no row, or the main view: unchanged");
        CHECK(WithTopicTitleRow(L"head\n" + marker + L"\nbody", row, 42) == L"head\n" + row + L"\nbody", "a marker takes the row in its place");
        CHECK(WithTopicTitleRow(L"head\n" + marker + L"\nbody", L"", 42) == L"head\nbody", "a marker with no row: its line goes");
        CHECK(WithTopicTitleRow(L"a" + marker + L"b" + marker + L"c", L"R", 42) == L"aRbc", "one row only: a second marker is dropped");
        CHECK(WithTopicTitleRow(L"you " + row + L"\n" + marker, row, 42) == L"you " + row + L"\n" + row, "a marker places the row even beside another title");

        // WithTopicTitleRowAfterHeader: line 2, under the header.
        CHECK(WithTopicTitleRowAfterHeader(L"\x26A0 <b>API error</b>\nOverloaded", row) == L"\x26A0 <b>API error</b>\n" + row + L"\nOverloaded",
              "a header-led message: the header, the row, the rest");
        CHECK(WithTopicTitleRowAfterHeader(L"<i>\x2014 Wednesday, 12 Nov \x2014</i>", row) == L"<i>\x2014 Wednesday, 12 Nov \x2014</i>\n" + row,
              "a one-line header: the row becomes line 2");
        CHECK(WithTopicTitleRowAfterHeader(L"a\nb", L"") == L"a\nb", "no row: unchanged");

        // The Markdown pair, through the real preparation: the header line gets its hard break, the row follows it in the same
        // paragraph, a blank line, then the body — the layout the dev bot kept (§21.54.1).
        const std::wstring planMd = L"\U0001F4CB **Plan ready for approval** \x2014 approve or reject it at the PC.\n\n1. one\n2. two";
        const std::wstring withMarker = MarkdownTitleRowAfterHeader(planMd);
        CHECK(withMarker == L"\U0001F4CB **Plan ready for approval** \x2014 approve or reject it at the PC.\n" + marker + L"\n\n1. one\n2. two",
              "MarkdownTitleRowAfterHeader: the header, the marker's line, a blank line, the body");
        CHECK(MarkdownTitleRowAfterHeader(L"\x25A0 *interrupted*") == L"\x25A0 *interrupted*\n" + marker, "a one-line Markdown header: the marker on line 2");
        const std::wstring rowRich = TgTitleRowRichInline(row, L"bot");
        const std::wstring prepared = WithTopicTitleRowMarkdown(TgPrepareRichMarkdown(withMarker), rowRich);
        CHECK(prepared == L"\U0001F4CB **Plan ready for approval** \x2014 approve or reject it at the PC.  \n" + rowRich + L"\n\n1. one  \n2. two",
              "prepared, then the row in the marker's place: the header's hard break, the row, a blank line, the list");
        CHECK(prepared.find(L"&lt;tg-button") == std::wstring::npos && TrCount(prepared, L"<tg-button") == 2, "the buttons go in AFTER the preparation: never escaped");
        CHECK(WithTopicTitleRowMarkdown(L"Done.", rowRich) == rowRich + L"\n\nDone.", "a part without a marker: the row is its first paragraph");
        CHECK(WithTopicTitleRowMarkdown(L"  \n", rowRich) == L"  \n" && WithTopicTitleRowMarkdown(L"Done.", L"") == L"Done.", "a blank part, or no row: unchanged");
        CHECK(WithTopicTitleRowMarkdown(L"a\n" + marker + L"\n\nb", L"") == L"a\n\nb", "no row: the marker's line goes");

        // The HTML forms the send layer produces, rendered: a work log, a reply.
        const std::wstring workLog = WithTopicTitleRow(L"<blockquote expandable>\x2022 Bash: build</blockquote>", row, 42);
        CHECK(TgRichHtmlFromClassic(workLog, L"bot") == L"<p>" + rowRich + L"</p><blockquote expandable>\x2022 Bash: build</blockquote>",
              "rich: the row is a paragraph of its own above the folded work log");
        CHECK(TgRenderTitleTokensClassic(workLog, L"bot") == blue + L" <a href=\"" + url42 + L"\">my tab</a>\n<blockquote expandable>\x2022 Bash: build</blockquote>",
              "classic: the linked title on line 1, the quote below it");
        CHECK(TgRichHtmlFromClassic(WithTopicTitleRow(L"Done.\n\nAll green.", row, 42), L"bot") == L"<p>" + rowRich + L"<br>Done.</p><p>All green.</p>",
              "rich: the row is the first line of the first paragraph");

        // A caption's row: classic (a caption has no rich form), the caption escaped below it, the whole within 1000 characters.
        const std::wstring rowClassic = TgRenderTitleTokensClassic(row, L"bot");
        CHECK(TitledCaptionHtml(rowClassic, L"Transcript \x00B7 full <x>") == rowClassic + L"\nTranscript \x00B7 full &lt;x&gt;", "a caption: the row, then the caption escaped");
        CHECK(TitledCaptionHtml(rowClassic, L"") == rowClassic, "no caption: the row alone");
        const std::wstring caption = TitledCaptionHtml(rowClassic, std::wstring(2000, L'c'));
        const size_t shown = TgStripTags(caption).size();
        CHECK(shown == kTitledCaptionMaxChars, "a long caption is cut so the row and the caption show exactly 1000 characters");
        const std::wstring emoji = TitledCaptionHtml(rowClassic, std::wstring(kTitledCaptionMaxChars - TgStripTags(rowClassic).size() - 2, L'c') + L"\U0001F600");
        CHECK(emoji.empty() || (emoji.back() < 0xD800 || emoji.back() > 0xDBFF), "the cut never leaves half a surrogate pair");
    }

    // ===== 3. the builders =======================================================================================================
    {
        // The sticky card: its title line lifts in the RICH form only; the hashed classic render is the old one exactly.
        SessionInfo s;
        s.id = L"card-session";
        s.title = L"tab";
        s.live = true;
        CardExtras ex;
        ex.colorSquare = blue;
        ex.threadId = 42;
        const std::wstring card = BuildCardText(s, 1'800'000'000'000, 0, false, &ex);
        std::wstring unlifted = card;
        for (auto& c : unlifted)
        {
            c = c == L'B' && (&c > unlifted.data() && *(&c - 1) == kTgTitleTokenOpen) ? L'b' : c; // the same card, its token without the lift
        }
        CHECK(card != unlifted, "the card's title token carries the lift");
        CHECK(TgRenderTitleTokensClassic(card) == TgRenderTitleTokensClassic(unlifted) && CardTextHash(TgRenderTitleTokensClassic(card)) == CardTextHash(TgRenderTitleTokensClassic(unlifted)),
              "the card's hashed (classic) render and its hash do not move with the lift: no card is edited because of it");
        CHECK(TgRichHtmlFromClassic(card, L"bot").find(L"\U0001F4CC <tg-button type=\"url\"") != std::wstring::npos && TgRichHtmlFromClassic(card, L"bot").find(lift42) != std::wstring::npos,
              "the rich card: the pin, the chip, the lift");
        CardExtras noThread = ex;
        noThread.threadId = 0;
        CHECK(TgRichHtmlFromClassic(BuildCardText(s, 1'800'000'000'000, 0, false, &noThread)).find(L"callback_data") == std::wstring::npos, "a card with no topic: no lift");

        // The history replay. Conversation only: the day line, then the user's line (its row on line 2, as before), and the
        // header-led posts with the row on line 2 — the token in the HTML, the marker in the Markdown.
        const std::wstring row = TopicTitleRow(L"tab", L"card-session", blue, 42);
        int64_t day = 0;
        const int64_t t0 = 1'800'000'000'000;
        const auto posts = RenderHistory({ TrItem(TopicItemKind::Prompt, L"go", t0), TrItem(TopicItemKind::Plan, L"1. a\n2. b"), TrItem(TopicItemKind::Prompt, L"again", t0 + 1000),
                                           TrItem(TopicItemKind::ApiError, L"Overloaded"), TrItem(TopicItemKind::Prompt, L"stop", t0 + 2000), TrItem(TopicItemKind::Interrupt, L"") },
                                         false, day, {}, false, row);
        CHECK(!posts.empty() && posts.front().html.rfind(L"<i>\x2014 ", 0) == 0 && posts.front().html.find(L"\x2014</i>\n" + row) != std::wstring::npos,
              "the day line, then the row on line 2");
        const auto plan = std::find_if(posts.begin(), posts.end(), [](const HistoryPost& p) { return p.html.rfind(L"\U0001F4CB <b>Plan</b>", 0) == 0; });
        CHECK(plan != posts.end() && plan->html.rfind(L"\U0001F4CB <b>Plan</b>\n" + row + L"\n", 0) == 0 && plan->markdown.rfind(L"\U0001F4CB **Plan**\n" + marker + L"\n\n", 0) == 0,
              "a plan posted on its own: \"Plan\", the row (the marker in its Markdown), then the plan");
        const auto error = std::find_if(posts.begin(), posts.end(), [](const HistoryPost& p) { return p.html.rfind(L"\x26A0 <b>API error</b>", 0) == 0; });
        CHECK(error != posts.end() && error->html == L"\x26A0 <b>API error</b>\n" + row + L"\nOverloaded" && error->markdown.find(L"**API error**  \n" + marker + L"\n\nOverloaded") != std::wstring::npos,
              "an API error posted on its own: the header, the row, the error");
        const auto stopped = std::find_if(posts.begin(), posts.end(), [](const HistoryPost& p) { return p.html.rfind(L"\x25A0 <i>interrupted</i>", 0) == 0; });
        CHECK(stopped != posts.end() && stopped->html == L"\x25A0 <i>interrupted</i>\n" + row, "an interrupt posted on its own: the mark, then the row");
        CHECK(std::count_if(posts.begin(), posts.end(), [&](const HistoryPost& p) { return p.html.find(row) != std::wstring::npos; }) == static_cast<long long>(posts.size()),
              "every post of this replay carries the row (the user lines on line 2, the header-led posts on line 2)");

        // A question is the conversation's own words: the replay adds no row to it (the sender puts one first), and a reply neither.
        int64_t day2 = 0;
        const auto asked = RenderHistory({ TrItem(TopicItemKind::Prompt, L"go", t0), TrItem(TopicItemKind::Question, L"Swap: now?"), TrItem(TopicItemKind::Prompt, L"yes", t0 + 1000),
                                           TrItem(TopicItemKind::Text, L"Done.") },
                                         false, day2, {}, false, row);
        const auto question = std::find_if(asked.begin(), asked.end(), [](const HistoryPost& p) { return p.html.rfind(L"\x2753", 0) == 0; });
        CHECK(question != asked.end() && question->html.find(row) == std::wstring::npos, "a question: no row from the replay (the sender's goes first)");
        CHECK(!asked.empty() && asked.back().html.find(row) == std::wstring::npos, "a reply: no row from the replay");

        // Verbose: a plan or an error inside a packed message gets no row of its own there (the message's row is its first line).
        int64_t day3 = 0;
        const auto packed = RenderHistory({ TrItem(TopicItemKind::Prompt, L"go", t0), TrItem(TopicItemKind::Text, L"Looking."), TrItem(TopicItemKind::ApiError, L"Overloaded") },
                                          false, day3, {}, true, row);
        CHECK(std::none_of(packed.begin(), packed.end(), [&](const HistoryPost& p) { return !p.fromUser && p.html.find(L"API error") != std::wstring::npos && p.html.find(row) != std::wstring::npos; }),
              "verbose: the packed error carries no row of its own");

        // No row asked: the replay byte for byte as before (the header-led posts unchanged).
        int64_t day4 = 0;
        int64_t day5 = 0;
        const auto before = RenderHistory({ TrItem(TopicItemKind::Prompt, L"go", t0), TrItem(TopicItemKind::ApiError, L"Overloaded") }, false, day4, {}, false);
        const auto again = RenderHistory({ TrItem(TopicItemKind::Prompt, L"go", t0), TrItem(TopicItemKind::ApiError, L"Overloaded") }, false, day5, {}, false, L"");
        CHECK(before.size() == again.size() && std::none_of(before.begin(), before.end(), [](const HistoryPost& p) { return TgHasTitleTokens(p.html) || p.markdown.find(kTgTitleRowMarker) != std::wstring::npos; }),
              "no row: no token and no marker anywhere");
    }
}
