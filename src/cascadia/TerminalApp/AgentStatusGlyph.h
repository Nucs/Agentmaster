// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the tab strip's STATUS ICON, built in code for a surface that is not a tab header: the status icon
// of every side-panel session card (SIDE_PANEL.md §2g; the user's "supporting what tab supports … copied to 100%").
//
// WHAT: the 18 x 22 DIP icon a tab header draws in front of its title (TabHeaderControl.xaml,
// HeaderAgentStatusDotWrap): the 10 DIP state dot with its thin black stroke in an 18 DIP cell, a 4 DIP band
// under the cell, and around the dot the tab's three indicators —
//   - the UNSENT-DRAFT "3 dots" in the band (2.5 DIP dots, 1.5 apart), pulsing in the staggered wave while the
//     session's tab has started, standing still on a dormant tab's remembered draft;
//   - the UNREAD FLASH RING, a 13 DIP ellipse behind the dot (so ~1.5 DIP of it shows around the dot's black stroke)
//     in the user's "status flashing color", blinking 600 ms on / 600 ms off in LOCKSTEP with every other ring of
//     the UI thread — the tab's own ring included (one shared composition clock, AgentLockstepBlink);
//   - the CACHE-WARM SPARK CROWN, a two-disc glow behind the dot that breathes (the shared AgentMakeCacheWarmGlow)
//     and three embers rising off it (AgentEmberSparks), in the fire color picked for the background.
// The same elements, sizes, z-order, static opacities and composition animations as the XAML, and the same motion
// rule (AgentMaster/StatusIconMotion.h — what TabHeaderControl's _Update*Animation methods apply). The two must be
// kept in step BY HAND: each constant in AgentStatusGlyph.cpp names its XAML twin, and the XAML points back here.
//
// WHY IN CODE: the side panel's cards are built imperatively (TerminalPage.AgentSidePanel.cpp), like the board's; a
// XAML UserControl would be a new XAML type of the package, and the tab header itself is a whole tab title, not an
// icon that could be reused.
//
// NOT CARRIED OVER from the tab: the favorite crown / star, the dormant half-hollow dot (a card keeps the full dot)
// and the wrap's own explanatory tooltip (a card has ONE tooltip for everything on it, and two floating tips on one
// card would fight — the AgentTipHelpers nested-tip rule).
//
// OWNERSHIP + LIFETIME (the AgentSessionToolTipHost idiom):
//   - The glyph lives in a std::shared_ptr (Create). The Loaded / Unloaded handlers it wires on its OWN root capture
//     a std::weak_ptr, so there is no element -> handler -> glyph -> element strong cycle (the one that leaked every
//     dropped card, UI_THREAD.md §12), and a glyph that dies before its root leaves handlers that do nothing.
//   - No destructor here touches XAML: a page can be torn down on a pool thread (the CLAUDE.md teardown gotcha). The
//     OWNER calls Shutdown() on the UI thread before it drops the glyph, which stops every motion and so gives the
//     flash ring's hold on the thread's blink clock back. A glyph dropped without Shutdown() still releases that
//     hold (AgentLockstepBlink's destructor, posted to the UI thread when needed); its other animations end with
//     their elements.
//   - The root is NOT hit-testable: the host's own element (the card) takes the pointer, so a click on the dot
//     selects the card like a click anywhere else on it.
//
// UI thread only: every member reads or writes XAML / Composition objects of the root's thread. Include from TUs
// that include pch.h first (the projections come from the pch, like AgentAmbientAnimation.h).

#pragma once

#include "AgentAmbientAnimation.h" // AgentDotsPulse / AgentOpacityLoop / AgentEmberSparks / AgentLockstepBlink — the four motions, by value

#include <memory> // std::shared_ptr / enable_shared_from_this — a stable identity for the weak-capturing Loaded / Unloaded handlers

namespace winrt::TerminalApp::implementation
{
    // The tab strip's status icon as a code-built element tree with its own motion bookkeeping. Create one per host
    // element (a side-panel card), put Root() into the host's layout, then drive it with the Set* members; every setter
    // writes only what it is handed, and the motions follow on their own (Loaded / Unloaded / the setters / the host's
    // SetHostShown). Call Shutdown() before dropping it.
    class AgentStatusGlyph final : public std::enable_shared_from_this<AgentStatusGlyph>
    {
    public:
        // Build one icon: the whole element tree with EVERY indicator hidden and the dot transparent (the host's first
        // SetStateColor paints it), the four motions set up but not started (nothing composition-side is created until
        // a motion first starts, so an icon that never flashes, warms or holds a draft allocates none of it), and the
        // root's Loaded / Unloaded handlers wired.
        //   returns — the new glyph (never null), its root not parented yet.
        //   throws  — winrt::hresult_error when XAML refuses to build an element (off the UI thread, a torn-down
        //             core); std::bad_alloc on allocation failure. Nothing half-built is handed out then.
        static std::shared_ptr<AgentStatusGlyph> Create();

