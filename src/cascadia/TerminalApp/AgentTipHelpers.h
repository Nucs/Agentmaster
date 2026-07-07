// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the ONE hover-tooltip recipe for islands-hosted UI, shared by the Manager
// content (AgentManagerContent), the full-window Sessions page, the per-tab overlay, and the
// tab-strip (thin TU-local wrappers, SessSetTip/etc., delegate here — the AgentStatusColors.h
// convergence pattern). ToolTipService's auto-dismiss bookkeeping is unreliable under XAML
// Islands (it keys on window-level pointer state the island input path doesn't deliver): a
// tip opened on hover routinely OUTLIVES the pointer leaving the element — and a boxed-string
// tip can't even be closed programmatically (GetToolTip returns the string, not a ToolTip).
//
// ⚠ ARCHITECTURE — ONE SHARED TIP PER UI THREAD, ZERO leak-prone state per element.
// The previous revision of this helper allocated, PER CALL: a ToolTip control (+ boxed string)
// + FOUR capturing event delegates + a shared_ptr'd DispatcherTimer holder, and registered the
// ToolTip with ToolTipService AT BUILD TIME (SetToolTip). Under XAML Islands the framework-side
// bookkeeping that SetToolTip creates is never torn down (the same broken half of ToolTipService
// this helper exists to work around), so every tipped element was PEGGED — and the Manager
// surfaces RECREATE their tipped elements on every registry-notify rebuild (~100 tips/refresh,
// two windows). Measured in production 2026-07-06: ~7.5 MILLION live tip clusters ≈ 68 GB of
// committed heap after 36 h (heap census: the two AgentSetTip handler-delegate vtables + the
// make_shared control block at ~1:1:1, plus ~1.5 billion Windows.UI.Xaml object refs), which
// exhausted system commit; XAML's composition pipeline took fatal allocation failures and
// latched dead — a window that pumps messages but never repaints ("frozen but not hung").
//
// The recipe now inverts the ownership. Cost PER ELEMENT is only:
//   - two custom attached DependencyProperties (tip text + optional open delay) — plain sparse
//     property-store values that die with the element; NO service, NO registration;
//   - three CAPTURE-LESS pointer handlers (Entered/Moved/Exited) that route to the shared host.
// Everything stateful lives in ONE per-UI-thread host: one ToolTip control, one one-shot open
// timer, one 1 s watchdog (armed only while a tip is open), and two weak element refs. The
// framework's ToolTipService is touched ONLY for the duration of a real, user-driven hover-open
// (SetToolTip(owner, tip) right before IsOpen(true); SetToolTip(owner, nullptr) on close), so
// its per-registration bookkeeping is bounded by actual hovers — not by rebuild churn.
//
// Behavior preserved from the old recipe:
//  - OPEN FAST: ~1/3 of the system tooltip hover time (SPI_GETMOUSEHOVERTIME, default 400 ms
//    ⇒ ~133 ms), per-call overridable (the dense Triage-Board cards hold back 4 s).
//  - Open only after the pointer comes to REST: while the tip is still waiting to open, a move
//    RE-ARMS the one-shot; once OPEN, moves are ignored (the flicker fix — opening a popup fires
//    a synthetic PointerMoved that would otherwise close+reopen it forever).
//  - CLICK-THROUGH: the tip is IsHitTestVisible(false), so it never eats the click it pops
//    up under.
//  - Always DARK (the tip renders in the popup root and does not inherit the host's forced
//    dark theme).
//  - Never crash over a tip: opening is gated on the owner still being loaded, wrapped in a
//    try/catch (an owner-less ToolTip open is a stowed 0xC000027B fail-fast in
//    Windows.UI.Xaml.dll), and a watchdog closes a tip whose owner was rebuilt away under a
//    parked pointer (the per-element Unloaded hook this replaces).
//
// WinRT XAML types, so this lives in TerminalApp, NOT the plain-C++ AgentMaster/ engine
// directory. Include from .cpp TUs only (relies on the pch projections, like the rest of
// TerminalApp).

#pragma once

#include <chrono> // the fast-open timer interval
#include <optional> // the optional per-call open-delay override

// The pch pulls the Xaml SUB-namespaces (Controls/Input/Media/…) but not these two, which the
// attached-property registration needs: the base namespace header defines PropertyMetadata's
// constructor + DependencyProperty's statics, Interop defines TypeName/xaml_typename.
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

