// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — WHEN the status icon's three indicators may MOVE (SIDE_PANEL.md §2g).
//
// The tab strip's status icon (TabHeaderControl.xaml, HeaderAgentStatusDotWrap) and its code-built twin on the
// side panel's cards (AgentStatusGlyph) carry three indicators around the state dot, each with a motion of its own:
//   - the unsent-draft "3 dots" PULSE (AgentDotsPulse),
//   - the cache-warm SPARK CROWN: the glow's breath (AgentOpacityLoop) and the rising embers (AgentEmberSparks),
//   - the unread FLASH RING's BLINK (AgentLockstepBlink, one shared clock per UI thread).
// Showing an indicator and moving it are two decisions. This header is the second one, the rule the tab header
// applies in TabHeaderControl::_UpdatePendingAnimation / _UpdateCacheWarmAnimation / _UpdateFlashRingAnimation,
// written once as a pure function so the side panel's glyph applies exactly the same rule and the harness can pin
// it (TestStatusIconMotion, tests_sidepanel.cpp). TabHeaderControl keeps its own inline copy of these conditions
// (an upstream-derived XAML control, left untouched): a change here must be made there too, and the reverse.
//
// Why each gate exists (the tab header's comments hold the measurements):
//   - Every motion needs the element IN THE LIVE TREE and its host SHOWN. A composition animation keeps the
//     compositor (DWM) working on every display frame, and an element nobody can see — a header the tab strip
//     virtualized away, a card scrolled out of the realized rows or inside a collapsed side panel — must not.
//   - The two DECORATIVE motions (the pulse, the crown) also need Windows' "Animation effects" switch ON
//     (SPI_GETCLIENTAREAANIMATION — an accessibility setting, and a cost one; RENDERING.md §4c). Off, the
//     indicator still SHOWS, standing still: still dots, the steady glow with no embers.
//   - The pulse also needs a STARTED tab: a dormant tab's REMEMBERED draft (a window-restored or deactivated tab
//     whose claude has not started) shows still dots, and the pulse begins the moment that claude starts.
//   - The flash ring's blink has NEITHER of the last two gates: it is an ALERT ("a session left Running while you
//     were not looking", or a manual Mark Unread). The timer blink it replaced ran whatever Windows' animation
//     setting said, a steady ring would read as "nothing new", and Mark Unread flashes a dormant tab too.
//
// Pure, header-only, std-free: the plain-C++ engine idiom (PointerCardPlacement.h, VirtualListWindow.h), so the m5
// harness compiles it without WinRT and TerminalApp includes it from any translation unit.

#pragma once

namespace Agentmaster
{
    // Everything the motion decision reads, sampled by the caller at decision time (on the UI thread, where the
    // element and the host live). Plain flags: comparable, copyable, no WinRT.
    struct StatusIconMotionInputs
    {
        bool inLiveTree{ false }; // the icon's element is in a live visual tree (UIElement::IsLoaded — the live answer, never the Loaded/Unloaded event that called)
        bool hostShown{ true }; // the surface that holds the icon is on screen (a side panel that is not collapsed); a tab header has no such host and passes true
        bool animationsEnabled{ true }; // Windows' "Animation effects" switch (AgentAnimationsEnabled), read at decision time
        bool pendingShown{ false }; // the 3 dots are SHOWN (the session has an unsent draft)
        bool pendingMayPulse{ false }; // the session's tab has STARTED (or it is an external session, which has no tab of ours to be dormant): its dots may pulse
        bool cacheWarmShown{ false }; // the spark crown is SHOWN (ServerCacheStillWarm holds for the session)
        bool flashRingShown{ false }; // the flash ring is SHOWN (the session is unread: an automatic flash or a Mark Unread)
    };

    // Which motions may run now. An indicator that is shown but may not move stands still on its still look.
    struct StatusIconMotion
    {
        bool pendingPulse{ false }; // run the 3 dots' staggered opacity wave
        bool cacheWarmCrown{ false }; // run the glow's breath AND the rising embers (one decision for both: a crown half-moving would read as broken)
        bool flashRingBlink{ false }; // run the ring's lockstep blink
    };

    // Decide which of the status icon's motions may run (see the file header for every gate's reason).
    //   in      — the sampled facts.
    //   returns — the three motion flags; all false while the element is out of the live tree or its host is hidden.
    //   throws  — nothing (pure boolean logic).
    constexpr StatusIconMotion DecideStatusIconMotion(const StatusIconMotionInputs& in) noexcept
    {
        // Shared gate: an element nobody can see never animates (the compositor would work every frame for no one).
        const bool visible = in.inLiveTree && in.hostShown;
        StatusIconMotion out{};
        // Decorative: needs Windows' animation switch, and — for the dots only — a started tab.
        out.pendingPulse = visible && in.pendingShown && in.pendingMayPulse && in.animationsEnabled;
        out.cacheWarmCrown = visible && in.cacheWarmShown && in.animationsEnabled;
        // The alert: shown means blinking, whatever the animation switch says and whether or not the tab has started.
        out.flashRingBlink = visible && in.flashRingShown;
        return out;
    }
}
