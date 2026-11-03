// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — /enter: ONE Enter pressed in a session's terminal from its Telegram topic (MESSAGING.md §21.47). The user's request
// (2026-11-03): "create a new slash command for Telegram ... slash enter, which will send an enter to the terminal. So let's say
// there's a prompt written to the screen. If I send slash enter, it should submit the prompt so Claude Code will start running
// with it."
//
// What it is for: a prompt that sits UNSENT in the session's input box at the PC - typed there and left, typed in by
// /handover-standby for the user to review, carried into a fork's box, typed back after a restart - is submitted from the phone,
// as if the user pressed Enter at the PC. Claude Code then does what its own Enter does: at rest a turn starts; while a turn runs
// it holds the prompt, and the running turn takes it in at its next step (or the next turn starts with it: the /steer mechanism,
// SPEC.md §4.6).
//
// Where the key goes, and where it does not. The window that HOSTS the session's tab presses the key, on its UI thread - the
// thread every keystroke of the tab goes through, and the one that owns the draft swap's box lock - right after it read the input
// box there (DecideRemoteEnter):
//   - a PROMPT in the box (a Draft) is the one case it presses. It takes the delivery gate first (one owner of the box at a time:
//     no runner send, swap or fill types into it while the Enter lands and is checked), writes "\r" (Claude Code's submit) through
//     the session's own injector, and then reads the box back (RemoteEnterVerify): the prompt leaving it is SUBMITTED; still there
//     after kRemoteEnterVerifyMs is KEPT (Claude Code took the key for something else: an entry of a list it shows, a new line
//     after a "\", or the clean-out of invisible characters that cancels a submit);
//   - an EMPTY box: nothing to submit, so nothing is pressed (a dim suggestion in the box is not a prompt, PENDING_INPUT.md §2d,
//     and what Enter does to one is not this command's business);
//   - a MENU or a DIALOG on the screen (a question, a permission or plan prompt, a picker): Enter would PICK the option it
//     highlights - an answer nobody chose - so nothing is pressed, and the reply names that option. The Enter rescue keeps the
//     same rule ("never press into a menu"), and a plan or a permission prompt is not answered from Telegram (SPEC.md §12.4);
//   - no input box at all (a dialog over it, claude still drawing): nothing is pressed;
//   - a box a delivery, a draft swap, a mail move or a fill holds (Busy), a parked tab or an exited claude (nothing reads the key),
//     a Codex session (Agentmaster types nothing into Codex): nothing is pressed, and the reply says why.
// An /enter written while Agentmaster was closed is never pressed later (the backlog rule, D2): an Enter belongs to the screen of
// the moment it was sent, and hours later the box may hold anything.
//
// This header is the pure half (no window, no Telegram call, no service): the command and action words, the gate's tag, the
// window's answer and its wire format, the window's decision, the read-back's verdict, the option a menu highlights, and the
// texts. The m5 suite pins every rule (tests_messaging_enter.cpp). The service (Messaging.Inbound.cpp: MessagingService::
// _enterRemote, _pressEnterRemote, _onEnterAnswered) asks and answers; the window (TerminalPage.AgentMessaging.cpp:
// _RemoteSessionAction's kPressEnterAction branch and _AwaitRemoteEnterSubmit) reads, presses and reads back.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "SessionModels.h" // SessionInfo, SessionState, AgentKind; InputBoxState (PendingInput.h, the tri-state box read)
#include "TelegramFormat.h" // TgEscapeHtml, TgFirstLine (the prompt and the option quoted in a reply)
#include "TuiMenus.h" // tuimenu_detail::SplitLines / ReadOptionLine / Trim (the option a menu on the screen highlights)

namespace Agentmaster::Msg
{
    // ===== the command, the window action and their constants ===========================================================

    /// <summary>
    /// The command word (§21.47): "/enter", typed in a session's topic. It is in kAgentmasterCommands, so the topic never queues it
    /// for Claude Code - which has no /enter of its own (no such name in the 2.1.310 binary's command table, checked 2026-11-03), so
    /// the name hides nothing of Claude Code's and "/cc /enter" still forces the word through. It takes no words: "/enter fix it"
    /// gets the usage line, never a press, because the words would otherwise be dropped while the box's own prompt goes (sending
    /// new words now is /steer's work). The 🔎 Search topic refuses it as a session's command (it has no terminal).
    /// </summary>
    inline constexpr std::wstring_view kEnterCommand = L"enter";

