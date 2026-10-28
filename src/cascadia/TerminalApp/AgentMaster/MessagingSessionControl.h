// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the sticky card's ⎋ Esc and 🌙 Deactivate (MESSAGING.md §21.41). The user's request (2026-10-27): "Telegram bot
// needs to be able to also per session: a. interrupt via sending esc onto the screen via esc button b. deactivate tab (and by
// that also claude stops running)".
//
// What each one does:
//   ⎋ Esc          ONE Esc key written into the session's terminal, exactly as the user would press it at the PC. A running turn
//                  stops (Claude Code writes "[Request interrupted by user]" and waits; the runner's interrupt hold parks an Auto
//                  queue until the user's next message, DELIVERY.md §14), a permission or question dialog is turned down, an open
//                  menu closes, and at rest with an empty box nothing happens. It asks nothing first: the button IS the key, and
//                  what it does is undone by the next message ("go on"). Only the card's Session page shows it (⚙ Session, the
//                  row above ◀ Back), whenever the session's claude reads keys — the user's rule of 2026-10-28, "The ESC button
//                  should be only inside Sessions menu": it no longer sits beside ⚙ Session on Main while a turn runs.
//   🌙 Deactivate   the tab menu's "Deactivate ▸ Deactivate Tab": claude stops (its ConPTY closes with a graceful CTRL_CLOSE), the
//                  tab keeps its place, title, color, tags, queue and runner mode, and the conversation resumes when the tab is
//                  entered or activated, or when a message arrives in its topic. At rest it goes at once; while a turn is in
//                  flight the service first asks in the topic (/deactivate's own question): the cut-off turn would read Error on
//                  its resume (HOOKS.md "Cut-off turns").
//
// Both are carried out by the window that HOSTS the session's tab, on its UI thread (the session-action sink, Engine.h
// RequestSessionActionInWindows; TerminalPage::_RemoteSessionAction). That thread is the one every keystroke of the tab goes
// through, the one that owns the draft swap's box lock (_draftSwapsInFlight), and the only one that may swap the pane's control
// for a park. The window judges the request AGAIN there, with what it sees at that moment (DecideRemoteEsc / DecideRemotePark),
// and answers with a word; the service turns the word into the tapped button's toast (a card button) or into a line in the
// topic (a typed /interrupt or /deactivate, which share these paths so the button and the command can never differ).
//
// This header is the pure half (no window, no Telegram call, no service): the two action words, who may get which button, the
// window's decisions, the answer's words and the texts that report them. The m5 suite pins every rule (tests_messaging_card.cpp,
// section 1c).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "SessionModels.h"

namespace Agentmaster::Msg
{
    // ===== the window actions (ServiceHooks::sessionAction) ==========================================================

    /// <summary>
    /// The session action that writes ONE Esc into the session's terminal (no argument; the answer is a RemoteEscResultWord). Only
    /// the window that hosts the session's tab acts; every other window ignores it. Distinct from every other action word (the
    /// bot's "start" / "screen" / "menu-answer" …, the session menu's "session-menu-*"), so nothing else can send it by accident.
    /// </summary>
    inline constexpr std::wstring_view kPressEscAction = L"press-esc";

    /// <summary>
    /// The session action that parks the session's tab as the tab menu's "Deactivate ▸ Deactivate Tab" does (the argument: "" =
    /// only while no turn is in flight, kDeactivateCutOffTurnArg = a turn in flight may be cut off; the answer is a
    /// RemoteParkResultWord).
    /// </summary>
    /// <remarks>
    /// ⚠ Any argument other than kDeactivateCutOffTurnArg, a typo included, reads as "at rest only" (DecideRemotePark's
    /// cutOffTurn false): a malformed request must never be read as the user's consent to cut a running turn off.
    /// </remarks>
    inline constexpr std::wstring_view kDeactivateAction = L"deactivate";

    /// <summary>
    /// The one argument of kDeactivateAction that lets the park cut a turn in flight off. The service sends it only after the user
    /// answered "Park now" to /deactivate's question (the token confirm-deactivate); the card's 🌙 Deactivate never sends it.
    /// </summary>
    inline constexpr std::wstring_view kDeactivateCutOffTurnArg = L"cut-off-turn";

