// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster (UI_THREAD.md §10a #1, §10i): AMBIENT animations on the Composition API — the looping,
// decorative motions (the unsent-draft "3 dots" pulse on tab headers and on board / side-panel cards, and the
// spark crown's glow breath + rising embers on tab headers) and the unread FLASH RING's blink on tab headers
// run as Windows.UI.Composition animations, never as XAML storyboards or as a timer that flips a XAML property.
// Since SIDE_PANEL.md §2g the side panel's cards carry the tab's whole status icon (AgentStatusGlyph), so the
// crown and the ring's blink run on those cards too — through these same classes, with the same parameters.
//
// WHY: XAML draws a frame on EVERY display refresh while ANY storyboard runs, and each frame walks the
// window's whole composition tree on the UI thread. On the 555-tab release (0.6.8.15, measured 2026-10-04,
// UI_THREAD.md §10) a handful of Forever storyboards kept that loop at 65 frames/s, ~2.7 ms per frame: 50-70%
// of the UI thread's busy time. The storyboard tick itself was only ~9 ms/s — the cost is the loop, so a
// single running storyboard costs as much as a hundred. A timer that flips a XAML property costs a frame per
// flip, plus a layout pass when the property is Visibility (the flash ring's 600 ms timer, UI_THREAD.md §10h).
// A composition animation runs in the compositor, and XAML's frame loop does not tick for it.
//
// THREE PATHS, picked per property by what XAML does with it (read in the WinUI source — FacadeAnimationHelper,
// FacadeStorage, PropertySetListener and UIElement.cpp, the code behind UIElement::StartAnimation):
//   - OPACITY of a XAML element (AgentOpacityLoop): UIElement::StartAnimation with Target "Opacity" (the RS5
//     "XAML property animation"). XAML attaches NO per-frame listener to Opacity — it only follows properties
//     that move hit-testing — and it marks the element as opacity-animating, so it keeps rendering an element
//     whose static Opacity is 0 (it culls such an element otherwise, which is why a hand-out visual animation
//     is not used). A restart hands over cleanly. UIElement::StopAnimation applies the animation's
//     StopBehavior (SetToFinalValue here) and XAML then writes the stopped value back into UIElement.Opacity,
//     so every loop BEGINS AND ENDS on its element's still value: a stopped indicator shows exactly the
//     still look, and the write-back changes nothing.
//   - LOCKSTEP BLINK of several XAML elements (AgentLockstepBlink — the flash rings of every unread tab, which
//     must blink TOGETHER): each element runs a composition ExpressionAnimation "clock.Opacity" through the
//     same UIElement::StartAnimation, and ONE key-frame loop per UI thread animates that clock (a
//     CompositionPropertySet). Every element reads one value, so they are in exact lockstep, and an element
//     that starts later takes the current phase at once. XAML treats an expression like a key-frame
//     animation here (no per-frame listener on Opacity). An expression has no stop behavior, and
//     UIElement::StopAnimation would let XAML write the clock's value of that moment (0 in the "off" half)
//     into UIElement.Opacity; so a blink is ended by a STATIC Opacity set instead, which XAML defines as
//     "cancel the running animation and push no value back" (CUIElement::OnSetLocalOpacity ->
//     FacadeAnimationHelper::CancelSingleAnimation).
//   - MOTION never runs on a XAML element (AgentEmberSparks). XAML follows an animated Translation (and a
//     hand-out visual's transform) on every frame for hit-testing, which would bring the per-frame UI-thread
//     work back. Moving content is a composition visual of our own, attached with
//     ElementCompositionPreview::SetElementChildVisual: XAML neither hit-tests it nor watches it.
//
// ONE COMPOSITOR PER UI THREAD: XAML's compositor belongs to the thread's core services (DCompTreeHost), which
// every island of the thread shares — every Agentmaster window. AgentCompositorOf therefore asks XAML ONCE per
// thread and keeps the answer, so a single element per thread gets a hand-out visual. A hand-out visual
// forces a composition node on its element for life, and the per-frame walk visits every node
// (UI_THREAD.md §10h); asking per anchor gave a node to every pulsing dots panel and every flashing tab.
//
// This header is LIGHT on purpose: it declares only. The bodies live in AgentAmbientAnimation.cpp, the one
// translation unit that pays for the full winrt/Windows.UI.Composition.h projection (1.1 MB), so the seven
// TUs that include AgentManagerContent.Internal.h and the four that include TabHeaderControl.h do not.
// The member types below are complete through the TerminalApp pch (the Xaml projection pulls in the
// Composition type definitions). Every member runs on the UI thread that owns the elements, except
// AgentLockstepBlink's destructor (see there).
#pragma once