    /// <summary>
    /// The session action that presses ONE Enter in the session's terminal (no argument; the answer is a reply in
    /// BuildRemoteEnterReply's format). Only the window that hosts the session's tab acts; every other window ignores it. Distinct
    /// from every other action word (§21.41's "press-esc" and "deactivate", §21.34's "menu-answer", the bot's "start" / "screen"
    /// …), so nothing else can send it by accident; a window that did not know it would answer "", which reads as Failed, never as
    /// a press.
    /// </summary>
    inline constexpr std::wstring_view kPressEnterAction = L"press-enter";

    /// <summary>
    /// The delivery gate's owner tag while an /enter owns the session's input box (SessionRegistry::TryOpenDeliveryGate): from right
    /// before its Enter is written until the box was read back. While it is held, a runner send, a Send now, a /steer, a draft swap,
    /// a mail move and a fork's fill are declined or held - they would type into a box whose content is being submitted, and their
    /// own box reads would take the submit for their own work - and so are ⎋ Esc and 🌙 Deactivate (they answer Busy: "again in a
    /// few seconds"). The '#' prefix is the reserved tags' convention (SessionModels.h kDeliveryGateClearTag): a prompt id is a GUID,
    /// so this tag can never collide with a real delivery's.
    /// </summary>
    /// <remarks>
    /// The hold is short: ~0.2 s when the prompt goes (kRemoteEnterGoneReads reads kRemoteEnterPollMs apart), kRemoteEnterVerifyMs at
    /// most when it stays. A window that dies while it holds the gate leaves it to the gate's expiry (SessionModels.h
    /// kDeliveryGateTimeoutMs, 75 s), the belt every gate holder relies on. The tag lives here, not beside the other reserved tags in
    /// SessionModels.h, because only this feature uses it - and a change to SessionModels.h forces a purged rebuild of every unit.
    /// </remarks>
    inline constexpr std::wstring_view kDeliveryGateEnterTag = L"#telegram-enter";

    /// <summary>
    /// How many rows of the terminal the window reads for the box (TermControl::ReadInputBoxProbe): the delivery's verification
    /// window (TerminalPage.AgentObserver.cpp kSendVerifyProbeRows), so a TALL prompt - a long briefing /handover-standby typed in,
    /// a big paste - is found as a box instead of reading as "no box" because its caret row sits above the scan's usual 120 rows.
    /// </summary>
    inline constexpr int32_t kRemoteEnterProbeRows = 1000;

    /// <summary>How often the window reads the box back after the Enter (ms): a few frames of Claude Code's repaint, far above ConPTY's round trip.</summary>
    inline constexpr int64_t kRemoteEnterPollMs = 100;

    /// <summary>
    /// How long the window reads the box back after the Enter before it calls the prompt KEPT (ms, from the press). Claude Code takes
    /// a submit within a frame or two; the margin covers a PC under load (hundreds of tabs, a busy UI thread). Past it the answer
    /// says the prompt is still there: a submit that lands later still shows on the card, and the /enter the answer invites then
    /// finds an empty box (nothing pressed). The delivery gate is held for this long at most.
    /// </summary>
    inline constexpr int64_t kRemoteEnterVerifyMs = 4000;

    /// <summary>
    /// How many reads IN A ROW must find the prompt gone (an empty box, or a dialog or no box in its place) before the window calls
    /// it submitted. One read can catch Claude Code mid-repaint, which reads empty or box-less for a moment (the fork carry's rule:
    /// "never merely the text changed", TerminalPage.AgentObserver.cpp); two reads kRemoteEnterPollMs apart do not.
    /// </summary>
    inline constexpr int kRemoteEnterGoneReads = 2;

    /// <summary>How many characters of the prompt a reply quotes (its first line, TgFirstLine): /sendnow's "Sending:" line quotes 100 too.</summary>
    inline constexpr size_t kRemoteEnterExcerptChars = 100;

    /// <summary>How many characters of a menu's highlighted option a refusal quotes (a narrow terminal cuts a long label anyway).</summary>
    inline constexpr size_t kRemoteEnterOptionChars = 80;

    // ===== the window's answer, and its wire format ======================================================================

