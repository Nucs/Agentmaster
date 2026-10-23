// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the ambient animations (AgentAmbientAnimation.h has the design: why composition, why the
// three paths, why every loop begins and ends on its still value, why one compositor per thread). This file is
// the whole implementation, and the ONE translation unit that includes the full Windows.UI.Composition
// projection:
//   * AgentAmbientThread                     — the per-UI-thread state (cached compositor, blink clock);
//   * AgentCompositorOf / AgentMakeScalarLoop — the cached compositor lookup and the linear forever-loop builder;
//   * AgentOpacityLoop                       — one XAML element's Opacity, through UIElement::StartAnimation;
//   * AgentMakeCacheWarmGlow                 — the spark crown's glow breath, the one recipe of the tab's and the
//                                              side-panel card's crowns;
//   * AgentDotsPulse                         — the three staggered dot loops of an unsent-draft cluster;
//   * AgentLockstepBlink                     — the flash ring's blink, every ring following ONE shared clock;
//   * AgentEmberSparks                       — the spark crown's rising embers, as composition visuals.
#include "pch.h"
#include "AgentAmbientAnimation.h"

#include "AgentCatchLog.h" // AgentLogCaughtException / AppendStateLog — Rule #18 forensics for the swallowing sites

#include <exception> // std::exception_ptr — "try every element, rethrow the first failure"
#include <utility> // std::exchange — hand over a blink's hold on the clock exactly once

#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>

namespace winrt::TerminalApp::implementation
{
    // The per-UI-thread state (declared in the header). Created by CurrentAmbientThread() on the thread's first
    // use, never freed; the composition members are touched on that thread only.
    struct AgentAmbientThread
    {
        // The owning thread, and its DispatcherQueue (null if the thread has none). Both are written once, when
        // the state is created, and never change, so another thread may read them: an AgentLockstepBlink
        // destroyed off this thread compares the id and posts its clock release to the queue.
        DWORD threadId{ 0 };
        winrt::Windows::System::DispatcherQueue queue{ nullptr };
        // XAML's compositor of this thread, taken from the first anchor that asks (AgentCompositorOf) and kept
        // for the life of the process: the thread's XAML core lives as long as App's WindowsXamlManager.
        winrt::Windows::UI::Composition::Compositor compositor{ nullptr };
        // The lockstep blink clock (AgentLockstepBlink), built on the thread's first blink:
        //   blinkClock  - the property set whose scalar "Opacity" every blinking element follows;
        //   blinkLoop   - the key-frame loop that animates that scalar (on, then off, forever);
        //   blinkFollow - the expression the elements run ("clock.Opacity", Target "Opacity"); one object for
        //                 every element, since Composition copies an animation when it starts it;
        //   blinkUsers  - how many blinks follow the clock now; the loop runs only while it is above 0.
        winrt::Windows::UI::Composition::CompositionPropertySet blinkClock{ nullptr };
        winrt::Windows::UI::Composition::ScalarKeyFrameAnimation blinkLoop{ nullptr };
        winrt::Windows::UI::Composition::ExpressionAnimation blinkFollow{ nullptr };
        size_t blinkUsers{ 0 };
    };
}

namespace
{
    namespace WUC = winrt::Windows::UI::Composition;
    using winrt::TerminalApp::implementation::AgentAmbientThread;