#include <array>
#include <chrono>
#include <vector>

namespace winrt::TerminalApp::implementation
{
    // One key frame of an ambient loop. A loop interpolates LINEARLY between its key frames, as the XAML
    // DoubleAnimations it replaces did (a composition key frame without an easing function would ease).
    struct AgentLoopKeyFrame
    {
        // The position in the cycle, normalized: 0 = the start of a cycle, 1 = its end.
        float progress;
        // The property value at that position.
        float value;
    };

    // The per-UI-thread state behind these animations: the thread's cached compositor and the lockstep blink
    // clock. Defined in AgentAmbientAnimation.cpp. One per UI thread, created on first use and never freed
    // (the AgentTipHelpers idiom — a destructing thread_local would release composition objects at thread
    // exit), so a pointer to it stays valid on any thread for the life of the process.
    struct AgentAmbientThread;

    // The compositor that renders `anchor` — the one compositor of its UI thread, which every animation and
    // visual used on that thread's elements must come from (Composition refuses a mix).
    //   - The FIRST call on a thread asks XAML through the anchor's hand-out visual (under XAML Islands there
    //     is no Window::Current() to ask); that gives the anchor a composition node for the rest of its life.
    //     So the anchor must be a container (a dots panel, a sparks canvas, a status-dot grid), never an
    //     element whose UIElement properties are animated.
    //   - Every later call on that thread returns the cached compositor and does not touch `anchor`.
    // Parameters: anchor - any element on the calling UI thread, in a live tree or not; used only while the
    //             thread has no cached compositor.
    // Returns: the compositor (never null for a valid element).
    // Throws winrt::hresult_error when XAML refuses on the first call (a torn-down element, a call off its UI
    // thread); nothing is cached then, so the next call asks again.
    winrt::Windows::UI::Composition::Compositor AgentCompositorOf(const winrt::Windows::UI::Xaml::UIElement& anchor);

    // Build an endless, LINEAR key-frame loop of one scalar property.
    //   - It repeats forever and always runs FORWARD: a cycle that must come back to its start says so in its
    //     own key frames, which is what lets every loop here end on the still value (see the file header).
    //   - Its delay applies ONCE, before the first cycle, like the XAML BeginTime it replaces; meanwhile the
    //     property keeps its static value (SetInitialValueAfterDelay).
    //   - Stopping it leaves the property on its LAST key frame (SetToFinalValue).
    // Parameters:
    //   compositor - the compositor of the thread that will start it (AgentCompositorOf).
    //   keyFrames  - at least two; progress strictly increasing from 0 to 1.
    //   cycle      - the length of one cycle; at least 1 ms (Composition refuses shorter).
    //   delay      - the one-time delay before the first cycle; 0 for none.
    //   target     - the property for UIElement::StartAnimation ("Opacity"), or nullptr when the animation
    //                is started on a visual or a property set (their StartAnimation names the property).
    // Returns: the animation, not started; one object per element (a restart reuses it).
    // Throws winrt::hresult_error when Composition refuses a value (a progress outside [0, 1], a zero cycle).
    winrt::Windows::UI::Composition::ScalarKeyFrameAnimation AgentMakeScalarLoop(const winrt::Windows::UI::Composition::Compositor& compositor,
                                                                               const std::vector<AgentLoopKeyFrame>& keyFrames,
                                                                               std::chrono::milliseconds cycle,
                                                                               std::chrono::milliseconds delay,
                                                                               const wchar_t* target);

