// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — TEAMS_CHANNEL.md: the bot-less Teams connector's pure core (TeamsModels.h). This file pins:
//   1. the text helpers: UTF-8 both ways, Base64url, Graph's ISO times (values computed outside the code, by Python's
//      datetime), the plain text of a Teams body;
//   2. the emoji: the exact forms sent, the keycaps, the comparison without variation selectors, the status set;
//   3. the actions: every legend emoji on every kind of message, the confirm rule;
//   4. the legend post and the card: the subject (Telegram's topic name), the card's own legend line per state, the body's
//      escaping, the change hash;
//   5. the post JSON: silent posts, the owner's @mention of an alert, high importance for "needs you";
//   6. Telegram HTML -> Teams HTML, and the split under the size limit (never inside a tag or an entity; joined = input);
//   7. the poll's cadence (5 s / 30 s / 90 s);
//   8. reactions: read from a message, compared between reads, the owner's to act on (the journal and the status emoji);
//   9. tokens: a JWT's claims (a test token built by Python, a non-ASCII name included), the scope ranks, the router;
//  10. inbound messages: parsing, "the owner wrote it", the new-post rule with other tools' markers;
//  11. the state file: a full round trip (a hash above 2^53 included), a newer format refused, the pruning;
//  12. Graph addresses: the guard that keeps a bearer token on graph.microsoft.com, the paths, the form encoding, the
//      tenant and client id checks, both error forms;
//  13. the sign-in's answers: the device code (numbers sent as digits too), every token-endpoint outcome, the Azure CLI's
//      output with and without its expiry;
//  14. the refresh token's parts in Credential Manager and the first part's user word;
//  15. the retry and 429 waits, the state file's name per channel, the cog's status line in each state;
//  16. the connector's settings (AppSettings' teams* members): their defaults and their JSON round trip (Persistence.cpp);
//  17. who gets a thread, the card's facts from a session, the live mirror (held texts, the final reply by stop reason, by
//      the next prompt and at rest, alerts and their freshness, commits that never pass a held text, the verbose
//      narrative), the history's packing and its hand-over to the live mirror;
//  18. a delta page and its links, the owner's new replies (journal, other users, system messages, age), a prompt's
//      status emoji, the backlog offer, refused commands, spans in words, the post fingerprint;
//  19. the confirms of a restart and a park, their watch words, the screen's text, the not-yet answer;
//  20. Base64 (RFC 4648's test vectors) and the body of a post that shows a PNG as its own hosted content;
//  21. a new session from a new post: the folder step's payload (a round trip, a separator inside the prompt, words alone),
//      its text (the first line quoted, the keycaps, a typed path when nothing is listed), a thread's kept first prompt in
//      the state file, and the card's cut of a long first prompt (in 4);
//  22. the whole conversation as pictures: the part height, the captions (the part, the turns, Last Turn, diffs), the cut
//      notice (the levels it suggests, typed) and the usage line;
//  23. a fork from Teams: what each answer launches in each situation (Telegram's tokens), the situations' words, the
//      question's payload (a round trip, what it refuses), its words in each situation; the fork question's emoji (in 3);
//  24. the Search thread: a reply classified (/clear, /reset, /help, a number, a keycap, words), the answer's lines (the
//      model's sentence, an open session's mark, the folder, the age, the prompts, the why, the cap's count), the root.
// The service's half (Graph calls, threads, polling) needs Graph and a window: its check is the owner's live run at home
// (TEAMS_CHANNEL.md §13).

#include "m5_tests.h"

#include <string>
#include <vector>

#include "../Persistence.h" // AppSettingsFromJson / ToJson: the connector's settings (§10)
#include "../TeamsModels.h"

using namespace Agentmaster;

namespace
{
    namespace T = Agentmaster::Teams;

    /// <summary>A reaction for the comparison tests, its emoji normalized as ParseReactions leaves it.</summary>
    /// <param name="emoji">The emoji (a view: the tests pass the header's constants and KeycapEmoji's results).</param>
    /// <param name="user">Who reacted.</param>
    /// <param name="atMs">When.</param>
    /// <returns>The reaction.</returns>
    /// <exception cref="std::bad_alloc">If the strings cannot be allocated.</exception>
    T::Reaction R(std::wstring_view emoji, const std::wstring& user, int64_t atMs)
    {
        T::Reaction r;
        r.emoji = T::NormalizeEmoji(emoji);
        r.userId = user;
        r.atMs = atMs;
        return r;
    }

    /// <summary>Pins the text helpers (§1).</summary>
    void TestTeamsText()
    {
        CHECK(T::Utf8Length(L"a") == 1, "teams: Utf8Length of ASCII is 1");
        CHECK(T::Utf8Length(L"\u00E9") == 2, "teams: Utf8Length of e-acute is 2");
        CHECK(T::Utf8Length(L"\u2705") == 3, "teams: Utf8Length of a BMP emoji is 3");
        CHECK(T::Utf8Length(L"\U0001F44C") == 4, "teams: Utf8Length of a surrogate pair is 4");
        CHECK(T::Utf8Length(std::wstring(1, static_cast<wchar_t>(0xD800))) == 3, "teams: a lone surrogate counts as U+FFFD (3)");
        const std::wstring mixed = L"a \u00E9 \u2705 \U0001F44C";
        CHECK(T::Utf16FromUtf8(T::Utf8FromUtf16(mixed)) == mixed, "teams: UTF-8 round trip keeps the text");
        CHECK(T::Utf8FromUtf16(mixed).size() == T::Utf8Length(mixed), "teams: Utf8Length equals the encoded size");
        CHECK(T::Utf16FromUtf8(std::string("\xC3")) == L"\xFFFD", "teams: a cut UTF-8 sequence becomes U+FFFD");
        CHECK(T::Utf16FromUtf8(std::string("a\xFF" "b")) == L"a\xFFFD" L"b", "teams: a bad byte becomes U+FFFD and decoding goes on");

        CHECK(T::Base64UrlDecode("aGVsbG8").value_or("") == "hello", "teams: base64url without padding");
        CHECK(T::Base64UrlDecode("aGVsbG8=").value_or("") == "hello", "teams: base64url with padding");
        CHECK(T::Base64UrlDecode("_-8").value_or("") == std::string("\xFF\xEF"), "teams: base64url's - and _");
        CHECK(!T::Base64UrlDecode("a*b").has_value(), "teams: base64url refuses a foreign character");

        // Expected values: Python's datetime (2026-11-18, TEAMS_CHANNEL.md's measurements' day).
        CHECK(T::ParseGraphTimeMs(L"2026-11-18T11:42:29.199Z") == 1795002149199LL, "teams: a Graph time with ms");
        CHECK(T::ParseGraphTimeMs(L"2026-11-18T11:42:29.1993Z") == 1795002149199LL, "teams: fraction digits past 3 are read past");
        CHECK(T::ParseGraphTimeMs(L"2026-11-18T13:42:29+02:00") == 1795002149000LL, "teams: an offset time");
        CHECK(T::ParseGraphTimeMs(L"1970-01-01T00:00:00Z") == 0, "teams: the epoch");
        CHECK(T::ParseGraphTimeMs(L"2000-02-29T12:00:00.5Z") == 951825600500LL, "teams: a leap day and one fraction digit");
        CHECK(T::ParseGraphTimeMs(L"garbage") == 0, "teams: not a time reads 0");
        CHECK(T::ParseGraphTimeMs(L"2026-13-01T00:00:00Z") == 0, "teams: month 13 reads 0");
        CHECK(T::ParseGraphTimeMs(L"") == 0, "teams: an empty time reads 0");

        CHECK(T::PlainTextFromTeamsHtml(L"<p>hello</p>") == L"hello", "teams: plain text of a paragraph");
        CHECK(T::PlainTextFromTeamsHtml(L"<p>line1</p><p>line2</p>") == L"line1\nline2", "teams: paragraphs become lines");
        CHECK(T::PlainTextFromTeamsHtml(L"x<br>y<br/>z") == L"x\ny\nz", "teams: <br> becomes a newline");
        CHECK(T::PlainTextFromTeamsHtml(L"a &amp; b &lt;c&gt; &#39;d&#39; &quot;e&quot; &#x1F44C;") == L"a & b <c> 'd' \"e\" \U0001F44C",
              "teams: entities and numeric references are decoded");
        CHECK(T::PlainTextFromTeamsHtml(L"&unknown; text") == L"&unknown; text", "teams: an unknown entity stays as it is");
        CHECK(T::PlainTextFromTeamsHtml(L"  <div><span style=\"x\">trim</span></div>  ") == L"trim", "teams: tags go, ends are trimmed");
        CHECK(T::PlainTextFromTeamsHtml(L"a&nbsp;b") == L"a b", "teams: &nbsp; is a space");
    }

    /// <summary>Pins the emoji (§2).</summary>
    void TestTeamsEmoji()
    {
        CHECK(T::NormalizeEmoji(T::kEmojiScreen) == L"\U0001F5BC", "teams: the picture without its selector");
        CHECK(T::SameEmoji(L"\U0001F5BC", T::kEmojiScreen), "teams: the picture with and without its selector is one emoji");
        CHECK(!T::SameEmoji(T::kEmojiYes, T::kEmojiNo), "teams: two different emoji differ");
        CHECK(T::KeycapEmoji(1) == L"1\uFE0F\u20E3", "teams: keycap 1 is fully qualified");
        CHECK(T::KeycapEmoji(10) == std::wstring{ T::kEmojiKeycapTen }, "teams: keycap 10 is the ten emoji");
        CHECK(T::KeycapEmoji(0).empty() && T::KeycapEmoji(11).empty(), "teams: no keycap outside 1-10");
        CHECK(T::KeycapNumber(L"1\u20E3").value_or(0) == 1, "teams: keycap 1 without its selector reads 1");
        for (int n = 1; n <= 10; ++n)
        {
            CHECK(T::KeycapNumber(T::KeycapEmoji(n)).value_or(0) == n, "teams: every keycap reads back its number");
        }
        CHECK(!T::KeycapNumber(L"\U0001F44C").has_value(), "teams: an emoji that is no keycap has no number");
        CHECK(!T::KeycapNumber(L"0\uFE0F\u20E3").has_value(), "teams: keycap 0 is no option");
        for (const std::wstring_view s : { T::kEmojiSeen, T::kEmojiWorking, T::kEmojiFromQueue, T::kEmojiDone, T::kEmojiWarn })
        {
            CHECK(T::IsStatusEmoji(s), "teams: every status emoji is one");
        }
        CHECK(!T::IsStatusEmoji(T::kEmojiScreen) && !T::IsStatusEmoji(T::kEmojiYes), "teams: action emoji are no status emoji");
    }

    /// <summary>Pins the actions (§3).</summary>
    void TestTeamsActions()
    {
        using T::Action;
        using T::MessageKind;
        const auto act = [](MessageKind k, std::wstring_view e) { return T::ActionForReaction(k, e).action; };
        CHECK(act(MessageKind::SessionCard, T::kEmojiScreen) == Action::Screen, "teams: picture on a card = screen");
        CHECK(act(MessageKind::SessionCard, L"\U0001F5BC") == Action::Screen, "teams: picture without selector on a card = screen");
        CHECK(act(MessageKind::SessionCard, T::kEmojiStatus) == Action::Status, "teams: chart on a card = status");
        CHECK(act(MessageKind::SessionCard, T::kEmojiEsc) == Action::Esc, "teams: stop sign on a card = esc");
        CHECK(act(MessageKind::SessionCard, T::kEmojiEnter) == Action::Enter, "teams: return arrow on a card = enter");
        CHECK(act(MessageKind::SessionCard, T::kEmojiAllScreen) == Action::AllScreen, "teams: scroll on a card = all screen");
        CHECK(act(MessageKind::SessionCard, T::kEmojiFork) == Action::Fork, "teams: shuffle on a card = fork");
        CHECK(act(MessageKind::SessionCard, T::kEmojiPark) == Action::Park, "teams: moon on a card = park");
        CHECK(act(MessageKind::SessionCard, T::kEmojiWake) == Action::Wake, "teams: sun on a card = wake");
        CHECK(act(MessageKind::SessionCard, T::kEmojiRestart) == Action::Restart, "teams: arrows on a card = restart");
        CHECK(act(MessageKind::SessionCard, T::kEmojiYes) == Action::None, "teams: a check on a card is nothing");
        CHECK(act(MessageKind::SessionCard, T::kEmojiDone) == Action::None, "teams: a status emoji on a card is nothing");
        CHECK(act(MessageKind::SessionCard, L"\U0001F44D") == Action::None, "teams: a thumbs up on a card is nothing");

        const T::ReactionAction pick = T::ActionForReaction(MessageKind::Question, T::KeycapEmoji(3));
        CHECK(pick.action == Action::Pick && pick.pick == 3, "teams: keycap 3 on a question picks 3");
        CHECK(act(MessageKind::Question, T::kEmojiYes) == Action::Yes, "teams: check on a question sends");
        CHECK(act(MessageKind::Question, T::kEmojiNo) == Action::No, "teams: cross on a question cancels");
        CHECK(act(MessageKind::Question, T::kEmojiScreen) == Action::None, "teams: picture on a question is nothing");
        CHECK(T::ActionForReaction(MessageKind::Step, T::KeycapEmoji(2)).pick == 2, "teams: keycap on a wizard step picks");
        CHECK(act(MessageKind::Step, T::kEmojiYes) == Action::None, "teams: check on a wizard step is nothing");
        const T::ReactionAction forkTwo = T::ActionForReaction(MessageKind::ForkChoice, T::KeycapEmoji(2));
        CHECK(forkTwo.action == Action::Pick && forkTwo.pick == 2, "teams: keycap 2 on a fork question = uninterrupted");
        CHECK(T::ActionForReaction(MessageKind::ForkChoice, T::KeycapEmoji(1)).pick == 1, "teams: keycap 1 on a fork question = interrupted");
        CHECK(act(MessageKind::ForkChoice, T::KeycapEmoji(3)) == Action::None, "teams: a third number on a fork question is nothing");
        CHECK(act(MessageKind::ForkChoice, T::kEmojiNo) == Action::No, "teams: cross on a fork question = no fork");
        CHECK(act(MessageKind::ForkChoice, T::kEmojiYes) == Action::None, "teams: check on a fork question is nothing");
        CHECK(act(MessageKind::Confirm, T::kEmojiYes) == Action::Yes, "teams: check on a confirm = yes");
        CHECK(act(MessageKind::Confirm, T::kEmojiNo) == Action::No, "teams: cross on a confirm = no");
        CHECK(act(MessageKind::Confirm, T::KeycapEmoji(1)) == Action::None, "teams: keycap on a confirm is nothing");
        CHECK(act(MessageKind::Backlog, T::kEmojiYes) == Action::Yes, "teams: check on a backlog answer sends it");
        CHECK(act(MessageKind::QueuedPrompt, T::kEmojiNo) == Action::No, "teams: cross on a queued prompt unqueues it");
        CHECK(act(MessageKind::QueuedPrompt, T::kEmojiYes) == Action::None, "teams: check on a queued prompt is nothing");
        CHECK(act(MessageKind::Legend, T::kEmojiNew) == Action::NewSession, "teams: plus on the legend = new session");
        CHECK(act(MessageKind::Legend, T::kEmojiScreen) == Action::None, "teams: picture on the legend is nothing");
        CHECK(act(MessageKind::Other, T::kEmojiScreen) == Action::None, "teams: nothing acts on another message");

        // The legend's card rows (the first nine) all act on a card: the post and the code cannot drift apart.
        const auto& rows = T::LegendRows();
        CHECK(rows.size() >= 9, "teams: the legend lists the card's nine actions");
        for (size_t k = 0; k < 9 && k < rows.size(); ++k)
        {
            CHECK(act(MessageKind::SessionCard, rows[k].emoji) != Action::None, "teams: each card row of the legend is an action");
        }
        CHECK(T::ActionNeedsConfirm(Action::Restart, false), "teams: a restart always asks");
        CHECK(!T::ActionNeedsConfirm(Action::Park, false), "teams: a park between turns does not ask");
        CHECK(T::ActionNeedsConfirm(Action::Park, true), "teams: a park during a turn asks");
        CHECK(!T::ActionNeedsConfirm(Action::Screen, true), "teams: a screenshot never asks");
        CHECK(std::wstring{ T::ActionWord(Action::Restart) } == L"restart", "teams: the action's word");
    }