    /// <summary>
    /// What came of an /enter: the hosting window's answer, and the service's own early verdict on what it can refuse alone
    /// (DecideRemoteEnterTarget). Only Submitted, Kept and Pressed mean an Enter was written; every other result wrote nothing.
    /// </summary>
    enum class RemoteEnterResult
    {
        Submitted, // the Enter went and the prompt LEFT the box (kRemoteEnterGoneReads reads in a row): Claude Code took it
        Kept, // the Enter went, but the box still holds a prompt after kRemoteEnterVerifyMs: Claude Code took the key for something else
        Pressed, // the Enter went with a prompt in the box, but the screen could not be read after it (the tab closing or parking): unverified
        Empty, // the box is empty: no prompt to submit, nothing pressed
        MenuOpen, // a menu or dialog is on the screen: Enter would pick its highlighted option, so nothing was pressed
        NoBox, // no input box on the screen (a dialog over it, claude still drawing, its screen not up): nothing pressed
        Busy, // a delivery, a draft swap, a mail move or a fill owns the box right now: nothing pressed (ask again in a moment)
        NotRunning, // nothing reads the key - a parked tab, an exited claude, a closed session - and as the service's verdict it means
                    // "do not ask the window": nothing pressed
        NotClaude, // a Codex session: Agentmaster types nothing into Codex - nothing pressed
        Failed, // the write itself failed (no injector bound: the tab closes or parks) or the answer was not understood
    };

    /// <summary>The word of an /enter result, as the window replies it and the logs print it.</summary>
    /// <param name="r">The result.</param>
    /// <returns>submitted / kept / pressed / empty / menu-open / no-box / busy / not-running / not-claude / failed (an unknown value
    /// reads as failed).</returns>
    inline std::wstring_view RemoteEnterResultWord(RemoteEnterResult r) noexcept
    {
        switch (r)
        {
        case RemoteEnterResult::Submitted:
            return L"submitted";
        case RemoteEnterResult::Kept:
            return L"kept";
        case RemoteEnterResult::Pressed:
            return L"pressed";
        case RemoteEnterResult::Empty:
            return L"empty";
        case RemoteEnterResult::MenuOpen:
            return L"menu-open";
        case RemoteEnterResult::NoBox:
            return L"no-box";
        case RemoteEnterResult::Busy:
            return L"busy";
        case RemoteEnterResult::NotRunning:
            return L"not-running";
        case RemoteEnterResult::NotClaude:
            return L"not-claude";
        case RemoteEnterResult::Failed:
        default:
            return L"failed";
        }
    }

    /// <summary>
    /// One /enter answer: the result, and the text it is about - the prompt that was pressed (Submitted, Pressed), the text the box
    /// still holds (Kept), or the option a menu highlights (MenuOpen; "" when none could be read). "" for every other result.
    /// </summary>
    struct RemoteEnterAnswer
    {
        RemoteEnterResult result{ RemoteEnterResult::Failed }; // Failed until a decision says otherwise: nothing claims a press unasked
        std::wstring detail; // the prompt or the option the reply quotes (see above); never sent to Telegram unescaped
    };

    /// <summary>
    /// The window's reply for kPressEnterAction: the result's word, then - when there is a detail - one LF and the detail exactly as
    /// it is (it may hold line breaks of its own: a multi-line prompt). The word never holds an LF, so the FIRST LF always ends it.
    /// </summary>
    /// <param name="a">The answer.</param>
    /// <returns>"&lt;word&gt;" or "&lt;word&gt;\n&lt;detail&gt;".</returns>
    /// <exception cref="std::bad_alloc">If the reply cannot be allocated.</exception>
    inline std::wstring BuildRemoteEnterReply(const RemoteEnterAnswer& a)
    {
        std::wstring out{ RemoteEnterResultWord(a.result) };
        if (!a.detail.empty())
        {
            out += L'\n';
            out += a.detail;
        }
        return out;
    }

