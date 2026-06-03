// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentTabOverlay.h"

#include "AgentMaster/SessionRegistry.h"

#include <winrt/Windows.UI.h> // Color / ColorHelper / Colors
#include <winrt/Windows.UI.Text.h> // FontWeights
#include <winrt/Windows.UI.Xaml.Documents.h> // Run / Inlines
#include <winrt/Windows.UI.Xaml.Input.h> // PointerRoutedEventArgs
#include <winrt/Windows.UI.Xaml.Media.h> // SolidColorBrush

#include <string>

using namespace winrt::Windows::Foundation;
// Narrow using-DECLARATIONS for the color helpers: a `using namespace winrt::Windows::UI;` would
// also pull the nested `Text` namespace into scope and clash with the Text() helpers (see the
// AgentManagerContent gotcha / CLAUDE.md).
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;
using namespace winrt::Windows::UI::Text; // FontWeights
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Documents; // Run / Inlines
using namespace winrt::Windows::UI::Xaml::Input; // PointerRoutedEventArgs
using namespace winrt::Windows::UI::Xaml::Media; // brushes
using namespace winrt::Windows::System; // DispatcherQueue
using namespace Agentmaster;

namespace
{
    // Geometric, monochrome glyphs only (match the Manager / Triage Board; no wide color emoji).
    constexpr const wchar_t* kDot = L"\x00B7"; // ·
    constexpr const wchar_t* kHourglass = L"\x23F3"; // ⏳
    constexpr const wchar_t* kLink = L"\x26D3"; // ⛓

    SolidColorBrush Fill(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
    {
        return SolidColorBrush{ ColorHelper::FromArgb(a, r, g, b) };
    }

    // Color-matched to the Triage Board (AgentManagerContent's StateColor); kept in sync by hand —
    // a small duplication until the palette is factored into a shared header.
    Color StateColor(SessionState s)
    {
        using winrt::Windows::UI::Colors;
        switch (s)
        {
        case SessionState::Running:
            return Colors::DodgerBlue();
        case SessionState::WaitingForInput:
            return Colors::Goldenrod();
        case SessionState::NeedsApproval:
            return Colors::OrangeRed();
        case SessionState::Error:
            return Colors::Crimson();
        case SessionState::Done:
            return Colors::MediumSeaGreen();
        case SessionState::Idle:
        default:
            return Colors::Gray();
        }
    }

    const wchar_t* StateGlyph(SessionState s)
    {
        switch (s)
        {
        case SessionState::Running:
            return L"\x25CF"; // ●
        case SessionState::WaitingForInput:
            return L"\x25D0"; // ◐
        case SessionState::NeedsApproval:
            return L"\x26A0"; // ⚠
        case SessionState::Error:
            return L"\x2715"; // ✕
        case SessionState::Done:
            return L"\x2713"; // ✓
        case SessionState::Idle:
        default:
            return L"\x25CB"; // ○
        }
    }

    const wchar_t* StateLabel(SessionState s)
    {
        switch (s)
        {
        case SessionState::Running:
            return L"running";
        case SessionState::WaitingForInput:
            return L"waiting";
        case SessionState::NeedsApproval:
            return L"needs-approval";
        case SessionState::Error:
            return L"error";
        case SessionState::Done:
            return L"done";
        case SessionState::Idle:
        default:
            return L"idle";
        }
    }

    const wchar_t* ModeLabel(AutopilotMode m)
    {
        switch (m)
        {
        case AutopilotMode::SemiAuto:
            return L"Semi";
        case AutopilotMode::Full:
            return L"Full";
        case AutopilotMode::Off:
        default:
            return L"Off";
        }
    }
}

namespace winrt::TerminalApp::implementation
{
    AgentTabOverlay::AgentTabOverlay()
    {
        _dispatcher = DispatcherQueue::GetForCurrentThread();

        _line = TextBlock{};
        _line.FontSize(12);
        _line.Foreground(Fill(0xFF, 0xEC, 0xEC, 0xEC));
        _line.IsTextSelectionEnabled(false);
        _line.TextWrapping(TextWrapping::NoWrap);

        _root = Border{};
        _root.Background(Fill(0xCC, 0x20, 0x20, 0x20)); // dark translucent so it reads on any terminal
        _root.BorderBrush(Fill(0x40, 0xFF, 0xFF, 0xFF));
        _root.BorderThickness(ThicknessHelper::FromUniformLength(1));
        _root.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        _root.Padding(ThicknessHelper::FromLengths(7, 2, 7, 2));
        _root.Opacity(0.55); // dim at rest; full on hover (the chosen interaction)
        _root.Child(_line);

        _root.PointerEntered([](const IInspectable& sender, const PointerRoutedEventArgs&) {
            if (const auto b = sender.try_as<Border>())
            {
                b.Opacity(1.0);
            }
        });
        _root.PointerExited([](const IInspectable& sender, const PointerRoutedEventArgs&) {
            if (const auto b = sender.try_as<Border>())
            {
                b.Opacity(0.55);
            }
        });
    }