    // An endless Opacity loop on ONE XAML element, run by XAML's own composition path
    // (UIElement::StartAnimation, Target "Opacity"). The OWNER decides when it runs (a header: the
    // RENDERING.md §4c gates; a card: Loaded / Unloaded); this class only starts and stops it. The animation
    // object is built on the first Start() and reused by every later Start() / Stop().
    // ⚠ Anything that sets the element's Opacity while the loop runs cancels the loop (XAML treats a static
    // set as taking over): keep the element's XAML Opacity at the loop's still value, and drive it only here.
    class AgentOpacityLoop
    {
    public:
        // An inert loop: Start() / Stop() do nothing until a real loop is assigned over it. Exists so an
        // owner can declare the member before its XAML elements exist.
        AgentOpacityLoop() = default;

        // Parameters:
        //   anchor    - an element of the same UI thread that is NOT `element` (see AgentCompositorOf). Held
        //               WEAKLY: a card's anchor is the card's own dots panel, whose Loaded / Unloaded handlers
        //               own this loop, and a strong reference would close a cycle that leaks every card.
        //   element   - the element whose Opacity loops.
        //   keyFrames - the cycle; its FIRST and LAST values must equal the element's still Opacity, so a
        //               stopped loop lands on the still look (see the file header).
        //   cycle     - the length of one cycle (>= 1 ms).
        //   delay     - the one-time delay before the first cycle (a cluster's stagger); 0 for none.
        AgentOpacityLoop(const winrt::Windows::UI::Xaml::UIElement& anchor,
                         winrt::Windows::UI::Xaml::UIElement element,
                         std::vector<AgentLoopKeyFrame> keyFrames,
                         std::chrono::milliseconds cycle,
                         std::chrono::milliseconds delay);

        // Start (or restart) the loop. A restart of a running loop jumps it back to the start of its cycle,
        // so an owner should start only on a real change (each owner keeps a "running" flag for that).
        // Throws winrt::hresult_error when XAML or Composition refuses (a torn-down element), or E_ABORT when
        // the anchor is gone before the first start AND the thread has no cached compositor (nothing to take
        // the compositor from).
        void Start();

        // Stop the loop: the element lands on the loop's last key frame (its still Opacity), and XAML writes
        // that value back into UIElement.Opacity. A loop that is not running is a no-op (XAML answers S_FALSE),
        // so Stop() is safe after a failed Start(), on an inert loop, and twice in a row.
        // Throws winrt::hresult_error when XAML refuses (a torn-down element).
        void Stop();

    private:
        // The element that supplies the compositor while the thread has none cached (never `_element`); weak
        // (see the ctor).
        winrt::weak_ref<winrt::Windows::UI::Xaml::UIElement> _anchor;
        // The element whose Opacity loops; null for an inert loop.
        winrt::Windows::UI::Xaml::UIElement _element{ nullptr };
        // The cycle, kept until the animation is built on the first Start().
        std::vector<AgentLoopKeyFrame> _keyFrames;
        std::chrono::milliseconds _cycle{ 0 };
        std::chrono::milliseconds _delay{ 0 };
        // The animation, built on the first Start(); StopAnimation needs the same Target, so it is kept.
        winrt::Windows::UI::Composition::ScalarKeyFrameAnimation _animation{ nullptr };
    };

    // The unsent-draft pulse: one cycle of each dot, and the lag between neighboring dots. The SAME numbers as
    // the storyboards they replace (500 ms down + 500 ms up, 160 ms apart), shared by the tab header and the
    // board / side-panel cards so the two surfaces cannot drift apart.
    inline constexpr std::chrono::milliseconds kAgentDotsPulseCycle{ 1000 };
    inline constexpr std::chrono::milliseconds kAgentDotsPulseStagger{ 160 };

    // The pulse of ONE 3-dot cluster (a tab header's, or a board / side-panel card's): dot i runs Opacity
    // 1.0 -> 0.3 -> 1.0 per cycle, delayed by i x kAgentDotsPulseStagger — the left-to-right "typing" wave.
    // The storyboards it replaces ran 0.3 -> 1.0 -> 0.3: the same wave, half a cycle later. Starting AND
    // ending on 1.0, the still dots' opacity, is what makes a stopped pulse land on the still look.
    // The owner decides WHEN it runs; a card holds it in its Loaded / Unloaded handlers, so a dropped card
    // releases it with them (it holds the dots alive only that long).
    class AgentDotsPulse
    {
    public:
        // An inert pulse (see AgentOpacityLoop's default constructor).
        AgentDotsPulse() = default;

