// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — the ONE hover-tooltip recipe for islands-hosted UI, shared by the Manager
// content (AgentManagerContent) and the full-window Archive + Sessions pages (which keep
// thin TU-local wrappers, SessSetTip/ArchiveSetTip, delegating here — the AgentStatusColors.h
// convergence pattern). ToolTipService's auto-dismiss bookkeeping is unreliable under XAML
// Islands (it keys on window-level pointer state the island input path doesn't deliver): a
// tip opened on hover routinely OUTLIVES the pointer leaving the element — and a boxed-string
// tip can't even be closed programmatically (GetToolTip returns the string, not a ToolTip).
// MinMaxCloseControl fights the same bug for the caption buttons (closeToolTipForButton);
// Tab::_UpdateToolTip is the upstream precedent for the explicit-ToolTip shape (its tab
// tooltips auto-open on hover, so the explicit object preserves open behavior).
//
// WinRT XAML types, so this lives in TerminalApp, NOT the plain-C++ AgentMaster/ engine
// directory. Include from .cpp TUs only (relies on the pch projections, like the rest of
// TerminalApp).

#pragma once

#include <chrono> // the fast-open timer interval
#include <memory> // shared_ptr holder for the lazily-created per-hover timer

namespace winrt::TerminalApp::implementation
{
    // Force-close el's tooltip if it is an explicit ToolTip (a boxed-string tip is
    // unreachable — which is WHY AgentSetTip never sets one). No-op when none / not open.
    inline void AgentCloseTipOn(const winrt::Windows::UI::Xaml::UIElement& el)
    {
        if (const auto tt = winrt::Windows::UI::Xaml::Controls::ToolTipService::GetToolTip(el))
        {
            if (const auto open = tt.try_as<winrt::Windows::UI::Xaml::Controls::ToolTip>())
            {
                open.IsOpen(false);
            }
        }
    }