        // One icon per host: a copy would share the elements (a XAML element has ONE parent) and the motions.
        AgentStatusGlyph(const AgentStatusGlyph&) = delete;
        AgentStatusGlyph& operator=(const AgentStatusGlyph&) = delete;

        // The icon's root — a fixed 18 DIP wide, 22 DIP tall Grid (the 18 DIP glyph cell over the 4 DIP pending band)
        // with no margin and no alignment of its own: the HOST places it (the tab header dips its wrap by 1 DIP with a
        // margin; a card aligns the dot with its label — TerminalPage::_SidePanelCreateCardView). Never null after Create.
        //   returns — the root element.
        winrt::Windows::UI::Xaml::Controls::Grid Root() const noexcept;

        // Paint the state dot (the Triage-Board palette — the caller resolves the state to its color, StateColor /
        // AgentStatusColorFor). The dot's black 1 DIP stroke never changes, so the ring behind it always reads as a
        // separate band.
        //   color  — the dot's fill.
        //   throws — winrt::hresult_error when XAML refuses the write (a torn-down tree).
        void SetStateColor(const winrt::Windows::UI::Color& color);

        // Show or hide the unsent-draft "3 dots" and say whether they may pulse.
        //   shown    — the session has an unsent draft (a non-empty SessionInfo::pendingInput).
        //   color    — the dots' fill, already contrast-picked for the background the icon sits on (PendingDotsColorFor);
        //              applied BEFORE the dots show, so they never appear in a stale color. Ignored while hidden.
        //   mayPulse — the session's tab has STARTED (or the session is external): its dots pulse. False = still dots
        //              (a dormant tab's remembered draft), exactly as on the tab.
        //   throws   — winrt::hresult_error when XAML refuses a write; the motion change itself never throws (logged).
        void SetPending(bool shown, const winrt::Windows::UI::Color& color, bool mayPulse);

        // Show or hide the unread flash ring. A shown ring blinks (the alert ignores Windows' animation switch and the
        // tab's dormancy — StatusIconMotion.h); a hidden one stops blinking at once.
        //   shown  — the session is unread (its tab's ring is flashing: the automatic "left Running" flash or a manual
        //            Mark Unread, as published to the process-wide unread census).
        //   brush  — the ring's fill: the host window's ONE shared flash-ring brush (TerminalPage::_flashRingBrush, the
        //            user's "status flashing color", its alpha = the ring's opacity). Pass the SAME instance the tabs
        //            use, so a settings change that recolors it in place recolors this ring too. Set BEFORE the ring
        //            shows (never shown with a null fill); a null brush leaves the current fill. Ignored while hidden.
        //   throws — winrt::hresult_error when XAML refuses a write; the blink change itself never throws (logged).
        // ⚠ Never set the ring's Opacity anywhere: the blink is a composition expression on it, a static set cancels
        //   it, and its end (AgentLockstepBlink::Stop) is exactly such a set back to the default 1.0.
        void SetFlashRing(bool shown, const winrt::Windows::UI::Xaml::Media::Brush& brush);

        // Show or hide the cache-warm spark crown (the glow + the embers).
        //   shown  — ServerCacheStillWarm holds for the session (it is at rest and its last real API turn is younger
        //            than the server cache's lifetime).
        //   color  — the fire color, already contrast-picked for the background (CacheWarmSparkColorFor): it paints both
        //            glow discs and the three embers. Applied BEFORE the crown shows. Ignored while hidden.
        //   throws — winrt::hresult_error when XAML refuses a write; the motion change itself never throws (logged).
        void SetCacheWarm(bool shown, const winrt::Windows::UI::Color& color);

        // Tell the icon whether its HOST is on screen (a side panel that is not collapsed). A hidden host keeps the
        // icon loaded (a collapsed panel only collapses its content), so without this gate a collapsed panel's cards
        // would keep the compositor animating for nobody. Re-decides every motion.
        //   shown — true while the host can be seen. Starts true.
        void SetHostShown(bool shown) noexcept;