    // The spark crown's embers: the places and sizes of the XAML ellipses they replace (TabHeaderControl.xaml,
    // the 18x18 sparks canvas), unchanged so the crown looks exactly as before.
    constexpr std::array<winrt::TerminalApp::implementation::AgentEmberSpec, 3> kEmbers{ {
        { 3.8f, 2.8f, 2.4f },
        { 7.6f, 1.6f, 2.8f },
        { 11.8f, 2.8f, 2.4f },
    } };
    // The host canvas's side, in DIPs: the container visual is given the canvas's size.
    constexpr float kEmberHostSide = 18.0f;
    // One ember cycle (rise + fade), and each ember's one-time delay: the storyboard's 1150 ms and
    // 0 / 380 / 760 ms stagger. XAML has no negative delay, so the first cycle ramps in one by one and every
    // later cycle is evenly phased — the same as before.
    constexpr std::chrono::milliseconds kEmberCycle{ 1150 };
    constexpr std::array<std::chrono::milliseconds, 3> kEmberStagger{ std::chrono::milliseconds{ 0 },
                                                                      std::chrono::milliseconds{ 380 },
                                                                      std::chrono::milliseconds{ 760 } };
    // The rise, relative to an ember's resting top: +1.4 DIP (just below its spot) to -1.6 DIP (above it), a
    // 3 DIP climb. The highest ember rests at 1.6, so the top of its arc lands at 0 — flush with the 18 DIP
    // cell; anything higher is clipped by the tab header (the trap that once squared the flash ring).
    constexpr float kEmberRiseFrom = 1.4f;
    constexpr float kEmberRiseTo = -1.6f;

    // The name of the blink clock's scalar: the clock's property set holds it, its loop animates it, and the
    // elements' expression reads it as "clock.<name>". One constant, so those three can never drift apart.
    constexpr wchar_t kBlinkClockProperty[] = L"Opacity";
    // The blink's edges: each takes this share of the cycle (0.05%, i.e. 0.6 ms of the 1200 ms cycle) — shorter
    // than one display frame, so the blink reads as the hard on / off of the Visibility timer it replaces, while
    // the key frames still rise strictly (Composition needs increasing progress values).
    constexpr float kBlinkEdge = 0.0005f;

    // The calling thread's AgentAmbientThread, created on first use. Heap-allocated and DELIBERATELY LEAKED (one
    // small struct per UI thread, alive for the process) — the AgentTipHelpers idiom: a plain thread_local object
    // would release its composition objects in a CRT thread-exit destructor, possibly after the thread's XAML
    // core is gone (a shutdown-crash class), and a never-freed state is also what lets an AgentLockstepBlink
    // destroyed on another thread post a release that runs later. Throws only if the allocation does.
    AgentAmbientThread& CurrentAmbientThread()
    {
        static thread_local AgentAmbientThread* state = [] {
            auto* created = new AgentAmbientThread{};
            created->threadId = ::GetCurrentThreadId();
            // The UI thread has one (App creates its XAML on a DispatcherQueue thread); null elsewhere, which
            // only matters to a blink destroyed off-thread (its release then has nowhere to go — logged there).
            created->queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
            return created;
        }();
        return *state;
    }

    // The thread's compositor for an owner that holds its anchor WEAKLY: the cached one when the thread already
    // has it (no element touched), else the anchor's (AgentCompositorOf, which then caches it).
    // Parameters:
    //   anchor - the owner's weak anchor.
    //   owner  - the owner's name, for the error text.
    // Returns: the compositor.
    // Throws winrt::hresult_error E_ABORT when the anchor is gone and nothing is cached (an owner torn down before
    // its first start: there is no compositor to build from), or whatever AgentCompositorOf throws.
    WUC::Compositor CompositorForWeakAnchor(const winrt::weak_ref<winrt::Windows::UI::Xaml::UIElement>& anchor, const wchar_t* owner)
    {
        if (const auto& cached = CurrentAmbientThread().compositor)
        {
            return cached;
        }
        const auto strong = anchor.get();
        if (!strong)
        {
            // Thrown like every other refusal, so the owner logs it and keeps its "not running" state.
            throw winrt::hresult_error(E_ABORT, winrt::hstring{ std::wstring{ owner } + L": the anchor element is gone" });
        }
        return winrt::TerminalApp::implementation::AgentCompositorOf(strong);
    }