namespace winrt::TerminalApp::implementation
{
    namespace agent_tip_details
    {
        // The two attached properties carrying the per-element tip state. Registered ONCE per
        // process (thread-safe function-local static); the (FrameworkElement, "Agentmaster*")
        // names cannot collide with anything the framework registers. Plain property-store
        // values: setting them creates NO framework-side registration and they are released
        // with the element.
        inline winrt::Windows::UI::Xaml::DependencyProperty TipTextProperty()
        {
            static const auto prop = winrt::Windows::UI::Xaml::DependencyProperty::RegisterAttached(
                L"AgentmasterTipText",
                winrt::xaml_typename<winrt::hstring>(),
                winrt::xaml_typename<winrt::Windows::UI::Xaml::FrameworkElement>(),
                winrt::Windows::UI::Xaml::PropertyMetadata{ winrt::box_value(winrt::hstring{}) });
            return prop;
        }

        inline winrt::Windows::UI::Xaml::DependencyProperty TipDelayMsProperty()
        {
            static const auto prop = winrt::Windows::UI::Xaml::DependencyProperty::RegisterAttached(
                L"AgentmasterTipDelayMs",
                winrt::xaml_typename<int32_t>(),
                winrt::xaml_typename<winrt::Windows::UI::Xaml::FrameworkElement>(),
                winrt::Windows::UI::Xaml::PropertyMetadata{ winrt::box_value(static_cast<int32_t>(0)) });
            return prop;
        }

        // The open delay DEFAULTS to 1/3 of the system tooltip hover time (process-global; read
        // once); a caller may OVERRIDE it per element via the delay attached property.
        inline std::chrono::milliseconds DefaultOpenDelay()
        {
            static const auto delay = []() {
                unsigned int hoverMs{ 400 };
                if (!::SystemParametersInfoW(SPI_GETMOUSEHOVERTIME, 0, &hoverMs, 0) || hoverMs == 0)
                {
                    hoverMs = 400;
                }
                return std::chrono::milliseconds{ hoverMs / 3 };
            }();
            return delay;
        }

        // The per-UI-thread shared tip host — the ONLY stateful part of the recipe. thread_local
        // (not process-global): every window's XAML tree lives on a UI thread, and pointer events,
        // DispatcherTimers, and the ToolTip itself are all thread-affine.
        struct TipHost
        {
            winrt::Windows::UI::Xaml::Controls::ToolTip tip{ nullptr }; // created lazily, reused forever
            winrt::Windows::UI::Xaml::DispatcherTimer openTimer{ nullptr }; // one-shot hover-rest timer
            winrt::Windows::UI::Xaml::DispatcherTimer watchdog{ nullptr }; // runs ONLY while a tip is open
            winrt::weak_ref<winrt::Windows::UI::Xaml::UIElement> armed{ nullptr }; // element awaiting open
            winrt::weak_ref<winrt::Windows::UI::Xaml::UIElement> openOwner{ nullptr }; // element whose tip is open
        };

        inline TipHost& Host()
        {
            static thread_local TipHost host;
            return host;
        }

        // Close the currently-open shared tip (if any) and detach it from its owner. The service
        // attachment is removed as soon as the hover ends, so ToolTipService's islands-broken
        // per-registration bookkeeping never accumulates beyond the one live hover.
        inline void CloseOpenTip()
        {
            auto& h = Host();
            if (h.watchdog)
            {
                h.watchdog.Stop();
            }
            if (h.tip)
            {
                try
                {
                    h.tip.IsOpen(false);
                }
                catch (...)
                {
                }
            }
            if (const auto owner = h.openOwner.get())
            {
                winrt::Windows::UI::Xaml::Controls::ToolTipService::SetToolTip(owner, nullptr);
            }
            h.openOwner = nullptr;
        }

