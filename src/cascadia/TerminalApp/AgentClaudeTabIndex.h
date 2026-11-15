// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster (UI_THREAD.md §19): which managed session a window binds to each of its tabs, answered from ONE pass over
// the window's _claudeTabs (session id -> weak tab) instead of one pass per tab.
//
// The map is keyed by SESSION, so "which session does this tab host?" (TerminalPage::_ClaudeSessionForTab) walks every
// entry, resolves its weak reference and compares two COM objects. A loop over the window's tabs that asks it for each
// tab is quadratic in the tab count. On 2026-11-15 the production window of 745 tabs did that twice per ~2.5 s liveness
// sweep in the Fleet Observer's UI lane (_ObserverProbeImpl): 18 % of the UI thread's busy time, all of it in
// weak_ref::get, operator== and Release, and the main part of the 50-166 ms stalls that delayed typing in every window
// (they all share one UI thread). The window-record capture made the same scan for every tab after each tab switch.
//
// Build one AgentClaudeTabIndex at the start of such a pass and ask it per tab: n weak resolves and n identity reads to
// build, one identity read and one hash lookup per question.
//
// ⚠ A SNAPSHOT. A bind, a re-home, an archive or a close that changes _claudeTabs after the build is not in it, and
// the old answers stay. A pass that changes _claudeTabs itself (the observer's bind) must drop the index after the
// change and build a new one before its next question. Never keep one across a co_await, a dialog or a pumped message:
// it also keeps every bound tab alive while it exists (below).
//
// Header-only, UI thread only (it resolves weak references to XAML-owning tabs). Include it after TerminalPage.h, which
// brings the winrt TerminalApp projection it names (winrt::TerminalApp::Tab); the pch brings the rest.
#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class AgentClaudeTabIndex
{
public:
    // The shape of TerminalPage::_claudeTabs: session id -> the tab hosting that session in this window.
    using BoundTabs = std::unordered_map<std::wstring, winrt::weak_ref<winrt::TerminalApp::Tab>>;

    // Snapshots `boundTabs` now.
    // When one tab is listed under two session ids (never meant to happen, but the map does not forbid it), the entry
    // the map ITERATES first wins: exactly the answer the linear scan in TerminalPage::_ClaudeSessionForTab gives, so
    // replacing that scan with this index changes no result. An entry whose tab is gone (its weak reference lapsed) is
    // left out, as the scan skips it.
    // Parameters:
    //   boundTabs - the window's _claudeTabs. Only read; nothing of it is referenced after the constructor returns.
    // Exceptions: std::bad_alloc on allocation failure. Resolving a weak reference and asking a live tab for its
    //   IUnknown do not throw (try_as answers null; such an entry is left out).
    explicit AgentClaudeTabIndex(const BoundTabs& boundTabs)
    {
        _sessionByTab.reserve(boundTabs.size());
        _alive.reserve(boundTabs.size());
        for (const auto& [sessionId, weakTab] : boundTabs)
        {
            const auto tab = weakTab.get();
            if (!tab)
            {
                continue; // the tab is gone (its close path owns the entry); the scan skips it too
            }
            auto identity = _IdentityOf(tab);
            if (!identity)
            {
                continue; // no COM identity to key on; never seen for a Tab (C++/WinRT objects always answer IUnknown)
            }
            // emplace keeps the FIRST session of a tab in iteration order: the linear scan's first match.
            if (_sessionByTab.emplace(winrt::get_abi(identity), sessionId).second)
            {
                // The key is a raw identity pointer. Holding a reference keeps that object, and so its address, alive
                // for as long as the index exists, so the address can never be reused by another object meanwhile
                // and answer for a tab that is not bound.
                _alive.push_back(std::move(identity));
            }
        }
    }

    // The session this window binds to `tab`, or nullptr when it binds none (a shell tab, the Settings tab, a tab whose
    // session is archived, or a tab bound after this index was built).
    // Parameters:
    //   tab - any tab; null answers nullptr.
    // Returns: a pointer into the index, valid while the index lives and is not reassigned. Copy the string to keep it.
    // Exceptions: none in practice. try_as never throws; a std::unordered_map lookup on a pointer key does not
    //   allocate.
    const std::wstring* Find(const winrt::TerminalApp::Tab& tab) const
    {
        if (!tab || _sessionByTab.empty())
        {
            return nullptr;
        }
        const auto identity = _IdentityOf(tab);
        if (!identity)
        {
            return nullptr;
        }
        const auto it = _sessionByTab.find(winrt::get_abi(identity));
        return it == _sessionByTab.end() ? nullptr : &it->second;
    }

    // The session this window binds to `tab` as a copy: the drop-in answer of TerminalPage::_ClaudeSessionForTab.
    // Parameters:
    //   tab - any tab; null answers "".
    // Returns: the session id, or "" when the tab hosts no managed session.
    // Exceptions: std::bad_alloc when the copy cannot be allocated.
    std::wstring SessionFor(const winrt::TerminalApp::Tab& tab) const
    {
        const auto* const sessionId = Find(tab);
        return sessionId ? *sessionId : std::wstring{};
    }

    // How many tabs the index binds (each tab once, whatever the duplicates in the map).
    // Returns: the count. Never throws.
    size_t Size() const noexcept
    {
        return _sessionByTab.size();
    }

private:
    // The COM identity of a tab: its IUnknown, the one interface every projection of one object answers with the same
    // pointer. winrt's operator== on two projections falls back to exactly this comparison, so keying on it gives
    // the scan's answer.
    // Parameters:
    //   tab - a non-null tab.
    // Returns: the identity, holding one reference; null when the object refuses IUnknown (never for a Tab).
    // Exceptions: none (try_as answers null instead of throwing).
    static winrt::Windows::Foundation::IUnknown _IdentityOf(const winrt::TerminalApp::Tab& tab) noexcept
    {
        return tab.try_as<winrt::Windows::Foundation::IUnknown>();
    }

    std::unordered_map<void*, std::wstring> _sessionByTab; // the tab's identity pointer -> the session bound to it
    std::vector<winrt::Windows::Foundation::IUnknown> _alive; // keeps each keyed identity's object (and address) alive
};