    // Build the thread's blink clock if it does not exist yet: the property set, its loop, and the expression the
    // elements run. The members are assigned only once all three exist, so a refusal part-way leaves the thread
    // without a clock and the next start retries the whole build.
    // Parameters:
    //   thread     - the calling thread's state.
    //   compositor - that thread's compositor.
    // Throws winrt::hresult_error when Composition refuses.
    void EnsureBlinkClock(AgentAmbientThread& thread, const WUC::Compositor& compositor)
    {
        if (thread.blinkFollow)
        {
            return;
        }
        auto clock = compositor.CreatePropertySet();
        // 1 at rest: an element that still follows a stopped clock (a blink torn down without its Stop) shows
        // steadily instead of vanishing.
        clock.InsertScalar(kBlinkClockProperty, 1.0f);
        // On for the first half of the cycle, off for the second, ending where it began (on) so the cycles join
        // without a seam. The edges are kBlinkEdge wide (see there): a step to the eye.
        auto loop = winrt::TerminalApp::implementation::AgentMakeScalarLoop(compositor,
                                                                            { { 0.0f, 1.0f },
                                                                              { 0.5f - kBlinkEdge, 1.0f },
                                                                              { 0.5f, 0.0f },
                                                                              { 1.0f - kBlinkEdge, 0.0f },
                                                                              { 1.0f, 1.0f } },
                                                                            winrt::TerminalApp::implementation::kAgentBlinkHalfPeriod * 2,
                                                                            std::chrono::milliseconds{ 0 },
                                                                            nullptr);
        // The elements' side: their Opacity equals the clock's scalar on every compositor frame. "clock" is the
        // reference parameter's name inside the expression; the property name is the constant above.
        auto follow = compositor.CreateExpressionAnimation(winrt::hstring{ std::wstring{ L"clock." } + kBlinkClockProperty });
        follow.SetReferenceParameter(L"clock", clock);
        follow.Target(L"Opacity");
        thread.blinkClock = std::move(clock);
        thread.blinkLoop = std::move(loop);
        thread.blinkFollow = std::move(follow);
    }

    // Count one more follower of the thread's blink clock. The first follower of a burst (re)starts the loop at the
    // START of its cycle — the "on" half — so the first ring of a burst shows at once, as the timer it replaces did
    // (its fresh burst began on "on").
    // Parameters: thread - the calling thread's state; its clock is built.
    // Throws winrt::hresult_error when Composition refuses the start; nothing is counted then.
    void AcquireBlinkClock(AgentAmbientThread& thread)
    {
        if (thread.blinkUsers == 0)
        {
            thread.blinkClock.StartAnimation(kBlinkClockProperty, thread.blinkLoop);
        }
        ++thread.blinkUsers;
    }

    // Count one follower less. The last one stops the loop (the scalar lands on the loop's final key frame, 1), so
    // an idle fleet leaves the compositor nothing to animate. On the owning UI thread only.
    // Parameters: thread - the state whose clock is released.
    // Never throws: a refused stop is logged (Rule #18); the loop then keeps running until the next burst restarts
    // it — compositor work for nothing, but never a wrong pixel.
    void ReleaseBlinkClock(AgentAmbientThread& thread) noexcept
    {
        if (thread.blinkUsers == 0)
        {
            return; // never below zero (a release with no matching acquire would otherwise wrap around)
        }
        if (--thread.blinkUsers != 0)
        {
            return; // other rings still follow the clock
        }
        try
        {
            thread.blinkClock.StopAnimation(kBlinkClockProperty);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentLockstepBlink clock stop");
        }
    }
}

namespace winrt::TerminalApp::implementation
{
    WUC::Compositor AgentCompositorOf(const winrt::Windows::UI::Xaml::UIElement& anchor)
    {
        auto& thread = CurrentAmbientThread();
        if (!thread.compositor)
        {
            // The FIRST request of this thread: the anchor's hand-out visual is the documented way to XAML's
            // compositor under Islands. Cached, so this anchor is the only element of the thread that ever gets a
            // hand-out visual (each one forces a composition node on its element for life, and the per-frame
            // walk visits every node — UI_THREAD.md §10h). Assigned only after the call succeeds, so a refusal
            // caches nothing.
            thread.compositor = winrt::Windows::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(anchor).Compositor();
        }
        return thread.compositor;
    }