    // Attach a hover tooltip wrapped in an explicit ToolTip object (Content = the text) and:
    //  - OPEN it FAST. The framework's built-in hover delay is sluggish, and this SDK exposes no
    //    ToolTipService.InitialShowDelay to shorten it, so we drive the open OURSELVES on a short
    //    one-shot DispatcherTimer — ~1/3 of the system tooltip hover time (SPI_GETMOUSEHOVERTIME,
    //    default 400ms => ~133ms) — exactly how WT's MinMaxCloseControl opens its caption-button
    //    tips: a manual ToolTip.IsOpen(true) with NO PlacementTarget (the target SetToolTip stored
    //    on the element handles placement — verified by that shipping control, whose XAML sets no
    //    PlacementTarget — so the tip holds no reference back to `el` and there is no leak-prone
    //    element<->tip cycle). The framework's own auto-open at the full delay then no-ops (already
    //    open). A boxed-string tip can't be opened programmatically — the OTHER reason this helper
    //    always wraps the text in an explicit ToolTip object.
    //  - force-close it from the element's own PointerExited (routes reliably through the island
    //    input HWND — the half of the service's bookkeeping that works) and its Unloaded (an
    //    element REMOVED from the tree while hovered — a board / table rebuild under a parked
    //    pointer — never gets the exit, and its open tip is a popup in the popup ROOT that nothing
    //    else would close); both also STOP the open timer.
    // Popup open/close is NOT a tree mutation — safe synchronously in a pointer handler (the
    // XAML-Islands defer rule is about visual-tree changes). No-op on an empty tip.
    inline void AgentSetTip(const winrt::Windows::UI::Xaml::UIElement& el, const winrt::hstring& tip)
    {
        if (tip.empty())
        {
            return;
        }
        winrt::Windows::UI::Xaml::Controls::ToolTip t;
        t.Content(winrt::box_value(tip));
        winrt::Windows::UI::Xaml::Controls::ToolTipService::SetToolTip(el, t);

        // The open delay = 1/3 of the system tooltip hover time (process-global; read once).
        static const auto openDelay = []() {
            unsigned int hoverMs{ 400 };
            if (!::SystemParametersInfoW(SPI_GETMOUSEHOVERTIME, 0, &hoverMs, 0) || hoverMs == 0)
            {
                hoverMs = 400;
            }
            return std::chrono::milliseconds{ hoverMs / 3 };
        }();

        // Lazily-created per-hover one-shot timer, kept in a shared_ptr holder so un-hovered
        // elements (e.g. every cell of a large table) never allocate one. The handlers capture the
        // holder + the tip by value but NOT `el`, and the tip carries no PlacementTarget back to
        // `el`, so there is no element<->handler reference cycle (which would otherwise leak the
        // element across a board / table rebuild).
        auto timer = std::make_shared<winrt::Windows::UI::Xaml::DispatcherTimer>(nullptr);
        el.PointerEntered([timer, t](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs&) {
            if (!*timer)
            {
                winrt::Windows::UI::Xaml::DispatcherTimer dt;
                dt.Interval(openDelay);
                dt.Tick([t](const winrt::Windows::Foundation::IInspectable& s, const winrt::Windows::Foundation::IInspectable&) {
                    if (const auto self = s.try_as<winrt::Windows::UI::Xaml::DispatcherTimer>())
                    {
                        self.Stop(); // one-shot: open once, then idle until the next hover
                    }
                    t.IsOpen(true);
                });
                *timer = dt;
            }
            (*timer).Start();
        });
        el.PointerExited([timer](const winrt::Windows::Foundation::IInspectable& s, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs&) {
            if (*timer)
            {
                (*timer).Stop();
            }
            if (const auto owner = s.try_as<winrt::Windows::UI::Xaml::UIElement>())
            {
                AgentCloseTipOn(owner);
            }
        });
        if (const auto fe = el.try_as<winrt::Windows::UI::Xaml::FrameworkElement>())
        {
            fe.Unloaded([timer](const winrt::Windows::Foundation::IInspectable& s, const winrt::Windows::UI::Xaml::RoutedEventArgs&) {
                if (*timer)
                {
                    (*timer).Stop();
                }
                if (const auto owner = s.try_as<winrt::Windows::UI::Xaml::UIElement>())
                {
                    AgentCloseTipOn(owner);
                }
            });
        }
    }

    // Force-close every AgentSetTip tooltip under root — for hosts about to be HIDDEN
    // (Visibility toggles do NOT unload, so the Unloaded close never fires for a collapsed
    // page — and a collapsed host does not hide a popup) or Clear()ed synchronously before
    // a same-tick rebuild. Walks Panel children / Border.Child / Popup.Child /
    // ContentControl.Content — the last because ScrollViewer IS a ContentControl and the
    // pages mount their rows/detail hosts inside ScrollViewers.
    inline void AgentCloseTipsIn(const winrt::Windows::UI::Xaml::UIElement& root)
    {
        AgentCloseTipOn(root);
        if (const auto panel = root.try_as<winrt::Windows::UI::Xaml::Controls::Panel>())
        {
            for (const auto& child : panel.Children())
            {
                AgentCloseTipsIn(child);
            }
        }
        else if (const auto border = root.try_as<winrt::Windows::UI::Xaml::Controls::Border>())
        {
            if (const auto child = border.Child())
            {
                AgentCloseTipsIn(child);
            }
        }
        else if (const auto popup = root.try_as<winrt::Windows::UI::Xaml::Controls::Primitives::Popup>())
        {
            if (const auto child = popup.Child())
            {
                AgentCloseTipsIn(child);
            }
        }
        else if (const auto content = root.try_as<winrt::Windows::UI::Xaml::Controls::ContentControl>())
        {
            if (const auto inner = content.Content())
            {
                if (const auto child = inner.try_as<winrt::Windows::UI::Xaml::UIElement>())
                {
                    AgentCloseTipsIn(child);
                }
            }
        }
    }
}