    /// <summary>
    /// One Esc per session per this many ms (monotonic). A second Esc within it is refused before anything is written, because
    /// two Esc presses close together on an EMPTY input box open Claude Code's Rewind menu (the e2e suite keeps its two Esc
    /// presses 2 s apart for that reason, test_prompt_queue_runner.py) - and a phone's double tap, or a tap right after a
    /// confirmed /interrupt, is exactly that. 3 s is longer than a double tap and than the window's answer, short enough that a
    /// deliberate second Esc still goes.
    /// </summary>
    inline constexpr int64_t kRemoteEscGapMs = 3 * 1000;

    /// <summary>Whether a session's Esc request at `nowMs` is too soon after the one at `lastPressMs` (kRemoteEscGapMs).</summary>
    /// <param name="lastPressMs">When the session's previous Esc was asked for (monotonic ms); 0 = never.</param>
    /// <param name="nowMs">Now, on the same clock.</param>
    /// <returns>True when the request must be refused (it would land inside the previous one's window).</returns>
    inline bool RemoteEscTooSoon(int64_t lastPressMs, int64_t nowMs) noexcept
    {
        // A clock that went backwards (nowMs < lastPressMs) reads as "too soon" too: the monotonic clock never does, and a
        // refusal costs one retry, where a second Esc can open the Rewind menu.
        return lastPressMs > 0 && nowMs - lastPressMs < kRemoteEscGapMs;
    }

    // ===== who gets which button =======================================================================================

    /// <summary>
    /// Whether a session state is a turn in flight: Running, or Needs approval (a permission or question dialog the turn waits on).
    /// Both are cut off by a park or a restart and stopped by an Esc; every other state is at rest.
    /// </summary>
    /// <param name="state">The state.</param>
    /// <returns>True for Running and NeedsApproval.</returns>
    inline bool TurnInFlightState(SessionState state) noexcept
    {
        return state == SessionState::Running || state == SessionState::NeedsApproval;
    }

    /// <summary>
    /// Whether a session has a turn in flight in an open tab: what makes 🌙 Deactivate and /deactivate ask before they park (the
    /// turn would be cut off), and what /interrupt's question reads. (It no longer places ⎋ Esc on the card's Main page: the user's
    /// rule of 2026-10-28 keeps Esc on the Session page only.)
    /// </summary>
    /// <param name="s">The session (a lean record is enough).</param>
    /// <returns>True for a live session in Running or NeedsApproval.</returns>
    inline bool SessionTurnInFlight(const SessionInfo& s) noexcept
    {
        return s.live && TurnInFlightState(s.state);
    }

    /// <summary>
    /// Whether an Esc written into the session's terminal reaches a claude that reads it: an open Claude Code session whose claude
    /// runs. False for a parked tab (its connection never started: nothing reads the key), a claude that exited (Done), a closed
    /// session, and a Codex session (Agentmaster binds no stdin injector to Codex: lifecycle and state only).
    /// </summary>
    /// <remarks>
    /// An ADOPTED session (`external`: a claude the user typed into a shell tab) counts as running although its `started` flag is
    /// false - that flag is meaningless for it (SessionInfo::started) - because the injector bound at the adoption writes into
    /// that very tab, so the Esc reaches its claude the way a key pressed in the tab would.
    /// </remarks>
    /// <param name="s">The session (a lean record is enough).</param>
    /// <returns>True when ⎋ Esc may be offered for it.</returns>
    inline bool SessionTakesEsc(const SessionInfo& s) noexcept
    {
        return s.live && s.kind == AgentKind::Claude && (s.started || s.external) && s.state != SessionState::Done;
    }

    /// <summary>
    /// Whether 🌙 Deactivate has a tab to park: an open session whose agent process was started in a tab Agentmaster launched
    /// (Claude or Codex - the park handles both). False for a parked tab (it is in that state already), an adopted session (its
    /// ConPTY is the user's own shell, which a park would end - TerminalPage::_ResolveDeactivateTarget refuses it) and a closed one.
    /// </summary>
    /// <param name="s">The session (a lean record is enough).</param>
    /// <returns>True when 🌙 Deactivate may be offered for it.</returns>
    inline bool SessionIsDeactivatable(const SessionInfo& s) noexcept
    {
        return s.live && s.started && !s.external;
    }

    // ===== ⎋ Esc: the window's decision and its answer =================================================================