    WUC::ScalarKeyFrameAnimation AgentMakeScalarLoop(const WUC::Compositor& compositor,
                                                     const std::vector<AgentLoopKeyFrame>& keyFrames,
                                                     std::chrono::milliseconds cycle,
                                                     std::chrono::milliseconds delay,
                                                     const wchar_t* target)
    {
        auto animation = compositor.CreateScalarKeyFrameAnimation();
        // LINEAR, like the XAML DoubleAnimations these loops replace: a composition key frame inserted
        // without an easing function eases, which would change the look. One easing object serves all.
        const auto linear = compositor.CreateLinearEasingFunction();
        for (const auto& frame : keyFrames)
        {
            animation.InsertKeyFrame(frame.progress, frame.value, linear);
        }
        animation.Duration(cycle);
        animation.IterationBehavior(WUC::AnimationIterationBehavior::Forever);
        if (delay.count() > 0)
        {
            animation.DelayTime(delay);
        }
        // Stated, not left to the defaults: these two are what make a delayed dot stay on its still value
        // until its turn, and a stopped loop land on its last key frame (the still value).
        animation.DelayBehavior(WUC::AnimationDelayBehavior::SetInitialValueAfterDelay);
        animation.StopBehavior(WUC::AnimationStopBehavior::SetToFinalValue);
        if (target != nullptr && *target != L'\0')
        {
            animation.Target(target);
        }
        return animation;
    }

    AgentOpacityLoop::AgentOpacityLoop(const winrt::Windows::UI::Xaml::UIElement& anchor,
                                       winrt::Windows::UI::Xaml::UIElement element,
                                       std::vector<AgentLoopKeyFrame> keyFrames,
                                       std::chrono::milliseconds cycle,
                                       std::chrono::milliseconds delay) :
        _anchor{ anchor ? winrt::make_weak(anchor) : winrt::weak_ref<winrt::Windows::UI::Xaml::UIElement>{} },
        _element{ std::move(element) },
        _keyFrames{ std::move(keyFrames) },
        _cycle{ cycle },
        _delay{ delay }
    {
    }

    void AgentOpacityLoop::Start()
    {
        if (!_element)
        {
            return; // inert
        }
        if (!_animation)
        {
            // Built on the FIRST start only: most indicators never run (a cold cache, no draft), so a header or
            // card that never animates allocates nothing. The thread's cached compositor serves when it exists;
            // only the thread's first animation asks its anchor (see CompositorForWeakAnchor).
            _animation = AgentMakeScalarLoop(CompositorForWeakAnchor(_anchor, L"AgentOpacityLoop"), _keyFrames, _cycle, _delay, L"Opacity");
        }
        // XAML's own composition path: it marks the element as opacity-animating (so an element whose static
        // Opacity is 0 is still rendered) and adds no per-frame UI-thread work (no listener on Opacity).
        _element.StartAnimation(_animation);
    }

    void AgentOpacityLoop::Stop()
    {
        if (!_element || !_animation)
        {
            return; // inert, or never started: nothing runs
        }
        // StopBehavior SetToFinalValue lands the element on the loop's last key frame — its still Opacity —
        // and XAML then writes that same value back into UIElement.Opacity (no change). A loop that already
        // ended (XAML cancelled it, or Stop ran twice) answers S_FALSE, which is not an error.
        _element.StopAnimation(_animation);
    }

