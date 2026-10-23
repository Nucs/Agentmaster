// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the tab strip's status icon, built in code (see AgentStatusGlyph.h for what it is, what it leaves out,
// and its ownership rules). Every size, order and static opacity below is the XAML's (TabHeaderControl.xaml,
// HeaderAgentStatusDotWrap); each constant names its twin, so a change on either side can be carried to the other.

#include "pch.h"
#include "AgentStatusGlyph.h"

#include "AgentCatchLog.h" // AgentLogCaughtException — a refused motion start / stop is logged, never thrown (Rule #18)
#include "AgentStatusColors.h" // AgentAnimationsEnabled — Windows' "Animation effects" switch, read at every motion decision
#include "AgentMaster/StatusIconMotion.h" // DecideStatusIconMotion — the tab header's motion rule, shared

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace
{
    // [HeaderAgentStatusDotWrap Width="18"] — wide enough for the 16 DIP glow and the 18 DIP sparks canvas.
    constexpr double kWrapWidth = 18.0;
    // [the wrap's RowDefinition Height="18"] — the glyph cell every indicator except the dots is centered in.
    constexpr double kGlyphCellHeight = 18.0;
    // [the wrap's RowDefinition Height="4"] — the band reserved for the 3 dots, FIXED so a draft appearing or clearing
    // never moves the dot (the tab's "the status dot stays STATIONARY" rule).
    constexpr double kPendingBandHeight = 4.0;
    // [HeaderAgentStatusDot Width/Height="10" StrokeThickness="1" Stroke="Black"]
    constexpr double kDotDiameter = 10.0;
    constexpr double kDotStroke = 1.0;
    // [HeaderAgentStatusFlashRing Width/Height="13"] — 1.5 DIP of ring around the 10 DIP dot.
    constexpr double kRingDiameter = 13.0;
    // [HeaderCacheWarmGlow's two Ellipses] — the outer disc 16 DIP at 0.5, the inner 12.5 DIP at 0.55: two flat discs
    // fake a falloff (no RadialGradientBrush anywhere in this codebase), and the grid's own Opacity breathes both.
    constexpr double kGlowOuterDiameter = 16.0;
    constexpr double kGlowOuterOpacity = 0.5;
    constexpr double kGlowInnerDiameter = 12.5;
    constexpr double kGlowInnerOpacity = 0.55;
    // [HeaderCacheWarmSparks Width/Height="18"] — AgentEmberSparks sizes its container to this host (kEmberHostSide).
    constexpr double kSparksSide = 18.0;
    // [HeaderPendingDots: Spacing="1.5", Ellipses Width/Height="2.5"] — ~10.5 DIP across, centered in the band.
    constexpr double kPendingDotDiameter = 2.5;
    constexpr double kPendingDotSpacing = 1.5;
}

namespace winrt::TerminalApp::implementation
{
    std::shared_ptr<AgentStatusGlyph> AgentStatusGlyph::Create()
    {
        // `new` instead of std::make_shared: the constructor is private (the glyph must exist inside a shared_ptr
        // before _Build can hand its handlers a weak_ptr), and make_shared cannot reach a private constructor.
        std::shared_ptr<AgentStatusGlyph> glyph(new AgentStatusGlyph());
        glyph->_Build();
        return glyph;
    }

    Grid AgentStatusGlyph::Root() const noexcept
    {
        return _root;
    }