        // Re-decide every motion against the current facts — for a host that knows something the setters did not
        // carry changed (Windows' "Animation effects" switch, read at every decision). Cheap when nothing changed (each
        // motion has a change gate: a restart of a running loop would make it jump).
        void RefreshMotion() noexcept;

        // Stop every motion for good, on the UI thread, before the owner drops the glyph (a card leaving the list, the
        // list reset). After it the setters still write the elements, but nothing moves again — a late Loaded on a
        // root that is re-used by nobody can never restart a motion. Idempotent.
        void Shutdown() noexcept;

    private:
        // Use Create: the glyph must live in a std::shared_ptr before its handlers can weak-capture it.
        AgentStatusGlyph() = default;

        // Build the element tree, set up the four motions and wire the root's Loaded / Unloaded pair (Create's body).
        // Members are assigned only once everything exists, so a refusal part-way leaves no half-built icon.
        //   throws — what Create documents.
        void _Build();

        // Decide every motion (StatusIconMotion.h) from the recorded flags + the live facts (IsLoaded, Windows'
        // animation switch) and start / stop what changed. Never throws: each motion's failure is logged (Rule #18)
        // and leaves its "running" flag as it was, so the next decision retries the transition.
        void _ApplyMotion() noexcept;

        // Start or stop ONE motion when its wanted state differs from its running state (the change gate). Each never
        // throws (logged); see _ApplyMotion for the retry rule.
        //   on — the wanted state.
        void _ApplyPendingPulse(bool on) noexcept;
        void _ApplyCacheWarmCrown(bool on) noexcept;
        void _ApplyFlashRingBlink(bool on) noexcept;

        // --- The elements (TabHeaderControl.xaml twins in brackets) ---
        winrt::Windows::UI::Xaml::Controls::Grid _root{ nullptr }; // [HeaderAgentStatusDotWrap] 18 x (18 + 4): the glyph cell over the pending band
        winrt::Windows::UI::Xaml::Controls::Grid _glow{ nullptr }; // [HeaderCacheWarmGlow] the two-disc glow, drawn first (behind everything)
        winrt::Windows::UI::Xaml::Shapes::Ellipse _ring{ nullptr }; // [HeaderAgentStatusFlashRing] the 13 DIP unread ring
        winrt::Windows::UI::Xaml::Shapes::Ellipse _dot{ nullptr }; // [HeaderAgentStatusDot] the 10 DIP state dot, black 1 DIP stroke
        winrt::Windows::UI::Xaml::Controls::Canvas _sparks{ nullptr }; // [HeaderCacheWarmSparks] the embers' 18 x 18 host, drawn last in the cell
        winrt::Windows::UI::Xaml::Controls::StackPanel _pending{ nullptr }; // [HeaderPendingDots] the 3 dots, in the band
        // One brush per role, re-colored in place (a state change writes one Color, never a new brush per update).
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _stateBrush{ nullptr }; // the dot's fill  [TabStatus.AgentStatusBrush]
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _warmBrush{ nullptr }; // both glow discs  [TabStatus.AgentCacheWarmBrush]
        winrt::Windows::UI::Xaml::Media::SolidColorBrush _pendingBrush{ nullptr }; // the 3 dots  [TabStatus.AgentPendingBrush]

        // --- The motions (the tab header's members of the same names, TabHeaderControl.h) ---
        AgentDotsPulse _pendingPulse; // the dots' staggered wave; anchor: the dots' panel
        AgentOpacityLoop _glowLoop; // the glow's breath (AgentMakeCacheWarmGlow); anchor: the sparks canvas
        AgentEmberSparks _embers; // the three rising embers, composition visuals on the sparks canvas
        AgentLockstepBlink _ringBlink; // the ring's blink on the UI thread's shared clock; anchor: the root

        // --- What the host asked for (the setters) ---
        bool _pendingShown{ false }; // the 3 dots are shown
        bool _pendingMayPulse{ false }; // ...and may pulse (a started / external session)
        bool _cacheWarmShown{ false }; // the crown is shown
        bool _flashRingShown{ false }; // the ring is shown
        bool _hostShown{ true }; // the host is on screen (SetHostShown)
        bool _shutDown{ false }; // Shutdown() ran: nothing moves again

        // --- What runs now (the change gates; written only after a transition succeeded, except the ring's stop) ---
        bool _pendingPulsing{ false };
        bool _cacheWarmRunning{ false };
        bool _flashRingBlinking{ false };
    };
}
