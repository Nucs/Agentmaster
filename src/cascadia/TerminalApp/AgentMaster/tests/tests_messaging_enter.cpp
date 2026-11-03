// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.47: /enter, ONE Enter pressed in a session's terminal from its Telegram topic, so the prompt
// that sits unsent in its input box is submitted (the user's request of 2026-11-03: "slash enter, which will send an enter to the
// terminal ... it should submit the prompt so Claude Code will start running with it"). This file pins the pure half
// (MessagingEnter.h) and the command's reading:
//   1. the command: read like every command (any case, a "@bot" suffix, its words kept for the usage check), Agentmaster's in a
//      session's topic, refused in the 🔎 Search topic, forced through to Claude Code only by /cc, a longer word not taken for it;
//      the window action word and the gate's tag;
//   2. the answer's words and the reply's wire format: every result round-trips with and without a detail, a multi-line detail
//      survives, and anything unknown reads as failed with no detail;
//   3. the decisions: the record's checks (DecideRemoteEnterTarget) on every kind of session, and the window's decision
//      (DecideRemoteEnter) on every box read, the held box, and the order of the checks;
//   4. the option a menu highlights (FocusedMenuOption): the probe's /add-dir dialog, another focus, a preview layout, a sent
//      prompt's echo above a menu, no menu, a long label, CRLF;
//   5. the read-back (RemoteEnterVerify): submitted only after two gone reads in a row, a prompt that comes back, kept (the same
//      prompt, a changed one), the deadline after one gone read, an unreadable screen;
//   6. the texts: every result answered, the quote escaped and cut to its first line, the running-turn wording, the backlog, usage,
//      closed and unanswered lines.
// The service's half (the round trip, the replies) and the window's (the press, the gate, the reads) need Telegram and a window: the
// live check is the dev bot (MESSAGING.md §21.47, "Check after the build").

#include "m5_tests.h"

#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "../MessagingEnter.h"
#include "../MessagingModels.h" // ParseRemoteCommand, kAgentmasterCommands, kSearchTopicFleetCommands, ClassifySearchTopicMessage
#include "../MessagingSessionControl.h" // kPressEscAction / kDeactivateAction: the action words /enter's must differ from

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    /// <summary>A live Claude session in a given state, its claude started (a parked tab: started = false).</summary>
    /// <param name="state">The session's state.</param>
    /// <param name="started">Its claude was started this run.</param>
    /// <returns>The record.</returns>
    SessionInfo EnterSession(SessionState state, bool started = true)
    {
        SessionInfo s = MakeSession(L"0123abcd-4567-89ef-0123-456789abcdef", state);
        s.live = true;
        s.started = started;
        s.title = L"Agentmaster";
        return s;
    }

    /// <summary>
    /// A screen like Claude Code draws a numbered dialog (the probe's /add-dir capture, shortened): a sent prompt's echo in the
    /// scrollback, the dialog's title and options, the caret on option `focus`.
    /// </summary>
    /// <param name="focus">The option the selection cursor rests on (1 to 3).</param>
    /// <returns>The screen text, LF lines.</returns>
    std::wstring DialogScreen(int focus)
    {
        const auto row = [focus](int n, const wchar_t* label) {
            return std::wstring{ focus == n ? L"   \x276F " : L"     " } + std::to_wstring(n) + L". " + label + L"\n";
        };
        return std::wstring{ L"\x276F 2. fix the tests first\n" } + // a SENT prompt's echo: same caret, same numbered shape
               L"  \x23BF  Added Q:\\tmp\\one as a working directory\n\n" + L"   Add directory to workspace\n\n" + L"   Q:\\tmp\\two\n\n" +
               row(1, L"Yes, for this session") + row(2, L"Yes, and remember this directory") + row(3, L"No") + L"\n" +
               L"   Enter to confirm \x00B7 Esc to cancel";
    }

    /// <summary>Whether `text` contains `part`.</summary>
    bool Has(const std::wstring& text, const std::wstring& part)
    {
        return text.find(part) != std::wstring::npos;
    }
}