    /// <summary>The hosting window's answer to kPressEscAction (and the service's own early verdict on a tap it can refuse alone).</summary>
    enum class RemoteEscResult
    {
        Pressed, // one Esc was written into the session's terminal (as DecideRemoteEsc's verdict: it may be written now)
        NotRunning, // nothing reads the key: the tab is parked, its claude exited, or the session is closed - nothing was written
        NotClaude, // a Codex session: Agentmaster types nothing into Codex - nothing was written
        Busy, // a delivery or a draft operation owns the input box right now - nothing was written (ask again in a moment)
        Failed, // the write itself failed (no injector bound: the tab is closing or parking) or the answer was not understood
    };

    /// <summary>The word of an Esc result, as the window replies it and the log prints it.</summary>
    /// <param name="r">The result.</param>
    /// <returns>pressed / not-running / not-claude / busy / failed (an unknown value reads as failed).</returns>
    inline std::wstring_view RemoteEscResultWord(RemoteEscResult r) noexcept
    {
        switch (r)
        {
        case RemoteEscResult::Pressed:
            return L"pressed";
        case RemoteEscResult::NotRunning:
            return L"not-running";
        case RemoteEscResult::NotClaude:
            return L"not-claude";
        case RemoteEscResult::Busy:
            return L"busy";
        case RemoteEscResult::Failed:
        default:
            return L"failed";
        }
    }

    /// <summary>
    /// Reads a window's answer to kPressEscAction (RemoteEscResultWord's words). Anything else - an empty reply, a window of an
    /// older build that does not know the action and answers "" - reads as Failed, never as Pressed: the toast must not claim a
    /// key went that nobody confirmed.
    /// </summary>
    /// <param name="word">The reply.</param>
    /// <returns>The result.</returns>
    inline RemoteEscResult ParseRemoteEscResult(std::wstring_view word) noexcept
    {
        for (const RemoteEscResult r : { RemoteEscResult::Pressed, RemoteEscResult::NotRunning, RemoteEscResult::NotClaude, RemoteEscResult::Busy })
        {
            if (word == RemoteEscResultWord(r))
            {
                return r;
            }
        }
        return RemoteEscResult::Failed;
    }

    /// <summary>
    /// The hosting window's decision before it writes the Esc (TerminalPage::_RemoteSessionAction, kPressEscAction), made on its
    /// UI thread at the moment of the write - and the service's early verdict on a tap (with `boxHeld` false: only the window can
    /// see the box). Pressed means "write it now"; anything else says why not, and nothing is written.
    /// </summary>
    /// <remarks>
    /// The box test is the one the /add-dir menu press uses (DecideMenuAnswer): while a delivery types a prompt into the box (the
    /// delivery gate) or a draft swap / mail move owns it (_draftSwapsInFlight), an Esc would land between its keystrokes. Right
    /// after a delivery's Enter it would also put the prompt back into the box (an Esc before any output writes no row, HOOKS.md
    /// "Interrupted turns"), and the Enter-retry watch would then press Enter again - sending the very prompt the user stopped.
    /// So the Esc waits until the delivery is done; the turn it started can then be stopped like any other.
    /// </remarks>
    /// <param name="s">The session as the registry has it now (a lean record is enough).</param>
    /// <param name="boxHeld">True while the session's delivery gate is held or a draft swap owns its input box.</param>
    /// <returns>Pressed when one Esc may be written now; else why not.</returns>
    inline RemoteEscResult DecideRemoteEsc(const SessionInfo& s, bool boxHeld) noexcept
    {
        // "Nothing reads the key" first: a parked Codex tab is better told it is parked than that it is Codex.
        if (!s.live || (!s.started && !s.external) || s.state == SessionState::Done)
        {
            return RemoteEscResult::NotRunning;
        }
        if (s.kind != AgentKind::Claude)
        {
            return RemoteEscResult::NotClaude;
        }
        return boxHeld ? RemoteEscResult::Busy : RemoteEscResult::Pressed;
    }