        // Parameters:
        //   anchor - an element of the same UI thread that is not a dot (the dots' panel).
        //   dots   - the three dots, left to right; their XAML Opacity must stay 1.0 (see AgentOpacityLoop).
        AgentDotsPulse(const winrt::Windows::UI::Xaml::UIElement& anchor, const std::array<winrt::Windows::UI::Xaml::UIElement, 3>& dots);

        // Start (or restart) all three dots; restart only on a real change (it jumps the wave back). Every dot
        // is tried even when one fails, so one torn-down dot cannot leave its neighbors still.
        // Throws winrt::hresult_error (the first failure) when XAML or Composition refuses; the dots that did
        // start keep pulsing until Stop().
        void Start();

        // Stop all three dots on the still opacity 1.0. Safe on an inert pulse, after a partial Start(), and
        // twice in a row. Throws winrt::hresult_error when XAML refuses (a torn-down element).
        void Stop();

    private:
        // One loop per dot, each with its own stagger.
        std::array<AgentOpacityLoop, 3> _dots{};
    };

    // The spark crown's GLOW BREATH (the cache-warm hint): the glow grid's Opacity runs 0.45 -> 0.62 -> 0.28 -> 0.45 over
    // 2.8 s — the storyboard's 0.28 <-> 0.62 breath (1.4 s each way), begun and ended on the glow's still 0.45, so a
    // stopped crown lands on its still look. Shared by the tab header (TabHeaderControl's constructor) and the side
    // panel's card icon (AgentStatusGlyph, SIDE_PANEL.md §2g) so the two crowns cannot drift apart.
    // ⚠ The still value is ALSO the glow element's static Opacity: TabHeaderControl.xaml's HeaderCacheWarmGlow
    // Opacity="0.45" and AgentStatusGlyph's code twin. Change all three together, or a stopped crown jumps (XAML writes
    // the loop's last key frame back into UIElement.Opacity on a stop, AgentOpacityLoop::Stop).
    inline constexpr float kAgentCacheWarmGlowStill = 0.45f;
    inline constexpr std::array<AgentLoopKeyFrame, 4> kAgentCacheWarmGlowKeyFrames{ { { 0.0f, kAgentCacheWarmGlowStill },
                                                                                      { 0.25f, 0.62f },
                                                                                      { 0.75f, 0.28f },
                                                                                      { 1.0f, kAgentCacheWarmGlowStill } } };
    inline constexpr std::chrono::milliseconds kAgentCacheWarmGlowCycle{ 2800 };

    // Build the spark crown's glow breath (kAgentCacheWarmGlowKeyFrames over kAgentCacheWarmGlowCycle, no delay) — the
    // ONE recipe both crowns use, so a tab header and a side-panel card breathe identically.
    //   anchor  - the element that supplies the compositor while the thread has none cached (the sparks canvas, as the
    //             tab header does); never `glow` itself. Held WEAKLY (see AgentOpacityLoop's constructor).
    //   glow    - the glow grid whose Opacity breathes; its static Opacity must be kAgentCacheWarmGlowStill.
    //   returns - the loop, NOT started; nothing composition-side exists until its first Start().
    //   throws  - std::bad_alloc only (the key-frame list is copied into the loop).
    AgentOpacityLoop AgentMakeCacheWarmGlow(const winrt::Windows::UI::Xaml::UIElement& anchor, winrt::Windows::UI::Xaml::UIElement glow);

    // The flash ring's beat: on for this long, then off for this long — the SAME 600 ms at which the per-window
    // DispatcherTimer it replaces toggled the ring's Visibility (TerminalPage::_OnAgentFlashTick).
    inline constexpr std::chrono::milliseconds kAgentBlinkHalfPeriod{ 600 };

