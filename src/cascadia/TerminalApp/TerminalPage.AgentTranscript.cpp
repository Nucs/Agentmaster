// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster TerminalPage implementation — the TRANSCRIPT OVERLAY's page side.
// Same class (TerminalPage, declared in TerminalPage.h), a same-class partial TU like the other
// TerminalPage.Agent*.cpp files (the upstream TabManagement.cpp pattern), so TerminalPage.cpp stays
// close to upstream.
// ======================================================================================
//
// The overlay itself (AgentTranscriptOverlay.{h,cpp}) is self-contained: it renders, lays out, keeps
// its own focus and closes itself on Esc / a click outside / its ✕. What only the PAGE can do lives
// here:
//   * build it once into this window's Root (_EnsureTranscriptOverlay) and give it the focus hand-back;
//   * open it from every menu — the WT tab menu (TerminalPage.cpp's ShowTranscriptRequested wiring),
//     the Agent Manager's session / external menus (_WireAgentManagerContent), the per-tab badge and
//     the summary panels (_AttachClaudeOverlay / the side panel) — resolving a managed session's
//     facts from the registry (_ShowTranscriptOverlay*);
//   * close it when a tab moves or the selection changes (_CloseTranscriptOverlay, called from the
//     tab-switch funnel, _TryMoveTab and the drag gesture) — the user's "moving a tab closes it".

#include "pch.h"
#include "TerminalPage.h"

#include "AgentTranscriptOverlay.h" // the overlay class (complete type for _transcriptOverlay)
#include "AgentCatchLog.h" // AgentLogCaughtException — Rule #18 forensics
#include "AgentMaster/ClaudeSpawn.h" // LogNav / ShortId
#include "AgentMaster/SessionRegistry.h" // the managed session's kind / rollout id / title

using namespace winrt::Windows::UI::Core; // CoreDispatcherPriority

namespace winrt::TerminalApp::implementation
{
    void TerminalPage::_EnsureTranscriptOverlay()
    {
        if (_transcriptOverlay)
        {
            return;
        }
        auto overlay = winrt::make_self<implementation::AgentTranscriptOverlay>();
        overlay->Attach(Root());
        // After the overlay closes ITSELF (Esc, a click outside, its ✕) — or the page closes it — the
        // keyboard goes back to what is on screen: the focused tab's terminal, or the Agent Manager page.
        // Weak: the overlay is a member of this page and must not keep it alive.
        overlay->SetClosedHandler([weak = get_weak()]() {
            if (const auto page = weak.get())
            {
                page->_FocusCurrentTab(true);
            }
        });
        _transcriptOverlay = overlay;
    }

    void TerminalPage::_ShowTranscriptOverlay(const std::wstring& sessionId, bool codex, const std::wstring& codexRolloutId, const std::wstring& title, std::wstring_view origin)
    {
        try
        {
            TranscriptOverlaySource request;
            request.sessionId = sessionId;
            request.codex = codex;
            request.codexRolloutId = codexRolloutId;
            request.title = title;
            // Claude only: the fork link, read HERE — every menu (tab, session, badge, side panel) funnels
            // through this function — so a fork with no messages yet shows the conversation it was forked
            // from instead of "nothing to show". An external row's id is unknown to the registry (or names
            // a record with no link), which leaves the field empty.
            if (!codex && _sessionRegistry)
            {
                if (const auto info = _sessionRegistry->GetWithoutQueue(sessionId))
                {
                    request.forkParentId = info->forkParentId;
                }
            }
            const std::wstring originText{ origin };
            // DEFERRED one tick: every caller is a menu item's Click, and a closing MenuFlyout hands focus
            // back to its target as it goes — that hand-back must land BEFORE the overlay takes focus, or
            // it would pull the keyboard straight back out of it (the tag editor's _OpenTagEditorAt recipe).
            Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), source = std::move(request), originText]() {
                // A throw escaping a dispatcher item is a crash: contain it (the overlay simply doesn't open).
                try
                {
                    const auto page = weak.get();
                    if (!page)
                    {
                        return;
                    }
                    page->_EnsureTranscriptOverlay();
                    ::Agentmaster::LogNav(L"transcript-show " + ::Agentmaster::ShortId(source.sessionId) +
                                          (source.codex ? L" codex" : L"") + L" (from " + originText + L")");
                    // At the level the user last picked, in any window (settings.json, read by the overlay
                    // as it opens) — Default until a first pick.
                    page->_transcriptOverlay->Show(source, AgentTranscriptOverlay::kRememberedLevel);
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"_ShowTranscriptOverlay open");
                }
            });
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ShowTranscriptOverlay");
        }
    }

    void TerminalPage::_ShowTranscriptOverlayForSession(const std::wstring& sessionId, std::wstring_view origin)
    {
        try
        {
            if (sessionId.empty())
            {
                return;
            }
            bool codex = false;
            std::wstring codexRolloutId;
            std::wstring title;
            if (_sessionRegistry)
            {
                if (const auto info = _sessionRegistry->Get(sessionId))
                {
                    codex = info->kind == ::Agentmaster::AgentKind::Codex;
                    codexRolloutId = info->codexSessionId;
                    title = info->title;
                }
            }
            _ShowTranscriptOverlay(sessionId, codex, codexRolloutId, title, origin);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ShowTranscriptOverlayForSession");
        }
    }

    void TerminalPage::_CloseTranscriptOverlay(std::wstring_view reason) noexcept
    {
        // restoreFocus=true even though the PAGE is closing it: while open, the overlay's focus guard
        // refuses focus moves to the page behind it — including the one the closing tab switch itself
        // just made (_UpdatedSelectedTab focuses the new tab before _ApplyAgentTabSelection runs us). The
        // hand-back (_FocusCurrentTab) gives that focus to the tab / the Manager page now. A no-op when
        // the overlay is closed, so the funnel's every-switch call costs nothing.
        if (_transcriptOverlay && _transcriptOverlay->IsOpen())
        {
            _transcriptOverlay->Close(reason, true);
        }
    }

    bool TerminalPage::_TranscriptOverlayIsOpen() const noexcept
    {
        return _transcriptOverlay && _transcriptOverlay->IsOpen();
    }
}