    /// <summary>Pins the legend post and the session card (§4).</summary>
    void TestTeamsCard()
    {
        const std::wstring empty = T::BuildLegendHtml({}, L"");
        CHECK(empty.find(L"No sessions yet.") != std::wstring::npos, "teams: an empty fleet says so");
        CHECK(empty.find(L"<table>") != std::wstring::npos && empty.find(L"(updated") == std::wstring::npos,
              "teams: the legend table, and no update time when none is given");
        T::FleetLine line;
        line.title = L"drag <crash> fix";
        line.state = SessionState::WaitingForInput;
        line.webUrl = L"https://teams.example/l/message/1?a=1&b=\"2\"";
        const std::wstring legend = T::BuildLegendHtml({ line }, L"14:09");
        CHECK(legend.find(L"drag &lt;crash&gt; fix") != std::wstring::npos, "teams: the fleet line's title is escaped");
        CHECK(legend.find(L"a=1&amp;b=&quot;2&quot;") != std::wstring::npos, "teams: the deep link is escaped for an attribute");
        CHECK(legend.find(Msg::StateLabel(SessionState::WaitingForInput)) != std::wstring::npos, "teams: the fleet line's state");
        CHECK(legend.find(L"(updated 14:09)") != std::wstring::npos, "teams: the board's update time");

        CHECK(T::CardSubject(SessionState::WaitingForInput, L"#4FC1FF", L"drag crash fix") ==
                  Msg::TopicDisplayName(SessionState::WaitingForInput, Msg::TgTitleColorSquare(L"#4FC1FF"), L"drag crash fix"),
              "teams: the subject is Telegram's topic name in its glyph form");
        CHECK(T::CardSubject(SessionState::Idle, L"", L"").find(L"session") != std::wstring::npos, "teams: a title-less card says session");

        Msg::CardButtonFacts idle;
        CHECK(T::CardLegendLine(idle) == L"\U0001F4CA status · \U0001F4DC all · \U0001F500 fork",
              "teams: a session with no tab offers status, all and fork");
        Msg::CardButtonFacts running;
        running.takesEsc = true;
        running.turnInFlight = true;
        running.deactivatable = true;
        const std::wstring runningLine = T::CardLegendLine(running);
        for (const std::wstring_view e : { T::kEmojiScreen, T::kEmojiEsc, T::kEmojiEnter, T::kEmojiPark, T::kEmojiRestart })
        {
            CHECK(runningLine.find(e) != std::wstring::npos, "teams: a running tab offers screen, Esc, Enter, park and restart");
        }
        CHECK(runningLine.find(T::kEmojiWake) == std::wstring::npos, "teams: a running tab is not woken");
        Msg::CardButtonFacts parked;
        parked.reactivatable = true;
        CHECK(T::CardLegendLine(parked).find(T::kEmojiWake) != std::wstring::npos, "teams: a parked tab offers wake");
        CHECK(T::CardLegendLine(parked).find(T::kEmojiScreen) == std::wstring::npos, "teams: a parked tab has no screen");
        Msg::CardButtonFacts errored;
        errored.error = true;
        CHECK(T::CardLegendLine(errored).find(T::kEmojiRestart) != std::wstring::npos, "teams: an errored session offers restart");

        T::CardFacts f;
        f.state = SessionState::Running;
        f.sinceWord = L"since 14:02";
        f.title = L"a<b";
        f.folder = L"/src/app";
        f.branch = L"main";
        f.model = L"opus";
        f.runnerAuto = true;
        f.queued = 2;
        f.buttons = running;
        const std::wstring card = T::BuildCardHtml(f);
        CHECK(card.find(Msg::StateLabel(SessionState::Running)) != std::wstring::npos, "teams: the card's state line");
        CHECK(card.find(L"a&lt;b") != std::wstring::npos, "teams: the card's title is escaped");
        CHECK(card.find(L"/src/app · main · opus") != std::wstring::npos, "teams: the card's facts, empty ones left out");
        CHECK(card.find(L"Runner Auto · 2 queued") != std::wstring::npos, "teams: the card's runner line");
        CHECK(card.find(T::kEmojiEsc) != std::wstring::npos, "teams: the card ends with its legend line");
        CHECK(card.find(L"closed") == std::wstring::npos, "teams: a live card does not say closed");
        f.live = false;
        f.firstPrompt = L"fix it";
        const std::wstring closed = T::BuildCardHtml(f);
        CHECK(closed.find(L" · closed") != std::wstring::npos, "teams: a closed session's card says so");
        CHECK(closed.find(L"first prompt: <i>fix it</i>") != std::wstring::npos, "teams: the kept first prompt");
        f.firstPrompt = std::wstring(T::kCardFirstPromptMaxChars + 50, L'x');
        const std::wstring longCard = T::BuildCardHtml(f);
        CHECK(longCard.find(std::wstring(T::kCardFirstPromptMaxChars, L'x') + L"\x2026 (cut here") != std::wstring::npos &&
                  longCard.find(std::wstring(T::kCardFirstPromptMaxChars + 1, L'x')) == std::wstring::npos,
              "teams: a long first prompt is cut on the card");

        CHECK(T::CardHash(L"s", L"b") == T::CardHash(L"s", L"b"), "teams: the same card hashes the same");
        CHECK(T::CardHash(L"s", L"b") != T::CardHash(L"t", L"b"), "teams: a changed subject changes the hash");
        CHECK(T::CardHash(L"s", L"b") != T::CardHash(L"s", L"c"), "teams: a changed body changes the hash");
        CHECK(T::CardHash(L"ab", L"c") != T::CardHash(L"a", L"bc"), "teams: the subject and body stay apart in the hash");
    }

    /// <summary>Pins the post JSON (§5).</summary>
    void TestTeamsPostJson()
    {
        T::Owner owner;
        owner.id = L"11111111-2222-3333-4444-555555555555";
        owner.name = L"Test Owner";
        const json::Value silent = T::BuildPostJson(L"<p>x</p>", L"", T::AlertKind::None, owner);
        CHECK(silent.Find(L"mentions") == nullptr && silent.Find(L"importance") == nullptr && silent.Find(L"subject") == nullptr,
              "teams: a silent reply has no mention, importance or subject");
        const json::Value* body = silent.Find(L"body");
        CHECK(body && body->StrAt(L"contentType") == L"html" && body->StrAt(L"content") == L"<p>x</p>", "teams: the body is the HTML");

        const json::Value turnEnd = T::BuildPostJson(L"<p>done</p>", L"", T::AlertKind::TurnEnd, owner);
        const json::Value* mentions = turnEnd.Find(L"mentions");
        CHECK(mentions && mentions->type == json::Value::Type::Arr && mentions->arr.size() == 1, "teams: an alert carries one mention");
        if (mentions && mentions->arr.size() == 1)
        {
            const json::Value& m = mentions->arr[0];
            const json::Value* mentioned = m.Find(L"mentioned");
            const json::Value* user = mentioned ? mentioned->Find(L"user") : nullptr;
            CHECK(user && user->StrAt(L"id") == owner.id, "teams: the mention names the owner's id");
            CHECK(m.StrAt(L"mentionText") == owner.name, "teams: the mention's text is the owner's name");
        }
        const json::Value* turnBody = turnEnd.Find(L"body");
        CHECK(turnBody && turnBody->StrAt(L"content").find(L"<at id=\"0\">Test Owner</at>") != std::wstring::npos,
              "teams: the HTML carries the <at> tag the mention matches");
        CHECK(turnEnd.Find(L"importance") == nullptr, "teams: a turn's end is not important, only audible");

        const json::Value needs = T::BuildPostJson(L"<p>?</p>", L"", T::AlertKind::NeedsYou, owner);
        CHECK(needs.StrAt(L"importance") == L"high", "teams: Claude needs you = high importance");
        const json::Value root = T::BuildPostJson(L"<p>card</p>", L"\U0001F7E1 title", T::AlertKind::None, owner);
        CHECK(root.StrAt(L"subject") == L"\U0001F7E1 title", "teams: a root carries its subject");
        T::Owner nobody;
        const json::Value noOwner = T::BuildPostJson(L"<p>x</p>", L"", T::AlertKind::Error, nobody);
        CHECK(noOwner.Find(L"mentions") == nullptr, "teams: an alert without a known owner stays silent");
    }

    /// <summary>Pins the HTML adapter and the split (§6).</summary>
    void TestTeamsHtml()
    {
        CHECK(T::TeamsHtmlFromTelegramHtml(L"a\nb") == L"a<br>b", "teams: a newline becomes <br>");
        CHECK(T::TeamsHtmlFromTelegramHtml(L"a\r\nb") == L"a<br>b", "teams: CR goes");
        CHECK(T::TeamsHtmlFromTelegramHtml(L"<pre>x\ny</pre>\nz") == L"<pre>x\ny</pre><br>z", "teams: newlines in <pre> stay");
        CHECK(T::TeamsHtmlFromTelegramHtml(L"<pre><code class=\"language-cpp\">a\nb</code></pre>") ==
                  L"<pre><code class=\"language-cpp\">a\nb</code></pre>",
              "teams: a code block keeps its language and lines");
        CHECK(T::TeamsHtmlFromTelegramHtml(L"<tg-spoiler>s</tg-spoiler>") == L"<span>s</span>", "teams: a spoiler is a span");
        CHECK(T::TeamsHtmlFromTelegramHtml(L"<blockquote expandable>q</blockquote>") == L"<blockquote>q</blockquote>",
              "teams: an expandable quote is a quote");
        CHECK(T::TeamsHtmlFromTelegramHtml(L"<tg-emoji emoji-id=\"5\">\U0001F44D</tg-emoji>") == L"\U0001F44D",
              "teams: a custom emoji keeps only its emoji");
        CHECK(T::TeamsHtmlFromTelegramHtml(L"<b>x</b> &amp; <a href=\"u\">y</a>") == L"<b>x</b> &amp; <a href=\"u\">y</a>",
              "teams: other tags and entities pass unchanged");

        CHECK(T::SplitTeamsHtml(L"").empty(), "teams: nothing to split, no pieces");
        const std::vector<std::wstring> one = T::SplitTeamsHtml(L"<p>small</p>");
        CHECK(one.size() == 1 && one[0] == L"<p>small</p>", "teams: a small post is one piece");

        std::wstring lines;
        for (int k = 0; k < 200; ++k)
        {
            lines += L"line " + std::to_wstring(k) + L" \u2705<br>";
        }
        const std::vector<std::wstring> parts = T::SplitTeamsHtml(lines, 200);
        CHECK(parts.size() > 1, "teams: a long post splits");
        std::wstring joined;
        bool allFit = true;
        bool cutAtBreaks = true;
        for (size_t k = 0; k < parts.size(); ++k)
        {
            joined += parts[k];
            allFit = allFit && T::Utf8Length(parts[k]) <= 200;
            if (k + 1 < parts.size())
            {
                cutAtBreaks = cutAtBreaks && parts[k].size() >= 4 && parts[k].compare(parts[k].size() - 4, 4, L"<br>") == 0;
            }
        }
        CHECK(joined == lines, "teams: the pieces join back into the post exactly");
        CHECK(allFit, "teams: every piece fits the limit");
        CHECK(cutAtBreaks, "teams: pieces are cut after a <br>");

        std::wstring run;
        for (int k = 0; k < 120; ++k)
        {
            run += L"a&amp;\U0001F44C";
        }
        const std::vector<std::wstring> runParts = T::SplitTeamsHtml(run, 100);
        std::wstring runJoined;
        bool whole = true;
        for (const std::wstring& p : runParts)
        {
            runJoined += p;
            whole = whole && T::Utf8Length(p) <= 100;
            // Never a piece that ends inside an entity or between the halves of a surrogate pair.
            const size_t amp = p.rfind(L'&');
            whole = whole && (amp == std::wstring::npos || p.find(L';', amp) != std::wstring::npos);
            whole = whole && !(static_cast<uint16_t>(p.back()) >= 0xD800 && static_cast<uint16_t>(p.back()) <= 0xDBFF);
        }
        CHECK(runJoined == run && whole, "teams: a run with no breaks splits between whole characters and entities");
    }