    AgentOpacityLoop AgentMakeCacheWarmGlow(const winrt::Windows::UI::Xaml::UIElement& anchor, winrt::Windows::UI::Xaml::UIElement glow)
    {
        // The shared constants (AgentAmbientAnimation.h), copied into the loop's own key-frame list: the loop keeps
        // them until its first Start() builds the composition animation. No delay — the crown breathes from its first
        // frame, as the storyboard it replaces did.
        return AgentOpacityLoop{ anchor,
                                 std::move(glow),
                                 std::vector<AgentLoopKeyFrame>(kAgentCacheWarmGlowKeyFrames.begin(), kAgentCacheWarmGlowKeyFrames.end()),
                                 kAgentCacheWarmGlowCycle,
                                 std::chrono::milliseconds{ 0 } };
    }

    AgentDotsPulse::AgentDotsPulse(const winrt::Windows::UI::Xaml::UIElement& anchor, const std::array<winrt::Windows::UI::Xaml::UIElement, 3>& dots)
    {
        for (size_t i = 0; i < _dots.size(); ++i)
        {
            // 1.0 -> 0.3 -> 1.0: the storyboard's 0.3 <-> 1.0 wave, begun and ended on the still 1.0.
            _dots[i] = AgentOpacityLoop{ anchor,
                                         dots[i],
                                         { { 0.0f, 1.0f }, { 0.5f, 0.3f }, { 1.0f, 1.0f } },
                                         kAgentDotsPulseCycle,
                                         kAgentDotsPulseStagger * static_cast<int64_t>(i) };
        }
    }

    void AgentDotsPulse::Start()
    {
        // Try EVERY dot, then rethrow the first failure: one torn-down dot must not leave its neighbors
        // un-started, and the owner still learns that the start did not fully succeed.
        std::exception_ptr first;
        for (auto& dot : _dots)
        {
            try
            {
                dot.Start();
            }
            catch (...)
            {
                if (!first)
                {
                    first = std::current_exception();
                }
            }
        }
        if (first)
        {
            std::rethrow_exception(first);
        }
    }

    void AgentDotsPulse::Stop()
    {
        // The same "try every dot, rethrow the first failure" rule: a dot left pulsing behind a failed
        // neighbor would keep the compositor busy for nothing.
        std::exception_ptr first;
        for (auto& dot : _dots)
        {
            try
            {
                dot.Stop();
            }
            catch (...)
            {
                if (!first)
                {
                    first = std::current_exception();
                }
            }
        }
        if (first)
        {
            std::rethrow_exception(first);
        }
    }

    AgentLockstepBlink::AgentLockstepBlink(const winrt::Windows::UI::Xaml::UIElement& anchor, winrt::Windows::UI::Xaml::UIElement element) :
        _anchor{ anchor ? winrt::make_weak(anchor) : winrt::weak_ref<winrt::Windows::UI::Xaml::UIElement>{} },
        _element{ std::move(element) }
    {
    }

    AgentLockstepBlink::~AgentLockstepBlink()
    {
        // Normally a no-op: the owner's Unloaded / flag change already ran Stop(). This is the belt for an owner
        // destroyed while its ring still blinked (see the declaration). The element is NOT touched — it is being
        // torn down with its owner, possibly on another thread.
        _ReleaseClock();
    }

    AgentLockstepBlink::AgentLockstepBlink(AgentLockstepBlink&& other) noexcept :
        _anchor{ std::move(other._anchor) },
        _element{ std::move(other._element) },
        _clockOwner{ std::exchange(other._clockOwner, nullptr) }
    {
    }

    AgentLockstepBlink& AgentLockstepBlink::operator=(AgentLockstepBlink&& other) noexcept
    {
        if (this != &other)
        {
            // A blink overwritten while it held the clock would otherwise leave one follower counted forever.
            _ReleaseClock();
            _anchor = std::move(other._anchor);
            _element = std::move(other._element);
            _clockOwner = std::exchange(other._clockOwner, nullptr);
        }
        return *this;
    }

