// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.57: queue management from a session's Telegram topic (the user's request of 2026-11-16: "queued
// messages should maybe get a Steer button and Delete button, what else is necessary for queue management lifecycle on telegram").
// This file pins the pure half (MessagingQueueTickets.h) and the engine rules it leans on:
//   1. the keycaps and the button data (under Telegram's 64 bytes, read back, foreign data refused);
//   2. a prompt's messages and merged parts (the merged text on a NEW line, never appended);
//   3. where a prompt stands (its waiting place, the prompt above it, a delivery in flight, the fresh insert point);
//   4. why a message waits (every reason, and the order they are read in);
//   5. the ticket's text and buttons in every phase (the keycap on the line and on every button, the title row under the first
//      line, an own post's words on top), and the hash that spares an unchanged ticket its edit;
//   6. the X / plus replies and a bot line's words;
//   7. an edit's reading (the §21.57.9 fix: an edit into /exit is refused, /cc /x queues /x) and its refusals;
//   8. the /queue list and its buttons;
//   9. the tickets' state-file round trip;
//  10. the LEFT-OVER rule (D30's pass gone: the runner's mode, the full settle, no question pass) and MarkRemotePromptsLeftOver;
//  11. the new prompt fields' round trip in sessions.json (omitted at their defaults).
// The service's half (the tickets' posts and edits, the taps, the delete check's Telegram call) needs Telegram: the live checks are
// MESSAGING.md §21.57's "Check after the build".

#include "m5_tests.h"

#include <string>
#include <vector>

#include "../MessagingQueueTickets.h"
#include "../Persistence.h" // ToJson(QueuedPrompt) / PromptFromJson: the new fields' round trip
#include "../Scheduler.h" // DecideAdvance, NextPromptIsRemote, StateSettleRemainingMs, kAnswersQuestionOk

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    /// <summary>A queued prompt with an id, words, a status and (optionally) the topic message it came from.</summary>
    /// <param name="id">Its id.</param>
    /// <param name="text">Its words.</param>
    /// <param name="status">Its status.</param>
    /// <param name="messageId">Its Telegram message (0 = a prompt the PC or the runner queued: no remoteSource).</param>
    /// <returns>The prompt.</returns>
    QueuedPrompt QPrompt(const wchar_t* id, const wchar_t* text, PromptStatus status, int64_t messageId)
    {
        QueuedPrompt q;
        q.id = id;
        q.text = text;
        q.status = status;
        q.origin = PromptOrigin::Autorun;
        if (messageId > 0)
        {
            q.remoteSource = L"telegram:" + std::to_wstring(messageId);
            q.guardPattern = std::wstring{ kAnswersQuestionOk };
        }
        return q;
    }

    /// <summary>A live, started session at rest (Waiting for you), its runner Auto, settled long ago.</summary>
    /// <param name="id">Its id.</param>
    /// <returns>The session.</returns>
    SessionInfo QSession(const wchar_t* id)
    {
        SessionInfo s;
        s.id = id;
        s.live = true;
        s.started = true;
        s.state = SessionState::WaitingForInput;
        s.stateChangedAtUnixMs = 1000;
        s.autorunner.mode = AutorunnerMode::Full;
        return s;
    }

    /// <summary>Does the text hold the needle?</summary>
    /// <param name="text">The text.</param>
    /// <param name="needle">What to look for.</param>
    /// <returns>True when found.</returns>
    bool Has(const std::wstring& text, std::wstring_view needle)
    {
        return text.find(needle) != std::wstring::npos;
    }
}

