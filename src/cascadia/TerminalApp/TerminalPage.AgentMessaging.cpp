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

    void TerminalPage::_RemoteLaunch(const ::Agentmaster::RemoteLaunchRequest& request)
    {
        try
        {
            using Kind = ::Agentmaster::RemoteLaunchRequest::Kind;
            const wchar_t* what = request.kind == Kind::Fork ? L"fork" : (request.kind == Kind::Resume ? L"resume" : L"new");
            ::Agentmaster::LogNav(std::wstring{ L"remote launch " } + what + L" " + ::Agentmaster::ShortId(request.sessionId) + L" dir=" + request.dir + (request.model.empty() ? std::wstring{} : L" model=" + request.model) +
                                  (request.newSessionId.empty() ? std::wstring{} : L" id=" + ::Agentmaster::ShortId(request.newSessionId)));
            if (request.kind != Kind::NewSession && request.dir.empty())
            {
                ::Agentmaster::LogNav(L"remote launch dropped: no folder for the session");
                return;
            }
            // The phone's launches never take focus: the Sessions page's bulk-open latch keeps the new tab in
            // the background and skips the page hide / activation.
            const bool previous = _openClaudeTabInBackground;
            _openClaudeTabInBackground = true;
            auto restore = wil::scope_exit([this, previous] { _openClaudeTabInBackground = previous; });
            switch (request.kind)
            {
            case Kind::NewSession:
                // MESSAGING.md §21.17: the id the requester chose, when it chose one (the bot's new-thread picker binds the user's
                // thread to it), else a minted one.
                _SpawnClaudeSession(winrt::hstring{ request.dir }, winrt::hstring{ request.title }, static_cast<uint32_t>(-1), winrt::hstring{ request.model }, request.newSessionId);
                break;
            case Kind::Resume:
                _ResumeSessionFromDisk(request.sessionId, request.dir, request.title);
                break;
            case Kind::Fork:
                _ForkSessionFromDisk(request.sessionId, request.dir, request.title, request.model);
                break;
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_RemoteLaunch");
        }
    }
}