    void AgentStatusGlyph::_Build()
    {
        // Everything is built into locals first and moved into the members at the end: a refusal part-way throws out
        // of Create, and the caller never sees a glyph whose members are half set.
        Grid root{};
        root.Width(kWrapWidth);
        // Decorative as a whole: the host's own element takes the pointer (a click on the dot selects the card), and no
        // part of the icon carries a tooltip of its own (see the header).
        root.IsHitTestVisible(false);
        {
            RowDefinition glyphRow{};
            glyphRow.Height(GridLengthHelper::FromPixels(kGlyphCellHeight));
            RowDefinition bandRow{};
            bandRow.Height(GridLengthHelper::FromPixels(kPendingBandHeight));
            root.RowDefinitions().Append(glyphRow);
            root.RowDefinitions().Append(bandRow);
        }

        Media::SolidColorBrush stateBrush{ winrt::Windows::UI::Colors::Transparent() }; // painted by the host's first SetStateColor
        Media::SolidColorBrush warmBrush{ winrt::Windows::UI::Colors::Transparent() }; // painted before the crown first shows
        Media::SolidColorBrush pendingBrush{ winrt::Windows::UI::Colors::Transparent() }; // painted before the dots first show

        // 1. The GLOW, drawn FIRST so it sits behind every other glyph: the red ring is the louder "needs you" alert and
        //    must never be tinted or hidden by a cosmetic cache hint. Its static Opacity is the breath's still value —
        //    the loop begins and ends there, and XAML writes the last key frame back on a stop.
        Grid glow{};
        glow.HorizontalAlignment(HorizontalAlignment::Center);
        glow.VerticalAlignment(VerticalAlignment::Center);
        glow.IsHitTestVisible(false);
        glow.Opacity(kAgentCacheWarmGlowStill);
        glow.Visibility(Visibility::Collapsed);
        {
            Shapes::Ellipse outer{};
            outer.Width(kGlowOuterDiameter);
            outer.Height(kGlowOuterDiameter);
            outer.Opacity(kGlowOuterOpacity);
            outer.Fill(warmBrush);
            Shapes::Ellipse inner{};
            inner.Width(kGlowInnerDiameter);
            inner.Height(kGlowInnerDiameter);
            inner.Opacity(kGlowInnerOpacity);
            inner.Fill(warmBrush);
            glow.Children().Append(outer);
            glow.Children().Append(inner);
        }
        root.Children().Append(glow);

        // 2. The FLASH RING, behind the dot: only its outer ~1.5 DIP shows, outside the dot's black stroke (read outside-in:
        //    ring, black stroke, state fill). No Fill until the first SetFlashRing hands the window's shared brush, and
        //    deliberately NO Opacity write anywhere (the blink is an expression on it — see SetFlashRing's warning).
        Shapes::Ellipse ring{};
        ring.Width(kRingDiameter);
        ring.Height(kRingDiameter);
        ring.HorizontalAlignment(HorizontalAlignment::Center);
        ring.VerticalAlignment(VerticalAlignment::Center);
        ring.IsHitTestVisible(false);
        ring.Visibility(Visibility::Collapsed);
        root.Children().Append(ring);

        // 3. The STATE DOT, on top of the glow and the ring. The black stroke keeps it legible on any background and is
        //    what separates it from the ring.
        Shapes::Ellipse dot{};
        dot.Width(kDotDiameter);
        dot.Height(kDotDiameter);
        dot.HorizontalAlignment(HorizontalAlignment::Center);
        dot.VerticalAlignment(VerticalAlignment::Center);
        dot.Fill(stateBrush);
        dot.Stroke(Media::SolidColorBrush{ winrt::Windows::UI::Colors::Black() });
        dot.StrokeThickness(kDotStroke);
        root.Children().Append(dot);

        // 4. The SPARKS host, drawn LAST in the cell so the embers rise over the dot. Top-aligned and the cell's full
        //    size: AgentEmberSparks places its three visuals at the XAML ellipses' Canvas spots and bounds the rise to
        //    stay inside the 18 DIP cell. Collapsed with the crown, which hides the embers with it. Nothing else may
        //    call SetElementChildVisual on it (one child visual per element — a second call would replace the embers).
        Canvas sparks{};
        sparks.Width(kSparksSide);
        sparks.Height(kSparksSide);
        sparks.HorizontalAlignment(HorizontalAlignment::Center);
        sparks.VerticalAlignment(VerticalAlignment::Top);
        sparks.IsHitTestVisible(false);
        sparks.Visibility(Visibility::Collapsed);
        root.Children().Append(sparks);

        // 5. The 3 DOTS, alone in the band (row 1): strictly below the cell, so they clear the dot and the glow with no
        //    offset of their own. Each dot's static Opacity stays 1.0 — the pulse begins and ends there.
        StackPanel pending{};
        pending.Orientation(Orientation::Horizontal);
        pending.Spacing(kPendingDotSpacing);
        pending.HorizontalAlignment(HorizontalAlignment::Center);
        pending.VerticalAlignment(VerticalAlignment::Center);
        pending.IsHitTestVisible(false);
        pending.Visibility(Visibility::Collapsed);
        Grid::SetRow(pending, 1);
        std::array<UIElement, 3> dots{ nullptr, nullptr, nullptr };
        for (auto& slot : dots)
        {
            Shapes::Ellipse d{};
            d.Width(kPendingDotDiameter);
            d.Height(kPendingDotDiameter);
            d.Fill(pendingBrush);
            pending.Children().Append(d);
            slot = d;
        }
        root.Children().Append(pending);

        // The motions, with the tab header's anchors: each anchor is a container that is never itself animated, and it
        // is asked for the compositor only while the UI thread has none cached (AgentCompositorOf) — in practice a tab
        // header asked first, so no card element ever gets a hand-out visual (a composition node per card would cost
        // every frame, UI_THREAD.md §10h / §14). Nothing composition-side is created before a motion's first start.
        AgentDotsPulse pendingPulse{ pending, dots };
        AgentOpacityLoop glowLoop = AgentMakeCacheWarmGlow(sparks, glow);
        AgentEmberSparks embers{ sparks };
        AgentLockstepBlink ringBlink{ root, ring };

        // The motions follow the root in and out of the live tree: a card realized into the list starts what its flags
        // want (it was built detached, so the setters' decisions found it unloaded), and a card leaving the tree stops
        // everything — the compositor never animates an icon nobody can see. The handlers hold the glyph WEAKLY: the
        // root holds them, the glyph holds the root, and a strong capture would close a cycle that leaks every card.
        // _ApplyMotion reads IsLoaded() itself — the live answer, never the event: XAML can raise a STALE Unloaded after
        // the next Loaded when an element is re-parented within one tick.
        const std::weak_ptr<AgentStatusGlyph> weakSelf = weak_from_this();
        root.Loaded([weakSelf](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakSelf.lock())
            {
                self->_ApplyMotion();
            }
        });
        root.Unloaded([weakSelf](const winrt::Windows::Foundation::IInspectable&, const RoutedEventArgs&) {
            if (const auto self = weakSelf.lock())
            {
                self->_ApplyMotion();
            }
        });