    void AgentLockstepBlink::Start()
    {
        if (!_element)
        {
            return; // inert
        }
        auto& thread = CurrentAmbientThread();
        if (!thread.blinkFollow)
        {
            // The thread's first blink: build its clock. Throws before anything is counted.
            EnsureBlinkClock(thread, CompositorForWeakAnchor(_anchor, L"AgentLockstepBlink"));
        }
        // Join the clock once per Start/Stop pair: a restart of a running blink must not count it twice (one
        // Stop() would then leave the clock running for no one).
        const bool joining = (_clockOwner == nullptr);
        if (joining)
        {
            AcquireBlinkClock(thread);
            _clockOwner = &thread;
        }
        try
        {
            // XAML's own composition path, as for AgentOpacityLoop: the element is marked opacity-animating and
            // gets no per-frame UI-thread work. A running blink hands over to this start (same Target), so a
            // restart does not jump: the expression reads the same clock either way.
            _element.StartAnimation(thread.blinkFollow);
        }
        catch (...)
        {
            if (joining)
            {
                // The element follows nothing: do not keep the clock running for it. A restart that failed keeps
                // its earlier hold — the element may still follow from that earlier start.
                _ReleaseClock();
            }
            throw;
        }
    }

    void AgentLockstepBlink::Stop()
    {
        if (!_element || _clockOwner == nullptr)
        {
            return; // inert, or not blinking
        }
        std::exception_ptr failure;
        try
        {
            // A STATIC set of the still value ends the expression: XAML cancels a running facade animation when its
            // property is set, and pushes no value back (CUIElement::OnSetLocalOpacity -> CancelSingleAnimation;
            // in the WinUI source this holds even for an unchanged value). Deliberately NOT
            // UIElement::StopAnimation: an expression has no stop behavior, and XAML would write the clock's value
            // of that moment (0 in the "off" half) into UIElement.Opacity, leaving an element that is invisible
            // whenever it later shows without a blink.
            _element.Opacity(1.0);
        }
        catch (...)
        {
            failure = std::current_exception();
        }
        // Released even when the set failed: a torn-down element follows nothing any more, and a clock held for it
        // would keep the compositor animating for no one.
        _ReleaseClock();
        if (failure)
        {
            std::rethrow_exception(failure);
        }
    }

    void AgentLockstepBlink::_ReleaseClock() noexcept
    {
        // Taken out first, so the hold is given up exactly once whatever happens below.
        auto* const owner = std::exchange(_clockOwner, nullptr);
        if (owner == nullptr)
        {
            return;
        }
        if (owner->threadId == ::GetCurrentThreadId())
        {
            ReleaseBlinkClock(*owner);
            return;
        }
        // Another thread (an owner destroyed off its UI thread): the clock's composition objects belong to the UI
        // thread, so the release is posted there. `owner` is never freed, so the posted call may outlive this
        // object. threadId / queue are read-only after creation, so reading them here is safe.
        try
        {
            if (!owner->queue || !owner->queue.TryEnqueue([owner]() noexcept { ReleaseBlinkClock(*owner); }))
            {
                // No queue, or it is shut down (the app is exiting): the clock goes with the process.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[anim] blink clock release not posted: the UI thread's queue is gone\n");
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"AgentLockstepBlink clock release (post)");
        }
    }

    AgentEmberSparks::AgentEmberSparks(winrt::Windows::UI::Xaml::UIElement host) :
        _host{ std::move(host) }
    {
    }

    void AgentEmberSparks::SetColor(const winrt::Windows::UI::Color& color) noexcept
    {
        try
        {
            _color = color;
            if (_brush)
            {
                // One write recolors all three embers: they share this brush.
                _brush.Color(color);
            }
        }
        catch (...)
        {
            // The embers keep their previous color; the next brush change retries (Rule #18: logged).
            ::Agentmaster::AgentLogCaughtException(L"AgentEmberSparks::SetColor");
        }
    }