void TestMessagingEnter()
{
    wprintf(L"== TestMessagingEnter (MESSAGING.md 21.47: /enter presses ONE Enter on the prompt in the box - the command, the words, the decisions, the menu's option, the read-back, the texts) ==\n");

    // ---- 1. the command, the action word, the gate's tag ----
    {
        auto c = ParseRemoteCommand(L"/enter");
        CHECK(c.kind == RemoteKind::AgentmasterCommand && c.name == kEnterCommand && c.args.empty(), "/enter is Agentmaster's command, with no words");
        c = ParseRemoteCommand(L"  /Enter@ExampleSessionsBot  ");
        CHECK(c.kind == RemoteKind::AgentmasterCommand && c.name == kEnterCommand && c.args.empty(), "any case and a @bot suffix: still /enter, still no words");
        c = ParseRemoteCommand(L"/enter fix the tests\nand push");
        CHECK(c.kind == RemoteKind::AgentmasterCommand && c.name == kEnterCommand && c.args == L"fix the tests\nand push",
              "/enter with words keeps them as its args (the service answers them with the usage line, never a press)");
        c = ParseRemoteCommand(L"/enter   \n  ");
        CHECK(c.kind == RemoteKind::AgentmasterCommand && c.args.empty(), "blanks after /enter are no words (the args are trimmed)");
        c = ParseRemoteCommand(L"/entering the room");
        CHECK(c.kind == RemoteKind::ClaudeCommand && c.name == L"entering", "a longer word is not /enter (the word is read up to whitespace)");
        c = ParseRemoteCommand(L"/cc /enter");
        CHECK(c.kind == RemoteKind::ClaudeCommand && c.name == L"enter", "/cc forces /enter through to Claude Code (the user's choice)");
        // Qualified: Agentmaster has a detail namespace too.
        CHECK(::Agentmaster::Msg::detail::InList(kEnterCommand, std::begin(kAgentmasterCommands), std::end(kAgentmasterCommands)) &&
                  !::Agentmaster::Msg::detail::InList(kEnterCommand, std::begin(kSearchTopicFleetCommands), std::end(kSearchTopicFleetCommands)),
              "enter is a session's command: in kAgentmasterCommands, not one the Search topic runs");
        CHECK(ClassifySearchTopicMessage(L"/enter").verb == SearchTopicVerb::SessionCommand, "the Search topic refuses /enter as a session's command (it has no terminal)");
        CHECK(kPressEnterAction == L"press-enter" && kPressEnterAction != kPressEscAction && kPressEnterAction != kDeactivateAction &&
                  kPressEnterAction != kMenuAnswerAction && kPressEnterAction != kMenuScreenAction,
              "the window action word, distinct from Esc's, Deactivate's and the menu card's");
        CHECK(kDeliveryGateEnterTag.size() > 1 && kDeliveryGateEnterTag.front() == L'#' && kDeliveryGateEnterTag != kDeliveryGateClearTag &&
                  kDeliveryGateEnterTag != kDeliveryGateFillTag,
              "the gate's tag is a reserved one ('#', never a prompt id) and differs from the mail move's and the fill's");
        CHECK(kRemoteEnterGoneReads == 2 && kRemoteEnterPollMs == 100 && kRemoteEnterVerifyMs == 4000 && kRemoteEnterProbeRows == 1000,
              "the read-back's pacing: two gone reads 100 ms apart, 4 s at most, the delivery's 1000-row box window");
    }

    // ---- 2. the answer's words and the reply's wire format ----
    {
        const RemoteEnterResult all[] = { RemoteEnterResult::Submitted, RemoteEnterResult::Kept, RemoteEnterResult::Pressed, RemoteEnterResult::Empty,
                                          RemoteEnterResult::MenuOpen, RemoteEnterResult::NoBox, RemoteEnterResult::Busy, RemoteEnterResult::NotRunning,
                                          RemoteEnterResult::NotClaude, RemoteEnterResult::Failed };
        bool roundTrip = true;
        bool distinct = true;
        for (const RemoteEnterResult r : all)
        {
            const RemoteEnterAnswer bare = ParseRemoteEnterReply(BuildRemoteEnterReply(RemoteEnterAnswer{ r, {} }));
            roundTrip = roundTrip && bare.result == r && bare.detail.empty();
            for (const RemoteEnterResult other : all)
            {
                distinct = distinct && (r == other || RemoteEnterResultWord(r) != RemoteEnterResultWord(other));
            }
            distinct = distinct && RemoteEnterResultWord(r).find(L'\n') == std::wstring_view::npos;
        }
        CHECK(roundTrip && distinct, "every result's word reads back as that result, every word is its own, and none holds an LF");
        const RemoteEnterAnswer withDetail = ParseRemoteEnterReply(BuildRemoteEnterReply(RemoteEnterAnswer{ RemoteEnterResult::Submitted, L"fix it\nthen push\n" }));
        CHECK(withDetail.result == RemoteEnterResult::Submitted && withDetail.detail == L"fix it\nthen push\n",
              "a multi-line detail survives the reply exactly (the FIRST LF ends the word)");
        CHECK(BuildRemoteEnterReply(RemoteEnterAnswer{ RemoteEnterResult::Empty, {} }) == L"empty" &&
                  BuildRemoteEnterReply(RemoteEnterAnswer{ RemoteEnterResult::MenuOpen, L"1. Yes" }) == L"menu-open\n1. Yes",
              "the wire format: the word alone, or the word, one LF and the detail");
        const RemoteEnterAnswer empty = ParseRemoteEnterReply(L"");
        const RemoteEnterAnswer unknown = ParseRemoteEnterReply(L"Submitted\nx");
        const RemoteEnterAnswer spaced = ParseRemoteEnterReply(L"submitted \nx");
        const RemoteEnterAnswer failed = ParseRemoteEnterReply(L"failed\nsomething");
        CHECK(empty.result == RemoteEnterResult::Failed && unknown.result == RemoteEnterResult::Failed && spaced.result == RemoteEnterResult::Failed &&
                  unknown.detail.empty() && spaced.detail.empty(),
              "an empty reply, another case or a padded word reads as failed with no detail - never as a press");
        CHECK(failed.result == RemoteEnterResult::Failed && failed.detail.empty(), "an explicit failed drops any detail (a failure quotes nothing)");
    }

    // ---- 3. the decisions ----
    {
        const SessionInfo running = EnterSession(SessionState::Running);
        const SessionInfo asking = EnterSession(SessionState::NeedsApproval);
        const SessionInfo waiting = EnterSession(SessionState::WaitingForInput);
        const SessionInfo idle = EnterSession(SessionState::Idle);
        const SessionInfo error = EnterSession(SessionState::Error);
        const SessionInfo exited = EnterSession(SessionState::Done);
        const SessionInfo parked = EnterSession(SessionState::WaitingForInput, false);
        SessionInfo codex = EnterSession(SessionState::WaitingForInput);
        codex.kind = AgentKind::Codex;
        SessionInfo parkedCodex = codex;
        parkedCodex.started = false;
        // An adopted claude: its started flag stays false (meaningless for it, SessionInfo::started) while its claude runs.
        SessionInfo adopted = EnterSession(SessionState::WaitingForInput, false);
        adopted.external = true;
        SessionInfo closed = waiting;
        closed.live = false;

        // The record's checks.
        CHECK(DecideRemoteEnterTarget(running) == RemoteEnterResult::Pressed && DecideRemoteEnterTarget(asking) == RemoteEnterResult::Pressed &&
                  DecideRemoteEnterTarget(waiting) == RemoteEnterResult::Pressed && DecideRemoteEnterTarget(idle) == RemoteEnterResult::Pressed &&
                  DecideRemoteEnterTarget(error) == RemoteEnterResult::Pressed && DecideRemoteEnterTarget(adopted) == RemoteEnterResult::Pressed,
              "target: a Claude session whose claude reads keys goes on to the box (Running, a dialog, at rest, Error, an adopted claude)");
        CHECK(DecideRemoteEnterTarget(parked) == RemoteEnterResult::NotRunning && DecideRemoteEnterTarget(exited) == RemoteEnterResult::NotRunning &&
                  DecideRemoteEnterTarget(closed) == RemoteEnterResult::NotRunning,
              "target: a parked tab, an exited claude (its tab may hold a shell) and a closed session read the key nowhere");
        CHECK(DecideRemoteEnterTarget(codex) == RemoteEnterResult::NotClaude && DecideRemoteEnterTarget(parkedCodex) == RemoteEnterResult::NotRunning,
              "target: a running Codex session is refused as Codex, a parked one as parked first");

        // The window's decision on every box read.
        const RemoteEnterAnswer draft = DecideRemoteEnter(waiting, false, InputBoxState::Draft, L"fix the drag crash", L"");
        CHECK(draft.result == RemoteEnterResult::Pressed && draft.detail == L"fix the drag crash", "a prompt in the box: pressed, carrying the prompt for the read-back");
        CHECK(DecideRemoteEnter(waiting, false, InputBoxState::Draft, L" \t\r\n", L"").result == RemoteEnterResult::Empty,
              "a Draft of blanks alone is an empty box: never pressed");
        const RemoteEnterAnswer emptyBox = DecideRemoteEnter(waiting, false, InputBoxState::Empty, L"", L"");
        CHECK(emptyBox.result == RemoteEnterResult::Empty && emptyBox.detail.empty(), "an empty box: nothing to submit, nothing pressed");
        const RemoteEnterAnswer menu = DecideRemoteEnter(asking, false, InputBoxState::MenuOpen, L"", L"1. Yes");
        CHECK(menu.result == RemoteEnterResult::MenuOpen && menu.detail == L"1. Yes", "a menu on the screen: never pressed (Enter would pick), naming the option");
        CHECK(DecideRemoteEnter(waiting, false, InputBoxState::MenuOpen, L"stale draft", L"").result == RemoteEnterResult::MenuOpen,
              "a menu read wins over any text: the box is not there");
        CHECK(DecideRemoteEnter(waiting, false, InputBoxState::NoBox, L"", L"").result == RemoteEnterResult::NoBox &&
                  DecideRemoteEnter(waiting, false, InputBoxState::Unknown, L"", L"").result == RemoteEnterResult::NoBox,
              "no box, or a screen not up yet: nothing pressed");
        CHECK(DecideRemoteEnter(running, false, InputBoxState::Draft, L"also run the tests", L"").result == RemoteEnterResult::Pressed &&
                  DecideRemoteEnter(asking, false, InputBoxState::Draft, L"go", L"").result == RemoteEnterResult::Pressed &&
                  DecideRemoteEnter(error, false, InputBoxState::Draft, L"retry", L"").result == RemoteEnterResult::Pressed &&
                  DecideRemoteEnter(adopted, false, InputBoxState::Draft, L"go", L"").result == RemoteEnterResult::Pressed,
              "a prompt is pressed on in a running turn (Claude Code holds it), a stale dialog state, Error and an adopted claude");
        CHECK(DecideRemoteEnter(waiting, true, InputBoxState::Draft, L"fix it", L"").result == RemoteEnterResult::Busy &&
                  DecideRemoteEnter(waiting, true, InputBoxState::MenuOpen, L"", L"1. Yes").result == RemoteEnterResult::Busy,
              "a held box (a delivery, a swap, a fill): busy, whatever it reads - its content is that operation's");
        CHECK(DecideRemoteEnter(parked, true, InputBoxState::Draft, L"x", L"").result == RemoteEnterResult::NotRunning &&
                  DecideRemoteEnter(codex, true, InputBoxState::Draft, L"x", L"").result == RemoteEnterResult::NotClaude &&
                  DecideRemoteEnter(closed, false, InputBoxState::Draft, L"x", L"").result == RemoteEnterResult::NotRunning,
              "the order: the record's checks first, then the box's owner, then what the box shows");
        CHECK(DecideRemoteEnter(parked, false, InputBoxState::Draft, L"x", L"").detail.empty() && DecideRemoteEnter(waiting, true, InputBoxState::Draft, L"x", L"").detail.empty(),
              "a verdict that presses nothing carries no prompt");
    }

    // ---- 4. the option a menu highlights ----
    {
        CHECK(FocusedMenuOption(DialogScreen(1)) == L"1. Yes, for this session", "the dialog's focused option, not the sent prompt's echo above it");
        CHECK(FocusedMenuOption(DialogScreen(2)) == L"2. Yes, and remember this directory" && FocusedMenuOption(DialogScreen(3)) == L"3. No",
              "the caret moved: the option it rests on now");
        std::wstring crlf = DialogScreen(1);
        for (size_t at = crlf.find(L'\n'); at != std::wstring::npos; at = crlf.find(L'\n', at + 2))
        {
            crlf.insert(at, 1, L'\r');
        }
        CHECK(FocusedMenuOption(crlf) == L"1. Yes, for this session", "CRLF rows (ReadEntireBuffer's) read the same");
        CHECK(FocusedMenuOption(L"Pick a layout\n \x276F 1. Center \x2502 +-----+\n   2. Left   \x2502 |  x  |\n") == L"1. Center",
              "a preview layout: the label ends at the preview column's bar");
        CHECK(FocusedMenuOption(L"\x276F 2. fix the tests first\n  \x23BF  ok\n") == L"2. fix the tests first",
              "an echo ALONE reads as an option - which is why the window asks only when the box read says a menu is open, and only to name it");
        CHECK(FocusedMenuOption(L"   1. Yes\n   2. No\n").empty() && FocusedMenuOption(L"").empty() && FocusedMenuOption(L"\x276F \n\x276F  \n").empty(),
              "no caret on any option, an empty screen, a bare caret: nothing to name");
        const std::wstring longLabel = FocusedMenuOption(L"\x276F 1. " + std::wstring(200, L'a') + L"\n");
        CHECK(longLabel.size() <= kRemoteEnterOptionChars && longLabel.rfind(L"1. aaa", 0) == 0 && longLabel.back() == L'\x2026',
              "a long label is cut to the limit with an ellipsis");
        CHECK(FocusedMenuOption(L"\x203A 2. Older caret\n") == L"2. Older caret", "the older caret (U+203A) counts too");
    }

    // ---- 5. the read-back ----
    {
        const std::wstring prompt = L"fix the drag crash";
        {
            RemoteEnterVerify v{ prompt };
            const auto first = v.Observe(InputBoxState::Empty, L"");
            const auto second = v.Observe(InputBoxState::Empty, L"");
            CHECK(!first && second && second->result == RemoteEnterResult::Submitted && second->detail == prompt,
                  "submitted after TWO empty reads in a row, quoting the prompt that was pressed");
        }
        {
            RemoteEnterVerify v{ prompt };
            const bool held = !v.Observe(InputBoxState::Empty, L"") && !v.Observe(InputBoxState::Draft, prompt) && !v.Observe(InputBoxState::Empty, L"");
            const auto then = v.Observe(InputBoxState::Empty, L"");
            CHECK(held && then && then->result == RemoteEnterResult::Submitted, "one empty read is a repaint frame: a prompt read after it restarts the count");
        }
        {
            RemoteEnterVerify v{ prompt };
            const auto a = v.Observe(InputBoxState::MenuOpen, L"");
            const auto b = v.Observe(InputBoxState::NoBox, L"");
            CHECK(!a && b && b->result == RemoteEnterResult::Submitted, "a dialog or no box in the prompt's place counts as gone (a submitted /add-dir, /model, /exit)");
        }
        {
            RemoteEnterVerify v{ prompt };
            bool waited = true;
            for (int i = 0; i < 39; ++i)
            {
                waited = waited && !v.Observe(InputBoxState::Draft, prompt);
            }
            const RemoteEnterAnswer kept = v.Expire();
            CHECK(waited && kept.result == RemoteEnterResult::Kept && kept.detail == prompt, "the prompt never left: kept, quoting what the box holds");
        }
        {
            RemoteEnterVerify v{ L"fix @src/fi" };
            const bool waited = !v.Observe(InputBoxState::Draft, L"fix @src/file.ts ") && !v.Observe(InputBoxState::Draft, L"fix @src/file.ts ");
            const RemoteEnterAnswer kept = v.Expire();
            CHECK(waited && kept.result == RemoteEnterResult::Kept && kept.detail == L"fix @src/file.ts ",
                  "a CHANGED prompt is not a submit (an entry picked from a list, a new line after a backslash): kept, quoting the box as it is now");
        }
        {
            RemoteEnterVerify v{ prompt };
            const bool one = !v.Observe(InputBoxState::Draft, prompt) && !v.Observe(InputBoxState::Empty, L"");
            CHECK(one && v.Expire().result == RemoteEnterResult::Submitted, "one gone read right at the deadline: submitted (a late submit)");
        }
        {
            RemoteEnterVerify v{ prompt };
            const auto gone = v.Observe(InputBoxState::Unknown, L"");
            CHECK(gone && gone->result == RemoteEnterResult::Pressed && gone->detail == prompt, "an unreadable screen ends the watch: pressed, unverified");
        }
        {
            const RemoteEnterVerify v{ prompt };
            const RemoteEnterAnswer none = v.Expire();
            CHECK(none.result == RemoteEnterResult::Kept && none.detail == prompt, "no read at all before the deadline: kept, quoting the pressed prompt");
        }
    }

    // ---- 6. the texts ----
    {
        bool everyResult = true;
        for (const RemoteEnterResult r : { RemoteEnterResult::Submitted, RemoteEnterResult::Kept, RemoteEnterResult::Pressed, RemoteEnterResult::Empty,
                                           RemoteEnterResult::MenuOpen, RemoteEnterResult::NoBox, RemoteEnterResult::Busy, RemoteEnterResult::NotRunning,
                                           RemoteEnterResult::NotClaude, RemoteEnterResult::Failed })
        {
            for (const SessionState st : { SessionState::Running, SessionState::WaitingForInput })
            {
                everyResult = everyResult && !RemoteEnterOutcomeHtml(RemoteEnterAnswer{ r, L"x" }, st).empty() && !RemoteEnterOutcomeHtml(RemoteEnterAnswer{ r, {} }, st).empty();
            }
        }
        CHECK(everyResult, "every result has a line, with or without a detail, in a running turn and at rest");
        const std::wstring atRest = RemoteEnterOutcomeHtml(RemoteEnterAnswer{ RemoteEnterResult::Submitted, L"fix <b>this</b> & that\nsecond line" }, SessionState::WaitingForInput);
        CHECK(Has(atRest, L"fix &lt;b&gt;this&lt;/b&gt; &amp; that") && !Has(atRest, L"<b>") && !Has(atRest, L"second line"),
              "submitted: the prompt quoted by its first line, escaped");
        const std::wstring inTurn = RemoteEnterOutcomeHtml(RemoteEnterAnswer{ RemoteEnterResult::Submitted, L"also run the tests" }, SessionState::Running);
        CHECK(inTurn != RemoteEnterOutcomeHtml(RemoteEnterAnswer{ RemoteEnterResult::Submitted, L"also run the tests" }, SessionState::WaitingForInput) &&
                  Has(inTurn, L"running turn") && Has(inTurn, L"also run the tests"),
              "submitted into a running turn says Claude Code takes it in at its next step");
        const std::wstring longOne = RemoteEnterOutcomeHtml(RemoteEnterAnswer{ RemoteEnterResult::Submitted, std::wstring(300, L'z') }, SessionState::Idle);
        CHECK(!Has(longOne, std::wstring(kRemoteEnterExcerptChars, L'z')) && Has(longOne, std::wstring(kRemoteEnterExcerptChars - 1, L'z') + L"\x2026"),
              "a long prompt is quoted to its excerpt length, with an ellipsis");
        const std::wstring kept = RemoteEnterOutcomeHtml(RemoteEnterAnswer{ RemoteEnterResult::Kept, L"fix @src/file.ts " }, SessionState::WaitingForInput);
        CHECK(Has(kept, L"fix @src/file.ts") && Has(kept, L"/enter again") && Has(kept, L"did not submit"), "kept: the box's text quoted, why it may have stayed, and the retry");
        const std::wstring menuNamed = RemoteEnterOutcomeHtml(RemoteEnterAnswer{ RemoteEnterResult::MenuOpen, L"1. Yes" }, SessionState::NeedsApproval);
        const std::wstring menuBare = RemoteEnterOutcomeHtml(RemoteEnterAnswer{ RemoteEnterResult::MenuOpen, {} }, SessionState::NeedsApproval);
        CHECK(Has(menuNamed, L"1. Yes") && Has(menuNamed, L"nothing was pressed") && !Has(menuBare, L"()") && Has(menuBare, L"nothing was pressed"),
              "a menu: the highlighted option named when it was read, never an empty pair of brackets, and nothing pressed");
        CHECK(Has(RemoteEnterOutcomeHtml(RemoteEnterAnswer{ RemoteEnterResult::NotRunning, {} }, SessionState::Idle), L"/activate") &&
                  Has(RemoteEnterOutcomeHtml(RemoteEnterAnswer{ RemoteEnterResult::Busy, {} }, SessionState::Idle), L"/enter again") &&
                  Has(RemoteEnterOutcomeHtml(RemoteEnterAnswer{ RemoteEnterResult::NoBox, {} }, SessionState::Idle), L"/screenimage") &&
                  Has(RemoteEnterOutcomeHtml(RemoteEnterAnswer{ RemoteEnterResult::NotClaude, {} }, SessionState::Idle), L"Codex"),
              "each refusal points at its way out: /activate, a retry, the screen's picture, Codex named");
        bool pressedNothing = true;
        for (const RemoteEnterResult r : { RemoteEnterResult::Empty, RemoteEnterResult::MenuOpen, RemoteEnterResult::NoBox, RemoteEnterResult::Busy,
                                           RemoteEnterResult::NotRunning, RemoteEnterResult::NotClaude })
        {
            const std::wstring line = RemoteEnterOutcomeHtml(RemoteEnterAnswer{ r, {} }, SessionState::Idle);
            pressedNothing = pressedNothing && !Has(line, L"Submitted") && !Has(line, L"Enter pressed");
        }
        CHECK(pressedNothing, "no line of a result that wrote nothing claims a press");
        const std::wstring backlog = RemoteEnterBacklogHtml(L"2 h");
        CHECK(Has(backlog, L"2 h") && Has(backlog, L"nothing was pressed") && Has(backlog, L"/enter again"), "the backlog line: its age, nothing pressed, and how to press now");
        CHECK(Has(RemoteEnterBacklogHtml(L"<1 m>"), L"&lt;1 m&gt;"), "the backlog line escapes the age");
        const std::wstring usage = RemoteEnterUsageHtml();
        CHECK(Has(usage, L"/steer &lt;text&gt;") && Has(usage, L"nothing was pressed") && !Has(usage, L"<text>"), "the usage line: no words, /steer for words, the placeholder escaped");
        const std::wstring closedLine{ kRemoteEnterClosedText };
        const std::wstring unansweredLine{ kRemoteEnterUnansweredText };
        CHECK(!closedLine.empty() && !unansweredLine.empty() && closedLine.find_first_of(L"<>&") == std::wstring::npos &&
                  unansweredLine.find_first_of(L"<>&") == std::wstring::npos && Has(unansweredLine, L"may not have been pressed"),
              "the closed and unanswered lines hold no markup (posted as they are), and the unanswered one claims no press");
    }
}
