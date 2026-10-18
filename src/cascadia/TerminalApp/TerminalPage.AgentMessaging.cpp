// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TerminalPage, the MESSAGING partial (MESSAGING.md §8.2): the window-hosted half of the
// Telegram bot. The bot runs on engine threads; what needs a window — starting a dormant tab's claude,
// reading its screen, the unread ring, a rename that pins the tab title, the tag badges, and opening a
// session as a background tab — arrives here through the two sinks TerminalPage.AgentEngine.cpp registers
// (session actions fanned out to every window, launches aimed at the most recently activated one).

#include "pch.h"
#include "TerminalPage.h"

#include "AgentCatchLog.h" // AgentLogCaughtException (Rule #18)
#include "AgentMaster/ClaudeSpawn.h" // LogNav, ShortId
#include "AgentMaster/SessionRegistry.h"

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
                // MESSAGING.md §21.17: the id the requester chose, when it chose one (the bot's new-thread picker binds the user's
                // thread to it), else a minted one.
                openedId = _SpawnClaudeSession(winrt::hstring{ request.dir }, winrt::hstring{ request.title }, static_cast<uint32_t>(-1), winrt::hstring{ request.model }, request.newSessionId);
                break;
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
                openedId = _ForkSessionFromDisk(request.sessionId, request.dir, request.title, request.model);
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