    /// <summary>
    /// Reads a window's reply to kPressEnterAction (BuildRemoteEnterReply's format). A reply whose word is not one of
    /// RemoteEnterResultWord's - an empty reply, a window that does not know the action, a garbled one - reads as Failed with no
    /// detail, never as a press: the line in the topic must not claim a key went that nobody confirmed. An explicit "failed" reads
    /// the same way.
    /// </summary>
    /// <param name="reply">The window's reply.</param>
    /// <returns>The answer.</returns>
    /// <exception cref="std::bad_alloc">If the detail cannot be allocated.</exception>
    inline RemoteEnterAnswer ParseRemoteEnterReply(std::wstring_view reply)
    {
        const size_t nl = reply.find(L'\n');
        const std::wstring_view word = reply.substr(0, nl);
        RemoteEnterAnswer a;
        for (const RemoteEnterResult r : { RemoteEnterResult::Submitted, RemoteEnterResult::Kept, RemoteEnterResult::Pressed, RemoteEnterResult::Empty,
                                           RemoteEnterResult::MenuOpen, RemoteEnterResult::NoBox, RemoteEnterResult::Busy, RemoteEnterResult::NotRunning,
                                           RemoteEnterResult::NotClaude })
        {
            if (word == RemoteEnterResultWord(r))
            {
                a.result = r;
                if (nl != std::wstring_view::npos)
                {
                    a.detail.assign(reply.substr(nl + 1));
                }
                return a;
            }
        }
        return a; // Failed, and a detail it might carry is dropped: a failure quotes nothing
    }

    // ===== the decisions =================================================================================================

    /// <summary>
    /// The checks the session's RECORD settles alone, before anything reads its screen: the service's early verdict (an /enter it
    /// can refuse needs no round trip to a window) and the window's first step. Pressed here means only "a claude that reads keys
    /// is there: go on to the box".
    /// </summary>
    /// <remarks>
    /// The rule is ⎋ Esc's (MessagingSessionControl.h DecideRemoteEsc): a closed session, a parked tab (its connection never started,
    /// so nothing reads the key) and an exited claude (Done: the tab may hold the shell that hosted it, where an Enter would RUN
    /// whatever the shell's line holds) read NotRunning; an ADOPTED claude counts as running although its started flag is false,
    /// because the injector bound at the adoption writes into that very tab. "Nothing reads the key" comes first, so a parked Codex
    /// tab is told it is parked rather than that it is Codex.
    /// </remarks>
    /// <param name="s">The session as the registry has it now (a lean record is enough).</param>
    /// <returns>NotRunning, NotClaude, or Pressed (the box decides the rest).</returns>
    inline RemoteEnterResult DecideRemoteEnterTarget(const SessionInfo& s) noexcept
    {
        if (!s.live || (!s.started && !s.external) || s.state == SessionState::Done)
        {
            return RemoteEnterResult::NotRunning;
        }
        if (s.kind != AgentKind::Claude)
        {
            return RemoteEnterResult::NotClaude; // no stdin injector is bound to Codex: lifecycle and state only
        }
        return RemoteEnterResult::Pressed;
    }

    /// <summary>
    /// The hosting window's decision before it presses (TerminalPage::_RemoteSessionAction, kPressEnterAction), made on its UI thread
    /// from what it read at that moment. Pressed means "press now, then read the box back" and carries the prompt the box holds (the
    /// read-back's reference); anything else says why not, and nothing is written.
    /// </summary>
    /// <remarks>
    /// The order: the record's checks first (DecideRemoteEnterTarget), then the box's owner - while a delivery or a draft swap holds
    /// it, its content is that operation's (a prompt half typed in, a draft stashed aside) and an Enter would submit the wrong text,
    /// so even a read that looks like a prompt is not trusted - and only then what the box shows. A Draft of blanks alone is read as
    /// Empty (the detector reports Draft only with text; this keeps a blank read from ever being pressed). An Unknown read of a
    /// session that passed the record's checks is a screen not up yet (a terminal still initializing): NoBox, nothing pressed.
    /// </remarks>
    /// <param name="s">The session as the registry has it now (a lean record is enough).</param>
    /// <param name="boxHeld">True while the session's delivery gate is held or a draft swap / mail move owns its box.</param>
    /// <param name="box">The box's tri-state read (TermControl::ReadInputBoxProbe).</param>
    /// <param name="boxText">The prompt that read found (a Draft's text; ignored otherwise).</param>
    /// <param name="focusedOption">The option a menu highlights (FocusedMenuOption; read by the caller only for a MenuOpen box, "" when none).</param>
    /// <returns>The verdict; its detail is the prompt for Pressed, the option for MenuOpen, "" otherwise.</returns>
    /// <exception cref="std::bad_alloc">If the detail cannot be allocated.</exception>
    inline RemoteEnterAnswer DecideRemoteEnter(const SessionInfo& s, bool boxHeld, InputBoxState box, std::wstring_view boxText, std::wstring_view focusedOption)
    {
        RemoteEnterAnswer a;
        a.result = DecideRemoteEnterTarget(s);
        if (a.result != RemoteEnterResult::Pressed)
        {
            return a;
        }
        if (boxHeld)
        {
            a.result = RemoteEnterResult::Busy;
            return a;
        }
        switch (box)
        {
        case InputBoxState::Draft:
            if (boxText.find_first_not_of(L" \t\r\n") != std::wstring_view::npos)
            {
                a.detail.assign(boxText); // stays Pressed: the one case that writes the key
                return a;
            }
            a.result = RemoteEnterResult::Empty;
            return a;
        case InputBoxState::Empty:
            a.result = RemoteEnterResult::Empty;
            return a;
        case InputBoxState::MenuOpen:
            a.result = RemoteEnterResult::MenuOpen;
            a.detail.assign(focusedOption);
            return a;
        case InputBoxState::NoBox:
        case InputBoxState::Unknown:
        default:
            a.result = RemoteEnterResult::NoBox;
            return a;
        }
    }