    /// <summary>
    /// What the user is told about an Esc: the tapped button's toast (a card's ⎋ Esc) or the line in the topic (a confirmed
    /// /interrupt). One plain sentence, within Telegram's 200-character toast (MessagingTaps.h kTapToastMaxChars, 190), no HTML
    /// (a caller that posts it as a line escapes it).
    /// </summary>
    /// <param name="r">The result.</param>
    /// <param name="stateAtTap">The session's state when the user asked (what a pressed Esc did depends on it: a turn stops, a
    /// dialog is turned down, or nothing ran).</param>
    /// <returns>The text.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring RemoteEscOutcomeText(RemoteEscResult r, SessionState stateAtTap)
    {
        switch (r)
        {
        case RemoteEscResult::Pressed:
            if (stateAtTap == SessionState::Running)
            {
                return L"\x238B Esc pressed \x2014 the turn stops and the session waits for you. A message here goes on from where it stopped.";
            }
            if (stateAtTap == SessionState::NeedsApproval)
            {
                return L"\x238B Esc pressed \x2014 as at the PC, the permission or question it waited on is turned down, and the turn stops.";
            }
            return L"\x238B Esc pressed. No turn was running, so it only closes a menu or dialog, if the screen shows one.";
        case RemoteEscResult::NotRunning:
            return L"Its claude is not running (a parked tab, or claude exited), so there is nothing to press Esc into.";
        case RemoteEscResult::NotClaude:
            return L"Esc goes to Claude Code sessions only \x2014 Agentmaster types nothing into a Codex session.";
        case RemoteEscResult::Busy:
            return L"A prompt is being typed into its input box right now \x2014 tap \x238B Esc again in a few seconds.";
        case RemoteEscResult::Failed:
        default:
            return L"The Esc could not be written (no input is bound to the session) \x2014 hooks.log says why.";
        }
    }

    /// <summary>
    /// The toast (or line) of an Esc refused because the session's previous one went less than kRemoteEscGapMs ago (RemoteEscTooSoon).
    /// It names the reason, so a user whose first tap seemed to do nothing knows a second one in a moment will go.
    /// </summary>
    inline constexpr std::wstring_view kRemoteEscRepeatedText =
        L"\x238B Esc went a moment ago \x2014 one per 3 s, since two quick ones open Claude Code's Rewind menu. Tap again in a moment if the turn still runs.";

    /// <summary>
    /// The toast (or line) when NO window answered an Esc request: no window hosts the session's tab (it closed, or a claude outside
    /// Agentmaster's tabs), or the hosting window's UI thread did not get to it. Worded so it is true either way - the key may or
    /// may not have gone.
    /// </summary>
    inline constexpr std::wstring_view kRemoteEscUnansweredText =
        L"\x238B No window answered for its tab (it is closing, or the PC is busy) \x2014 the Esc may not have been pressed.";

    // ===== 🌙 Deactivate: the window's decision and its answer =========================================================

    /// <summary>The hosting window's answer to kDeactivateAction (and the service's own early verdict on a tap it can refuse alone).</summary>
    enum class RemoteParkResult
    {
        Parked, // the tab is parked: its claude stopped (as DecideRemotePark's verdict: it may be parked now)
        AlreadyParked, // the tab was parked already (restored and never entered, or deactivated before) - nothing changed
        Adopted, // an adopted session: its ConPTY is the user's own shell, which a park would end - nothing changed
        TurnRunning, // a turn is in flight and the request did not allow cutting it off - nothing changed (the service asks first)
        Busy, // a delivery or a draft operation owns the input box right now - nothing changed (ask again in a moment)
        Closed, // the session is not open in a tab any more - nothing changed
        Failed, // the park was refused by its own checks (no claude.exe, a preset that cannot launch, a pane that did not build -
                // hooks.log's [deactivate] line says which) or the answer was not understood
    };

    /// <summary>The word of a park result, as the window replies it and the log prints it.</summary>
    /// <param name="r">The result.</param>
    /// <returns>parked / already-parked / adopted / turn-running / busy / closed / failed (an unknown value reads as failed).</returns>
    inline std::wstring_view RemoteParkResultWord(RemoteParkResult r) noexcept
    {
        switch (r)
        {
        case RemoteParkResult::Parked:
            return L"parked";
        case RemoteParkResult::AlreadyParked:
            return L"already-parked";
        case RemoteParkResult::Adopted:
            return L"adopted";
        case RemoteParkResult::TurnRunning:
            return L"turn-running";
        case RemoteParkResult::Busy:
            return L"busy";
        case RemoteParkResult::Closed:
            return L"closed";
        case RemoteParkResult::Failed:
        default:
            return L"failed";
        }
    }

    /// <summary>
    /// Reads a window's answer to kDeactivateAction (RemoteParkResultWord's words). Anything else - an empty reply, a window of an
    /// older build - reads as Failed, never as Parked: the toast must not claim a park nobody confirmed.
    /// </summary>
    /// <param name="word">The reply.</param>
    /// <returns>The result.</returns>
    inline RemoteParkResult ParseRemoteParkResult(std::wstring_view word) noexcept
    {
        for (const RemoteParkResult r : { RemoteParkResult::Parked, RemoteParkResult::AlreadyParked, RemoteParkResult::Adopted, RemoteParkResult::TurnRunning,
                                          RemoteParkResult::Busy, RemoteParkResult::Closed })
        {
            if (word == RemoteParkResultWord(r))
            {
                return r;
            }
        }
        return RemoteParkResult::Failed;
    }