        _root = std::move(root);
        _glow = std::move(glow);
        _ring = std::move(ring);
        _dot = std::move(dot);
        _sparks = std::move(sparks);
        _pending = std::move(pending);
        _stateBrush = std::move(stateBrush);
        _warmBrush = std::move(warmBrush);
        _pendingBrush = std::move(pendingBrush);
        _pendingPulse = std::move(pendingPulse);
        _glowLoop = std::move(glowLoop);
        _embers = std::move(embers);
        _ringBlink = std::move(ringBlink);
    }

    void AgentStatusGlyph::SetStateColor(const winrt::Windows::UI::Color& color)
    {
        // One brush per glyph, re-colored in place: the dot is bound to it, so one Color write repaints the dot.
        if (_stateBrush.Color() != color)
        {
            _stateBrush.Color(color);
        }
    }

    void AgentStatusGlyph::SetPending(bool shown, const winrt::Windows::UI::Color& color, bool mayPulse)
    {
        // Color FIRST: the dots must never appear in the previous draft's color (the tab sets its brush before the
        // visibility for the same reason, TerminalPage::_SetTabPending).
        if (shown && _pendingBrush.Color() != color)
        {
            _pendingBrush.Color(color);
        }
        _pending.Visibility(shown ? Visibility::Visible : Visibility::Collapsed);
        _pendingShown = shown;
        _pendingMayPulse = mayPulse;
        _ApplyMotion();
    }

    void AgentStatusGlyph::SetFlashRing(bool shown, const winrt::Windows::UI::Xaml::Media::Brush& brush)
    {
        // Fill FIRST, and only when it is a different object: the window's ONE flash-ring brush is re-colored in place
        // by a settings change (TerminalPage::_RefreshFlashRingBrush), so once the ring holds it nothing needs writing
        // again — a same-object write would still invalidate the element's render.
        if (shown && brush && _ring.Fill() != brush)
        {
            _ring.Fill(brush);
        }
        _ring.Visibility(shown ? Visibility::Visible : Visibility::Collapsed);
        _flashRingShown = shown;
        // Shown -> the blink joins the shared clock in its current phase (in lockstep with the tab's own ring); hidden ->
        // the blink ends with the ring's static Opacity back at 1.0 (AgentLockstepBlink::Stop), ready for the next flash.
        _ApplyMotion();
    }

    void AgentStatusGlyph::SetCacheWarm(bool shown, const winrt::Windows::UI::Color& color)
    {
        if (shown)
        {
            // Both glow discs share the brush; the embers are composition visuals, which cannot bind a XAML brush, so
            // their color is copied (the tab header's _SyncCacheWarmEmberColor). Both BEFORE the crown shows.
            if (_warmBrush.Color() != color)
            {
                _warmBrush.Color(color);
            }
            _embers.SetColor(color); // noexcept: a refused color is logged and the embers keep the previous one
        }
        const auto visibility = shown ? Visibility::Visible : Visibility::Collapsed;
        _glow.Visibility(visibility);
        _sparks.Visibility(visibility);
        _cacheWarmShown = shown;
        _ApplyMotion();
    }

    void AgentStatusGlyph::SetHostShown(bool shown) noexcept
    {
        _hostShown = shown;
        _ApplyMotion();
    }

    void AgentStatusGlyph::RefreshMotion() noexcept
    {
        _ApplyMotion();
    }

    void AgentStatusGlyph::Shutdown() noexcept
    {
        _shutDown = true;
        // Every motion is unwanted now (the shut-down glyph counts as out of the live tree), so this stops what runs —
        // above all the ring's blink, whose hold on the thread's shared clock must be given back, or the compositor
        // keeps animating that clock for nobody.
        _ApplyMotion();
    }

    void AgentStatusGlyph::_ApplyMotion() noexcept
    {
        ::Agentmaster::StatusIconMotionInputs in{};
        try
        {
            // The live answer (see the Loaded / Unloaded handlers); a shut-down glyph is "out of the tree" for good.
            in.inLiveTree = !_shutDown && _root && _root.IsLoaded();
        }
        catch (...)
        {
            // A torn-down root reads as unloaded: every motion stops, which is the safe direction.
            ::Agentmaster::AgentLogCaughtException(L"status glyph IsLoaded read");
            in.inLiveTree = false;
        }
        in.hostShown = _hostShown;
        in.animationsEnabled = AgentAnimationsEnabled();
        in.pendingShown = _pendingShown;
        in.pendingMayPulse = _pendingMayPulse;
        in.cacheWarmShown = _cacheWarmShown;
        in.flashRingShown = _flashRingShown;
        const auto want = ::Agentmaster::DecideStatusIconMotion(in);
        // Independent of each other: one motion's failure (logged inside) never keeps the other two from their change.
        _ApplyPendingPulse(want.pendingPulse);
        _ApplyCacheWarmCrown(want.cacheWarmCrown);
        _ApplyFlashRingBlink(want.flashRingBlink);
    }

    void AgentStatusGlyph::_ApplyPendingPulse(bool on) noexcept
    {
        // A start on a running pulse RESTARTS it (the wave visibly jumps back), and the decisions re-run often (every
        // setter, Loaded, Unloaded): act only on a real change.
        if (on == _pendingPulsing)
        {
            return;
        }
        try
        {
            if (on)
            {
                _pendingPulse.Start();
            }
            else
            {
                _pendingPulse.Stop(); // every dot lands on its still opacity 1.0: the draft stays visible
            }
            _pendingPulsing = on;
        }
        catch (...)
        {
            // The flag keeps its old value, so the next decision retries the transition (the tab header's rule).
            ::Agentmaster::AgentLogCaughtException(L"status glyph pending-dots pulse");
        }
    }

    void AgentStatusGlyph::_ApplyCacheWarmCrown(bool on) noexcept
    {
        // One decision for the glow AND the embers (a half-moving crown would read as broken), with the same change
        // gate: a restart jumps the breath and every ember back to the start of its cycle.
        if (on == _cacheWarmRunning)
        {
            return;
        }
        try
        {
            if (on)
            {
                _glowLoop.Start();
                _embers.Start(); // builds the ember visuals on the icon's first warm moment, never before
            }
            else
            {
                _glowLoop.Stop(); // lands on the still 0.45: the steady glow
                _embers.Stop(); // every ember hidden at its resting spot
            }
            _cacheWarmRunning = on;
        }
        catch (...)
        {
            // A half-started crown is restarted whole by the next decision, a half-stopped one stopped again — both safe
            // to repeat (the tab header's rule).
            ::Agentmaster::AgentLogCaughtException(L"status glyph cache-warm crown");
        }
    }

    void AgentStatusGlyph::_ApplyFlashRingBlink(bool on) noexcept
    {
        if (on == _flashRingBlinking)
        {
            return;
        }
        try
        {
            if (on)
            {
                _ringBlink.Start();
                // Recorded only after a successful start: a failed one leaves the flag false, so the next decision
                // retries it — the ring meanwhile shows steadily at its static Opacity 1.0, never hidden.
                _flashRingBlinking = true;
            }
            else
            {
                // Recorded BEFORE the stop: Stop() releases the clock even when its Opacity write throws, so the blink has
                // ended either way, and a flag left true would make the next flash skip its start.
                _flashRingBlinking = false;
                _ringBlink.Stop();
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"status glyph flash-ring blink");
        }
    }
}