void TestMessagingQueue()
{
    wprintf(L"== TestMessagingQueue (MESSAGING.md 21.57: queue management from a topic - tickets, buttons, replies, edits, left-over) ==\n");

    // ---- 1. keycaps and button data ----
    {
        CHECK(QueueKeycap(1) == std::wstring{ L"1\xFE0F\x20E3" } && QueueKeycap(9) == std::wstring{ L"9\xFE0F\x20E3" },
              "1..9 are the digit, VS16 and the combining keycap");
        CHECK(QueueKeycap(10) == L"\U0001F51F", "10 is the keycap ten emoji");
        CHECK(QueueKeycap(11) == L"#11" && QueueKeycap(0) == L"#0", "past ten (and 0) a plain #n");
        const std::wstring id = L"0123abcd-0000-4000-8000-0123456789ab"; // a GUID's 36 characters
        const std::wstring longest = EncodeQueueCallback(kQueueVerbMergeYes, id, id.substr(0, 8));
        CHECK(longest.size() <= 64, "the longest button data (a merge's yes) fits Telegram's 64 bytes");
        CHECK(EncodeQueueCallback(kQueueVerbDismiss, id).size() <= 64, "the longest verb with a GUID fits too");
        const auto steer = ParseQueueCallback(EncodeQueueCallback(kQueueVerbSteer, id));
        CHECK(steer && steer->verb == kQueueVerbSteer && steer->id == id && steer->arg.empty(), "a steer button reads back");
        const auto yes = ParseQueueCallback(longest);
        CHECK(yes && yes->verb == kQueueVerbMergeYes && yes->id == id && yes->arg == id.substr(0, 8), "a merge yes reads back with its argument");
        CHECK(!ParseQueueCallback(L"q:card:verb") && !ParseQueueCallback(L"pq:") && !ParseQueueCallback(L"pq:steer:") && !ParseQueueCallback(L"pq::x") &&
                  !ParseQueueCallback(L"c:queue:x"),
              "foreign data (the question card's q:, a card's c:) and damaged queue data are refused");
    }

    // ---- 2. a prompt's messages and parts ----
    {
        QueuedPrompt q = QPrompt(L"a", L"one", PromptStatus::Pending, 100);
        CHECK(TopicMessageIdOf(q) == 100, "the message id from telegram:<id>");
        CHECK(TopicMessageIdOf(QPrompt(L"b", L"x", PromptStatus::Pending, 0)) == 0, "a PC prompt has no message");
        QueuedPrompt bad = q;
        bad.remoteSource = L"telegram:abc";
        CHECK(TopicMessageIdOf(bad) == 0, "a damaged source reads 0");
        const auto parts = PromptPartsOf(q);
        CHECK(parts.size() == 1 && parts[0].messageId == 100 && parts[0].text == L"one", "a plain prompt is one part: its message and words");
        q.remoteParts = { RemotePromptPart{ 100, L"one" }, RemotePromptPart{ 101, L"two" }, RemotePromptPart{ 100, L"dup" } };
        const auto ids = PromptMessageIds(q);
        CHECK(ids.size() == 2 && ids[0] == 100 && ids[1] == 101, "a merged prompt's messages: each once, in order");
        CHECK(PromptHasMessage(q, 101) && PromptHasMessage(q, 100) && !PromptHasMessage(q, 102) && !PromptHasMessage(q, 0), "a part's message is the prompt's");
        CHECK(MergedRemoteText({ RemotePromptPart{ 1, L"first" }, RemotePromptPart{ 2, L"second" } }) == L"first\nsecond",
              "the merged text puts the second message on a NEW line (the user's rule: no just appending)");
        CHECK(MergedRemoteText({ RemotePromptPart{ 1, L"only" } }) == L"only", "one part is its own text");
    }

    // ---- 3. where a prompt stands ----
    {
        SessionInfo s = QSession(L"place");
        s.queue.push_back(QPrompt(L"sent", L"old", PromptStatus::Sent, 1));
        s.queue[0].echoed = true;
        s.queue.push_back(QPrompt(L"p1", L"first", PromptStatus::Pending, 2));
        s.queue.push_back(QPrompt(L"runner", L"planned", PromptStatus::Pending, 0));
        s.queue.push_back(QPrompt(L"p2", L"second", PromptStatus::Pending, 3));
        CHECK(WaitingPosition(s, L"p1") == 1 && WaitingPosition(s, L"runner") == 2 && WaitingPosition(s, L"p2") == 3, "places count the waiting prompts only");
        CHECK(WaitingPosition(s, L"sent") == 0 && WaitingPosition(s, L"nope") == 0, "a sent or unknown prompt has no place");
        CHECK(WaitingPromptAbove(s, L"p1") == nullptr && WaitingPromptAbove(s, L"p2") != nullptr && WaitingPromptAbove(s, L"p2")->id == L"runner",
              "the prompt above is the previous WAITING one");
        CHECK(!DeliveryInFlightOf(s), "an echoed sent prompt is no delivery in flight");
        s.queue[0].echoed = false;
        CHECK(DeliveryInFlightOf(s), "a sent prompt not picked up yet is");
        CHECK(FreshRemoteInsertIndex(s.queue) == 2, "a fresh phone prompt goes after p1, before the runner's prompt");
        s.queue[1].remoteLeftOver = true;
        CHECK(FreshRemoteInsertIndex(s.queue) == 1, "and before a LEFT-OVER one too (it never blocks a new message)");
        std::vector<QueuedPrompt> empty;
        CHECK(FreshRemoteInsertIndex(empty) == 0, "an empty queue: at the end");
    }

    // ---- 4. why a message waits ----
    {
        TicketWhyFacts f;
        CHECK(DecideTicketWhy(f) == TicketWhy::None, "first, at rest, nothing in flight: no reason (it goes)");
        f.live = false;
        CHECK(DecideTicketWhy(f) == TicketWhy::ClosedConversation, "a closed session first");
        f = TicketWhyFacts{};
        f.state = SessionState::NeedsApproval;
        f.position = 3;
        CHECK(DecideTicketWhy(f) == TicketWhy::Question, "an open question holds every message, whatever its place");
        f = TicketWhyFacts{};
        f.state = SessionState::Error;
        CHECK(DecideTicketWhy(f) == TicketWhy::None, "a FRESH message is the Error's retry: not held by it");
        f.leftOver = true;
        CHECK(DecideTicketWhy(f) == TicketWhy::Error, "a left-over one is held by the Error");
        f = TicketWhyFacts{};
        f.state = SessionState::Done;
        CHECK(DecideTicketWhy(f) == TicketWhy::Ended, "claude ended: held");
        f = TicketWhyFacts{};
        f.started = false;
        CHECK(DecideTicketWhy(f) == TicketWhy::TabStarting, "a dormant tab a fresh message started");
        f.leftOver = true;
        CHECK(DecideTicketWhy(f) == TicketWhy::TabParked, "a left-over one in a parked tab waits for the tab");
        f.external = true;
        CHECK(DecideTicketWhy(f) != TicketWhy::TabParked && DecideTicketWhy(f) != TicketWhy::TabStarting, "an adopted session is never dormant");
        f = TicketWhyFacts{};
        f.leftOver = true;
        f.runnerAuto = false;
        f.position = 2;
        CHECK(DecideTicketWhy(f) == TicketWhy::RunnerPaused, "a left-over one under a Paused runner, even when not first");
        f.runnerAuto = true;
        f.globalPause = true;
        CHECK(DecideTicketWhy(f) == TicketWhy::RunnerPaused, "Pause all holds a left-over one too");
        f = TicketWhyFacts{};
        f.position = 2;
        f.state = SessionState::Running;
        CHECK(DecideTicketWhy(f) == TicketWhy::After, "not first: after the one above (before the turn)");
        f.position = 1;
        CHECK(DecideTicketWhy(f) == TicketWhy::RunningTurn, "first, a turn runs");
        f.state = SessionState::WaitingForInput;
        f.deliveryInFlight = true;
        CHECK(DecideTicketWhy(f) == TicketWhy::DeliveryInFlight, "first, a send in flight");
        f.deliveryInFlight = false;
        f.leftOver = true;
        CHECK(DecideTicketWhy(f) == TicketWhy::RunnerSettle, "first, a left-over one under an Auto runner: the 40 s settle");
        CHECK(Has(TicketWhyWords(TicketWhy::After, QueueKeycap(1)), QueueKeycap(1)), "after names the keycap above");
        bool allSaid = true;
        for (const TicketWhy w : { TicketWhy::ClosedConversation, TicketWhy::Question, TicketWhy::Error, TicketWhy::Ended, TicketWhy::TabStarting, TicketWhy::TabParked,
                                   TicketWhy::RunnerPaused, TicketWhy::RunningTurn, TicketWhy::DeliveryInFlight, TicketWhy::RunnerSettle, TicketWhy::None })
        {
            allSaid = allSaid && !TicketWhyWords(w, {}).empty();
        }
        CHECK(allSaid, "every reason has words");
        // The facts read from a session.
        SessionInfo s = QSession(L"facts");
        s.autorunner.mode = AutorunnerMode::Off;
        s.queue.push_back(QPrompt(L"p", L"x", PromptStatus::Pending, 5));
        s.queue[0].remoteLeftOver = true;
        const TicketWhyFacts read = TicketWhyFactsFor(s, s.queue[0], 1, false);
        CHECK(read.leftOver && !read.runnerAuto && read.live && read.started && read.position == 1, "the facts come from the session and the prompt");
    }

    // ---- 5. the ticket's render ----
    {
        TicketViewFacts f;
        f.promptId = L"pid";
        f.titleRow = L"ROW";
        f.position = 1;
        f.why = TicketWhy::RunningTurn;
        f.nowMs = 1000;
        TicketView v = BuildTicketView(f);
        CHECK(!v.remove && v.html.rfind(QueueKeycap(1), 0) == 0 && Has(v.html, L"Queued") && Has(v.html, L"running turn"),
              "a live ticket leads with its keycap, then Queued and why");
        CHECK(Has(v.html, L"\nROW"), "the title row sits on line 2 (an edit adds none, so the text carries it)");
        CHECK(v.rows.size() == 1 && v.rows[0].size() == 2, "first place: one row, Steer and Delete");
        CHECK(v.rows[0][0].first.rfind(QueueKeycap(1), 0) == 0 && v.rows[0][1].first.rfind(QueueKeycap(1), 0) == 0, "both buttons carry the keycap");
        CHECK(v.rows[0][0].second == EncodeQueueCallback(kQueueVerbSteer, L"pid") && v.rows[0][1].second == EncodeQueueCallback(kQueueVerbDelete, L"pid"),
              "their data name the prompt");
        f.position = 2;
        f.why = TicketWhy::After;
        f.canMoveUp = true;
        f.canMerge = true;
        v = BuildTicketView(f);
        CHECK(v.rows.size() == 2 && v.rows[1].size() == 2 && v.rows[1][0].second == EncodeQueueCallback(kQueueVerbMoveUp, L"pid") &&
                  v.rows[1][1].second == EncodeQueueCallback(kQueueVerbMerge, L"pid"),
              "second place: Move Up and Merge on a second row");
        CHECK(Has(v.html, QueueKeycap(1)), "after names the keycap above it");
        f.canSteer = false;
        f.canMerge = false;
        v = BuildTicketView(f);
        CHECK(v.rows[0].size() == 1 && v.rows[0][0].second == EncodeQueueCallback(kQueueVerbDelete, L"pid") && v.rows[1].size() == 1,
              "no Steer in a closed session; no Merge when the one above is not the topic's");
        f.canSteer = true;
        f.partCount = 2;
        f.editedAtMs = 900;
        v = BuildTicketView(f);
        CHECK(Has(v.html, L"2 messages") && Has(v.html, L"edited"), "a merged prompt and an edit are named on the line");
        // An own post: its header first, the row under it, its words, then the ticket's line.
        f = TicketViewFacts{};
        f.kind = TicketKind::OwnPost;
        f.baseHtml = L"HEADER\nwords";
        f.titleRow = L"ROW";
        f.promptId = L"own";
        f.why = TicketWhy::RunningTurn;
        v = BuildTicketView(f);
        CHECK(v.html.rfind(L"HEADER\nROW\nwords\n\n", 0) == 0 && Has(v.html, L"Queued"), "an own post keeps its header, row and words on top");
        // The merge's preview.
        f = TicketViewFacts{};
        f.phase = TicketPhase::MergePreview;
        f.promptId = L"low";
        f.position = 2;
        f.mergeUpperId = L"UPPER123-rest";
        f.mergedText = L"a<b>\nc";
        v = BuildTicketView(f);
        CHECK(Has(v.html, L"<blockquote>a&lt;b&gt;\nc</blockquote>"), "the preview quotes the merged text, escaped, with its new line");
        CHECK(v.rows.size() == 1 && v.rows[0].size() == 2 && v.rows[0][0].second == EncodeQueueCallback(kQueueVerbMergeYes, L"low", L"UPPER123") &&
                  v.rows[0][1].second == EncodeQueueCallback(kQueueVerbMergeNo, L"low"),
              "yes names the upper prompt's first 8 characters; cancel goes back");
        // Removed, with and without Undo.
        f = TicketViewFacts{};
        f.phase = TicketPhase::Removed;
        f.promptId = L"r";
        f.undoOpen = true;
        v = BuildTicketView(f);
        CHECK(Has(v.html, L"Removed") && v.rows.size() == 1 && v.rows[0][0].second == EncodeQueueCallback(kQueueVerbUndo, L"r"), "removed: Undo while open");
        f.undoOpen = false;
        v = BuildTicketView(f);
        CHECK(v.rows.empty() && Has(v.html, L"Removed"), "the window closed: no button");
        // Failed, merged, requeued, dismissed.
        f = TicketViewFacts{};
        f.phase = TicketPhase::Failed;
        f.promptId = L"f";
        f.runnerAuto = false;
        v = BuildTicketView(f);
        CHECK(Has(v.html, L"Not delivered") && Has(v.html, L"Paused") && !Has(v.html, L"/sendnow"), "failed: says the runner's mode, never points at /sendnow");
        CHECK(v.rows.size() == 1 && v.rows[0][0].second == EncodeQueueCallback(kQueueVerbRetry, L"f") && v.rows[0][1].second == EncodeQueueCallback(kQueueVerbDismiss, L"f"),
              "failed: Retry and Dismiss");
        f.phase = TicketPhase::Merged;
        f.mergedIntoPosition = 1;
        v = BuildTicketView(f);
        CHECK(Has(v.html, L"Merged into") && Has(v.html, QueueKeycap(1)) && v.rows.empty(), "merged: names the prompt it went into");
        f.phase = TicketPhase::Requeued;
        CHECK(Has(BuildTicketView(f).html, L"Queued again"), "retried: queued again");
        f.phase = TicketPhase::Dismissed;
        CHECK(BuildTicketView(f).rows.empty(), "dismissed: no buttons");
        // Sent and Gone: a reply goes (deleted) while it may, an old one is edited; an own post keeps its words.
        f = TicketViewFacts{};
        f.phase = TicketPhase::Sent;
        f.sentAtMs = 1000;
        f.nowMs = 2000;
        CHECK(BuildTicketView(f).remove, "a sent prompt's reply ticket is deleted");
        f.ageMs = kTicketDeleteMaxAgeMs;
        v = BuildTicketView(f);
        CHECK(!v.remove && Has(v.html, L"Sent"), "too old to delete (48 h): edited to Sent");
        f.ageMs = 0;
        f.kind = TicketKind::OwnPost;
        f.baseHtml = L"HEAD\nmy words";
        v = BuildTicketView(f);
        CHECK(!v.remove && Has(v.html, L"my words") && Has(v.html, L"Sent"), "an own post keeps its words and says sent");
        f = TicketViewFacts{};
        f.phase = TicketPhase::Gone;
        CHECK(BuildTicketView(f).remove, "gone: deleted");
        f.ageMs = kTicketDeleteMaxAgeMs + 1;
        CHECK(!BuildTicketView(f).remove && Has(BuildTicketView(f).html, L"No longer"), "gone but too old: edited");
        // The hash.
        TicketViewFacts h1;
        h1.promptId = L"h";
        h1.why = TicketWhy::RunningTurn;
        TicketViewFacts h2 = h1;
        h2.why = TicketWhy::DeliveryInFlight;
        CHECK(TicketViewHash(BuildTicketView(h1)) == TicketViewHash(BuildTicketView(h1)), "the same render, the same hash");
        CHECK(TicketViewHash(BuildTicketView(h1)) != TicketViewHash(BuildTicketView(h2)), "a different reason, a different hash");
        CHECK(TicketViewHash(TicketView{}) != 0, "a hash is never 0 (0 means nothing shown yet)");
    }

    // ---- 6. the X and plus replies; a bot line's words ----
    {
        bool removes = true;
        for (const wchar_t* t : { L"x", L"X", L" x ", L"\x274C", L"\x274C\xFE0F", L"\x2716", L"\x2716\xFE0F", L"\x274E", L"\x26D4", L"\x00D7", L"\U0001F6AB", L"\U0001F5D1", L"\U0001F5D1\xFE0F" })
        {
            removes = removes && ClassifyQueueReply(t) == QueueReplyKind::Remove;
        }
        CHECK(removes, "every X form (with spaces and variation selectors) removes");
        bool again = true;
        for (const wchar_t* t : { L"+", L" + ", L"\x2795", L"\x271A", L"\xFF0B" })
        {
            again = again && ClassifyQueueReply(t) == QueueReplyKind::Again;
        }
        CHECK(again, "every plus form queues again");
        bool none = true;
        for (const wchar_t* t : { L"", L"xx", L"x y", L"fix it", L"++", L"+1", L"ok", L"\x274C\x274C\x274C" })
        {
            none = none && ClassifyQueueReply(t) == QueueReplyKind::None;
        }
        CHECK(none, "anything longer is an ordinary message");
        const std::wstring line = L"\U0001F464 You \x00B7 14:02 \x00B7 typed at the PC\n\U0001F7E6 my title\nhello\nworld";
        CHECK(PromptWordsFromBotLine(line) == L"hello\nworld", "a bot line's words: no header, no title row");
        CHECK(PromptWordsFromBotLine(L"\U0001F464 You \x00B7 14:02\nplain words") == L"plain words", "no title row: the header goes only");
        CHECK(PromptWordsFromBotLine(L"just text") == L"just text", "a text that is no such line comes back as it is");
        CHECK(Has(AgainPostHtml(0, L"a<b"), L"a&lt;b") && Has(AgainPostHtml(0, L"x"), L"again"), "the again post escapes its words");
        CHECK(Has(RepostedPromptHtml(L"a&b"), L"a&amp;b") && Has(RepostedPromptHtml(L"x"), L"from the phone"), "the re-post escapes its words");
    }

    // ---- 7. an edit's reading ----
    {
        QueuedEditDecision d = DecideQueuedEdit(ParseRemoteCommand(L"  new words  "));
        CHECK(d.outcome == QueuedEditOutcome::Replace && d.text == L"new words" && QueuedEditRefusalHtml(d).empty(), "plain words replace, trimmed, silently");
        d = DecideQueuedEdit(ParseRemoteCommand(L"/cc /mycommand arg"));
        CHECK(d.outcome == QueuedEditOutcome::Replace && d.text == L"/mycommand arg", "/cc /x queues /x (no /cc left in it)");
        d = DecideQueuedEdit(ParseRemoteCommand(L"/compact"));
        CHECK(d.outcome == QueuedEditOutcome::Replace && d.text == L"/compact", "a Claude Code command that leaves a record replaces");
        d = DecideQueuedEdit(ParseRemoteCommand(L"/exit"));
        CHECK(d.outcome == QueuedEditOutcome::RefuseLeavesTab && Has(QueuedEditRefusalHtml(d), L"keeps its words"),
              "THE BUG: an edit into /exit is refused (it used to be typed into claude)");
        d = DecideQueuedEdit(ParseRemoteCommand(L"/config"));
        CHECK(d.outcome == QueuedEditOutcome::RefuseNoRecord, "a no-record command is refused");
        d = DecideQueuedEdit(ParseRemoteCommand(L"/steer now"));
        CHECK(d.outcome == QueuedEditOutcome::RefuseCommand && Has(QueuedEditRefusalHtml(d), L"/steer"), "an Agentmaster command is refused");
        d = DecideQueuedEdit(ParseRemoteCommand(L"/add-dir K:\\x"));
        CHECK(d.outcome == QueuedEditOutcome::RefuseAddDir, "/add-dir is refused (its dialog watch starts only with a sent message)");
        d = DecideQueuedEdit(ParseRemoteCommand(L"   "));
        CHECK(d.outcome == QueuedEditOutcome::RefuseEmpty, "an empty edit is refused");
    }

    // ---- 8. the /queue list ----
    {
        std::vector<QueueListItem> items;
        CHECK(Has(BuildQueueListHtml(items), L"empty") && BuildQueueListButtons(items, L"sid").empty(), "an empty queue: says so, no buttons");
        items.push_back(QueueListItem{ L"p1", L"first line\nsecond", true, L"waits for the running turn" });
        items.push_back(QueueListItem{ L"p2", L"planned <x>", false, L"" });
        const std::wstring html = BuildQueueListHtml(items);
        CHECK(Has(html, L"2 waiting") && Has(html, QueueKeycap(1) + L" \U0001F4F1 first line") && Has(html, L"running turn") && Has(html, L"planned &lt;x&gt;"),
              "the list: keycaps, the phone's mark and why, the runner's escaped");
        const auto rows = BuildQueueListButtons(items, L"sid");
        CHECK(rows.size() == 2 && rows[0].size() == 2 && rows[0][0].second == EncodeQueueCallback(kQueueVerbListRemove, L"p1") &&
                  rows[1][0].second == EncodeQueueCallback(kQueueVerbListNext, L"sid"),
              "a remove per prompt, then Send next now naming the session");
        std::vector<QueueListItem> many;
        for (int i = 0; i < 11; ++i)
        {
            many.push_back(QueueListItem{ L"id" + std::to_wstring(i), L"t", true, L"" });
        }
        const auto manyRows = BuildQueueListButtons(many, L"sid");
        size_t removes = 0;
        for (const auto& r : manyRows)
        {
            for (const auto& b : r)
            {
                removes += b.second.rfind(L"pq:x:", 0) == 0 ? 1 : 0;
            }
        }
        CHECK(removes == 8 && manyRows.size() == 3 && manyRows[0].size() == 4, "at most eight removes, four to a row");
    }

    // ---- 9. the tickets' state-file round trip ----
    {
        std::map<std::wstring, QueueTicket> tickets;
        CHECK(QueueTicketsToJson(tickets).type == json::Value::Type::Null, "no ticket: no key");
        QueueTicket a;
        a.promptId = L"pa";
        a.sessionId = L"s1";
        a.threadId = 77;
        a.messageId = 500;
        a.replyTo = 400;
        a.kind = TicketKind::OwnPost;
        a.baseHtml = L"base";
        a.phase = TicketPhase::Removed;
        a.hash = 1234;
        a.postedMs = 10;
        a.undoUntilMs = 20;
        a.editedAtMs = 30;
        a.mergedIntoPosition = 2;
        a.runnerAuto = false;
        a.sentAtMs = 40;
        tickets[a.promptId] = a;
        QueueTicket b;
        b.promptId = L"pb";
        b.sessionId = L"s1";
        b.messageId = -1; // in flight
        tickets[b.promptId] = b;
        std::map<std::wstring, QueueTicket> back;
        QueueTicketsFromJson(QueueTicketsToJson(tickets), back);
        const auto& ra = back[L"pa"];
        CHECK(back.size() == 2 && ra.sessionId == L"s1" && ra.threadId == 77 && ra.messageId == 500 && ra.replyTo == 400 && ra.kind == TicketKind::OwnPost &&
                  ra.baseHtml == L"base" && ra.phase == TicketPhase::Removed && ra.hash == 1234 && ra.postedMs == 10 && ra.undoUntilMs == 20 && ra.editedAtMs == 30 &&
                  ra.mergedIntoPosition == 2 && !ra.runnerAuto && ra.sentAtMs == 40,
              "every field comes back");
        CHECK(back[L"pb"].messageId == 0, "a post in flight is saved as not posted (it goes again)");
        auto arr = json::Value::MkArr();
        auto o = json::Value::MkObj();
        o.Set(L"promptId", json::Value::MkStr(L"x"));
        o.Set(L"sessionId", json::Value::MkStr(L"s"));
        o.Set(L"phase", json::Value::MkNum(99));
        arr.Push(o);
        auto missing = json::Value::MkObj();
        missing.Set(L"promptId", json::Value::MkStr(L"y"));
        arr.Push(missing);
        QueueTicketsFromJson(arr, back);
        CHECK(back.size() == 1 && back[L"x"].phase == TicketPhase::Gone, "an unknown phase reads as Gone; a record without a session is dropped");
    }

    // ---- 10. the left-over rule ----
    {
        SessionInfo s = QSession(L"leftover");
        s.autorunner.mode = AutorunnerMode::Off;
        s.queue.push_back(QPrompt(L"p", L"from the phone", PromptStatus::Pending, 9));
        s.queue[0].queuedAtUnixMs = 1000;
        const int64_t later = 1000 + kStateSettleMs + 1;
        CHECK(NextPromptIsRemote(s) && DecideAdvance(s, later, 0, false).action == AdvanceAction::Send, "a FRESH phone prompt goes with the runner Paused (D30)");
        CHECK(MarkRemotePromptsLeftOver(s.queue) == 1 && s.queue[0].remoteLeftOver, "the user's close marks it left over");
        CHECK(MarkRemotePromptsLeftOver(s.queue) == 0, "marked once");
        CHECK(!NextPromptIsRemote(s) && !IsFreshRemotePrompt(s.queue[0]), "a left-over prompt is not a fresh remote one");
        const AdvancePlan held = DecideAdvance(s, later, 0, false);
        CHECK(held.action == AdvanceAction::None && held.reason == L"autorunner off", "a Paused runner holds it, like the PC's own prompts");
        s.autorunner.mode = AutorunnerMode::Full;
        CHECK(DecideAdvance(s, later, 0, false).action == AdvanceAction::Send, "an Auto runner sends it");
        CHECK(DecideAdvance(s, later, 0, true).action == AdvanceAction::None, "Pause all holds it");
        s.stateChangedAtUnixMs = 2000;
        CHECK(StateSettleRemainingMs(s, s.queue[0], 2000 + kRemotePromptSettleMs) == kStateSettleMs - kRemotePromptSettleMs,
              "the full 40 s settle, not the phone's 5 s");
        s.stateChangedAtUnixMs = 1000;
        s.lastMessageWasQuestion = true;
        CHECK(DecideAdvance(s, later, 0, false).action == AdvanceAction::None, "it no longer answers a question by itself");
        s.queue[0].remoteLeftOver = false;
        CHECK(DecideAdvance(s, later, 0, false).action == AdvanceAction::Send, "a fresh one still does (D30)");
        // Only waiting phone prompts are marked.
        std::vector<QueuedPrompt> q{ QPrompt(L"a", L"x", PromptStatus::Sent, 1), QPrompt(L"b", L"x", PromptStatus::Pending, 0), QPrompt(L"c", L"x", PromptStatus::Held, 3),
                                     QPrompt(L"d", L"x", PromptStatus::Failed, 4) };
        CHECK(MarkRemotePromptsLeftOver(q) == 1 && !q[0].remoteLeftOver && !q[1].remoteLeftOver && q[2].remoteLeftOver && !q[3].remoteLeftOver,
              "a sent or failed prompt is history, a PC prompt is not the phone's: only waiting phone prompts are marked");
        // A fresh message overtakes a left-over one in the queue's order.
        SessionInfo order = QSession(L"order");
        order.autorunner.mode = AutorunnerMode::Off;
        order.queue.push_back(QPrompt(L"old", L"left over", PromptStatus::Pending, 1));
        order.queue[0].remoteLeftOver = true;
        order.queue[0].queuedAtUnixMs = 1000;
        QueuedPrompt fresh = QPrompt(L"new", L"fresh", PromptStatus::Pending, 2);
        fresh.queuedAtUnixMs = 1000;
        order.queue.insert(order.queue.begin() + static_cast<std::ptrdiff_t>(FreshRemoteInsertIndex(order.queue)), fresh);
        CHECK(order.queue[0].id == L"new" && NextPromptIsRemote(order) && DecideAdvance(order, later, 0, false).action == AdvanceAction::Send,
              "a new message goes in front of a left-over one and is not blocked by its Paused runner");
    }

    // ---- 11. the new prompt fields in sessions.json ----
    {
        QueuedPrompt p = QPrompt(L"rt", L"one\ntwo", PromptStatus::Pending, 11);
        const json::Value plain = ToJson(p);
        CHECK(!plain.Find(L"remoteThreadId") && !plain.Find(L"remoteParts") && !plain.Find(L"remoteLeftOver"), "omitted at their defaults (the file is byte-unchanged)");
        p.remoteThreadId = 4242;
        p.remoteParts = { RemotePromptPart{ 11, L"one" }, RemotePromptPart{ 12, L"two" } };
        p.remoteLeftOver = true;
        const QueuedPrompt back = PromptFromJson(ToJson(p));
        CHECK(back.remoteThreadId == 4242 && back.remoteLeftOver && back.remoteParts.size() == 2 && back.remoteParts[1].messageId == 12 &&
                  back.remoteParts[1].text == L"two",
              "the thread, the parts and the left-over mark come back");
        // A hand-edited file with a negative thread: the document is rebuilt without the good value (json::Value::Set appends, and
        // Find returns the FIRST member of a name), then given the bad one.
        const json::Value good = ToJson(p);
        json::Value damaged = json::Value::MkObj();
        for (const auto& [name, value] : good.members)
        {
            if (name != L"remoteThreadId")
            {
                damaged.Set(name, value);
            }
        }
        damaged.Set(L"remoteThreadId", json::Value::MkNum(-5));
        CHECK(PromptFromJson(damaged).remoteThreadId == 0, "a negative thread reads as unknown");
    }
}