    /// <summary>
    /// The option a menu on the screen HIGHLIGHTS - what an Enter would pick - as "N. label": the bottom-most line that reads as a
    /// numbered option WITH the selection cursor (❯, or the older ›; TuiMenus.h ReadOptionLine), for the MenuOpen refusal to name.
    /// </summary>
    /// <remarks>
    /// Bottom-most, because the scrollback above a menu can hold a SENT prompt that starts with the same caret and a number ("❯ 2. fix
    /// the tests"), while an open menu is drawn below everything. A side-by-side menu (an AskUserQuestion with previews) draws the
    /// preview column after a "│" on the option's own row: the label is cut there. Advisory only - the press itself never depends
    /// on it (a MenuOpen box is never pressed).
    /// </remarks>
    /// <param name="screen">The terminal's last rows as text (TerminalPage::_ReadScreenTextForSession, TuiMenus.h kMenuScreenRows).</param>
    /// <returns>"N. label" cut to kRemoteEnterOptionChars, or "" when no line reads as a highlighted option.</returns>
    /// <exception cref="std::bad_alloc">If the lines or the result cannot be allocated.</exception>
    inline std::wstring FocusedMenuOption(std::wstring_view screen)
    {
        const auto lines = tuimenu_detail::SplitLines(screen);
        for (size_t i = lines.size(); i-- > 0;)
        {
            int digit = 0;
            std::wstring_view label;
            bool caret = false;
            if (!tuimenu_detail::ReadOptionLine(lines[i], digit, label, caret) || !caret)
            {
                continue;
            }
            if (const size_t bar = label.find(L'\x2502'); bar != std::wstring_view::npos)
            {
                label = tuimenu_detail::Trim(label.substr(0, bar));
            }
            if (label.empty())
            {
                continue; // a row that is all preview column: not a label the user would recognize
            }
            return TgFirstLine(std::to_wstring(digit) + L". " + std::wstring{ label }, kRemoteEnterOptionChars);
        }
        return {};
    }

    /// <summary>
    /// The read-back after an /enter's press (TerminalPage::_AwaitRemoteEnterSubmit): fed each read of the box, kRemoteEnterPollMs
    /// apart, it says when the prompt has gone, or why it cannot tell.
    /// </summary>
    /// <remarks>
    /// GONE is a box that holds no prompt any more: an Empty box (the submit), a dialog in its place (a submitted "/add-dir x" or
    /// "/model" opens one) or no box at all (a submitted "/exit" ends claude) - kRemoteEnterGoneReads such reads IN A ROW, since a
    /// single one can be a repaint frame. A Draft, whatever its text, is NOT gone: the same prompt (Claude Code cleaned out
    /// invisible characters, which cancels the submit), a new line after a trailing "\" (Claude Code's line continuation), an entry
    /// picked from a list it showed ("@src/fi" completed to a file, "/mo" to "/model "), or the user typing at the PC - none of
    /// them was submitted. An Unknown read means the screen went away (the tab closes or parks): the watch stops, and the answer is
    /// Pressed - the key went, unverified. A Ctrl+S stash that Claude Code puts back ~0.4 s after a real submit refills the box
    /// only after the empty reads that already decided it.
    /// </remarks>
    class RemoteEnterVerify
    {
    public:
        /// <summary>Starts a read-back for the prompt the Enter was pressed on.</summary>
        /// <param name="pressedPrompt">The box's text at the press (DecideRemoteEnter's detail): what a Submitted answer quotes.</param>
        explicit RemoteEnterVerify(std::wstring pressedPrompt) :
            _pressed{ std::move(pressedPrompt) },
            _box{ _pressed }
        {
        }