    /// <summary>
    /// The hosting window's decision before it parks the tab (TerminalPage::_RemoteSessionAction, kDeactivateAction), made on its
    /// UI thread right before the park - and the service's early verdict on a tap (with `boxHeld` false: only the window can see
    /// the box). Parked means "park it now" (the park's own checks still apply and may still refuse: no claude.exe, a preset that
    /// cannot launch); anything else says why not, and nothing is touched.
    /// </summary>
    /// <remarks>
    /// The turn check is made AGAIN here because the service's check and the park are a dispatcher hop apart: a turn that started
    /// in between (the user typed at the PC, the runner sent a queued prompt) must not be cut off by a request that the user made
    /// for a session at rest. It comes before the box check: whether to cut a turn off is the user's decision, and a held box
    /// only says "in a moment". The adopted check comes before the parked one because an adopted session's `started` flag is
    /// always false (meaningless for it), which would otherwise read as "parked already".
    /// </remarks>
    /// <param name="s">The session as the registry has it now (a lean record is enough).</param>
    /// <param name="boxHeld">True while the session's delivery gate is held or a draft swap owns its input box (the park closes
    /// the very control such an operation is verifying, so it refuses).</param>
    /// <param name="cutOffTurn">True when the user confirmed that a turn in flight may be cut off (kDeactivateCutOffTurnArg).</param>
    /// <returns>Parked when the tab may be parked now; else why not.</returns>
    inline RemoteParkResult DecideRemotePark(const SessionInfo& s, bool boxHeld, bool cutOffTurn) noexcept
    {
        if (!s.live)
        {
            return RemoteParkResult::Closed;
        }
        if (s.external)
        {
            return RemoteParkResult::Adopted;
        }
        if (!s.started)
        {
            return RemoteParkResult::AlreadyParked;
        }
        if (TurnInFlightState(s.state) && !cutOffTurn)
        {
            return RemoteParkResult::TurnRunning;
        }
        return boxHeld ? RemoteParkResult::Busy : RemoteParkResult::Parked;
    }

    /// <summary>
    /// What the user is told about a park: the tapped button's toast (the card's 🌙 Deactivate) or the line in the topic (/deactivate,
    /// typed or confirmed). One plain sentence within Telegram's toast limit (190 characters here), no HTML.
    /// </summary>
    /// <remarks>
    /// TurnRunning has a text for completeness (a caller that cannot ask), but the service answers it with /deactivate's
    /// question instead: the user decides whether the turn may be cut off.
    /// </remarks>
    /// <param name="r">The result.</param>
    /// <returns>The text.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring RemoteParkOutcomeText(RemoteParkResult r)
    {
        switch (r)
        {
        case RemoteParkResult::Parked:
            return L"\U0001F319 Deactivated \x2014 its claude is stopped and the tab keeps its place. A message here or \x25B6 Reactivate resumes it.";
        case RemoteParkResult::AlreadyParked:
            return L"\U0001F319 It is parked already \x2014 a message here or \x25B6 Reactivate resumes it.";
        case RemoteParkResult::Adopted:
            return L"This tab runs a claude you started by hand in a shell; Deactivate never parks it (that would end your shell).";
        case RemoteParkResult::TurnRunning:
            return L"A turn is running \x2014 deactivating would cut it off; confirm below.";
        case RemoteParkResult::Busy:
            return L"A prompt is being typed into its input box right now \x2014 tap \U0001F319 Deactivate again in a few seconds.";
        case RemoteParkResult::Closed:
            return L"This session is closed. Resume it from the \U0001F50E Search topic.";
        case RemoteParkResult::Failed:
        default:
            return L"It was not deactivated \x2014 hooks.log's [deactivate] and [nav] deactivate-done lines say why.";
        }
    }

    /// <summary>
    /// The toast (or line) when NO window answered a park request (no window hosts the session's tab, or the hosting window's UI
    /// thread did not get to it). Worded so it is true either way - the park may or may not have happened.
    /// </summary>
    inline constexpr std::wstring_view kRemoteParkUnansweredText =
        L"\U0001F319 No window answered for its tab (it is closing, or the PC is busy) \x2014 it may not have been deactivated.";
}
