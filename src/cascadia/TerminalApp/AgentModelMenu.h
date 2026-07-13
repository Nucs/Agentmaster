// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the ONE "Open New Session Here ▸ <model>" picker recipe (launch-model picker),
// shared by every surface that offers the action: the Manager's session menu (board card + tree
// row) and External row menu (AgentManagerContent.Tree.cpp), the Sessions page's row menu +
// detail-pane split button (TerminalPage.AgentSessionsPage.cpp), and the WT tab context menu
// (Tab.cpp) — the AgentCopyActions.h convergence pattern, so the five menus can never drift.
//
// The picker shape is: a "Default" item (launch EXACTLY as the plain item always did — the
// Settings model, or Claude's own default when that is blank) over one item per configured model
// from AppSettings.launchModels ("Display name | model-id", parsed by ParseLaunchModels). Picking
// a model launches that ONE session with `--model <id>` (a CLI flag outranks the shared settings
// file); nothing is persisted — a later resume/restart follows the settings model again.
//
// The models are USER-EDITABLE (the Settings cog → Sessions → "Launch models"), and the user
// asked the tooltips to say so — AgentModelEditHint() is that pointer, appended to every parent
// AND item tip here so no caller can forget it.
//
// The caller owns gating + deferral: `pick(modelId)` receives L"" for Default, else the model id,
// and each site wraps its own EnsureClaudeAvailable gate / dispatcher defer / background-open
// latch around it (they differ per surface — see the call sites).
//
// WinRT XAML types, so this lives in TerminalApp, NOT the plain-C++ AgentMaster/ engine dir.
// Include from .cpp TUs only (relies on the pch projections, like AgentTipHelpers.h).

#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "AgentTipHelpers.h" // AgentSetTip — the islands-safe hover tooltip (leak-free on rebuilt items)
#include "AgentMaster/ClaudeSpawn.h" // ParseLaunchModels (so a caller needs only this header)

namespace winrt::TerminalApp::implementation
{
    // "Where do I edit these" — the hint every picker tooltip carries (the Settings cog's
    // Sessions tab hosts the editable list right under the global Model box).
    inline winrt::hstring AgentModelEditHint()
    {
        return winrt::hstring{ L"Edit this list in the Manager's Settings (\x2699) \x2192 Sessions \x2192 Launch models." };
    }

    // Fill `items` — a MenuFlyoutSubItem's Items() or a MenuFlyout's Items(), same collection
    // type — with the picker:
    //     Default            → pick(L"")     (the plain "Open New Session Here" behavior)
    //     ─────────────
    //     <Display name>     → pick(<id>)    (one per configured model, top-to-bottom as typed)
    // With no models configured only "Default" is offered (no separator) — the submenu still
    // works, it just has nothing extra to pick. `pick` is copied into every item's Click.
    inline void AgentFillModelPickItems(const winrt::Windows::Foundation::Collections::IVector<winrt::Windows::UI::Xaml::Controls::MenuFlyoutItemBase>& items,
                                        const std::vector<std::pair<std::wstring, std::wstring>>& models,
                                        std::function<void(winrt::hstring)> pick)
    {
        namespace WUXC = winrt::Windows::UI::Xaml::Controls;

        WUXC::MenuFlyoutItem def;
        def.Text(L"Default");
        AgentSetTip(def, winrt::hstring{ L"Launch with the Settings model (blank there = Claude's own default) \x2014 exactly what a plain \x201COpen New Session Here\x201D always did. " } + AgentModelEditHint());
        def.Click([pick](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::RoutedEventArgs&) {
            pick(winrt::hstring{});
        });
        items.Append(def);

        if (models.empty())
        {
            return;
        }
        items.Append(WUXC::MenuFlyoutSeparator{});
        for (const auto& [name, id] : models)
        {
            WUXC::MenuFlyoutItem it;
            it.Text(winrt::hstring{ name });
            AgentSetTip(it, winrt::hstring{ L"Launch this session with --model " + id + L" (this launch only \x2014 resume follows the Settings model again). " } + AgentModelEditHint());
            it.Click([pick, mid = winrt::hstring{ id }](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::RoutedEventArgs&) {
                pick(mid);
            });
            items.Append(it);
        }
    }
}
