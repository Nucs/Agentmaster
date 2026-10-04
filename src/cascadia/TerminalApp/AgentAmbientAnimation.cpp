// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the ambient animations (AgentAmbientAnimation.h has the design: why composition, why the
// two paths, why every loop begins and ends on its still value). This file is the whole implementation, and
// the ONE translation unit that includes the full Windows.UI.Composition projection:
//   * AgentCompositorOf / AgentMakeScalarLoop — the compositor lookup and the linear forever-loop builder;
//   * AgentOpacityLoop                       — one XAML element's Opacity, through UIElement::StartAnimation;
//   * AgentDotsPulse                         — the three staggered dot loops of an unsent-draft cluster;
//   * AgentEmberSparks                       — the spark crown's rising embers, as composition visuals.
#include "pch.h"
#include "AgentAmbientAnimation.h"

#include "AgentCatchLog.h" // AgentLogCaughtException — Rule #18 forensics for the one swallowing site (SetColor)

#include <exception> // std::exception_ptr — "try every element, rethrow the first failure"

#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>

namespace
{
    namespace WUC = winrt::Windows::UI::Composition;

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
}

namespace winrt::TerminalApp::implementation
{
    WUC::Compositor AgentCompositorOf(const winrt::Windows::UI::Xaml::UIElement& anchor)
    {
        return winrt::Windows::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(anchor).Compositor();
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
            // Built on the FIRST start only: most indicators never run (a cold cache, no draft), so a
            // header or card that never animates allocates nothing, and the anchor gets its hand-out visual
            // only when something really runs.
            const auto anchor = _anchor.get();
            if (!anchor)
            {
                // The anchor went away before the first start (a torn-down card): there is no compositor
                // to build from. Throw like every other refusal, so the owner logs it and keeps its
                // "not running" state.
                throw winrt::hresult_error(E_ABORT, L"AgentOpacityLoop: the anchor element is gone");
            }
            _animation = AgentMakeScalarLoop(AgentCompositorOf(anchor), _keyFrames, _cycle, _delay, L"Opacity");
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
