// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.33: /steer, a message sent NOW from a session's Telegram topic, into a running turn too (the
// user's request of 2026-10-19: "Add support for /steer which is like send-now"). This file pins the pure half (MessagingSteer.h)
// and the command's reading:
//   1. the command: read like every command (any case, a "@bot" suffix, the words on every line), Agentmaster's in a session's
//      topic, refused in the 🔎 Search topic, forced through to Claude Code only by /cc, a longer word not taken for it;
//   2. the decision (DecideSteer): every fact and state, and the order the facts are read in;
//   3. the texts: the replies of the cases that queue, the usage line, the declined notice by its reason, the reactions;
//   4. the prompt: its fields when sent and when queued, and its place in the queue;
//   5. the watch's verdict (ClassifySteerWatch) on every status of the prompt, and its limit.
// The service's half (the send seam, the reactions, the tick's sweep) needs Telegram and a window: the live check is the dev bot
// (MESSAGING.md §21.33, "Check after the build").

#include "m5_tests.h"

#include <iterator>
#include <string>
#include <vector>

#include "../MessagingModels.h" // ParseRemoteCommand, kAgentmasterCommands, kSearchTopicFleetCommands, ClassifySearchTopicMessage
#include "../MessagingSteer.h"

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    /// <summary>A queue entry in the given status: what the steer's place and the watch's verdict read of a prompt.</summary>
    /// <param name="id">Its id (also its text).</param>
    /// <param name="status">Its status.</param>
    /// <param name="echoed">Its echo arrived (claude took it).</param>
    /// <returns>The entry.</returns>
    QueuedPrompt SteerQueued(const wchar_t* id, PromptStatus status, bool echoed = false)
    {
        QueuedPrompt q;
        q.id = id;
        q.text = id;
        q.status = status;
        q.echoed = echoed;
        return q;
    }
}