        /// <summary>Takes one read of the box.</summary>
        /// <param name="state">The read's verdict (TermControl::ReadInputBoxProbe).</param>
        /// <param name="boxText">Its text (a Draft's; ignored otherwise).</param>
        /// <returns>The answer once there is one - Submitted after kRemoteEnterGoneReads gone reads in a row, Pressed when the screen
        /// could not be read - else nullopt (read again in kRemoteEnterPollMs, or Expire at the deadline).</returns>
        /// <exception cref="std::bad_alloc">If the box's text or the answer cannot be allocated.</exception>
        std::optional<RemoteEnterAnswer> Observe(InputBoxState state, std::wstring_view boxText)
        {
            switch (state)
            {
            case InputBoxState::Draft:
                _goneReads = 0; // a prompt is (back) in the box: any gone read before it was a repaint frame
                _box.assign(boxText);
                return std::nullopt;
            case InputBoxState::Empty:
            case InputBoxState::MenuOpen:
            case InputBoxState::NoBox:
                if (++_goneReads >= kRemoteEnterGoneReads)
                {
                    return RemoteEnterAnswer{ RemoteEnterResult::Submitted, _pressed };
                }
                return std::nullopt;
            case InputBoxState::Unknown:
            default:
                return RemoteEnterAnswer{ RemoteEnterResult::Pressed, _pressed };
            }
        }

        /// <summary>
        /// The answer when kRemoteEnterVerifyMs passed without one: Kept with the text the box held at its last read - unless that
        /// last read already found the prompt gone (one gone read right before the deadline: a late submit is far likelier than a
        /// repaint frame at exactly that moment), which reads as Submitted.
        /// </summary>
        /// <returns>The answer.</returns>
        /// <exception cref="std::bad_alloc">If the answer cannot be allocated.</exception>
        RemoteEnterAnswer Expire() const
        {
            if (_goneReads > 0)
            {
                return RemoteEnterAnswer{ RemoteEnterResult::Submitted, _pressed };
            }
            return RemoteEnterAnswer{ RemoteEnterResult::Kept, _box };
        }

    private:
        std::wstring _pressed; // the prompt the Enter was pressed on (Submitted / Pressed quote it)
        std::wstring _box; // the text the box held at the last Draft read (Kept quotes it: the prompt as it is NOW)
        int _goneReads{ 0 }; // gone reads in a row since the last Draft read
    };

    // ===== the texts =====================================================================================================

