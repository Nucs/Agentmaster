// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the ONE "<launch a Claude session> ▸ <model>" picker recipe (launch-model
// picker), shared by every surface that offers a session-CREATING action: "Open New Session
// Here" AND "Fork session"/"Fork here" — the Manager's session menu (board card + tree row) and
// External row menu (AgentManagerContent.Tree.cpp), the Sessions page's row menu + detail-pane
// split buttons (TerminalPage.AgentSessionsPage.cpp), and the WT tab context menu (Tab.cpp) —
// the AgentCopyActions.h convergence pattern, so the many menus can never drift. (A fork IS a
// launch variant — `claude --resume <src> --fork-session …` — so `--model <id>` applies to it
// exactly like to a fresh spawn: the forked session starts on the picked model.)
//
// The picker shape is: a "Default" item (act EXACTLY as the plain item always did — the
// Settings model, or Claude's own default when that is blank) over one item per configured model
// from AppSettings.launchModels ("Display name | model-id", parsed by ParseLaunchModels). Picking
// a model starts that ONE session with `--model <id>` (a CLI flag outranks the shared settings
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

    // How a surface opens the "Specify a model..." prompt: it is handed the SAME `pick` callback
    // the menu items use and calls it with the typed id once the user commits ("" / never calling
    // it == cancelled, in which case nothing launches). It is a caller-supplied functor because
    // the prompt must live in a real visual tree — a text box inside a ContentDialog gets no
    // keypresses under XAML Islands (Gotchas) — so each host shows the shared card
    // (AgentBuildSpecifyModelCard, AgentModelPrompt.h) in ITS own modal layer. Passing nothing
    // omits the item entirely, so an un-wired caller still compiles and behaves as before.
    using AgentSpecifyModelOpener = std::function<void(std::function<void(winrt::hstring)>)>;

    // Fill `items` — a MenuFlyoutSubItem's Items() or a MenuFlyout's Items(), same collection
    // type — with the picker:
    //     Default            → pick(L"")     (the plain "Open New Session Here" behavior)
    //     Specify…           → prompt, then pick(<typed id>)   (only when `specify` is supplied)
    //     ─────────────
    //     <Display name>     → pick(<id>)    (one per configured model, top-to-bottom as typed)
    // With no models configured only Default (+ Specify…) is offered (no separator) — the submenu
    // still works, it just has nothing extra to pick. `pick` is copied into every item's Click.

    inline void AgentFillModelPickItems(const winrt::Windows::Foundation::Collections::IVector<winrt::Windows::UI::Xaml::Controls::MenuFlyoutItemBase>& items,
                                        const std::vector<std::pair<std::wstring, std::wstring>>& models,
                                        std::function<void(winrt::hstring)> pick,
                                        AgentSpecifyModelOpener specify = nullptr)
    {
        namespace WUXC = winrt::Windows::UI::Xaml::Controls;

        WUXC::MenuFlyoutItem def;
        def.Text(L"Default");
        AgentSetTip(def, winrt::hstring{ L"Use the Settings model (blank there = Claude's own default) \x2014 exactly what the plain action always did. " } + AgentModelEditHint());
        def.Click([pick](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::RoutedEventArgs&) {
            pick(winrt::hstring{});
        });
        items.Append(def);

        // "Specify..." sits directly BELOW Default (before the configured list): the two of them are
        // the "not from the list" choices, and a user reaching for a one-off model should not have to
        // scan past N configured entries to find it.
        if (specify)
        {
            WUXC::MenuFlyoutItem spec;
            spec.Text(L"Specify\x2026");
            AgentSetTip(spec, winrt::hstring{ L"Type any model id for this ONE session \x2014 e.g. a model newer than your list, or a pinned version like claude-opus-4-8. The prompt remembers what you type and links the published model lists. " } + AgentModelEditHint());
            spec.Click([pick, specify](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::RoutedEventArgs&) {
                // The opener is responsible for deferring past this flyout's close (its refocus must
                // not fight the prompt's text box) — the same discipline the tag editor's open uses.
                specify(pick);
            });
            items.Append(spec);
        }

        if (models.empty())
        {
            return;
        }
        items.Append(WUXC::MenuFlyoutSeparator{});
        for (const auto& [name, id] : models)
        {
            WUXC::MenuFlyoutItem it;
            it.Text(winrt::hstring{ name });
            AgentSetTip(it, winrt::hstring{ L"Start this session on --model " + id + L" (this launch only \x2014 a later resume follows the Settings model again). " } + AgentModelEditHint());
            it.Click([pick, mid = winrt::hstring{ id }](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::RoutedEventArgs&) {
                pick(mid);
            });
            items.Append(it);
        }
    }
}