    // A hard on / off blink of ONE XAML element's Opacity (1 for kAgentBlinkHalfPeriod, then 0 for as long), in
    // LOCKSTEP with every other AgentLockstepBlink of the same UI thread — the tab strip's unread flash rings,
    // in every window.
    //   - HOW: the element runs a composition ExpressionAnimation "clock.Opacity" (UIElement::StartAnimation,
    //     Target "Opacity"), and ONE key-frame loop per UI thread animates the clock. All elements read one
    //     value: exact lockstep, and an element that starts mid-cycle shows the current phase at once.
    //   - The clock runs only while at least one blink follows it. The first Start() of a burst restarts it at
    //     the START of its cycle — the "on" half, so the first ring shows at once — and the last Stop() stops
    //     it, so an idle fleet leaves the compositor nothing to animate.
    //   - WHY not a timer: the DispatcherTimer this replaces flipped the ring's XAML Visibility every 600 ms —
    //     a layout pass and a frame each time, in every window with an unread tab (UI_THREAD.md §10h). A
    //     composition animation needs neither: no layout pass, no XAML frame, no UI-thread work per blink.
    //   - Stop() ends the expression with a STATIC Opacity set of 1.0 (see the file header), so the element's
    //     XAML Opacity must stay at the default 1.0, and nothing else may set it.
    // UI thread only (the thread that owns the element), except the destructor (see there).
    class AgentLockstepBlink
    {
    public:
        // An inert blink: Start() / Stop() do nothing until a real one is moved over it. Exists so an owner can
        // declare the member before its XAML elements exist.
        AgentLockstepBlink() = default;

        // Parameters:
        //   anchor  - an element of the same UI thread that is NOT `element`; it supplies the compositor only
        //             while the thread has none cached (see AgentCompositorOf). Held WEAKLY, like
        //             AgentOpacityLoop's anchor.
        //   element - the element whose Opacity blinks; its XAML Opacity must stay at the default 1.0.
        AgentLockstepBlink(const winrt::Windows::UI::Xaml::UIElement& anchor, winrt::Windows::UI::Xaml::UIElement element);

        // Gives up this blink's hold on the clock if it still has one (an owner torn down without a Stop() —
        // e.g. a tab header destroyed before its Unloaded handler ran). On the owning UI thread the release is
        // immediate. On any other thread (a page destroyed on a pool thread — the CLAUDE.md teardown gotcha) it
        // is posted to the UI thread's DispatcherQueue, because the clock's composition objects belong to that
        // thread; a held clock that is never released would keep the compositor animating for no one. Does not
        // touch the element. Never throws (a failure is logged).
        ~AgentLockstepBlink();

        // Not copyable: two copies would release the clock twice. Movable: the source gives up its hold on the
        // clock, so exactly one object releases it.
        AgentLockstepBlink(const AgentLockstepBlink&) = delete;
        AgentLockstepBlink& operator=(const AgentLockstepBlink&) = delete;
        // Parameters: other - the blink to take over; left inert (holding no clock, no element).
        AgentLockstepBlink(AgentLockstepBlink&& other) noexcept;
        // Releases a clock this object still holds (as the destructor does), then takes over `other`'s.
        // Parameters: other - the blink to take over; left inert.
        // Returns: *this.
        AgentLockstepBlink& operator=(AgentLockstepBlink&& other) noexcept;

        // Start (or restart) the blink: join the thread's clock (restarting it at "on" when no other blink
        // follows it) and make the element follow it. Builds the clock on the first start of the thread. A
        // restart is harmless — the element follows the same clock again, with no jump — but costs a XAML
        // commit, so an owner keeps a "running" flag and starts only on a real change.
        // Throws winrt::hresult_error when XAML or Composition refuses (a torn-down element), or E_ABORT when the
        // anchor is gone and the thread has no cached compositor. A failed Start() holds no clock and leaves the
        // element at its static Opacity 1.0: shown steadily, not blinking.
        void Start();

        // Stop the blink: a static Opacity of 1.0 ends the element's expression (XAML pushes no value back), and
        // the clock is released (stopped when no blink follows it any more). Safe when not started, on an inert
        // blink, and twice in a row.
        // Throws winrt::hresult_error when XAML refuses the Opacity set (a torn-down element); the clock is
        // released even then.
        void Stop();

    private:
        // Give up this blink's hold on the clock, if it has one: at once on the owning UI thread, else posted to
        // it. Never throws (logged).
        void _ReleaseClock() noexcept;