    /// <summary>
    /// The line an /enter is answered with in the topic (a reply to the /enter message): what came of it, the prompt (or the option)
    /// quoted by its first line. Telegram HTML: the quote is escaped here, the rest holds no markup; every other command in it
    /// ("/enter", "/activate", "/screenimage") is a tap-to-send link in the clients.
    /// </summary>
    /// <param name="a">The answer (ParseRemoteEnterReply of the window's reply, or the service's own early verdict).</param>
    /// <param name="stateAtAsk">The session's state when the user sent /enter: a prompt submitted into a RUNNING turn is held by
    /// Claude Code until the turn's next step, and the line says so.</param>
    /// <returns>The line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::wstring RemoteEnterOutcomeHtml(const RemoteEnterAnswer& a, SessionState stateAtAsk)
    {
        // «first line», escaped - the form /sendnow's "Sending:" line quotes a prompt in.
        const std::wstring quoted = a.detail.empty() ? std::wstring{} : L"\x00AB" + TgEscapeHtml(TgFirstLine(a.detail, kRemoteEnterExcerptChars)) + L"\x00BB";
        switch (a.result)
        {
        case RemoteEnterResult::Submitted:
            if (stateAtAsk == SessionState::Running)
            {
                return L"\x23CE Submitted into the running turn: " + quoted + L" \x2014 Claude Code takes it in at its next step, or starts the next turn with it.";
            }
            return L"\x23CE Submitted: " + quoted;
        case RemoteEnterResult::Kept:
            return L"\x23CE Enter pressed, but the box still holds " + (quoted.empty() ? std::wstring{ L"a prompt" } : quoted) +
                   L" \x2014 Claude Code did not submit it. An Enter can also pick from a list it shows (a file, a command), start a new line after a \\, or only clean out invisible characters: send /enter again to submit it as it is now.";
        case RemoteEnterResult::Pressed:
            return L"\x23CE Enter pressed with " + (quoted.empty() ? std::wstring{ L"a prompt" } : quoted) +
                   L" in the box, but its screen could not be read after it (the tab is closing or parking): whether Claude Code took it is not known.";
        case RemoteEnterResult::Empty:
            return L"Its input box is empty: there is no prompt to submit, so nothing was pressed. (A dim suggestion in the box is not a prompt.)";
        case RemoteEnterResult::MenuOpen:
            return L"A menu or dialog is open on its screen, and Enter would pick the option it highlights" + (quoted.empty() ? std::wstring{} : L" (" + quoted + L")") +
                   L", so nothing was pressed. A question has its own card here; \x238B Esc on \x2699\xFE0F Session's page closes a dialog.";
        case RemoteEnterResult::NoBox:
            return L"Its screen shows no input box right now (a dialog over it, or claude still drawing), so nothing was pressed \x2014 /screenimage shows what is there.";
        case RemoteEnterResult::Busy:
            return L"A prompt is being typed into its input box right now \x2014 send /enter again in a few seconds.";
        case RemoteEnterResult::NotRunning:
            return L"Its claude is not running (a parked tab, or claude exited), so there is no input box to press Enter in \x2014 /activate starts a parked tab.";
        case RemoteEnterResult::NotClaude:
            return L"Enter goes to Claude Code sessions only \x2014 Agentmaster types nothing into a Codex session.";
        case RemoteEnterResult::Failed:
        default:
            return L"The Enter could not be written (no input is bound to the session) \x2014 hooks.log's [enter] line says why.";
        }
    }

    /// <summary>
    /// The line for an /enter written while Agentmaster was closed (older than kBacklogAgeMs when it is read, MessagingModels.h
    /// IsBacklogUpdate): nothing is pressed - an Enter belongs to the screen of the moment it was sent, and the box may hold anything
    /// by now. Unlike a plain message's backlog question it offers no button: pressing now is one more /enter.
    /// </summary>
    /// <param name="age">How old the message is (TgShortSpan: "2 h").</param>
    /// <returns>Telegram HTML (the age escaped).</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::wstring RemoteEnterBacklogHtml(std::wstring_view age)
    {
        return L"This /enter is " + TgEscapeHtml(age) + L" old (sent while Agentmaster was closed), so nothing was pressed: an Enter belongs to the screen of its moment. Send /enter again to press it now.";
    }

    /// <summary>
    /// The usage line for "/enter &lt;words&gt;": nothing is pressed, because the words would be dropped while the prompt in the box
    /// went - and the user who typed them meant them to go. It points at the two ways words go: /steer now, a plain message queued.
    /// </summary>
    /// <returns>Telegram HTML.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::wstring RemoteEnterUsageHtml()
    {
        return L"/enter takes no words: it presses Enter on the prompt already in the session's input box at the PC, so nothing was pressed. To send new words now, use /steer &lt;text&gt;; a plain message queues them.";
    }

    /// <summary>The line for an /enter in the topic of a session that is closed (no open tab, so no terminal): Esc's wording for the same case.</summary>
    inline constexpr std::wstring_view kRemoteEnterClosedText = L"This session is closed. Resume it from the \U0001F50E Search topic.";

    /// <summary>
    /// The line when NO window answered an /enter (no window hosts the session's tab - it is closing - or the hosting window's UI
    /// thread did not get to it): worded so it is true either way, since the key may or may not have gone. Plain text with no
    /// markup, so it is posted as is.
    /// </summary>
    inline constexpr std::wstring_view kRemoteEnterUnansweredText = L"\x23CE No window answered for its tab (it is closing, or the PC is busy) \x2014 the Enter may not have been pressed.";
}
