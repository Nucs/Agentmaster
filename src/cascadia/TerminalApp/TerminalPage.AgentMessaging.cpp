// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TerminalPage, the MESSAGING partial (MESSAGING.md §8.2): the window-hosted half of the
// Telegram bot. The bot runs on engine threads; what needs a window — starting a dormant tab's claude,
// reading its screen, the unread ring, a rename that pins the tab title, the tag badges, answering a local
// command's dialog (§21.34: /add-dir's menu, pressed only after the screen shows it), and opening a
// session as a background tab — arrives here through the two sinks TerminalPage.AgentEngine.cpp registers
// (session actions fanned out to every window, launches aimed at the most recently activated one).

#include "pch.h"
#include "TerminalPage.h"

#include "AgentCatchLog.h" // AgentLogCaughtException (Rule #18)
#include "AgentMaster/ClaudeSpawn.h" // LogNav, ShortId
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/TuiMenus.h" // MESSAGING.md §21.34: the menu actions (kMenuScreenAction / kMenuAnswerAction, DecideMenuAnswer)

using namespace winrt::Microsoft::Terminal::Control;

namespace winrt::TerminalApp::implementation
{
    bool TerminalPage::_RemoteSessionAction(const std::wstring& sessionId, const std::wstring& action, const std::wstring& arg, const std::function<void(std::wstring)>& reply)
    {
        try
        {
            if (sessionId.empty() || _claudeTabs.find(sessionId) == _claudeTabs.end())
            {
                return false; // not hosted here: another window's sink acts
            }
            ::Agentmaster::LogNav(L"remote session-action " + action + L" " + ::Agentmaster::ShortId(sessionId));
            std::wstring answer;
            if (action == L"start")
            {
                // A dormant tab (window-restored, revived, parked): start its claude in place, no focus change, so a
                // queued remote prompt can be delivered (the queue resumes by itself on SetStarted).
                _ActivateDormantSession(sessionId);
            }
            else if (action == L"screen")
            {
                answer = _ReadScreenTextForSession(sessionId, 60);
            }
            else if (action == L"screen-snapshot")
            {
                // MESSAGING.md §21.24 (the card's 🖼 Screen Image): the live screen with every cell's colors, for the bot to draw
                // as a picture off this UI thread. The read itself is one pass over the viewport's cells under the terminal's read lock.
                answer = _ReadScreenSnapshotForSession(sessionId);
            }
            else if (action == L"markunread")
            {
                _MarkSessionUnread(sessionId);
            }
            else if (action == L"rename")
            {
                if (!arg.empty())
                {
                    _RenameClaudeSession(winrt::hstring{ sessionId }, winrt::hstring{ arg }); // registry + the pinned tab title (Rule #11)
                }
            }
            else if (action == L"refresh-tags")
            {
                _RefreshTabTags(sessionId);
            }
            else if (action == L"refresh-favorite")
            {
                _RefreshTabFavoriteCrown(sessionId);
            }
            else if (action == ::Agentmaster::Msg::kMenuScreenAction)
            {
                // MESSAGING.md §21.34: the menu watch reads the terminal's last rows to find the dialog a delivered /add-dir opened
                // (ParseAddDirMenuScreen runs on the bot's side). "" for a dormant tab, whose claude shows nothing.
                answer = _ReadScreenTextForSession(sessionId, ::Agentmaster::Msg::kMenuScreenRows);
            }
            else if (action == ::Agentmaster::Msg::kMenuAnswerAction)
            {
                // MESSAGING.md §21.34: press a dialog's option for the bot (a tap on its card, or its default after a minute). A digit
                // typed anywhere but the dialog lands in the input box as text, so the screen is read again AT the press, on this UI
                // thread — the one every keystroke of this tab goes through, so nothing of ours runs between the read and the write —
                // and the digit is typed only into the very dialog the card shows (DecideMenuAnswer: the dialog still open, its path
                // the card's, and no delivery or draft swap holding the box, whose own keystrokes it would interleave with).
                ::Agentmaster::Msg::MenuAnswerResult result = ::Agentmaster::Msg::MenuAnswerResult::Failed;
                std::wstring detail;
                if (const auto request = ::Agentmaster::Msg::ParseMenuAnswerArg(arg))
                {
                    const std::wstring screen = _ReadScreenTextForSession(sessionId, ::Agentmaster::Msg::kMenuScreenRows);
                    const bool boxHeld = _draftSwapsInFlight.count(sessionId) != 0 || (_sessionRegistry && _sessionRegistry->DeliveryGateHeld(sessionId));
                    result = ::Agentmaster::Msg::DecideMenuAnswer(*request, screen, boxHeld);
                    if (result == ::Agentmaster::Msg::MenuAnswerResult::Pressed)
                    {
                        // The digit alone: the dialog picks its option on the key (no Enter — an Enter after it would reach the
                        // input box the dialog gives back).
                        if (!_sessionRegistry || !_sessionRegistry->Inject(sessionId, std::wstring(1, static_cast<wchar_t>(L'0' + request->digit))))
                        {
                            result = ::Agentmaster::Msg::MenuAnswerResult::Failed;
                            detail = L" (no injector is bound to the session)";
                        }
                    }
                    const auto* option = ::Agentmaster::Msg::AddDirOptionFor(request->digit);
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[menu] " + ::Agentmaster::ShortId(sessionId) + L" /" + request->command + L" dialog: option " +
                                                                    std::to_wstring(request->digit) + (option ? L" (" + std::wstring{ option->value } + L")" : std::wstring{}) +
                                                                    L" -> " + std::wstring{ ::Agentmaster::Msg::MenuAnswerResultWord(result) } + detail + L"\n");
                }
                else
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[menu] " + ::Agentmaster::ShortId(sessionId) + L" a malformed menu answer was refused: nothing typed\n");
                }
                answer = std::wstring{ ::Agentmaster::Msg::MenuAnswerResultWord(result) };
            }
            else if (action == kSessionMenuDeactivateAction || action == kSessionMenuCloseAction)
            {
                // Agentmaster (session menu "Deactivate ▸" / "Close ▸" — not the bot): another window's board card, tree row
                // or side-panel card menu picked one of those items for a session whose tab lives HERE, and every scope is
                // counted from that tab, in this window (TerminalPage::_RunSessionMenuScope). Run it as this tab's own
                // menu would. The argument is the scope code in decimal — 1 to 9 digits and nothing else; anything else is
                // refused, so a malformed request can never be read as "this tab" (scope 0).
                bool digitsOnly = !arg.empty() && arg.size() <= 9;
                int32_t scope = 0;
                // The length test comes FIRST (the loop runs only while digitsOnly holds), so the sum never sees a 10th
                // digit: 9 digits stay below INT32_MAX, and a longer argument is refused before any arithmetic.
                for (size_t i = 0; digitsOnly && i < arg.size(); ++i)
                {
                    const wchar_t c = arg[i];
                    if (c < L'0' || c > L'9')
                    {
                        digitsOnly = false;
                        break;
                    }
                    scope = scope * 10 + static_cast<int32_t>(c - L'0');
                }
                if (digitsOnly)
                {
                    _RunSessionMenuScopeHere(sessionId, action == kSessionMenuCloseAction, scope, /*fromOtherWindow*/ true);
                }
                else
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[session-menu] " + ::Agentmaster::ShortId(sessionId) + L" " + action + L" refused - the scope argument \"" + arg + L"\" is not a number (nothing done)\n");
                }
            }
            else
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[msg] unknown remote session action: " + action + L"\n");
            }
            if (reply)
            {
                reply(std::move(answer));
            }
            return true;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_RemoteSessionAction");
            return false;
        }
    }

    std::wstring TerminalPage::_ReadScreenSnapshotForSession(const std::wstring& sessionId)
    {
        try
        {
            // The same gate as the text read below: a session another window hosts has no control here, and a dormant tab
            // (a restored or parked one, its claude not started) has nothing on its screen yet.
            const auto control = _ControlForSession(sessionId);
            if (!control || control.ConnectionState() == winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::NotConnected)
            {
                return {};
            }
            const auto snapshot = control.ReadScreenSnapshot();
            return std::wstring{ snapshot.c_str(), snapshot.size() };
        }
        catch (...)
        {
            // Rule #18. Recovery: "" - the bot answers that the screen could not be read.
            ::Agentmaster::AgentLogCaughtException(L"_ReadScreenSnapshotForSession");
            return {};
        }
    }

    std::wstring TerminalPage::_ReadScreenTextForSession(const std::wstring& sessionId, size_t maxLines)
    {
        try
        {
            const auto control = _ControlForSession(sessionId);
            if (!control || control.ConnectionState() == winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::NotConnected)
            {
                return {};
            }
            const auto h = control.ReadEntireBuffer();
            std::wstring all{ h.c_str(), h.size() };
            // The screen is the END of the buffer: keep the last `maxLines` non-blank-tail lines.
            while (!all.empty() && (all.back() == L'\n' || all.back() == L'\r' || all.back() == L' '))
            {
                all.pop_back();
            }
            size_t pos = all.size();
            size_t lines = 0;
            while (pos > 0 && lines < maxLines)
            {
                const size_t nl = all.rfind(L'\n', pos - 1);
                if (nl == std::wstring::npos)
                {
                    pos = 0;
                    break;
                }
                pos = nl;
                ++lines;
            }
            std::wstring tail = all.substr(pos == 0 ? 0 : pos + 1);
            // Trailing spaces on every line (the buffer pads rows to the width) are noise in a <pre>.
            std::wstring out;
            out.reserve(tail.size());
            size_t start = 0;
            while (start <= tail.size())
            {
                const size_t nl = tail.find(L'\n', start);
                std::wstring line = tail.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
                while (!line.empty() && (line.back() == L' ' || line.back() == L'\r'))
                {
                    line.pop_back();
                }
                out += line;
                if (nl == std::wstring::npos)
                {
                    break;
                }
                out += L'\n';
                start = nl + 1;
            }
            return out;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ReadScreenTextForSession");
            return {};
        }
    }

    ::Agentmaster::RemoteLaunchOutcome TerminalPage::_RemoteLaunch(const ::Agentmaster::RemoteLaunchRequest& request) noexcept
    {
        using Kind = ::Agentmaster::RemoteLaunchRequest::Kind;
        using Result = ::Agentmaster::RemoteLaunchResult;
        // Failed until the seams say otherwise: an answer this function could not finish never passes for a success.
        ::Agentmaster::RemoteLaunchOutcome outcome;
        try
        {
            outcome.windowId = _windowId;
            const wchar_t* what = request.kind == Kind::Fork ? L"fork" : (request.kind == Kind::Resume ? L"resume" : L"new");
            ::Agentmaster::LogNav(std::wstring{ L"remote launch " } + what + L" " + ::Agentmaster::ShortId(request.sessionId) + L" dir=" + request.dir + (request.model.empty() ? std::wstring{} : L" model=" + request.model) +
                                  (request.newSessionId.empty() ? std::wstring{} : L" id=" + ::Agentmaster::ShortId(request.newSessionId)));
            if (request.kind != Kind::NewSession && request.dir.empty())
            {
                outcome.result = Result::NoFolder;
                ::Agentmaster::LogNav(L"remote launch done no-folder (dropped: no folder for the session)");
                return outcome;
            }
            // The phone's launches never take focus: the Sessions page's bulk-open latch keeps the new tab in the background and
            // skips the page hide / activation. MESSAGING.md §21.23: and they never show a dialog on the PC — the seams record a
            // refusal they would have shown (_NoteRemoteLaunchRefusal), and it becomes this launch's answer. Both latches cover
            // exactly this one synchronous launch; the guard restores them on every path out, a throw included.
            const bool previousBackground = _openClaudeTabInBackground;
            const bool previousInFlight = _remoteLaunchInFlight;
            _openClaudeTabInBackground = true;
            _remoteLaunchInFlight = true;
            _remoteLaunchRefusal.reset();
            auto restore = wil::scope_exit([this, previousBackground, previousInFlight] {
                _openClaudeTabInBackground = previousBackground;
                _remoteLaunchInFlight = previousInFlight;
                _remoteLaunchRefusal.reset(); // a refusal belongs to the launch that recorded it, never to the next one
            });
            std::wstring openedId;
            bool wasOpen = false; // a resume of a session that is live already: its tab stands, nothing new opens
            bool fresh = false; // a new conversation opens in place of the one asked for
            switch (request.kind)
            {
            case Kind::NewSession:
            {
                // MESSAGING.md §21.25: right after the anchor tab (the newest tab working in the same folder, the wizard's
                // default) when THIS window hosts it — _claudeTabs is the window's own binding map, so a session another window
                // hosts, or one that closed since the census, leaves the tab at the strip's end (-1, the NewTabPosition default).
                uint32_t insertAt = static_cast<uint32_t>(-1);
                if (!request.afterSessionId.empty())
                {
                    const auto it = _claudeTabs.find(request.afterSessionId);
                    const auto anchor = it != _claudeTabs.end() ? it->second.get() : nullptr;
                    uint32_t anchorIndex = 0;
                    if (anchor && _tabs.IndexOf(anchor, anchorIndex))
                    {
                        insertAt = anchorIndex + 1;
                    }
                    ::Agentmaster::LogNav(L"remote launch after " + ::Agentmaster::ShortId(request.afterSessionId) +
                                          (insertAt == static_cast<uint32_t>(-1) ? std::wstring{ L" -> not in this window: at the end" } : L" -> slot " + std::to_wstring(insertAt)));
                }
                // MESSAGING.md §21.17: the id the requester chose, when it chose one (the bot's new-thread picker binds the user's
                // thread to it), else a minted one.
                openedId = _SpawnClaudeSession(winrt::hstring{ request.dir }, winrt::hstring{ request.title }, insertAt, winrt::hstring{ request.model }, request.newSessionId);
                break;
            }
            case Kind::Resume:
            {
                const auto before = _sessionRegistry ? _sessionRegistry->GetWithoutQueue(request.sessionId) : std::nullopt;
                wasOpen = before && before->live;
                openedId = _ResumeSessionFromDisk(request.sessionId, request.dir, request.title);
                // A resume whose transcript was gone opens a FRESH session in the same folder (Rule #6) under a new id.
                fresh = !wasOpen && !openedId.empty() && openedId != request.sessionId;
                break;
            }
            case Kind::Fork:
                // A source with no transcript forks into a FRESH session (_ForkSessionFromDisk's own gate, read the same way
                // before the call, since the fork's new id is always new and cannot tell it).
                fresh = !::Agentmaster::ClaudeConversationExists(request.sessionId);
                // HOOKS.md "Interrupted turns": the topic asked "Fork interrupted or uninterrupted?" and this is the answer (the
                // bot resolves Ask before the request leaves it; a stray Ask forks as before).
                openedId = _ForkSessionFromDisk(request.sessionId, request.dir, request.title, request.model, static_cast<uint32_t>(-1),
                                                request.forkTurn == ::Agentmaster::ForkTurnChoice::Interrupted ? ::Agentmaster::ForkTurnChoice::Interrupted : ::Agentmaster::ForkTurnChoice::Uninterrupted);
                break;
            }
            if (!openedId.empty())
            {
                // The id first, the result last: a throw in between leaves the answer Failed, never a success without its id.
                outcome.sessionId = openedId;
                outcome.fresh = fresh;
                outcome.result = wasOpen ? Result::AlreadyOpen : Result::Opened;
            }
            else if (_remoteLaunchRefusal)
            {
                outcome.detail = _remoteLaunchRefusal->detail;
                outcome.result = _remoteLaunchRefusal->result;
            }
            // else: Failed with no detail — nothing opened and no seam said why (its own log line in hooks.log does).
            ::Agentmaster::LogNav(L"remote launch done " + std::wstring{ ::Agentmaster::RemoteLaunchResultName(outcome.result) } +
                                  (openedId.empty() ? std::wstring{} : L" new=" + ::Agentmaster::ShortId(openedId)) + (outcome.fresh ? L" (fresh: no transcript to continue)" : L"") +
                                  (outcome.detail.empty() ? std::wstring{} : L": " + outcome.detail));
        }
        catch (...)
        {
            // Rule #18. Recovery: the answer stands as far as it got — Failed (the default) unless the seams had opened the tab
            // already, whose id and result were set last and in that order. The bot is told; nothing else is retried.
            ::Agentmaster::AgentLogCaughtException(L"_RemoteLaunch");
        }
        return outcome; // a move (no allocation): safe in a noexcept function
    }

    bool TerminalPage::_NoteRemoteLaunchRefusal(::Agentmaster::RemoteLaunchResult result, const std::wstring& detail) noexcept
    {
        if (!_remoteLaunchInFlight)
        {
            return false; // a launch the user started at the PC: the caller shows its dialog, as before MESSAGING.md §21.23
        }
        try
        {
            if (!_remoteLaunchRefusal)
            {
                ::Agentmaster::RemoteLaunchOutcome refusal;
                refusal.result = result;
                refusal.detail = detail;
                refusal.windowId = _windowId;
                _remoteLaunchRefusal = std::move(refusal);
            }
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[remote-launch] refused (" + std::wstring{ ::Agentmaster::RemoteLaunchResultName(result) } + L")" + (detail.empty() ? std::wstring{} : L": " + detail) +
                                                            L" \x2014 no dialog on the PC; the reason goes back to the phone\n");
        }
        catch (...)
        {
            // Rule #18. Recovery: the dialog stays suppressed (nobody may be at the PC) and the answer falls back to Failed with
            // no detail; the refusal's own [launch-blocked] / [launch-fail] line in hooks.log still names it.
            ::Agentmaster::AgentLogCaughtException(L"_NoteRemoteLaunchRefusal");
        }
        return true;
    }
}