        // The element that supplies the compositor while the thread has none cached (never `_element`); weak.
        winrt::weak_ref<winrt::Windows::UI::Xaml::UIElement> _anchor;
        // The element whose Opacity blinks; null for an inert blink.
        winrt::Windows::UI::Xaml::UIElement _element{ nullptr };
        // The thread state whose clock this blink counts as a follower of, or null while it follows none — so
        // this pointer IS the "blinking" state that Stop() and the destructor undo.
        AgentAmbientThread* _clockOwner{ nullptr };
    };

    // One ember of the spark crown: its place inside the host element (the Canvas.Left / Canvas.Top of the
    // XAML ellipse it replaced, in DIPs) and its diameter.
    struct AgentEmberSpec
    {
        float left;
        float top;
        float diameter;
    };

    // The spark crown's three RISING EMBERS as composition visuals of our own, drawn on top of a host XAML
    // element (ElementCompositionPreview::SetElementChildVisual). Each ember repeats, staggered: it fades in
    // over the first quarter of its cycle while it rises, then fades out over the rest — the storyboard look it
    // replaces, unchanged (the same places, sizes, 3 DIP rise, 1150 ms cycle and 0 / 380 / 760 ms stagger).
    // Composition visuals because XAML follows a moving XAML element on every frame (see the file header);
    // XAML never sees these. The visuals are built on the first Start(), so a tab that is never warm builds
    // nothing; they then live as long as this object. When the host is collapsed, XAML hides them with it.
    class AgentEmberSparks
    {
    public:
        // An inert crown: SetColor / Start / Stop do nothing until a real one is assigned over it.
        AgentEmberSparks() = default;

        // Parameters: host - the XAML element the embers draw on (the 18x18 sparks canvas); it also supplies
        //             the compositor while the thread has none cached, and it must hold no other child visual
        //             (SetElementChildVisual replaces it).
        explicit AgentEmberSparks(winrt::Windows::UI::Xaml::UIElement host);

        // Set the embers' color (the tab's contrast-picked fire color, TabStatus.AgentCacheWarmBrush). Kept
        // and applied when the visuals are built; applied at once when they exist. Never throws: a refused
        // color is logged (Rule #18) and the embers keep their previous color.
        // Parameters: color - the fill color.
        void SetColor(const winrt::Windows::UI::Color& color) noexcept;

        // Start (or restart) the three embers, building the visuals on the first call. A restart jumps every
        // ember back to the start of its cycle, so the owner starts only on a real change.
        // Throws winrt::hresult_error when XAML or Composition refuses (a torn-down host); a later Start()
        // retries the build.
        void Start();

        // Stop the embers and leave each one HIDDEN at its resting place (opacity 0, its own Canvas spot):
        // the still crown shows the glow alone. Safe when never started and twice in a row.
        // Throws winrt::hresult_error when Composition refuses (a visual already closed).
        void Stop();

    private:
        // Build the brush, the three ellipse shape visuals and their container, and attach the container to
        // the host. Throws winrt::hresult_error on refusal; nothing half-built is kept (the next call retries).
        void _Build();

        // The XAML element the embers draw on; null for an inert crown.
        winrt::Windows::UI::Xaml::UIElement _host{ nullptr };
        // The container attached to the host, and one shape visual per ember (null until built).
        winrt::Windows::UI::Composition::ContainerVisual _container{ nullptr };
        std::array<winrt::Windows::UI::Composition::ShapeVisual, 3> _embers{ nullptr, nullptr, nullptr };
        // The one fill brush the three embers share, so SetColor recolors them in one write.
        winrt::Windows::UI::Composition::CompositionColorBrush _brush{ nullptr };
        // Per ember: the fade (Opacity) and the rise (Offset.Y) loops, built with the visuals.
        std::array<winrt::Windows::UI::Composition::ScalarKeyFrameAnimation, 3> _fades{ nullptr, nullptr, nullptr };
        std::array<winrt::Windows::UI::Composition::ScalarKeyFrameAnimation, 3> _rises{ nullptr, nullptr, nullptr };
        // The color to paint with; amber until the owner's first SetColor (the owner sets it before the first
        // Start, so this default is only ever seen if the tab's brush is missing).
        winrt::Windows::UI::Color _color{ 0xFF, 0xFF, 0xC1, 0x07 };
    };
}
