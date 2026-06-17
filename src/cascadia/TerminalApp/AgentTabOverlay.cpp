// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AgentTabOverlay.h"

#include "AgentStatusColors.h" // the ONE shared state->color palette (board / overlay / tab dot)
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

    // Color-matched to the Triage Board — now via the ONE shared palette (AgentStatusColors.h, the
    // factoring the old hand-synced copy's comment promised), so the overlay badge and the
    // tab-strip status dot can never drift apart.
    Color StateColor(SessionState s)
    {
        return winrt::TerminalApp::implementation::AgentStatusColorFor(s);
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
        _line.HorizontalAlignment(HorizontalAlignment::Right); // keep the right edge aligned when row 2 is wider

        // Row 2: "<root workdir folder>/<branch>" — secondary (smaller + dimmer), right-aligned to the
        // badge edge, width-capped + ellipsized so a long branch path can't balloon the HUD. Collapsed
        // until _Refresh() fills it; stays collapsed on the registry-less observe badge (no session).
        _subline = TextBlock{};
        _subline.FontSize(11);
        _subline.Foreground(Fill(0xFF, 0xB0, 0xB0, 0xB0));
        _subline.IsTextSelectionEnabled(false);
        _subline.TextWrapping(TextWrapping::NoWrap);
        _subline.TextTrimming(TextTrimming::CharacterEllipsis);
        _subline.HorizontalAlignment(HorizontalAlignment::Right);
        _subline.MaxWidth(380);
        _subline.Margin(ThicknessHelper::FromLengths(0, 1, 0, 0));
        _subline.Visibility(Visibility::Collapsed);

        StackPanel stack{};
        stack.Orientation(Orientation::Vertical);
        stack.Children().Append(_line);
        stack.Children().Append(_subline);

        _root = Border{};
        _root.Background(Fill(0xCC, 0x20, 0x20, 0x20)); // dark translucent so it reads on any terminal
        _root.BorderBrush(Fill(0x40, 0xFF, 0xFF, 0xFF));
        _root.BorderThickness(ThicknessHelper::FromUniformLength(1));
        _root.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        _root.Padding(ThicknessHelper::FromLengths(7, 2, 7, 2));
        _root.Opacity(0.55); // dim at rest; full on hover (the chosen interaction)
        _root.Child(stack);

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

    void AgentTabOverlay::ShowActivity(const std::wstring& kind)
    {
        // A tab the observer classified but that is NOT a linked Claude session: a shell ("pwsh" /
        // "cmd"), a never-prompted claude ("claude", no transcript id yet, §11d), or codex. Registry-
        // LESS static badge (no id to observe). The real Initialize()-bound overlay replaces this whole
        // element once a claude resolves its conversation id (first prompt).
        _pending = true;
        _sessionId.clear();
        if (!_dispatcher)
        {
            _dispatcher = DispatcherQueue::GetForCurrentThread();
        }
        if (!_line || !_root)
        {
            return;
        }
        if (kind == _lastActivitySig)
        {
            return; // unchanged -> no XAML churn (this runs every probe tick)
        }
        _lastActivitySig = kind;
        _root.Visibility(Visibility::Visible);
        _line.Inlines().Clear();
        Run glyph{};
        glyph.Text(winrt::hstring{ L"\x25CB" }); // ○ gray — observed, but not a linked session
        glyph.Foreground(Fill(0xFF, 0x9E, 0x9E, 0x9E)); // gray
        glyph.FontWeight(FontWeights::SemiBold());
        _line.Inlines().Append(glyph);
        Run text{};
        text.Text(winrt::hstring{ std::wstring{ L" " } + kind + L"  " + kDot + L"  unlinked" });
        _line.Inlines().Append(text);
    }

    void AgentTabOverlay::_Refresh()
    {
        if (_pending)
        {
            return; // a pending badge is static — it has no registry session to refresh from
        }
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
        // model · effort · kind adornment (O6, Fleet Observer enrichment): only the parts we know.
        {
            std::wstring me;
            const auto addPart = [&](const std::wstring& part) {
                if (part.empty())
                {
                    return;
                }
                if (!me.empty())
                {
                    me += L" ";
                    me += kDot;
                    me += L" ";
                }
                me += part;
            };
            addPart(s.model);
            addPart(s.effort);
            if (s.background)
            {
                addPart(L"bg");
            }
            if (!me.empty())
            {
                rest += L"  ";
                rest += kDot;
                rest += L"  ";
                rest += me;
            }
        }
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

        // Row 2: "<root workdir folder>/<branch>" — the leaf of the session's working dir joined with
        // its git branch (e.g. C:/folder/myworkdir + "feature/issue123" -> "myworkdir/feature/issue123").
        // Prefer the persisted M-axis workingDir (the dir the session belongs to); fall back to the live
        // PEB cwd. Hidden when neither a folder nor a branch is known.
        if (_subline)
        {
            std::wstring dir = !s.workingDir.empty() ? s.workingDir : s.liveCwd;
            while (!dir.empty() && (dir.back() == L'/' || dir.back() == L'\\'))
            {
                dir.pop_back(); // strip trailing separators so the leaf isn't empty
            }
            std::wstring leaf = dir;
            if (const auto pos = dir.find_last_of(L"/\\"); pos != std::wstring::npos)
            {
                leaf = dir.substr(pos + 1);
            }
            std::wstring sub = leaf;
            if (!s.branch.empty())
            {
                sub = sub.empty() ? s.branch : (sub + L"/" + s.branch);
            }
            if (sub.empty())
            {
                _subline.Visibility(Visibility::Collapsed);
            }
            else
            {
                _subline.Text(winrt::hstring{ sub });
                _subline.Visibility(Visibility::Visible);
            }
        }
    }
}