        // The one-shot open-timer tick: open the shared tip on the armed element, if it is still
        // alive, still in the tree, and still carries a tip text.
        inline void OnOpenTimerTick()
        {
            auto& h = Host();
            if (h.openTimer)
            {
                h.openTimer.Stop(); // one-shot: open once, then idle until the next hover
            }
            const auto el = h.armed.get();
            if (!el)
            {
                return;
            }
            // An element detached by a board / table rebuild while the one-shot was pending must
            // NOT be opened on: an owner-less ToolTip open is a stowed fail-fast (0xC000027B) in
            // Windows.UI.Xaml.dll. IsLoaded is the authoritative "still in the live tree" test.
            if (const auto fe = el.try_as<winrt::Windows::UI::Xaml::FrameworkElement>())
            {
                if (!fe.IsLoaded())
                {
                    return;
                }
            }
            const auto text = winrt::unbox_value_or<winrt::hstring>(el.GetValue(TipTextProperty()), winrt::hstring{});
            if (text.empty())
            {
                return;
            }
            CloseOpenTip(); // a tip open on another element (rebuilt-away owner, fast hover shift) yields first
            if (!h.tip)
            {
                winrt::Windows::UI::Xaml::Controls::ToolTip t;
                // CLICK-THROUGH: a ToolTip renders in a Popup whose content is hit-testable by
                // default, and we open FAST — so the tip routinely pops up right under where the
                // user is about to click and would EAT the click. A tooltip is purely informational
                // and never needs pointer input: hit-test-invisible makes the whole popup
                // transparent to input, and the click falls THROUGH to the element below.
                t.IsHitTestVisible(false);
                // Every surface that uses AgentSetTip is forced dark, but a ToolTip renders in the
                // popup ROOT — it does NOT inherit the host's RequestedTheme, and ToolTipService
                // theme propagation is unreliable under XAML Islands. Pin it Dark directly.
                t.RequestedTheme(winrt::Windows::UI::Xaml::ElementTheme::Dark);
                h.tip = t;
            }
            h.tip.Content(winrt::box_value(text));
            // Attach to the owner ONLY for the duration of the open — SetToolTip is what gives the
            // tip its placement (relative to the owner; the shipping MinMaxCloseControl pattern:
            // no PlacementTarget, so the tip holds no reference back to the element).
            winrt::Windows::UI::Xaml::Controls::ToolTipService::SetToolTip(el, h.tip);
            try
            {
                h.tip.IsOpen(true);
            }
            catch (...)
            {
                // A tooltip that can't open is moot — never crash over a tip. Detach again so the
                // failed open leaves no service registration behind.
                winrt::Windows::UI::Xaml::Controls::ToolTipService::SetToolTip(el, nullptr);
                return;
            }
            h.openOwner = el;
            // Watchdog: an element REBUILT AWAY under a parked pointer never fires PointerExited,
            // and this recipe deliberately hooks no per-element Unloaded — so a 1 s liveness check
            // (running ONLY while a tip is open) closes the tip once its owner leaves the tree.
            if (!h.watchdog)
            {
                winrt::Windows::UI::Xaml::DispatcherTimer wd;
                wd.Interval(std::chrono::milliseconds{ 1000 });
                wd.Tick([](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::Foundation::IInspectable&) {
                    const auto owner = Host().openOwner.get();
                    bool ownerGone = !owner;
                    if (!ownerGone)
                    {
                        if (const auto fe = owner.try_as<winrt::Windows::UI::Xaml::FrameworkElement>())
                        {
                            ownerGone = !fe.IsLoaded();
                        }
                    }
                    if (ownerGone)
                    {
                        CloseOpenTip();
                    }
                });
                h.watchdog = wd;
            }
            h.watchdog.Start();
        }

        // Arm (or re-arm) the one-shot open timer for `el`, using its per-element delay override
        // when set (the board cards' 4 s hold-back) else the fast process default.
        inline void ArmOpenTimer(const winrt::Windows::UI::Xaml::UIElement& el)
        {
            auto& h = Host();
            h.armed = el;
            if (!h.openTimer)
            {
                winrt::Windows::UI::Xaml::DispatcherTimer dt;
                dt.Tick([](const winrt::Windows::Foundation::IInspectable&, const winrt::Windows::Foundation::IInspectable&) {
                    OnOpenTimerTick();
                });
                h.openTimer = dt;
            }
            const auto overrideMs = winrt::unbox_value_or<int32_t>(el.GetValue(TipDelayMsProperty()), 0);
            const auto delay = overrideMs > 0 ? std::chrono::milliseconds{ overrideMs } : DefaultOpenDelay();
            h.openTimer.Stop();
            h.openTimer.Interval(delay);
            h.openTimer.Start();
        }

        // The three capture-less per-element handlers. They allocate nothing beyond the delegate
        // wrapper itself and hold no state — all routing resolves through the thread-local host.
        inline void OnTipPointerEntered(const winrt::Windows::Foundation::IInspectable& sender)
        {
            const auto el = sender.try_as<winrt::Windows::UI::Xaml::UIElement>();
            if (!el)
            {
                return;
            }
            if (winrt::unbox_value_or<winrt::hstring>(el.GetValue(TipTextProperty()), winrt::hstring{}).empty())
            {
                return;
            }
            ArmOpenTimer(el);
        }