void TestMessagingSteer()
{
    wprintf(L"== TestMessagingSteer (MESSAGING.md 21.33: /steer sends the words NOW - the command, the decision, the texts, the prompt, the watch) ==\n");

    // ---- 1. the command ----
    {
        auto c = ParseRemoteCommand(L"/steer also run the tests");
        CHECK(c.kind == RemoteKind::AgentmasterCommand && c.name == kSteerCommand && c.args == L"also run the tests",
              "/steer <words> is Agentmaster's command, its words the arguments");
        c = ParseRemoteCommand(L"  /Steer@ClaudeSessionsGateBot  first line\nsecond line  ");
        CHECK(c.kind == RemoteKind::AgentmasterCommand && c.name == kSteerCommand && c.args == L"first line\nsecond line",
              "any case, a @bot suffix dropped, the words on every line, trimmed");
        c = ParseRemoteCommand(L"/steer");
        CHECK(c.kind == RemoteKind::AgentmasterCommand && c.name == kSteerCommand && c.args.empty(), "a bare /steer is the command with no words");
        c = ParseRemoteCommand(L"/steering the wheel");
        CHECK(c.kind == RemoteKind::ClaudeCommand && c.name == L"steering", "a longer word is not /steer (the word is read up to whitespace)");
        c = ParseRemoteCommand(L"/cc /steer x");
        CHECK(c.kind == RemoteKind::ClaudeCommand && c.name == L"steer" && c.args == L"x", "/cc forces /steer through to Claude Code (the user's choice)");
        // Qualified: Agentmaster has a detail namespace too.
        CHECK(::Agentmaster::Msg::detail::InList(kSteerCommand, std::begin(kAgentmasterCommands), std::end(kAgentmasterCommands)) &&
                  !::Agentmaster::Msg::detail::InList(kSteerCommand, std::begin(kSearchTopicFleetCommands), std::end(kSearchTopicFleetCommands)),
              "steer is a session's command: in kAgentmasterCommands, not one the Search topic runs");
        CHECK(ClassifySearchTopicMessage(L"/steer find it").verb == SearchTopicVerb::SessionCommand, "the Search topic refuses /steer as a session's command");
    }

    // ---- 2. the decision ----
    {
        CHECK(DecideSteer(L"", true, true, false, SessionState::Running, InputBoxState::Empty) == SteerAction::Usage &&
                  DecideSteer(L" \t\r\n ", true, true, false, SessionState::Running, InputBoxState::Empty) == SteerAction::Usage,
              "no words (or blanks only): the usage line");
        CHECK(DecideSteer(L"go", false, true, false, SessionState::Running, InputBoxState::Empty) == SteerAction::Closed, "a closed session: nothing to type into");
        CHECK(DecideSteer(L"go", true, false, false, SessionState::Idle, InputBoxState::Unknown) == SteerAction::QueueDormant,
              "a tab whose claude is not started: the tab starts, the words wait first");
        CHECK(DecideSteer(L"go", true, false, true, SessionState::Running, InputBoxState::Empty) == SteerAction::Send,
              "an adopted session is never dormant (its tab is the user's running shell): sent");
        CHECK(DecideSteer(L"go", true, true, false, SessionState::Done, InputBoxState::Empty) == SteerAction::QueueEnded &&
                  DecideSteer(L"go", true, true, true, SessionState::Done, InputBoxState::Empty) == SteerAction::QueueEnded,
              "claude ended in the tab (Done): never typed (a shell may be there), the words wait first");
        CHECK(DecideSteer(L"go", true, true, false, SessionState::NeedsApproval, InputBoxState::MenuOpen) == SteerAction::QueueMenu &&
                  DecideSteer(L"go", true, true, false, SessionState::Running, InputBoxState::MenuOpen) == SteerAction::QueueMenu,
              "a menu read on the terminal: Enter would pick an option, so the words wait first, whatever the state");
        bool sent = true;
        for (const SessionState st : { SessionState::Running, SessionState::WaitingForInput, SessionState::Idle, SessionState::Error, SessionState::NeedsApproval })
        {
            for (const InputBoxState box : { InputBoxState::Unknown, InputBoxState::Empty, InputBoxState::Draft, InputBoxState::NoBox })
            {
                sent = sent && DecideSteer(L"go", true, true, false, st, box) == SteerAction::Send;
            }
        }
        CHECK(sent, "Running (the steer itself), Waiting, Idle, Error and a stale Needs approval with a plain box: sent at once");
        CHECK(DecideSteer(L"go", false, false, false, SessionState::Done, InputBoxState::MenuOpen) == SteerAction::Closed &&
                  DecideSteer(L"go", true, false, false, SessionState::Done, InputBoxState::MenuOpen) == SteerAction::QueueDormant &&
                  DecideSteer(L"go", true, true, false, SessionState::Done, InputBoxState::MenuOpen) == SteerAction::QueueEnded,
              "the order of the facts: closed, then not started, then ended, then a menu");
    }

    // ---- 3. the texts ----
    {
        CHECK(SteerReplyHtml(SteerAction::Send).empty(), "a steer sent at once gets no reply (its status is a reaction)");
        const std::wstring usage = SteerReplyHtml(SteerAction::Usage);
        CHECK(usage.find(L"/steer &lt;message&gt;") != std::wstring::npos && usage.find(L"queue") != std::wstring::npos,
              "the usage line names the command with its placeholder escaped, and says what a plain message does");
        CHECK(SteerReplyHtml(SteerAction::Closed).find(L"closed") != std::wstring::npos, "closed: says so");
        bool queuedFirst = true;
        for (const SteerAction a : { SteerAction::QueueDormant, SteerAction::QueueEnded, SteerAction::QueueMenu })
        {
            const std::wstring t = SteerReplyHtml(a);
            queuedFirst = queuedFirst && !t.empty() && t.find(L"first") != std::wstring::npos && t.find(L'<') == std::wstring::npos;
        }
        CHECK(queuedFirst, "every case that queues says the message goes first, in plain text (no tag Telegram could refuse)");
        CHECK(SteerReplyHtml(SteerAction::QueueEnded).find(L"/reactivate") != std::wstring::npos, "ended: names /reactivate (a command link)");
        CHECK(SteerDeclineFor(InputBoxState::MenuOpen, false, false) == SteerDecline::Dormant &&
                  SteerDeclineFor(InputBoxState::MenuOpen, true, false) == SteerDecline::Menu && SteerDeclineFor(InputBoxState::NoBox, true, false) == SteerDecline::NoBox &&
                  SteerDeclineFor(InputBoxState::Draft, true, false) == SteerDecline::Other && SteerDeclineFor(InputBoxState::Unknown, false, true) == SteerDecline::Other,
              "a decline's reason from the facts it left: not started first, then a menu, then no box, else other");
        bool notices = true;
        std::vector<std::wstring> seen;
        for (const SteerDecline d : { SteerDecline::BoxHeld, SteerDecline::Dormant, SteerDecline::Menu, SteerDecline::NoBox, SteerDecline::Other })
        {
            const std::wstring t = SteerDeclinedNoticeHtml(d);
            notices = notices && t.rfind(L"\x21A9 Not sent now: ", 0) == 0 && t.find(L"first in the queue") != std::wstring::npos && t.find(L'\n') == std::wstring::npos;
            for (const auto& other : seen)
            {
                notices = notices && other != t;
            }
            seen.push_back(t);
        }
        CHECK(notices, "the notice: one line, the arrow first, the words wait first, and each reason reads differently");
        CHECK(kSteerSendingReaction == L"\x26A1" && kSteerQueuedReaction == L"\x270D", "the reactions: the lightning without a variation selector, the writing hand");
    }

    // ---- 4. the prompt and its place ----
    {
        const int64_t now = 1'800'000'000'000;
        const QueuedPrompt sent = BuildSteerPrompt(L"p1", L"look at the failing test\nand fix it", 4711, true, now);
        CHECK(sent.id == L"p1" && sent.text == L"look at the failing test\nand fix it" && sent.label == TgFirstLine(L"look at the failing test\nand fix it", 56),
              "the prompt keeps its id, the words and a first-line label");
        CHECK(sent.status == PromptStatus::Sent && sent.attempts == 1 && sent.sentAtUnixMs == now && !sent.echoed && sent.enterRetries == 0 && sent.injectedAtUnixMs == 0,
              "sent now: marked Sent the way Send now marks it (one attempt, the send time, no echo, no Enter retry, no injection yet)");
        CHECK(sent.origin == PromptOrigin::Autorun && sent.guardPattern == kAnswersQuestionOk && sent.remoteSource == L"telegram:4711" &&
                  sent.gate == PromptGate::OnTurnComplete,
              "a remote prompt like every message of the topic: Autorun, the question guard's pass, its message as the source");
        const QueuedPrompt queued = BuildSteerPrompt(L"p2", L"later", 5, false, now);
        CHECK(queued.status == PromptStatus::Pending && queued.sentAtUnixMs == 0 && queued.attempts == 0 && queued.remoteSource == L"telegram:5",
              "queued: Pending, never sent");
        std::vector<QueuedPrompt> queue;
        CHECK(SteerInsertIndex(queue) == 0, "an empty queue: at the start");
        queue = { SteerQueued(L"a", PromptStatus::Sent), SteerQueued(L"b", PromptStatus::Failed), SteerQueued(L"c", PromptStatus::Sent) };
        CHECK(SteerInsertIndex(queue) == 3, "nothing waits: after the history, at the end");
        queue = { SteerQueued(L"a", PromptStatus::Sent), SteerQueued(L"b", PromptStatus::Pending), SteerQueued(L"c", PromptStatus::Pending) };
        CHECK(SteerInsertIndex(queue) == 1, "before the first prompt that waits (the topic's own and the runner's alike)");
        queue = { SteerQueued(L"a", PromptStatus::Held), SteerQueued(L"b", PromptStatus::Pending) };
        CHECK(SteerInsertIndex(queue) == 0, "a legacy Held prompt waits too");
    }

    // ---- 5. the watch ----
    {
        const int64_t t0 = 1'800'000'000'000;
        CHECK(ClassifySteerWatch(nullptr, t0, t0) == SteerOutcome::Gone, "not in the queue any more: gone, nothing to tell");
        const QueuedPrompt inFlight = SteerQueued(L"s", PromptStatus::Sent);
        CHECK(ClassifySteerWatch(&inFlight, t0, t0 + 1000) == SteerOutcome::Watching, "sent, not taken yet: watched on");
        CHECK(ClassifySteerWatch(&inFlight, t0, t0 + kSteerWatchMaxMs - 1) == SteerOutcome::Watching &&
                  ClassifySteerWatch(&inFlight, t0, t0 + kSteerWatchMaxMs) == SteerOutcome::Expired,
              "at the watch's limit: expired (the delivery's own safety nets own it)");
        const QueuedPrompt taken = SteerQueued(L"s", PromptStatus::Sent, true);
        CHECK(ClassifySteerWatch(&taken, t0, t0 + kSteerWatchMaxMs + 5) == SteerOutcome::Taken, "echoed: claude took it, late or not");
        const QueuedPrompt back = SteerQueued(L"s", PromptStatus::Pending);
        const QueuedPrompt held = SteerQueued(L"s", PromptStatus::Held);
        CHECK(ClassifySteerWatch(&back, t0, t0) == SteerOutcome::BackInQueue && ClassifySteerWatch(&held, t0, t0) == SteerOutcome::BackInQueue,
              "waiting again: the window put it back, tell the user once");
        const QueuedPrompt failed = SteerQueued(L"s", PromptStatus::Failed);
        const QueuedPrompt skipped = SteerQueued(L"s", PromptStatus::Skipped);
        CHECK(ClassifySteerWatch(&failed, t0, t0) == SteerOutcome::Resolved && ClassifySteerWatch(&skipped, t0, t0) == SteerOutcome::Resolved,
              "failed (its thumbs-down line speaks) or otherwise resolved: nothing more to tell");
    }
}