    AgentTabOverlay::~AgentTabOverlay()
    {
        _Detach();
    }

    void AgentTabOverlay::_Detach()
    {
        if (_registry && _observerToken)
        {
            _registry->RemoveObserver(_observerToken);
            _observerToken = 0;
        }
    }

    void AgentTabOverlay::Initialize(const std::wstring& sessionId, std::shared_ptr<::Agentmaster::SessionRegistry> registry)
    {
        _sessionId = sessionId;
        _registry = std::move(registry);
        if (!_dispatcher)
        {
            _dispatcher = DispatcherQueue::GetForCurrentThread();
        }
        if (_registry)
        {
            auto weak = get_weak();
            auto disp = _dispatcher;
            const std::wstring id = _sessionId;
            // Id-filtered observer marshaled to THIS UI thread; self-detached in the dtor (Rule #10).
            _observerToken = _registry->AddObserver([weak, disp, id](const SessionInfo& s, HookEvent) {
                if (s.id != id)
                {
                    return;
                }
                if (disp)
                {
                    disp.TryEnqueue([weak]() {
                        if (auto self = weak.get())
                        {
                            self->_Refresh();
                        }
                    });
                }
            });
        }
        _Refresh();
    }

    void AgentTabOverlay::_Refresh()
    {
        if (!_line || !_registry)
        {
            return;
        }
        const auto info = _registry->Get(_sessionId);
        if (!info)
        {
            if (_root)
            {
                _root.Visibility(Visibility::Collapsed);
            }
            return;
        }
        if (_root)
        {
            _root.Visibility(Visibility::Visible);
        }
        const auto& s = *info;

        int pending = 0;
        for (const auto& p : s.queue)
        {
            if (p.status == PromptStatus::Pending)
            {
                ++pending;
            }
        }

        const bool bound = _registry->HasInjector(_sessionId);
        const std::wstring link = bound ? (std::wstring{ kLink } + L" linked") :
                                          (s.external ? std::wstring{ L"observe" } : std::wstring{ L"unlinked" });

        std::wstring rest = L" ";
        rest += StateLabel(s.state);
        rest += L"  ";
        rest += kDot;
        rest += L"  ";
        rest += ModeLabel(s.autopilot.mode);
        if (pending > 0)
        {
            rest += L"  ";
            rest += kDot;
            rest += L"  ";
            rest += kHourglass;
            rest += std::to_wstring(pending);
        }
        rest += L"  ";
        rest += kDot;
        rest += L"  ";
        rest += link;

        _line.Inlines().Clear();
        Run glyph{};
        glyph.Text(winrt::hstring{ StateGlyph(s.state) });
        glyph.Foreground(SolidColorBrush{ StateColor(s.state) });
        glyph.FontWeight(FontWeights::SemiBold());
        _line.Inlines().Append(glyph);
        Run text{};
        text.Text(winrt::hstring{ rest });
        _line.Inlines().Append(text);
    }
}