    /// <summary>Pins the poll's cadence (§7).</summary>
    void TestTeamsPoll()
    {
        const int64_t now = 1'795'000'000'000LL;
        T::PollFacts f;
        f.nowMs = now;
        f.lastChangeMs = now;
        f.ownerAwaited = true;
        CHECK(T::NextPollDelayMs(f) == T::kPollHotMs, "teams: a session waiting for the owner polls every 5 s");
        f.ownerAwaited = false;
        f.lastOwnerActionMs = now - 60'000;
        CHECK(T::NextPollDelayMs(f) == T::kPollHotMs, "teams: the owner acted a minute ago: 5 s");
        f.lastOwnerActionMs = now - 180'000;
        f.lastChangeMs = now - 600'000;
        CHECK(T::NextPollDelayMs(f) == T::kPollNormalMs, "teams: normally 30 s");
        f.lastChangeMs = now - 61 * 60'000;
        CHECK(T::NextPollDelayMs(f) == T::kPollIdleMs, "teams: an hour with no change: 90 s");
        T::PollFacts zero;
        zero.nowMs = now;
        CHECK(T::NextPollDelayMs(zero) == T::kPollNormalMs, "teams: no facts yet: 30 s");
        CHECK(T::kPollHotMs == 5'000 && T::kPollNormalMs == 30'000 && T::kPollIdleMs == 90'000, "teams: the owner's cadence 5 / 30 / 90 s");
    }

    /// <summary>Pins the reactions (§8).</summary>
    void TestTeamsReactions()
    {
        const auto message = json::Parse(
            L"{\"reactions\":["
            L"{\"reactionType\":\"1\uFE0F\u20E3\",\"createdDateTime\":\"2026-11-18T11:43:08.177Z\",\"user\":{\"user\":{\"id\":\"owner\"}}},"
            L"{\"reactionType\":\"\U0001F44C\",\"createdDateTime\":\"2026-11-18T11:44:50.723Z\",\"user\":{\"user\":{\"id\":\"owner\"}}},"
            L"{\"reactionType\":\"\",\"user\":{\"user\":{\"id\":\"owner\"}}}"
            L"]}");
        CHECK(message.has_value(), "teams: the reactions sample parses");
        if (message)
        {
            const std::vector<T::Reaction> rs = T::ParseReactions(*message);
            CHECK(rs.size() == 2, "teams: two reactions read, the empty one skipped");
            if (rs.size() == 2)
            {
                CHECK(rs[0].emoji == L"1\u20E3" && rs[0].userId == L"owner", "teams: a reaction's emoji normalized and its user");
                CHECK(rs[0].atMs == T::ParseGraphTimeMs(L"2026-11-18T11:43:08.177Z"), "teams: a reaction's time");
            }
        }
        CHECK(T::ParseReactions(json::Value::MkObj()).empty(), "teams: a message without reactions has none");

        const T::Reaction a = R(T::KeycapEmoji(1), L"owner", 1000);
        const T::Reaction b = R(T::kEmojiScreen, L"owner", 2000);
        T::ReactionDiff d = T::DiffReactions({ a }, { a, b });
        CHECK(d.added.size() == 1 && d.added[0].emoji == b.emoji && d.removed.empty(), "teams: an added reaction");
        d = T::DiffReactions({ a }, {});
        CHECK(d.added.empty() && d.removed.size() == 1, "teams: a removed reaction");
        d = T::DiffReactions({ a }, { R(T::KeycapEmoji(1), L"owner", 5000) });
        CHECK(d.added.size() == 1 && d.removed.size() == 1, "teams: the same emoji put back later counts as added again");

        T::ReactionDiff mixed;
        mixed.added = { R(T::kEmojiScreen, L"owner", 3000), R(T::kEmojiDone, L"owner", 1500), R(T::kEmojiFork, L"other", 1600),
                        R(T::KeycapEmoji(2), L"owner", 2500) };
        std::vector<T::OwnReaction> ours;
        const std::vector<T::Reaction> act = T::OwnerReactionsToAct(L"m1", mixed, L"owner", ours);
        CHECK(act.size() == 2, "teams: the owner's two action emoji, not the status emoji or another user's");
        CHECK(act.size() == 2 && act[0].atMs == 2500 && act[1].atMs == 3000, "teams: oldest first");
        ours.push_back({ L"m1", T::NormalizeEmoji(T::kEmojiScreen), 3000 + 60'000 });
        CHECK(T::OwnerReactionsToAct(L"m1", mixed, L"owner", ours).size() == 1, "teams: a reaction Agentmaster set itself is not acted on");
        CHECK(T::OwnerReactionsToAct(L"m2", mixed, L"owner", ours).size() == 2, "teams: the journal is per message");
        ours.back().atMs = 3000 + 10 * 60'000;
        CHECK(T::OwnerReactionsToAct(L"m1", mixed, L"owner", ours).size() == 2, "teams: a journal entry far in time does not match");
    }

    /// <summary>Pins the tokens and the router (§9).</summary>
    void TestTeamsTokens()
    {
        // Built by Python: header {"alg":"none","typ":"JWT"}, a payload with four scopes and a non-ASCII name, signature "sig".
        const wchar_t* token =
            L"eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0."
            L"eyJhdWQiOiJodHRwczovL2dyYXBoLm1pY3Jvc29mdC5jb20iLCJzY3AiOiJDaGFubmVsTWVzc2FnZS5SZWFkLkFsbCBDaGFubmVsTWVzc2FnZS5TZW5kIEZpbGVzLlJl"
            L"YWRXcml0ZSBVc2VyLlJlYWQiLCJvaWQiOiIxMTExMTExMS0yMjIyLTMzMzMtNDQ0NC01NTU1NTU1NTU1NTUiLCJuYW1lIjoiw4lsb2RpZSBUZXN0IiwiYXBwX2Rpc3Bs"
            L"YXluYW1lIjoiVGVzdCBBcHAiLCJ0aWQiOiJ0ZW5hbnQtMSIsImV4cCI6MTc5NTAwMDAwMH0.c2ln";
        const auto claims = T::ReadJwtClaims(token);
        CHECK(claims.has_value(), "teams: the test token's claims are read");
        if (claims)
        {
            CHECK(claims->scopes.size() == 4 && claims->scopes[0] == L"ChannelMessage.Read.All" && claims->scopes[3] == L"User.Read",
                  "teams: the scopes, in order");
            CHECK(claims->oid == L"11111111-2222-3333-4444-555555555555", "teams: the owner's object id");
            CHECK(claims->name == L"\u00C9lodie Test", "teams: a non-ASCII name decoded from UTF-8");
            CHECK(claims->appName == L"Test App" && claims->tenantId == L"tenant-1", "teams: the app and the tenant");
            CHECK(claims->expiresMs == 1'795'000'000'000LL, "teams: the expiry in ms");
        }
        CHECK(!T::ReadJwtClaims(L"nodots").has_value(), "teams: a token without dots is no JWT");
        CHECK(!T::ReadJwtClaims(L"a.!!!.c").has_value(), "teams: a payload that is no base64url is refused");
        CHECK(!T::ReadJwtClaims(L"a.aGVsbG8.c").has_value(), "teams: a payload that is no JSON object is refused");

        CHECK(T::ScopeRank({ L"ChannelMessage.Send" }, T::GraphOp::Post) == 0, "teams: the best scope for a post");
        CHECK(T::ScopeRank({ L"Group.ReadWrite.All" }, T::GraphOp::Post) == 1, "teams: the second scope for a post");
        CHECK(T::ScopeRank({ L"group.readwrite.all" }, T::GraphOp::Edit) == 1, "teams: scope names are case-insensitive");
        CHECK(T::ScopeRank({ L"Group.ReadWrite.All" }, T::GraphOp::Read) == -1, "teams: Group.ReadWrite.All cannot read (measured)");
        CHECK(T::ScopeRank({ L"Group.ReadWrite.All" }, T::GraphOp::React) == -1, "teams: Group.ReadWrite.All cannot react (measured)");
        CHECK(T::ScopeRank({}, T::GraphOp::Delete) == -1, "teams: no scope, no call");

        std::vector<T::TokenSourceView> sources(2);
        sources[0].name = L"azure-cli";
        sources[0].scopes = { L"Directory.AccessAsUser.All", L"Group.ReadWrite.All", L"User.Read.All" };
        sources[0].available = true;
        sources[1].name = L"app";
        sources[1].scopes = { L"ChannelMessage.Read.All", L"ChannelMessage.Send", L"Files.ReadWrite" };
        sources[1].available = true;
        const std::vector<T::RefusedPair> none;
        CHECK(T::PickTokenFor(T::GraphOp::Post, sources, none).value_or(9) == 1, "teams: posts go to the app");
        CHECK(T::PickTokenFor(T::GraphOp::Edit, sources, none).value_or(9) == 0, "teams: edits go to the Azure CLI");
        CHECK(T::PickTokenFor(T::GraphOp::Read, sources, none).value_or(9) == 1, "teams: reads go to the app");
        CHECK(T::PickTokenFor(T::GraphOp::React, sources, none).value_or(9) == 1, "teams: reactions go to the app");
        CHECK(T::PickTokenFor(T::GraphOp::Upload, sources, none).value_or(9) == 1, "teams: uploads go to the app");
        CHECK(!T::PickTokenFor(T::GraphOp::Delete, sources, none).has_value(), "teams: no token may delete");
        CHECK(T::PickTokenFor(T::GraphOp::Post, sources, { { 1, T::GraphOp::Post } }).value_or(9) == 0,
              "teams: a post refused by the app falls back to the Azure CLI");
        sources[1].available = false;
        CHECK(!T::PickTokenFor(T::GraphOp::Read, sources, none).has_value(), "teams: no app token, no reads");
        CHECK(T::PickTokenFor(T::GraphOp::Post, sources, none).value_or(9) == 0, "teams: no app token, posts by the Azure CLI");
        CHECK(std::wstring{ T::GraphOpWord(T::GraphOp::Edit) } == L"edit", "teams: the call's word");
    }

    /// <summary>Pins the inbound messages (§10).</summary>
    void TestTeamsInbound()
    {
        const auto m = json::Parse(
            L"{\"id\":\"1795001790638\",\"replyToId\":null,\"messageType\":\"message\",\"subject\":null,"
            L"\"createdDateTime\":\"2026-11-18T11:43:08.177Z\",\"lastModifiedDateTime\":\"2026-11-18T11:44:00Z\",\"deletedDateTime\":null,"
            L"\"from\":{\"user\":{\"id\":\"owner\",\"displayName\":\"Owner\"}},"
            L"\"body\":{\"contentType\":\"html\",\"content\":\"<p>fix &amp; test</p>\"},"
            L"\"webUrl\":\"https://teams.example/l/1\",\"reactions\":[]}");
        CHECK(m.has_value(), "teams: the message sample parses");
        if (!m)
        {
            return;
        }
        const T::InboundMessage x = T::ParseInboundMessage(*m);
        CHECK(x.id == L"1795001790638" && x.replyToId.empty() && x.messageType == L"message", "teams: the message's id, root and type");
        CHECK(x.fromUserId == L"owner" && x.text == L"fix & test", "teams: the writer and the plain text");
        CHECK(x.createdMs == T::ParseGraphTimeMs(L"2026-11-18T11:43:08.177Z") && !x.deleted, "teams: the time; not deleted");
        CHECK(x.webUrl == L"https://teams.example/l/1", "teams: the deep link");

        std::map<std::wstring, int64_t> ours;
        CHECK(T::IsOwnerMessage(x, L"owner", ours), "teams: the owner's own message");
        CHECK(!T::IsOwnerMessage(x, L"someone", ours), "teams: another user's message is not the owner's");
        CHECK(!T::IsOwnerMessage(x, L"", ours), "teams: no known owner, no owner's message");
        ours[x.id] = 1;
        CHECK(!T::IsOwnerMessage(x, L"owner", ours), "teams: a post Agentmaster made is not the owner's");
        ours.clear();
        T::InboundMessage del = x;
        del.deleted = true;
        CHECK(!T::IsOwnerMessage(del, L"owner", ours), "teams: a deleted message is nobody's");
        T::InboundMessage sys = x;
        sys.messageType = L"systemEventMessage";
        CHECK(!T::IsOwnerMessage(sys, L"owner", ours), "teams: a system message is nobody's");

        const std::wstring markers = L"\U0001F916";
        CHECK(T::ClassifyNewRoot(x, L"owner", ours, markers) == T::NewRootVerdict::NewSession, "teams: the owner's new post opens a session");
        T::InboundMessage tool = x;
        tool.text = L"\U0001F916\uFE0F Claude \u00B7 [label] question";
        CHECK(T::ClassifyNewRoot(tool, L"owner", ours, markers) == T::NewRootVerdict::Ignore, "teams: another tool's post (its marker) is ignored");
        T::InboundMessage reply = x;
        reply.replyToId = L"1";
        CHECK(T::ClassifyNewRoot(reply, L"owner", ours, markers) == T::NewRootVerdict::Ignore, "teams: a reply is no new post");
        T::InboundMessage blank = x;
        blank.text.clear();
        CHECK(T::ClassifyNewRoot(blank, L"owner", ours, markers) == T::NewRootVerdict::Ignore, "teams: an empty post is ignored");

        CHECK(T::StartsWithAnyMarker(L"  \U0001F916\uFE0F hi", L"\U0001F916 x"), "teams: a marker after leading spaces, selector aside");
        CHECK(T::StartsWithAnyMarker(L"bcd", L"a b"), "teams: the second marker of the list");
        CHECK(!T::StartsWithAnyMarker(L"hello", L""), "teams: no markers, no match");
        CHECK(!T::StartsWithAnyMarker(L"hello", L"x  y"), "teams: double spaces in the list are no empty marker");
    }

    /// <summary>Pins the state file (§11).</summary>
    void TestTeamsState()
    {
        T::ChannelState s;
        s.teamId = L"team";
        s.channelId = L"19:abc@thread.tacv2";
        s.legendId = L"100";
        s.searchId = L"101";
        s.deltaLink = L"https://graph.microsoft.com/v1.0/x/messages/delta?$deltatoken=abc";
        T::Thread t;
        t.sessionId = L"sid-1";
        t.rootId = L"200";
        t.webUrl = L"https://teams.example/l/200";
        t.cardHash = 0xFEDCBA9876543210ull; // above 2^53: a JSON number would lose it
        t.createdMs = 1'795'000'000'000LL;
        t.lastCardEditMs = 1'795'000'001'000LL;
        t.transcriptPath = L"C:\\Users\\x\\.claude\\projects\\p\\sid-1.jsonl";
        t.transcriptOffset = 123456;
        t.historyDone = true;
        t.lastReplyMs = 1'795'000'002'000LL;
        t.rootReactions = { R(T::kEmojiScreen, L"owner", 7) };
        s.threads[t.sessionId] = t;
        T::Thread broken;
        broken.sessionId = L"sid-2";
        s.threads[broken.sessionId] = broken; // no root: dropped when read back
        T::Watched w;
        w.messageId = L"300";
        w.rootId = L"200";
        w.kind = T::MessageKind::Question;
        w.sessionId = L"sid-1";
        w.payload = L"q-1";
        w.expiresMs = 1'795'000'100'000LL;
        w.reactions = { R(T::KeycapEmoji(2), L"owner", 9) };
        s.watched.push_back(w);
        s.ownPosts[L"200"] = 1'795'000'000'000LL;
        s.ownReactions.push_back({ L"200", T::NormalizeEmoji(T::kEmojiDone), 1'795'000'000'500LL });
        s.legendReactions = { R(T::kEmojiNew, L"owner", 11) };

        const auto back = json::Parse(json::Dump(T::ChannelStateToJson(s)));
        CHECK(back.has_value(), "teams: the state's JSON parses");
        const auto r = back ? T::ChannelStateFromJson(*back) : std::nullopt;
        CHECK(r.has_value(), "teams: the state reads back");
        if (r)
        {
            CHECK(r->teamId == s.teamId && r->channelId == s.channelId && r->legendId == s.legendId && r->searchId == s.searchId &&
                      r->deltaLink == s.deltaLink,
                  "teams: the channel's ids and the delta link round-trip");
            CHECK(r->threads.size() == 1 && r->threads.count(L"sid-1") == 1, "teams: a thread without a root is dropped");
            if (r->threads.count(L"sid-1") == 1)
            {
                const T::Thread& u = r->threads.at(L"sid-1");
                CHECK(u.rootId == t.rootId && u.webUrl == t.webUrl && u.cardHash == t.cardHash, "teams: the root and the 64-bit hash");
                CHECK(u.createdMs == t.createdMs && u.lastCardEditMs == t.lastCardEditMs && u.lastReplyMs == t.lastReplyMs,
                      "teams: the thread's times");
                CHECK(u.transcriptPath == t.transcriptPath && u.transcriptOffset == t.transcriptOffset && u.historyDone,
                      "teams: the mirror's cursor");
                CHECK(u.rootReactions.size() == 1 && u.rootReactions[0].emoji == t.rootReactions[0].emoji && u.rootReactions[0].atMs == 7,
                      "teams: the root's reactions");
            }
            CHECK(r->watched.size() == 1 && r->watched[0].kind == T::MessageKind::Question && r->watched[0].payload == L"q-1" &&
                      r->watched[0].expiresMs == w.expiresMs && r->watched[0].reactions.size() == 1,
                  "teams: a watched question round-trips");
            CHECK(r->ownPosts.size() == 1 && r->ownPosts.at(L"200") == 1'795'000'000'000LL, "teams: the posts journal");
            CHECK(r->ownReactions.size() == 1 && r->ownReactions[0].messageId == L"200", "teams: the reactions journal");
            CHECK(r->legendReactions.size() == 1, "teams: the legend's reactions");
        }

        json::Value newer = T::ChannelStateToJson(s);
        for (auto& [k, v] : newer.members)
        {
            if (k == L"version")
            {
                v = json::Value::MkNum(T::kChannelStateVersion + 1);
            }
        }
        CHECK(!T::ChannelStateFromJson(newer).has_value(), "teams: a newer format is refused (never written over)");
        CHECK(!T::ChannelStateFromJson(json::Value::MkArr()).has_value(), "teams: a document that is no object is refused");
        CHECK(T::ChannelStateFromJson(json::Value::MkObj()).has_value(), "teams: an empty object is an empty state");

        for (const T::MessageKind k : { T::MessageKind::SessionCard, T::MessageKind::Question, T::MessageKind::Confirm,
                                        T::MessageKind::QueuedPrompt, T::MessageKind::Legend, T::MessageKind::Backlog,
                                        T::MessageKind::Step, T::MessageKind::ForkChoice, T::MessageKind::Other })
        {
            CHECK(T::MessageKindFromWord(T::MessageKindWord(k)) == k, "teams: every message kind round-trips by word");
        }
        CHECK(T::MessageKindFromWord(L"nonsense") == T::MessageKind::Other, "teams: an unknown kind reads Other");

        // Past the keep time for BOTH journal entries above (the newer one, 1'795'000'000'500, is the one that decides).
        const int64_t now = 1'795'000'001'000LL + T::kJournalKeepMs + 1;
        T::ChannelState p = s;
        p.ownPosts[L"new"] = now;
        p.ownReactions.push_back({ L"new", T::NormalizeEmoji(T::kEmojiSeen), now });
        T::Watched open;
        open.messageId = L"keep";
        p.watched.push_back(open); // never expires on its own
        CHECK(T::PruneChannelState(p, now), "teams: pruning drops something");
        CHECK(p.ownPosts.size() == 1 && p.ownPosts.count(L"new") == 1, "teams: old posts leave the journal, new stay");
        CHECK(p.ownReactions.size() == 1 && p.ownReactions[0].messageId == L"new", "teams: old reactions leave the journal");
        CHECK(p.watched.size() == 1 && p.watched[0].messageId == L"keep", "teams: an expired watch leaves, an open one stays");
        CHECK(p.threads.size() == s.threads.size(), "teams: threads are never pruned");
        CHECK(!T::PruneChannelState(p, now), "teams: a second prune changes nothing");
    }

    /// <summary>Pins the Graph addresses, the paths and the sign-in forms (§12): a bearer token can only ever go to Graph.</summary>
    void TestTeamsGraphAddress()
    {
        const auto path = [](std::wstring_view a) { return T::GraphRequestPath(a).value_or(L"<refused>"); };
        CHECK(path(L"/v1.0/me") == L"/v1.0/me", "teams: a v1.0 path is taken as it is");
        CHECK(path(L"/beta/teams/t/channels") == L"/beta/teams/t/channels", "teams: a beta path is taken as it is");
        CHECK(path(L"https://graph.microsoft.com/v1.0/teams/t/channels/c/messages/delta?$deltatoken=ab-_%3D") ==
                  L"/v1.0/teams/t/channels/c/messages/delta?$deltatoken=ab-_%3D",
              "teams: a delta link becomes its path and query");
        CHECK(path(L"HTTPS://Graph.Microsoft.COM/v1.0/x") == L"/v1.0/x", "teams: the scheme and the host are compared without case");
        CHECK(path(L"https://example.com/v1.0/x") == L"<refused>", "teams: another host is refused");
        CHECK(path(L"https://graph.microsoft.com.example.com/v1.0/x") == L"<refused>", "teams: a host that only starts like Graph is refused");
        CHECK(path(L"https://graph.microsoft.com@example.com/v1.0/x") == L"<refused>", "teams: a URL whose host is after user info is refused");
        CHECK(path(L"https://x@graph.microsoft.com/v1.0/x") == L"<refused>", "teams: user info before Graph is refused");
        CHECK(path(L"https://graph.microsoft.com:443/v1.0/x") == L"<refused>", "teams: a port is refused");
        CHECK(path(L"http://graph.microsoft.com/v1.0/x") == L"<refused>", "teams: plain http is refused");
        CHECK(path(L"//example.com/v1.0/x") == L"<refused>", "teams: a scheme-relative link is refused");
        CHECK(path(L"https://graph.microsoft.com") == L"<refused>", "teams: a host without a path is refused");
        CHECK(path(L"/v2.0/x") == L"<refused>", "teams: a path outside v1.0 and beta is refused");
        CHECK(path(L"/v1.0/a b") == L"<refused>", "teams: a space is refused");
        CHECK(path(L"/v1.0/a\\b") == L"<refused>", "teams: a backslash is refused");
        CHECK(path(L"/v1.0/a#b") == L"<refused>", "teams: a fragment is refused");
        CHECK(path(L"/v1.0/../x") == L"<refused>", "teams: a dot-dot segment is refused");
        CHECK(path(L"/v1.0/a/..") == L"<refused>", "teams: a trailing dot-dot segment is refused");
        CHECK(path(L"") == L"<refused>", "teams: an empty address is refused");

        CHECK(T::FormUrlEncode(T::kAppScope) == "https%3A%2F%2Fgraph.microsoft.com%2F.default%20offline_access",
              "teams: the scope is form-encoded byte by byte");
        CHECK(T::FormUrlEncode(L"a-b.c_d~e") == "a-b.c_d~e", "teams: unreserved characters stay");
        CHECK(T::FormUrlEncode(L"é") == "%C3%A9", "teams: a non-ASCII character is its UTF-8 bytes escaped");
        CHECK(T::PathSegment(L"19:abc_DEF-1.2~@thread.tacv2") == L"19:abc_DEF-1.2~@thread.tacv2", "teams: a channel id stays as it is");
        CHECK(T::PathSegment(L"a b/c?d") == L"a%20b%2Fc%3Fd", "teams: a space, a slash and a '?' are escaped in a segment");
        CHECK(T::ChannelMessagesPath(L"team-1", L"19:c@thread.tacv2") == L"/v1.0/teams/team-1/channels/19:c@thread.tacv2/messages",
              "teams: the channel's messages path");
        CHECK(T::MessagePath(L"t", L"c", L"123") == L"/v1.0/teams/t/channels/c/messages/123", "teams: a root's path");
        CHECK(T::MessagePath(L"t", L"c", L"123", L"456") == L"/v1.0/teams/t/channels/c/messages/123/replies/456", "teams: a reply's path");
        CHECK(T::RepliesPath(L"t", L"c", L"123") == L"/v1.0/teams/t/channels/c/messages/123/replies", "teams: a root's replies");
        CHECK(T::GraphRequestPath(T::MessagePath(L"t", L"19:c@thread.tacv2", L"1", L"2")).has_value(), "teams: a built path passes the guard");

        CHECK(T::IsTenantWord(L"organizations") && T::IsTenantWord(L"common"), "teams: the shared tenants are words");
        CHECK(T::IsTenantWord(L"11111111-2222-3333-4444-555555555555"), "teams: a tenant id is a word");
        CHECK(T::IsTenantWord(L"example.onmicrosoft.com"), "teams: a domain is a word");
        CHECK(!T::IsTenantWord(L"") && !T::IsTenantWord(L"a/b") && !T::IsTenantWord(L"a?b") && !T::IsTenantWord(L"a b"),
              "teams: a tenant that would reshape the path is refused");
        CHECK(!T::IsTenantWord(L"..") && !T::IsTenantWord(L"-x"), "teams: a tenant starting with a dot or a dash is refused");
        CHECK(!T::IsTenantWord(std::wstring(101, L'a')), "teams: a tenant over 100 characters is refused");
        CHECK(T::IsClientIdWord(L"11111111-2222-3333-4444-555555555555"), "teams: a client id is a GUID");
        CHECK(!T::IsClientIdWord(L"") && !T::IsClientIdWord(L"abc&x=1") && !T::IsClientIdWord(L"short"), "teams: anything else is no client id");

        const auto graphError = json::Parse(LR"({"error":{"code":"Forbidden","message":"Missing scope permissions on the request."}})");
        CHECK(graphError.has_value(), "teams: the Graph error parses");
        if (graphError)
        {
            const T::GraphError e = T::ReadGraphError(*graphError);
            CHECK(e.code == L"Forbidden" && e.message == L"Missing scope permissions on the request.", "teams: Graph's error form");
        }
        const auto oauthError = json::Parse(LR"({"error":"invalid_grant","error_description":"AADSTS70000: The grant is not valid.\r\nTrace ID: x\r\nTimestamp: y"})");
        CHECK(oauthError.has_value(), "teams: the sign-in error parses");
        if (oauthError)
        {
            const T::GraphError e = T::ReadGraphError(*oauthError);
            CHECK(e.code == L"invalid_grant" && e.message == L"AADSTS70000: The grant is not valid.", "teams: the sign-in's form, its first line only");
        }
        CHECK(T::ReadGraphError(json::Value::MkObj()).code.empty(), "teams: a body without an error has none");
    }

    /// <summary>Pins the sign-in's answers (§13): the device code, the token endpoint's outcomes, the Azure CLI's output.</summary>
    void TestTeamsSignInAnswers()
    {
        const auto code = json::Parse(
            LR"({"user_code":"ABCD1234","device_code":"DC-secret","verification_uri":"https://microsoft.com/devicelogin","expires_in":900,"interval":5,"message":"To sign in, use a web browser"})");
        CHECK(code.has_value(), "teams: the device-code answer parses");
        if (code)
        {
            const auto a = T::ParseDeviceCodeAnswer(*code);
            CHECK(a.has_value(), "teams: a full device-code answer is read");
            if (a)
            {
                CHECK(a->userCode == L"ABCD1234" && a->deviceCode == L"DC-secret", "teams: the codes");
                CHECK(a->verificationUri == L"https://microsoft.com/devicelogin", "teams: the address");
                CHECK(a->expiresInSec == 900 && a->intervalSec == 5, "teams: the lifetime and the interval");
            }
        }
        const auto codeAsText = json::Parse(LR"({"user_code":"X","device_code":"D","verification_url":"https://u","expires_in":"600","interval":"0"})");
        if (codeAsText)
        {
            const auto a = T::ParseDeviceCodeAnswer(*codeAsText);
            CHECK(a && a->verificationUri == L"https://u", "teams: the older spelling verification_url is read");
            CHECK(a && a->expiresInSec == 600, "teams: a lifetime sent as digits is read");
            CHECK(a && a->intervalSec == 5, "teams: an interval of 0 reads as 5 s");
        }
        const auto codeSlow = json::Parse(LR"({"user_code":"X","device_code":"D","verification_uri":"https://u","interval":600})");
        if (codeSlow)
        {
            const auto a = T::ParseDeviceCodeAnswer(*codeSlow);
            CHECK(a && a->intervalSec == 60 && a->expiresInSec == 900, "teams: an interval is capped at 60 s; a missing lifetime is 900 s");
        }
        const auto noCode = json::Parse(LR"({"user_code":"X","verification_uri":"https://u"})");
        CHECK(noCode && !T::ParseDeviceCodeAnswer(*noCode).has_value(), "teams: an answer without a device code is none");

        const auto ok = json::Parse(LR"({"token_type":"Bearer","scope":"x","expires_in":3599,"access_token":"AT","refresh_token":"RT"})");
        if (ok)
        {
            const T::TokenAnswer a = T::ParseTokenAnswer(*ok);
            CHECK(a.accessToken == L"AT" && a.refreshToken == L"RT" && a.expiresInSec == 3599, "teams: a token answer is read");
            CHECK(T::ClassifySignInPoll(a) == T::SignInPoll::Done, "teams: a token is Done");
        }
        const auto poll = [](const wchar_t* errorCode) {
            json::Value body = json::Value::MkObj();
            body.Set(L"error", json::Value::MkStr(errorCode));
            body.Set(L"error_description", json::Value::MkStr(L"AADSTS0: words\r\nTrace ID: x"));
            return T::ClassifySignInPoll(T::ParseTokenAnswer(body));
        };
        CHECK(poll(L"authorization_pending") == T::SignInPoll::Pending, "teams: pending");
        CHECK(poll(L"slow_down") == T::SignInPoll::SlowDown, "teams: slow down");
        CHECK(poll(L"authorization_declined") == T::SignInPoll::Declined, "teams: declined");
        CHECK(poll(L"expired_token") == T::SignInPoll::Expired, "teams: expired");
        CHECK(poll(L"invalid_grant") == T::SignInPoll::Failed, "teams: any other refusal fails");
        CHECK(T::ClassifySignInPoll(T::ParseTokenAnswer(json::Value::MkArr())) == T::SignInPoll::Failed, "teams: a body that is no answer fails");

        const auto az = T::ParseAzCliTokenAnswer(
            L"{\n  \"accessToken\": \"AT\",\n  \"expiresOn\": \"2026-11-18 15:04:05.000000\",\n  \"expires_on\": 1795000000,\n  \"tokenType\": \"Bearer\"\n}\n");
        CHECK(az && az->accessToken == L"AT" && az->expiresMs == 1795000000000LL, "teams: the Azure CLI's token and expiry");
        const auto azText = T::ParseAzCliTokenAnswer(L"WARNING: a note\n{\"accessToken\": \"AT2\", \"expires_on\": \"1795000000\"}");
        CHECK(azText && azText->accessToken == L"AT2" && azText->expiresMs == 1795000000000LL,
              "teams: text before the object is skipped; an expiry as digits is read");
        const auto azNoExpiry = T::ParseAzCliTokenAnswer(L"{\"accessToken\": \"AT3\"}");
        CHECK(azNoExpiry && azNoExpiry->expiresMs == 0, "teams: no expiry reads as 0 (the token's own exp is used)");
        CHECK(!T::ParseAzCliTokenAnswer(L"{\"tokenType\": \"Bearer\"}").has_value(), "teams: an answer without a token is none");
        CHECK(!T::ParseAzCliTokenAnswer(L"ERROR: Please run 'az login' to setup account.").has_value(), "teams: an error text is none");
    }

    /// <summary>Pins the refresh token's parts in Credential Manager (§14).</summary>
    void TestTeamsCredentialParts()
    {
        CHECK(T::SplitSecretParts("").empty(), "teams: an empty secret has no parts");
        CHECK(T::SplitSecretParts(std::string(T::kCredentialPartBytes, 'a')).size() == 1, "teams: a full part is one part");
        const auto two = T::SplitSecretParts(std::string(T::kCredentialPartBytes + 1, 'b'));
        CHECK(two.size() == 2 && two[0].size() == T::kCredentialPartBytes && two[1].size() == 1, "teams: one byte more is a second part");
        const std::string secret = std::string(3000, 'x') + "tail";
        std::string joined;
        for (const std::string& p : T::SplitSecretParts(secret))
        {
            joined += p;
        }
        CHECK(joined == secret, "teams: the parts join back to the secret");
        CHECK(T::SplitSecretParts(std::string(T::kCredentialPartBytes * T::kCredentialMaxParts, 'c')).size() == T::kCredentialMaxParts,
              "teams: the largest secret fills every part");
        CHECK(T::SplitSecretParts(std::string(T::kCredentialPartBytes * T::kCredentialMaxParts + 1, 'c')).empty(), "teams: a larger one is refused");
        CHECK(T::CredentialPartTarget(L"base", 0) == L"base" && T::CredentialPartTarget(L"base", 1) == L"base#2",
              "teams: the first part is the base, the next ones are numbered from 2");
        CHECK(T::CredentialUserWord(3, L"cid") == L"teams:3:cid", "teams: the user word");
        const auto parsed = T::ParseCredentialUserWord(L"teams:3:cid");
        CHECK(parsed && parsed->first == 3 && parsed->second == L"cid", "teams: the user word reads back");
        CHECK(!T::ParseCredentialUserWord(L"teams:0:cid") && !T::ParseCredentialUserWord(L"teams:9:cid"), "teams: a part count outside 1..8 is refused");
        CHECK(!T::ParseCredentialUserWord(L"bot") && !T::ParseCredentialUserWord(L"teams:2:") && !T::ParseCredentialUserWord(L"teams:x:cid"),
              "teams: any other name is refused");
    }

    /// <summary>Pins the pacing, the state file's name and the status line (§15).</summary>
    void TestTeamsStatusAndPacing()
    {
        CHECK(T::RetryBackoffMs(0) == 1000 && T::RetryBackoffMs(1) == 1000, "teams: the first retry waits 1 s");
        CHECK(T::RetryBackoffMs(2) == 2000 && T::RetryBackoffMs(3) == 4000, "teams: the wait doubles");
        CHECK(T::RetryBackoffMs(7) == 60'000 && T::RetryBackoffMs(100) == 60'000, "teams: the wait stops at 60 s");
        CHECK(T::ThrottleWaitMs(0) == 5000 && T::ThrottleWaitMs(3) == 3000 && T::ThrottleWaitMs(100'000) == 600'000,
              "teams: a 429 waits its Retry-After, 5 s without one, 10 minutes at most");

        const std::wstring leaf = T::ChannelStateFileLeaf(L"team", L"19:c@thread.tacv2");
        CHECK(leaf.size() == 19 && leaf.substr(0, 6) == L"state-" && leaf.substr(14) == L".json", "teams: state-<8 hex>.json");
        CHECK(leaf == T::ChannelStateFileLeaf(L"team", L"19:c@thread.tacv2"), "teams: the same channel, the same file");
        CHECK(leaf != T::ChannelStateFileLeaf(L"team", L"19:d@thread.tacv2"), "teams: another channel, another file");

        T::StatusFacts f;
        CHECK(T::BuildStatusLine(f) == L"Teams: off", "teams: off");
        f.enabled = true;
        CHECK(T::BuildStatusLine(f) == L"Teams: set the team and the channel (teamsTeamId, teamsChannelId)", "teams: no channel");
        f.channelSet = true;
        f.problem = L"the legend could not be posted (HTTP 403)";
        CHECK(T::BuildStatusLine(f) == L"Teams: stopped · the legend could not be posted (HTTP 403)", "teams: stopped, with the problem");
        f.problem.clear();
        f.running = true;
        f.signInUri = L"https://microsoft.com/devicelogin";
        f.signInCode = L"ABCD1234";
        f.signInUntil = L"14:32";
        CHECK(T::BuildStatusLine(f) ==
                  L"Teams: sign in: open https://microsoft.com/devicelogin and enter ABCD1234 (until 14:32) · nothing can post yet (sign in to the app, or run az login)",
              "teams: a sign-in waits and nothing can post");
        f.signInUri.clear();
        f.signInCode.clear();
        f.signInUntil.clear();
        f.ownerName = L"Owner";
        f.postsBy = L"the app";
        f.editsBy = L"the Azure CLI";
        f.readsBy = L"the app";
        f.reactsBy = L"the app";
        f.threads = 1;
        f.legendPosted = true;
        CHECK(T::BuildStatusLine(f) ==
                  L"Teams: on as Owner · 1 thread · posts by the app, edits by the Azure CLI, reads by the app, reactions by the app",
              "teams: on, every call routed");
        f.editsBy.clear();
        f.readsBy.clear();
        f.reactsBy.clear();
        f.threads = 2;
        f.legendPosted = false;
        CHECK(T::BuildStatusLine(f) ==
                  L"Teams: on as Owner · 2 threads · posts by the app, no edits, no reads (a mirror only), no reactions · the legend is not posted yet",
              "teams: on, the degraded features named");
    }

    /// <summary>Pins the connector's settings (§10): what a settings.json written before them reads as, and a round trip.</summary>
    void TestTeamsSettings()
    {
        const AppSettings fresh = AppSettingsFromJson(json::Value::MkObj());
        CHECK(!fresh.teamsEnabled && fresh.teamsTeamId.empty() && fresh.teamsChannelId.empty() && fresh.teamsClientId.empty(),
              "teams: absent settings read as off, no channel, no app");
        CHECK(fresh.teamsTenantId == L"organizations" && fresh.teamsUseAzureCli && !fresh.teamsVerbose && fresh.teamsForeignMarkers == L"\U0001F916",
              "teams: absent settings read as the shared tenant, the Azure CLI on, no tool lines, the robot marker");
        AppSettings s;
        s.teamsEnabled = true;
        s.teamsTeamId = L"team";
        s.teamsChannelId = L"19:c@thread.tacv2";
        s.teamsClientId = L"11111111-2222-3333-4444-555555555555";
        s.teamsTenantId = L"tenant-1";
        s.teamsUseAzureCli = false;
        s.teamsVerbose = true;
        s.teamsForeignMarkers = L"[bot] \U0001F916";
        const AppSettings back = AppSettingsFromJson(ToJson(s));
        CHECK(back.teamsEnabled && back.teamsTeamId == L"team" && back.teamsChannelId == L"19:c@thread.tacv2" && back.teamsClientId == s.teamsClientId,
              "teams: the switch, the channel and the app round-trip");
        CHECK(back.teamsTenantId == L"tenant-1" && !back.teamsUseAzureCli && back.teamsVerbose && back.teamsForeignMarkers == s.teamsForeignMarkers,
              "teams: the tenant, the Azure CLI switch, verbose and the markers round-trip");

        AppSettings pasted = s;
        pasted.teamsTeamId = L" team\r\n";
        pasted.teamsChannelId = L"\t19:c@thread.tacv2 ";
        pasted.teamsTenantId = L"  ";
        const T::TeamsConfig c = T::TeamsConfigFrom(pasted);
        CHECK(c.teamId == L"team" && c.channelId == L"19:c@thread.tacv2", "teams: pasted ids are trimmed");
        CHECK(c.tenant == L"organizations", "teams: a blank tenant reads as organizations");
        AppSettings linked = s;
        linked.teamsChannelId = L" 19%3ac%40thread.tacv2\n";
        CHECK(T::TeamsConfigFrom(linked).channelId == L"19:c@thread.tacv2", "teams: a channel id copied from a link is decoded");
        CHECK(T::detail::PercentDecodeAscii(L"a%4 b%zz c%E2%9C") == L"a%4 b%zz c%E2%9C" && T::detail::PercentDecodeAscii(L"%41%3A") == L"A:",
              "teams: only whole ASCII escapes are decoded");
        CHECK(T::WantsToRun(c), "teams: on with a team and a channel runs");
        T::TeamsConfig off = c;
        off.enabled = false;
        CHECK(!T::WantsToRun(off), "teams: off does not run");
        T::TeamsConfig noChannel = c;
        noChannel.channelId.clear();
        CHECK(!T::WantsToRun(noChannel), "teams: no channel does not run");
        T::TeamsConfig live = c;
        live.verbose = !c.verbose;
        live.foreignMarkers = L"x";
        CHECK(!T::RestartNeeded(c, live), "teams: verbose and the markers apply while running");
        T::TeamsConfig other = c;
        other.channelId = L"19:d@thread.tacv2";
        CHECK(T::RestartNeeded(c, other), "teams: another channel restarts");
        other = c;
        other.clientId = L"22222222-2222-3333-4444-555555555555";
        CHECK(T::RestartNeeded(c, other), "teams: another app restarts");
        other = c;
        other.useAzureCli = !c.useAzureCli;
        CHECK(T::RestartNeeded(c, other), "teams: the Azure CLI switch restarts");

        std::vector<T::FleetLine> fleet(1);
        fleet[0].title = L"one";
        CHECK(T::BuildLegendHtml(fleet, L"", 7).find(L"… and 7 more") != std::wstring::npos, "teams: the board counts the sessions it leaves out");
        CHECK(T::BuildLegendHtml(fleet, L"").find(L"more</i>") == std::wstring::npos, "teams: a board that lists every session counts none");
    }

    /// <summary>A transcript item for the mirror tests.</summary>
    /// <param name="kind">Its kind.</param>
    /// <param name="text">Its text.</param>
    /// <param name="tsMs">Its time (Unix ms).</param>
    /// <param name="turnEnds">Its stop reason ends the turn (a Text).</param>
    /// <returns>The item.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be copied.</exception>
    Msg::TopicItem Item(Msg::TopicItemKind kind, const std::wstring& text, int64_t tsMs, bool turnEnds = false)
    {
        Msg::TopicItem it;
        it.kind = kind;
        it.text = text;
        it.tsMs = tsMs;
        it.turnEnds = turnEnds;
        return it;
    }

    /// <summary>A transcript line of one item for the mirror tests.</summary>
    /// <param name="at">Its first byte.</param>
    /// <param name="end">The byte after its newline.</param>
    /// <param name="it">Its item.</param>
    /// <returns>The line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    T::MirrorLine Line(int64_t at, int64_t end, Msg::TopicItem it)
    {
        T::MirrorLine line;
        line.at = at;
        line.end = end;
        line.items.push_back(std::move(it));
        return line;
    }

    /// <summary>Pins section 17: who gets a thread, the card's facts, the live mirror's decisions and commits, the history's packing and hand-over.</summary>
    void TestTeamsMirror()
    {
        using K = Msg::TopicItemKind;
        const int64_t now = 1'795'000'000'000LL;
        const int64_t fresh = now - 1000;
        const int64_t alertSince = now - T::kAlertFreshMs;

        SessionInfo q;
        q.kind = AgentKind::Claude;
        q.live = true;
        q.tabToken = L"tab";
        q.state = SessionState::Running;
        CHECK(T::QualifiesForThread(q, now, 600'000), "teams: a running Claude tab gets a thread");
        q.state = SessionState::Idle;
        q.lastActivityUnixMs = now - 1000;
        CHECK(T::QualifiesForThread(q, now, 600'000), "teams: an idle tab used within the grace gets one");
        q.lastActivityUnixMs = now - 3'600'000;
        CHECK(!T::QualifiesForThread(q, now, 600'000), "teams: an idle tab past the grace does not");
        q.pinned = true;
        CHECK(T::QualifiesForThread(q, now, 600'000), "teams: a pinned tab does");
        q.pinned = false;
        q.state = SessionState::Running;
        q.tabToken.clear();
        CHECK(!T::QualifiesForThread(q, now, 600'000), "teams: a session without a tab does not");
        q.tabToken = L"tab";
        q.kind = AgentKind::Codex;
        CHECK(!T::QualifiesForThread(q, now, 600'000), "teams: a Codex session does not");

        SessionInfo s;
        s.id = L"12345678-aaaa-bbbb";
        s.state = SessionState::Running;
        s.workingDir = L"C:\\src\\app\\sub";
        s.branch = L"main";
        s.model = L"claude-opus-5-5";
        s.effort = L"max";
        s.contextTokens = 182'000;
        s.autorunner.mode = AutorunnerMode::Full;
        s.live = true;
        QueuedPrompt pending;
        pending.status = PromptStatus::Pending;
        s.queue.push_back(pending);
        QueuedPrompt sent;
        sent.status = PromptStatus::Sent;
        s.queue.push_back(sent);
        const T::CardFacts card = T::CardFactsFromSession(s, L"#3366FF", 0, L"first words");
        CHECK(card.title == L"12345678", "teams: an untitled session's card names its id's first 8 characters");
        CHECK(card.folder.find(L"sub") != std::wstring::npos && card.branch == L"main", "teams: the card's folder and branch");
        CHECK(card.model == Msg::CardModelLabel(L"claude-opus-5-5") && card.effort == L"max" && card.contextWord == L"182K", "teams: the card's model, effort, context");
        CHECK(card.runnerAuto && card.queued == 1, "teams: the runner and only the PENDING prompts");
        CHECK(card.live && card.colorHex == L"#3366FF" && card.firstPrompt == L"first words", "teams: open, the color, the kept first prompt");

        // A prompt posts at once, silently; a reply whose stop reason ends the turn posts with the turn's alert.
        {
            T::MirrorState st;
            const auto posts = T::MirrorLines(st, { Line(0, 100, Item(K::Prompt, L"fix it", fresh)), Line(100, 200, Item(K::Text, L"done", fresh, true)) }, 200,
                                              false, false, alertSince, now);
            CHECK(posts.size() == 2, "teams: a prompt and its reply are two posts");
            if (posts.size() == 2)
            {
                CHECK(posts[0].alert == T::AlertKind::None && posts[0].commitTo == 100 && posts[0].html.find(L"fix it") != std::wstring::npos,
                      "teams: the prompt is silent and commits its line");
                CHECK(posts[1].alert == T::AlertKind::TurnEnd && posts[1].commitTo == 200 && posts[1].html.find(L"Claude") != std::wstring::npos &&
                          posts[1].html.find(L"done") != std::wstring::npos,
                      "teams: the final reply alerts and commits its line");
            }
            CHECK(!st.holding, "teams: nothing is held after the turn ended");
        }
        // A text without an ending stop reason is held, and a tool call makes it a remark (not posted unless verbose).
        {
            T::MirrorState st;
            auto posts = T::MirrorLines(st, { Line(0, 100, Item(K::Prompt, L"p", fresh)), Line(100, 200, Item(K::Text, L"let me look", fresh)) }, 200, false,
                                        false, alertSince, now);
            CHECK(posts.size() == 1 && st.holding && st.heldAt == 100, "teams: a text that may be a remark is held");
            Msg::TopicItem call = Item(K::ToolCall, L"", fresh);
            call.summary = L"Bash: ls";
            posts = T::MirrorLines(st, { Line(200, 300, call) }, 300, false, false, alertSince, now);
            CHECK(posts.empty() && !st.holding, "teams: a tool call makes the held text a remark, not posted when not verbose");
        }
        // While a text is held, no commit passes its line: a restart reads it again.
        {
            T::MirrorState st;
            Msg::TopicItem mid = Item(K::Prompt, L"also this", fresh);
            mid.midTurn = true;
            const auto posts = T::MirrorLines(st, { Line(0, 100, Item(K::Prompt, L"p", fresh)), Line(100, 200, Item(K::Text, L"working", fresh)), Line(200, 300, mid) },
                                              300, false, false, alertSince, now);
            CHECK(posts.size() == 2 && posts[1].commitTo == 100, "teams: a message typed mid-turn commits only up to the held text");
        }
        // The session rests and everything was read: the held text is the final reply.
        {
            T::MirrorState st;
            const auto posts =
                T::MirrorLines(st, { Line(0, 100, Item(K::Prompt, L"p", fresh)), Line(100, 200, Item(K::Text, L"answer", fresh)) }, 200, true, false, alertSince, now);
            CHECK(posts.size() == 2 && posts[1].alert == T::AlertKind::TurnEnd && posts[1].commitTo == 200, "teams: at rest, the held text is the final reply");
        }
        // The next turn's prompt ends the last turn: its held text is the final reply, committed up to the new prompt.
        {
            T::MirrorState st;
            T::MirrorLines(st, { Line(0, 100, Item(K::Prompt, L"p", fresh)), Line(100, 200, Item(K::Text, L"answer", fresh)) }, 200, false, false, alertSince, now);
            const auto posts = T::MirrorLines(st, { Line(300, 400, Item(K::Prompt, L"next", fresh)) }, 400, false, false, alertSince, now);
            CHECK(posts.size() == 2 && posts[0].alert == T::AlertKind::TurnEnd && posts[0].commitTo == 300 && posts[1].commitTo == 400,
                  "teams: a new turn's prompt makes the held text the last turn's final reply");
        }
        // An old item never rings (a restart's backlog).
        {
            T::MirrorState st;
            const auto posts = T::MirrorLines(st, { Line(0, 100, Item(K::Text, L"old", now - T::kAlertFreshMs - 1, true)) }, 100, false, false, alertSince, now);
            CHECK(posts.size() == 1 && posts[0].alert == T::AlertKind::None, "teams: an old reply posts silently");
        }
        // A question and a plan alert as "needs you", an API error as an error, an interrupt not at all.
        {
            T::MirrorState st;
            Msg::TopicItem question = Item(K::Question, L"Which one?", fresh);
            question.options = { L"A", L"B" };
            const auto posts = T::MirrorLines(st,
                                              { Line(0, 100, question), Line(100, 200, Item(K::Plan, L"1. do it", fresh)), Line(200, 300, Item(K::ApiError, L"overloaded", fresh)),
                                                Line(300, 400, Item(K::Interrupt, L"", fresh)) },
                                              400, false, false, alertSince, now);
            CHECK(posts.size() == 4, "teams: four posts");
            if (posts.size() == 4)
            {
                CHECK(posts[0].alert == T::AlertKind::NeedsYou && posts[0].html.find(L"1. A<br>2. B") != std::wstring::npos, "teams: a question alerts, its options numbered");
                CHECK(posts[1].alert == T::AlertKind::NeedsYou, "teams: a plan alerts");
                CHECK(posts[2].alert == T::AlertKind::Error, "teams: an API error alerts as an error");
                CHECK(posts[3].alert == T::AlertKind::None && posts[3].html.find(L"interrupted") != std::wstring::npos, "teams: an interrupt is silent");
            }
        }
        // Verbose: the remark and the tool line are one narrative post, committed up to the held reply, then the reply.
        {
            T::MirrorState st;
            Msg::TopicItem call = Item(K::ToolCall, L"", fresh);
            call.summary = L"Bash: npm test";
            const auto posts = T::MirrorLines(st,
                                              { Line(0, 100, Item(K::Prompt, L"p", fresh)), Line(100, 200, Item(K::Text, L"checking", fresh)), Line(200, 300, call),
                                                Line(300, 400, Item(K::Text, L"all green", fresh, true)) },
                                              400, false, true, alertSince, now);
            CHECK(posts.size() == 3, "teams: verbose: the prompt, the narrative, the reply");
            if (posts.size() == 3)
            {
                CHECK(posts[1].html.find(L"checking") != std::wstring::npos && posts[1].html.find(L"npm test") != std::wstring::npos && posts[1].commitTo == 300,
                      "teams: the narrative holds the remark and the tool line, committed up to the reply's line");
                CHECK(posts[2].alert == T::AlertKind::TurnEnd && posts[2].commitTo == 400, "teams: the reply follows");
            }
        }
        // A prompt the owner wrote in the thread is not posted again; its turn still starts there.
        {
            T::MirrorState st;
            int asked = 0;
            const std::function<bool(const std::wstring&)> postedInTeams = [&asked](const std::wstring& text) {
                ++asked;
                return text == L"from teams";
            };
            const auto posts = T::MirrorLines(st, { Line(0, 100, Item(K::Prompt, L"from teams", fresh)), Line(100, 200, Item(K::Text, L"ok", fresh, true)) }, 200,
                                              false, false, alertSince, now, {}, postedInTeams);
            CHECK(asked == 1 && posts.size() == 1 && posts[0].alert == T::AlertKind::TurnEnd, "teams: only the reply is posted for a prompt written in Teams");
        }

        // The history's packing: entries share a post up to the limit; a post commits its newest turn end.
        {
            std::vector<Msg::HistoryPlanPost> plan(3);
            plan[0].post.html = L"a\nb";
            plan[1].post.html = L"c";
            plan[1].commitTo = 50;
            plan[2].commitTo = 80; // a turn that rendered nothing hands its end on
            const auto chunks = T::PackHistoryPosts(plan);
            CHECK(chunks.size() == 1 && chunks[0].html == L"a<br>b<br><br>c" && chunks[0].commitTo == 80, "teams: entries share one post, which commits the newest end");
            std::vector<Msg::HistoryPlanPost> big(3);
            for (size_t k = 0; k < big.size(); ++k)
            {
                big[k].post.html = std::wstring(40, L'x');
                big[k].commitTo = static_cast<int64_t>(10 * (k + 1));
            }
            const auto split = T::PackHistoryPosts(big, 90);
            CHECK(split.size() == 2 && split[0].commitTo == 20 && split[1].commitTo == 30, "teams: the size limit starts a new post");
            CHECK(T::PackHistoryPosts({}).empty(), "teams: an empty plan has no posts");
            std::vector<Msg::HistoryPlanPost> only(1);
            only[0].commitTo = 5;
            const auto commitOnly = T::PackHistoryPosts(only);
            CHECK(commitOnly.size() == 1 && commitOnly[0].html.empty() && commitOnly[0].commitTo == 5, "teams: a commit with nothing to post");
        }
        // The hand-over: the history stops before the window's newest turn.
        {
            Msg::HistoryWindow w;
            w.items = { Item(K::Prompt, L"one", 1), Item(K::Text, L"r1", 2), Item(K::Prompt, L"two", 3), Item(K::Text, L"r2", 4) };
            w.itemAt = { 0, 100, 200, 300 };
            w.consumedTo = 400;
            CHECK(T::CutNewestTurn(w) == 200 && w.items.size() == 2 && w.consumedTo == 200, "teams: the newest turn goes to the live mirror");
            Msg::HistoryWindow one;
            one.items = { Item(K::Prompt, L"only", 1), Item(K::Text, L"r", 2) };
            one.itemAt = { 0, 100 };
            one.consumedTo = 200;
            CHECK(T::CutNewestTurn(one) == 0 && one.items.empty() && one.consumedTo == 0, "teams: a window of one turn leaves no history");
            Msg::HistoryWindow none;
            none.items = { Item(K::Text, L"mid-turn", 1) };
            none.itemAt = { 0 };
            none.consumedTo = 100;
            CHECK(T::CutNewestTurn(none) == 100 && none.items.size() == 1, "teams: a window with no turn start is all history");
        }
    }

    /// <summary>Pins section 18: a delta page, the owner's new replies, a prompt's status emoji, the backlog offer, the answers.</summary>
    void TestTeamsInboundPoll()
    {
        const auto first = json::Parse(
            LR"({"@odata.nextLink":"https://graph.microsoft.com/v1.0/teams/t/channels/c/messages/delta?$skiptoken=1","value":[{"id":"1","createdDateTime":"2026-11-18T10:00:00Z","from":{"user":{"id":"owner"}},"body":{"contentType":"html","content":"<p>hi</p>"}},{"id":""}]})");
        CHECK(first.has_value(), "teams: the delta page parses");
        if (first)
        {
            const T::DeltaPage p = T::ParseDeltaPage(*first);
            CHECK(p.roots.size() == 1 && p.roots[0].id == L"1", "teams: a delta page's roots (one without an id is left out)");
            CHECK(p.nextLink.find(L"skiptoken=1") != std::wstring::npos && p.deltaLink.empty(), "teams: a middle page has a next link");
        }
        const auto last = json::Parse(LR"({"@odata.deltaLink":"https://graph.microsoft.com/v1.0/teams/t/channels/c/messages/delta?$deltatoken=2","value":[]})");
        if (last)
        {
            const T::DeltaPage p = T::ParseDeltaPage(*last);
            CHECK(p.roots.empty() && p.nextLink.empty() && p.deltaLink.find(L"deltatoken=2") != std::wstring::npos, "teams: the last page has the delta link");
        }
        CHECK(T::ParseDeltaPage(json::Value::MkArr()).roots.empty(), "teams: a body that is no page has nothing");

        const auto replies = json::Parse(LR"({"value":[
            {"id":"30","createdDateTime":"2026-11-18T10:00:30Z","messageType":"message","from":{"user":{"id":"owner"}},"body":{"content":"third"}},
            {"id":"20","createdDateTime":"2026-11-18T10:00:20Z","messageType":"message","from":{"user":{"id":"owner"}},"body":{"content":"ours"}},
            {"id":"15","createdDateTime":"2026-11-18T10:00:15Z","messageType":"message","from":{"user":{"id":"someone"}},"body":{"content":"other"}},
            {"id":"13","createdDateTime":"2026-11-18T10:00:13Z","messageType":"systemEventMessage","from":{"user":{"id":"owner"}},"body":{"content":"joined"}},
            {"id":"12","createdDateTime":"2026-11-18T10:00:12Z","messageType":"message","from":{"user":{"id":"owner"}},"body":{"content":"first"}},
            {"id":"5","createdDateTime":"2026-11-18T10:00:05Z","messageType":"message","from":{"user":{"id":"owner"}},"body":{"content":"old"}}]})");
        CHECK(replies.has_value(), "teams: the replies parse");
        if (replies)
        {
            const std::map<std::wstring, int64_t> own{ { L"20", 1 } };
            const auto fresh = T::NewOwnerReplies(*replies, L"owner", own, T::ParseGraphTimeMs(L"2026-11-18T10:00:10Z"));
            CHECK(fresh.size() == 2, "teams: two new replies of the owner");
            if (fresh.size() == 2)
            {
                CHECK(fresh[0].id == L"12" && fresh[1].id == L"30", "teams: oldest first; Agentmaster's own, another user's, a system message and an old reply are left out");
                CHECK(fresh[0].text == L"first", "teams: a reply's plain text");
            }
        }

        SessionInfo s;
        s.live = true;
        s.state = SessionState::Running;
        const auto prompt = [](const wchar_t* id, PromptStatus status, bool echoed) {
            QueuedPrompt q;
            q.id = id;
            q.status = status;
            q.echoed = echoed;
            return q;
        };
        s.queue = { prompt(L"a", PromptStatus::Pending, false), prompt(L"b", PromptStatus::Sent, false), prompt(L"c", PromptStatus::Sent, true),
                    prompt(L"d", PromptStatus::Failed, false) };
        CHECK(T::PromptReactionFor(s, L"a") == T::kEmojiSeen && T::PromptReactionFor(s, L"b") == T::kEmojiSeen, "teams: queued, or sent and not echoed: seen");
        CHECK(T::PromptReactionFor(s, L"c") == T::kEmojiWorking, "teams: echoed while the turn runs: working");
        CHECK(T::PromptReactionFor(s, L"d") == T::kEmojiWarn, "teams: failed: the warning");
        CHECK(T::PromptReactionFor(s, L"zz").empty(), "teams: a prompt the queue no longer holds: nothing");
        s.state = SessionState::WaitingForInput;
        CHECK(T::PromptReactionFor(s, L"c") == T::kEmojiDone, "teams: echoed and the turn is over: done");
        CHECK(T::IsFinalPromptReaction(T::kEmojiDone) && T::IsFinalPromptReaction(T::kEmojiWarn) && !T::IsFinalPromptReaction(T::kEmojiSeen),
              "teams: done and the warning end a prompt's follow");

        const std::wstring offer = T::BacklogOfferHtml(L"fix it\nand more", L"2 h");
        CHECK(offer.find(L"2 h ago") != std::wstring::npos && offer.find(L"fix it") != std::wstring::npos && offer.find(L"and more") == std::wstring::npos,
              "teams: the backlog offer quotes the first line and its age");
        CHECK(offer.find(T::kEmojiYes) != std::wstring::npos && offer.find(T::kEmojiNo) != std::wstring::npos, "teams: the offer names the yes and no emoji");
        Msg::RemoteCommand leaves;
        leaves.kind = Msg::RemoteKind::RefusedLeavesTab;
        leaves.name = L"resume";
        CHECK(T::RefusedCommandHtml(leaves).find(L"out of its tab") != std::wstring::npos, "teams: a command that leaves the tab is refused");
        Msg::RemoteCommand noRecord;
        noRecord.kind = Msg::RemoteKind::RefusedNoRecord;
        noRecord.name = L"config";
        CHECK(T::RefusedCommandHtml(noRecord).find(L"at the PC only") != std::wstring::npos, "teams: a command with no record is refused");
        CHECK(T::SpanWord(45'000) == L"45 s" && T::SpanWord(12 * 60'000) == L"12 min" && T::SpanWord(3 * 3'600'000) == L"3 h" && T::SpanWord(2 * 86'400'000LL) == L"2 d",
              "teams: spans in words");

        // The fingerprint reads letters and digits only, without case: what Teams rewrites (emoji, spacing, tags) does not count.
        CHECK(T::PostFingerprint(L"Hello, World 42!") == T::PostFingerprint(L"hello world42"), "teams: the fingerprint ignores case, spacing and punctuation");
        CHECK(T::PostFingerprint(L"\U0001F916 Claude · 14:05 done") == T::PostFingerprint(L"Claude 1405 done"), "teams: emoji and separators do not count");
        CHECK(T::PostFingerprint(L"done") != T::PostFingerprint(L"don e1"), "teams: other letters, another fingerprint");
        CHECK(T::PostFingerprint(L"\U0001F44C !") == 0, "teams: a post with no letter or digit has none");
    }

    /// <summary>Pins section 19: the confirms, their words, the screen's text, the answer for an action not built yet.</summary>
    void TestTeamsEmojiActions()
    {
        const std::wstring restart = T::ConfirmPostHtml(T::Action::Restart, L"drag fix");
        CHECK(restart.find(L"Restart drag fix?") != std::wstring::npos && restart.find(L"60 s") != std::wstring::npos, "teams: the restart confirm names the session");
        const std::wstring park = T::ConfirmPostHtml(T::Action::Park, L"");
        CHECK(park.find(L"this session") != std::wstring::npos && park.find(L"cuts it off") != std::wstring::npos, "teams: the park confirm says what it cuts");
        CHECK(T::ConfirmActionFromWord(T::ConfirmWord(T::Action::Restart)) == T::Action::Restart, "teams: the restart word reads back");
        CHECK(T::ConfirmActionFromWord(T::ConfirmWord(T::Action::Park)) == T::Action::Park, "teams: the park word reads back");
        CHECK(T::ConfirmActionFromWord(L"fork") == T::Action::None && std::wstring{ T::ConfirmWord(T::Action::Fork) }.empty(), "teams: nothing else is confirmed");
        CHECK(T::ScreenTextPostHtml(L"a < b\n\n") == L"<b>\U0001F5A5 Screen</b><pre>a &lt; b</pre>", "teams: the screen is an escaped code block, blank tail dropped");
        CHECK(T::ScreenTextPostHtml(L"  \n") == L"<i>The screen is empty.</i>", "teams: an empty screen says so");
        std::wstring big(T::kScreenTextMaxChars + 10, L'x');
        big[0] = L'Q';
        CHECK(T::ScreenTextPostHtml(big).find(L'Q') == std::wstring::npos, "teams: a long screen keeps its end");
        CHECK(T::NotYetHtml(T::Action::Fork).find(T::ActionWord(T::Action::Fork)) != std::wstring::npos, "teams: the not-yet answer names the action");
    }

    /// <summary>Pins section 20: Base64 (RFC 4648's own test vectors) and the picture post's body.</summary>
    void TestTeamsPictures()
    {
        CHECK(T::Base64Encode("").empty(), "teams: no bytes, no text");
        CHECK(T::Base64Encode("f") == "Zg==" && T::Base64Encode("fo") == "Zm8=" && T::Base64Encode("foo") == "Zm9v", "teams: RFC 4648 vectors 1-3");
        CHECK(T::Base64Encode("foob") == "Zm9vYg==" && T::Base64Encode("fooba") == "Zm9vYmE=" && T::Base64Encode("foobar") == "Zm9vYmFy", "teams: RFC 4648 vectors 4-6");
        CHECK(T::Base64Encode(std::string("\xFF\xFE", 2)) == "//4=", "teams: bytes above 0x7F");
        const json::Value post = T::BuildImagePostJson(L"<b>Screen</b>", std::string("\x89PNG", 4));
        const json::Value* body = post.Find(L"body");
        CHECK(body && body->StrAt(L"content") == L"<b>Screen</b><br><img src=\"../hostedContents/1/$value\">", "teams: the caption and the picture's reference");
        const json::Value* hosted = post.Find(L"hostedContents");
        CHECK(hosted && hosted->type == json::Value::Type::Arr && hosted->arr.size() == 1, "teams: one hosted content");
        if (hosted && hosted->arr.size() == 1)
        {
            const json::Value& h = hosted->arr[0];
            CHECK(h.StrAt(L"@microsoft.graph.temporaryId") == L"1" && h.StrAt(L"contentType") == L"image/png" && h.StrAt(L"contentBytes") == L"iVBORw==",
                  "teams: the picture's id, type and Base64 bytes");
        }
        CHECK(T::BuildImagePostJson(L"", std::string("x", 1)).Find(L"body")->StrAt(L"content") == L"<img src=\"../hostedContents/1/$value\">", "teams: no caption, the picture alone");
    }

    /// <summary>Pins section 21: the new-session step's payload and its text; and the thread record's kept first prompt.</summary>
    void TestTeamsWizard()
    {
        T::WizardData d;
        d.prompt = L"fix the crash\nplease";
        d.webUrl = L"https://teams.example/l/message/1";
        d.folders = { L"C:\\src\\a", L"D:\\b" };
        const T::WizardData back = T::ParseWizardPayload(T::WizardPayload(d));
        CHECK(back.prompt == d.prompt && back.webUrl == d.webUrl && back.folders == d.folders, "teams: the step's payload round-trips");
        T::WizardData sep;
        sep.prompt = std::wstring(L"a") + L'\x1F' + L"b";
        CHECK(T::ParseWizardPayload(T::WizardPayload(sep)).prompt == L"a b", "teams: a separator inside the prompt is replaced");
        CHECK(T::ParseWizardPayload(L"only words").prompt == L"only words" && T::ParseWizardPayload(L"only words").folders.empty(), "teams: a payload of words only");
        const std::wstring step = T::WizardStepHtml(d.prompt, d.folders);
        CHECK(step.find(L"fix the crash") != std::wstring::npos && step.find(L"please") == std::wstring::npos, "teams: the step quotes the first line");
        CHECK(step.find(T::KeycapEmoji(1) + L" <code>C:\\src\\a</code>") != std::wstring::npos && step.find(T::KeycapEmoji(2)) != std::wstring::npos,
              "teams: the folders are numbered with their keycaps");
        CHECK(T::WizardStepHtml(L"x", {}).find(L"Reply with the folder's full path.") != std::wstring::npos, "teams: no folder to list: a typed path");

        T::ChannelState s;
        s.teamId = L"t";
        s.channelId = L"c";
        T::Thread t;
        t.sessionId = L"sid";
        t.rootId = L"root";
        t.firstPrompt = L"the owner's words";
        s.threads[L"sid"] = t;
        const auto loaded = T::ChannelStateFromJson(T::ChannelStateToJson(s));
        CHECK(loaded && loaded->threads.count(L"sid") == 1 && loaded->threads.at(L"sid").firstPrompt == L"the owner's words", "teams: a thread's first prompt round-trips");
    }

    /// <summary>Pins section 22: the conversation pictures' part height, captions, cut notice and usage line.</summary>
    void TestTeamsAllScreen()
    {
        constexpr auto npos = std::wstring::npos;
        CHECK(T::kAllScreenPartHeightPx > 0 && T::kAllScreenPartHeightPx <= 32000, "teams: a part is never taller than Telegram's");

        // The literal is cut after the en dash: "\x2013" "7" keeps the 7 out of the escape.
        CHECK(T::AllScreenCaptionHtml(L"a<b", false, L"full", false, 0, 1, 1, 7) == L"\U0001F4DC <b>a&lt;b</b> \x00B7 conversation (full) \x00B7 turns 1\x2013" L"7",
              "teams: one part: the title escaped, no part number, the turns");
        const std::wstring part = T::AllScreenCaptionHtml(L"t", true, L"max", true, 1, 4, 31, 31);
        CHECK(part.find(L"last turn (max + diffs)") != npos && part.find(L"\x00B7 2/4") != npos && part.find(L"\x00B7 turn 31") != npos,
              "teams: Last Turn, diffs, the part's number, one turn");
        CHECK(T::AllScreenCaptionHtml(L"t", false, L"default", false, 0, 2, 0, 0).find(L"turn") == npos, "teams: no turn named when the sheet has none");
        CHECK(T::AllScreenCaptionHtml(L"t", false, L"default", false, 0, 2, -3, 5).find(L"turns 1\x2013" L"5") != npos, "teams: the rows above the first turn read as turn 1");

        const std::wstring cut = T::AllScreenCutNoticeHtml(false, 10, 1200, 14, TurnsLevel::Full, true);
        CHECK(cut.find(L"The conversation is longer than 10 pictures hold: its first 1200 rows (through turn 14) are left out.") == 0, "teams: the cut's size and its turn");
        CHECK(cut.find(L"<code>/allscreen default</code>") != npos && cut.find(L"<code>/allscreen compact</code>") != npos, "teams: Full suggests the shorter levels, typed");
        CHECK(cut.find(L"Without diffs") != npos, "teams: diffs take room too");
        const std::wstring lastCut = T::AllScreenCutNoticeHtml(true, 3, 50, 9, TurnsLevel::Default, false);
        CHECK(lastCut.find(L"The last turn is longer") == 0 && lastCut.find(L"through turn") == npos && lastCut.find(L"<code>/allscreen compact last</code>") != npos &&
                  lastCut.find(L"diffs") == npos,
              "teams: Last Turn names no turn, keeps \"last\" in the typed form, and says nothing of diffs");
        CHECK(T::AllScreenCutNoticeHtml(false, 10, 5, 1, TurnsLevel::Compact, false).find(L"/allscreen") == npos, "teams: Compact and below suggest no level");

        CHECK(T::AllScreenUsageHtml(TurnsLevel::Full).find(L"Bare, it draws full") != npos, "teams: the usage names the default level");
        CHECK(T::TeamsCommandsHelpHtml().find(L"/allscreen") != npos, "teams: the help lists /allscreen");
    }

    /// <summary>Pins section 23: what a fork question's answers launch, its payload, and its words in each situation.</summary>
    void TestTeamsFork()
    {
        constexpr auto npos = std::wstring::npos;
        using T::ForkSituation;
        // The answers per situation: Telegram's tokens (Messaging.Inbound.cpp), 1 = "Fork interrupted", 2 = "Fork uninterrupted".
        CHECK(T::ForkChoiceFor(ForkSituation::Continues, false) == ForkTurnChoice::Interrupted && T::ForkChoiceFor(ForkSituation::Continues, true) == ForkTurnChoice::Uninterrupted,
              "teams: A: the stop watch, or the turn goes on");
        CHECK(T::ForkChoiceFor(ForkSituation::ApiError, false) == ForkTurnChoice::TakeBackAndInterrupt &&
                  T::ForkChoiceFor(ForkSituation::ApiError, true) == ForkTurnChoice::TakeBackAndContinue,
              "teams: B: both take the error back; interrupted adds the stop watch");
        CHECK(T::ForkChoiceFor(ForkSituation::UserInterrupt, false) == ForkTurnChoice::Uninterrupted &&
                  T::ForkChoiceFor(ForkSituation::UserInterrupt, true) == ForkTurnChoice::TakeBackAndContinue,
              "teams: C: a plain fork that waits, or the interrupt taken back");
        for (const ForkSituation s : { ForkSituation::Continues, ForkSituation::ApiError, ForkSituation::UserInterrupt })
        {
            CHECK(T::ForkSituationFromWord(T::ForkSituationWord(s)) == s, "teams: every fork situation round-trips by word");
        }
        CHECK(!T::ForkSituationFromWord(L"nonsense"), "teams: an unknown situation reads nothing");

        T::ForkQuestionData d;
        d.situation = ForkSituation::ApiError;
        d.dir = L"C:\\src\\app";
        d.title = std::wstring(L"a") + L'\x1F' + L"b";
        d.model = L"opus";
        const auto back = T::ParseForkQuestionPayload(T::ForkQuestionPayload(d));
        CHECK(back && back->situation == ForkSituation::ApiError && back->dir == d.dir && back->title == L"a b" && back->model == L"opus",
              "teams: the fork payload round-trips (a separator in the title replaced)");
        CHECK(!T::ParseForkQuestionPayload(L"nonsense\x1F" L"C:\\x"), "teams: a payload of an unknown situation forks nothing");
        CHECK(!T::ParseForkQuestionPayload(L"continues\x1F"), "teams: a payload without a folder forks nothing");
        const auto shortForm = T::ParseForkQuestionPayload(L"continues\x1F" L"C:\\x");
        CHECK(shortForm && shortForm->title.empty() && shortForm->model.empty(), "teams: a payload's title and model may be missing");

        T::ForkQuestionFacts a;
        a.sourceRunning = true;
        a.droppedTool = L"Bash";
        const std::wstring qa = T::ForkQuestionHtml(L"x<y", a);
        CHECK(qa.find(L"<b>x&lt;y</b> is in the middle of a turn.") != npos && qa.find(L"<code>Bash</code> call it was running") != npos,
              "teams: A: the running turn and the call that runs again");
        CHECK(qa.find(T::KeycapEmoji(1) + L" <b>Fork interrupted</b>: Agentmaster locks") != npos && qa.find(T::KeycapEmoji(2) + L" <b>Fork uninterrupted</b>") != npos,
              "teams: A: the two numbered answers");
        CHECK(qa.find(L"within an hour") != npos && qa.find(std::wstring{ T::kEmojiNo } + L" for no fork") != npos, "teams: how to answer");
        T::ForkQuestionFacts b;
        b.situation = ForkSituation::ApiError;
        b.errorText = L"Overloaded";
        const std::wstring qb = T::ForkQuestionHtml(L"t", b);
        CHECK(qb.find(L"stopped on an API error: <i>Overloaded</i>") != npos && qb.find(L"carries on with the turn by itself") != npos, "teams: B: the error and the carry-on");
        b.switchOffInSettings = true;
        const std::wstring qbOff = T::ForkQuestionHtml(L"t", b);
        CHECK(qbOff.find(L"is off in your settings") != npos && qbOff.find(L"the fork waits for you.") != npos, "teams: B with the switch off: nothing carries on");
        T::ForkQuestionFacts c;
        c.situation = ForkSituation::UserInterrupt;
        c.duringToolUse = true;
        c.sendsPromptAgain = true;
        c.promptText = L"fix it";
        const std::wstring qc = T::ForkQuestionHtml(L"", c);
        CHECK(qc.find(L"<b>the session</b> was interrupted by you during a tool call.") != npos && qc.find(L"sends your prompt \x201C" L"fix it\x201D again.") != npos,
              "teams: C: the interrupt and the prompt sent again");
        CHECK(T::TeamsCommandsHelpHtml().find(L"/forksession") != npos && T::TeamsCommandsHelpHtml().find(L"/newsession") != npos &&
                  T::TeamsCommandsHelpHtml().find(L"a NEW post opens a session") != npos,
              "teams: the help lists /forksession, /newsession and the new post");
    }

    /// <summary>Pins section 24: the Search thread's replies (requests, picks, /clear, /help) and its answer.</summary>
    void TestTeamsSearch()
    {
        constexpr auto npos = std::wstring::npos;
        using T::SearchReplyKind;
        CHECK(T::ClassifySearchReply(L"/clear").kind == SearchReplyKind::Clear && T::ClassifySearchReply(L"/reset now").kind == SearchReplyKind::Clear,
              "teams: /clear and /reset start over");
        CHECK(T::ClassifySearchReply(L"/clearly the crash").kind == SearchReplyKind::Request, "teams: a word that only starts like /clear is a request");
        CHECK(T::ClassifySearchReply(L"/help").kind == SearchReplyKind::Help && T::ClassifySearchReply(L"/start").kind == SearchReplyKind::Help, "teams: /help and /start show the help");
        const T::SearchReply three = T::ClassifySearchReply(L" 3 ");
        CHECK(three.kind == SearchReplyKind::Pick && three.pick == 3, "teams: a lone number picks");
        CHECK(T::ClassifySearchReply(L"#12").pick == 12, "teams: a number after # picks");
        CHECK(T::ClassifySearchReply(T::KeycapEmoji(2)).pick == 2, "teams: a keycap typed picks");
        CHECK(T::ClassifySearchReply(L"0").kind == SearchReplyKind::Request && T::ClassifySearchReply(L"1234").kind == SearchReplyKind::Request,
              "teams: 0 and a long number are words");
        const T::SearchReply words = T::ClassifySearchReply(L"  the drag crash fix  ");
        CHECK(words.kind == SearchReplyKind::Request && words.text == L"the drag crash fix", "teams: words are a request, trimmed");

        const int64_t now = 1'795'000'000'000LL;
        T::SearchListing a;
        a.sessionId = L"aaaaaaaa-1111";
        a.title = L"drag <crash>";
        a.dir = L"C:\\src\\app";
        a.lastActivityMs = now - 2LL * 3'600'000;
        a.prompts = 1;
        a.why = L"it fixed the drag crash";
        a.open = true;
        T::SearchListing b;
        b.sessionId = L"bbbbbbbb-2222";
        b.prompts = 4;
        const std::wstring answer = T::SearchAnswerHtml(L"Two fit <well>", { a, b }, now);
        CHECK(answer.find(L"Two fit &lt;well&gt;") != npos, "teams: the model's sentence, escaped");
        CHECK(answer.find(L"<b>1.</b> \x25B6 <b>drag &lt;crash&gt;</b> \x00B7 <code>C:\\src\\app</code> \x00B7 2h ago \x00B7 1 prompt") != npos,
              "teams: an open session's line: the play mark, the folder, the age, one prompt");
        CHECK(answer.find(L"<i>it fixed the drag crash</i>") != npos, "teams: the model's why under it");
        CHECK(answer.find(L"<b>2.</b> <b>bbbbbbbb</b> \x00B7 4 prompts") != npos, "teams: an untitled closed session: its id's start, the plural");
        CHECK(answer.find(L"Reply with a number") != npos, "teams: how to pick");
        CHECK(T::SearchAnswerHtml(L"Nothing fits.", {}, now).find(L"Reply with a number") == npos, "teams: no list, no pick line");
        std::vector<T::SearchListing> many(T::kSearchListMax + 3, b);
        CHECK(T::SearchAnswerHtml(L"x", many, now).find(L"and 3 more") != npos, "teams: past the cap, a count");
        CHECK(T::SearchRootHtml().find(L"/clear") != npos && !T::kSearchSubject.empty(), "teams: the Search root's text and subject");
    }
}

void TestTeams()
{
    TestTeamsText();
    TestTeamsEmoji();
    TestTeamsActions();
    TestTeamsCard();
    TestTeamsPostJson();
    TestTeamsHtml();
    TestTeamsPoll();
    TestTeamsReactions();
    TestTeamsTokens();
    TestTeamsInbound();
    TestTeamsState();
    TestTeamsGraphAddress();
    TestTeamsSignInAnswers();
    TestTeamsCredentialParts();
    TestTeamsStatusAndPacing();
    TestTeamsSettings();
    TestTeamsMirror();
    TestTeamsInboundPoll();
    TestTeamsEmojiActions();
    TestTeamsPictures();
    TestTeamsWizard();
    TestTeamsAllScreen();
    TestTeamsFork();
    TestTeamsSearch();
}