        // Open the tip only after the pointer comes to REST — without flickering once it is up.
        // While the tip is still WAITING to open, a move means "not at rest yet", so RE-ARM the
        // one-shot. Once the tip is OPEN, do NOTHING on move: opening a mouse-relative popup fires
        // a SYNTHETIC PointerMoved (the input system re-hit-tests when the popup appears), and a
        // close-on-move here would re-arm → reopen → another synthetic move → a self-sustaining
        // flicker that runs even with the mouse perfectly still. Clicks already fall through the
        // open tip via IsHitTestVisible(false), so a lingering tip never swallows a click.
        inline void OnTipPointerMoved(const winrt::Windows::Foundation::IInspectable& sender)
        {
            auto& h = Host();
            if (h.tip && h.tip.IsOpen())
            {
                return;
            }
            const auto el = sender.try_as<winrt::Windows::UI::Xaml::UIElement>();
            if (!el)
            {
                return;
            }
            if (h.armed.get() == el)
            {
                ArmOpenTimer(el); // restart: the rest-detection re-arm
                return;
            }
            // NESTED tips (a tipped row containing tipped glyphs): exiting the inner element
            // cleared the arm, but the pointer never left the OUTER one — so its Entered will not
            // fire again. PointerMoved bubbles, so the outer element sees moves while the pointer
            // is inside it: re-arm it here when nothing is armed. While the INNER element is armed
            // the outer's bubbled moves land in the branch above (armed != el) and change nothing —
            // the innermost tip always wins.
            if (!h.armed.get() &&
                !winrt::unbox_value_or<winrt::hstring>(el.GetValue(TipTextProperty()), winrt::hstring{}).empty())
            {
                ArmOpenTimer(el);
            }
        }

        inline void OnTipPointerExited(const winrt::Windows::Foundation::IInspectable& sender)
        {
            auto& h = Host();
            const auto el = sender.try_as<winrt::Windows::UI::Xaml::UIElement>();
            if (!el)
            {
                return;
            }
            if (h.armed.get() == el)
            {
                if (h.openTimer)
                {
                    h.openTimer.Stop();
                }
                h.armed = nullptr;
            }
            if (h.openOwner.get() == el)
            {
                CloseOpenTip();
            }
        }
    }

    // Force-close el's tooltip. Covers both the shared host tip (when el is its current owner)
    // and any legacy explicit ToolTip attached to el. No-op when none / not open.
    inline void AgentCloseTipOn(const winrt::Windows::UI::Xaml::UIElement& el)
    {
        {
            auto& h = agent_tip_details::Host();
            if (h.openOwner.get() == el)
            {
                agent_tip_details::CloseOpenTip();
                return;
            }
        }
        if (const auto tt = winrt::Windows::UI::Xaml::Controls::ToolTipService::GetToolTip(el))
        {
            if (const auto open = tt.try_as<winrt::Windows::UI::Xaml::Controls::ToolTip>())
            {
                open.IsOpen(false);
            }
        }
    }

    // Attach a hover tooltip to `el`: record the text (+ optional open-delay override) on the
    // element and wire it to the shared per-thread tip host. Re-calling on the same element just
    // UPDATES the recorded text/delay (no second handler set — the host reads the properties
    // fresh at open time, so the newest text always shows). No-op on an empty tip.
    inline void AgentSetTip(const winrt::Windows::UI::Xaml::UIElement& el,
                            const winrt::hstring& tip,
                            std::optional<std::chrono::milliseconds> openDelayOverride = std::nullopt)
    {
        namespace atd = agent_tip_details;
        if (tip.empty())
        {
            return;
        }
        // "Already wired" == the text property has a local value from a prior call. The three
        // pointer handlers are hooked exactly once per element, on the first call.
        const bool alreadyWired =
            el.ReadLocalValue(atd::TipTextProperty()) != winrt::Windows::UI::Xaml::DependencyProperty::UnsetValue();
        el.SetValue(atd::TipTextProperty(), winrt::box_value(tip));
        if (openDelayOverride)
        {
            el.SetValue(atd::TipDelayMsProperty(), winrt::box_value(static_cast<int32_t>(openDelayOverride->count())));
        }
        if (alreadyWired)
        {
            return;
        }
        el.PointerEntered([](const winrt::Windows::Foundation::IInspectable& s, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs&) {
            atd::OnTipPointerEntered(s);
        });
        el.PointerMoved([](const winrt::Windows::Foundation::IInspectable& s, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs&) {
            atd::OnTipPointerMoved(s);
        });
        el.PointerExited([](const winrt::Windows::Foundation::IInspectable& s, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs&) {
            atd::OnTipPointerExited(s);
        });
    }

    // Force-close every AgentSetTip tooltip under root — for hosts about to be HIDDEN
    // (Visibility toggles do NOT unload, so no Unloaded/watchdog close fires for a collapsed
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