    void AgentEmberSparks::_Build()
    {
        // The thread's cached compositor; the host is asked only by the thread's first animation.
        const auto compositor = AgentCompositorOf(_host);
        auto brush = compositor.CreateColorBrush(_color);
        auto container = compositor.CreateContainerVisual();
        container.Size({ kEmberHostSide, kEmberHostSide });

        std::array<WUC::ShapeVisual, 3> embers{ nullptr, nullptr, nullptr };
        std::array<WUC::ScalarKeyFrameAnimation, 3> fades{ nullptr, nullptr, nullptr };
        std::array<WUC::ScalarKeyFrameAnimation, 3> rises{ nullptr, nullptr, nullptr };
        for (size_t i = 0; i < kEmbers.size(); ++i)
        {
            const auto& spec = kEmbers[i];
            const float radius = spec.diameter / 2.0f;

            // An ellipse shape in a visual of exactly its size, placed at the XAML ellipse's Canvas spot.
            auto geometry = compositor.CreateEllipseGeometry();
            geometry.Center({ radius, radius });
            geometry.Radius({ radius, radius });
            auto shape = compositor.CreateSpriteShape(geometry);
            shape.FillBrush(brush);
            auto ember = compositor.CreateShapeVisual();
            ember.Size({ spec.diameter, spec.diameter });
            ember.Shapes().Append(shape);
            ember.Offset({ spec.left, spec.top, 0.0f });
            // Hidden at rest: it shows only while its fade loop runs (the XAML ellipse's base Opacity="0").
            ember.Opacity(0.0f);
            container.Children().InsertAtTop(ember);
            embers[i] = ember;

            // Fade: in fast over the first quarter (an ember appears already glowing), out over the rest. It
            // begins and ends on 0, so a stopped ember lands hidden.
            fades[i] = AgentMakeScalarLoop(compositor, { { 0.0f, 0.0f }, { 0.25f, 1.0f }, { 1.0f, 0.0f } }, kEmberCycle, kEmberStagger[i], nullptr);
            // Rise: restarts at the bottom of its arc every cycle (no reverse) — a spark climbs, it does not
            // sail back down.
            rises[i] = AgentMakeScalarLoop(compositor, { { 0.0f, spec.top + kEmberRiseFrom }, { 1.0f, spec.top + kEmberRiseTo } }, kEmberCycle, kEmberStagger[i], nullptr);
        }

        // Attach last, and keep the pieces only once everything is built: a refusal part-way leaves this
        // object unbuilt, so the next Start() retries the whole build instead of using half of one.
        winrt::Windows::UI::Xaml::Hosting::ElementCompositionPreview::SetElementChildVisual(_host, container);
        _brush = std::move(brush);
        _container = std::move(container);
        _embers = std::move(embers);
        _fades = std::move(fades);
        _rises = std::move(rises);
    }

    void AgentEmberSparks::Start()
    {
        if (!_host)
        {
            return; // inert
        }
        if (!_container)
        {
            _Build(); // the first warm moment of this tab: build its embers now, never before
        }
        for (size_t i = 0; i < _embers.size(); ++i)
        {
            // Visual animations on visuals XAML does not know about: no UI-thread work per frame.
            _embers[i].StartAnimation(L"Opacity", _fades[i]);
            _embers[i].StartAnimation(L"Offset.Y", _rises[i]);
        }
    }

    void AgentEmberSparks::Stop()
    {
        if (!_container)
        {
            return; // never built: nothing runs
        }
        for (size_t i = 0; i < _embers.size(); ++i)
        {
            auto& ember = _embers[i];
            ember.StopAnimation(L"Opacity");
            ember.StopAnimation(L"Offset.Y");
            // Our own visuals, so the resting state is set outright rather than trusted to the stop
            // behavior: hidden, back on its Canvas spot (a direct set also detaches anything still bound).
            ember.Opacity(0.0f);
            ember.Offset({ kEmbers[i].left, kEmbers[i].top, 0.0f });
        }
    }
}
