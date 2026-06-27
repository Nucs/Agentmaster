// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster (DEV ONLY) — append a unique element identifier as the FIRST ROW of every
// tooltip, so the user can hover ANY control, read a short token, and tell the assistant
// exactly which control they mean (then the assistant greps the codebase for it in seconds).
//
// GATING: runtime, to the AgentmasterDev package identity ONLY (Profiles::IsDevPackage()).
// The shipped release package is "Agentmaster" (no "Dev" prefix), so InstallDevTooltipNames
// returns immediately there — the feature is invisible in release exactly as required. This
// matches the project's other dev-vs-release seams (e.g. _AgentmasterReopenTarget), which
// branch on the identity at runtime rather than on a compile flag.
//
// MECHANISM: ONE bubbling PointerMoved handler on the window's Root grid (TerminalPage.xaml's
// x:Name="Root") — title bar, tab strip, toolbar, Manager tab, settings, and the terminal
// panes all live under it. On a hover over a NEW leaf we walk up from args.OriginalSource and:
//   * find the NEAREST tooltip-bearing ancestor — the element whose tooltip the framework will
//     actually show — and prepend our first row to ITS tooltip (so we AUGMENT real tooltips,
//     never shadow them with a new one on a closer child); else
//   * find the nearest interactive (Control) or x:Named element and attach a name-only tip.
// The injected first row is idempotent (a marker sentinel begins it; we strip + rebuild from
// the recovered original, and skip when already correct), so re-renders / re-hovers never
// stack it.
//
// THE NAME (optimized for "identify in a matter of seconds"):
//   * x:Name when set            -> e.g.  "⟦id⟧ ReopenButton  (Button)"   [grep x:Name / member]
//   * else AutomationId / AutomationName
//   * else  Type «text-hint» ◂ <nearest x:Named ancestor>
//            e.g. "⟦id⟧ Button «Reopen Windows» ◂ TabContent"   [grep the literal tooltip text]
// Most Agentmaster controls are created in C++ without an x:Name, but they DO carry a
// descriptive tooltip (via AgentSetTip) — and that text is a source string literal, so the
// synthesized form stays trivially greppable.
//
// WinRT XAML types, so this lives in TerminalApp (not the plain-C++ AgentMaster/ engine dir)
// and is included from .cpp TUs only — it relies on the pch projections, like AgentTipHelpers.h.

#pragma once

#include <memory> // shared_ptr holder for the per-handler "last leaf" throttle
#include <string>

#include "AgentMaster/ProfileBootstrap.h" // Profiles::IsDevPackage — the dev-vs-release gate

namespace winrt::TerminalApp::implementation
{
    namespace devtip
    {
        // Visible marker that BEGINS our injected first row, and doubles as the idempotency
        // sentinel (no real tooltip starts with it). U+27E6/U+27E7 = mathematical white square
        // brackets: "⟦id⟧ ".
        inline constexpr std::wstring_view kMarker{ L"⟦id⟧ " };

        inline bool StartsWith(std::wstring_view s, std::wstring_view p) noexcept
        {
            return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
        }

        // The leaf class segment of a runtime class name, e.g.
        // "Windows.UI.Xaml.Controls.Button" -> "Button".
        inline std::wstring LastTypeSegment(const winrt::hstring& full)
        {
            const std::wstring_view s{ full };
            const auto pos = s.rfind(L'.');
            return std::wstring{ pos == std::wstring_view::npos ? s : s.substr(pos + 1) };
        }

        // Collapse whitespace runs to single spaces, trim, and cap at n chars (…-elided). Used
        // for the text hint so a multi-line tooltip / content stays a single tidy first-row tail.
        inline std::wstring Tidy(std::wstring_view in, size_t n)
        {
            std::wstring out;
            out.reserve(in.size());
            bool pendingSpace = false;
            for (const wchar_t c : in)
            {
                if (c == L'\n' || c == L'\r' || c == L'\t' || c == L' ')
                {
                    pendingSpace = !out.empty();
                    continue;
                }
                if (pendingSpace)
                {
                    out.push_back(L' ');
                    pendingSpace = false;
                }
                out.push_back(c);
            }
            if (out.size() > n)
            {
                out.resize(n);
                out.push_back(L'…'); // …
            }
            return out;
        }

        // Unwrap a tooltip value to its string, if it is one (a boxed hstring). Empty otherwise.
        inline winrt::hstring AsString(const winrt::Windows::Foundation::IInspectable& o)
        {
            return o ? winrt::unbox_value_or<winrt::hstring>(o, L"") : winrt::hstring{};
        }

        // The nearest x:Named ancestor's name (for disambiguating an un-named leaf), or "".
        inline std::wstring NamedAncestor(winrt::Windows::UI::Xaml::DependencyObject node)
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            for (int i = 0; i < 24 && node; ++i)
            {
                node = WUX::Media::VisualTreeHelper::GetParent(node);
                if (const auto fe = node.try_as<WUX::FrameworkElement>())
                {
                    if (!fe.Name().empty())
                    {
                        return std::wstring{ fe.Name() };
                    }
                }
            }
            return {};
        }

        // A greppable content hint for an un-named element: its TextBlock text or a stringy
        // ContentControl content. The caller prefers the element's existing tooltip text first
        // (the most descriptive thing for an Agentmaster button), falling back to this.
        inline std::wstring TextHint(const winrt::Windows::UI::Xaml::FrameworkElement& fe)
        {
            namespace WUXC = winrt::Windows::UI::Xaml::Controls;
            if (const auto tb = fe.try_as<WUXC::TextBlock>())
            {
                return std::wstring{ tb.Text() };
            }
            if (const auto cc = fe.try_as<WUXC::ContentControl>())
            {
                return std::wstring{ winrt::unbox_value_or<winrt::hstring>(cc.Content(), L"") };
            }
            return {};
        }

        // Build the first-row identifier (WITHOUT the marker). hintFallback is the element's
        // recovered original tooltip text (preferred hint when the element has no x:Name).
        inline std::wstring ComputeName(const winrt::Windows::UI::Xaml::FrameworkElement& fe,
                                        std::wstring_view hintFallback)
        {
            namespace WUXA = winrt::Windows::UI::Xaml::Automation;
            const auto type = LastTypeSegment(winrt::get_class_name(fe));

            // 1. x:Name — the ideal: short, unique, directly greppable as x:Name="…" / a member.
            if (const auto name = fe.Name(); !name.empty())
            {
                return std::wstring{ name } + L"  (" + type + L")";
            }

            // 2/3. automation id/name, then the tooltip/content text, as the disambiguating hint.
            std::wstring hint{ WUXA::AutomationProperties::GetAutomationId(fe) };
            if (hint.empty())
            {
                hint = std::wstring{ WUXA::AutomationProperties::GetName(fe) };
            }
            if (hint.empty())
            {
                hint = Tidy(hintFallback, 48);
            }
            if (hint.empty())
            {
                hint = Tidy(TextHint(fe), 48);
            }

            std::wstring res = type;
            if (!hint.empty())
            {
                res += L" «" + hint + L"»"; // «hint»
            }
            if (const auto anc = NamedAncestor(fe); !anc.empty())
            {
                res += L" ◂ " + anc; // ◂ ancestor
            }
            return res;
        }
    }

    // Install the dev-only tooltip-name injector on a window's Root element. No-op outside the
    // AgentmasterDev package, and no-op on a null root. Idempotent per element + cheap at rest
    // (one identity compare per pointer move, real work only on a new leaf).
    inline void InstallDevTooltipNames(const winrt::Windows::UI::Xaml::FrameworkElement& root)
    {
        if (!::Agentmaster::Profiles::IsDevPackage() || !root)
        {
            return;
        }

        namespace WUX = winrt::Windows::UI::Xaml;
        namespace WUXC = WUX::Controls;
        using winrt::Windows::Foundation::IInspectable;
        using winrt::Windows::Foundation::IReference;

        // The leaf we last processed — an identity throttle so a still / dragging pointer (e.g. a
        // terminal selection) does no work after the first move over it. The desired== guard
        // below is the real idempotency; this is only an optimization. (operator== on two
        // IInspectables is COM-identity, per cppwinrt's operator==(IUnknown, IUnknown).)
        auto lastLeaf = std::make_shared<IInspectable>(nullptr);

        root.PointerMoved([lastLeaf](const IInspectable& /*sender*/, const WUX::Input::PointerRoutedEventArgs& args) {
            const auto src = args.OriginalSource();
            if (!src)
            {
                return;
            }
            if (*lastLeaf && *lastLeaf == src)
            {
                return; // same leaf as the previous move
            }
            *lastLeaf = src;

            // Walk leaf -> up: stop at the FIRST tooltip-bearing element (whose tip the framework
            // shows — we augment THAT, never a closer child), remembering the first interactive /
            // x:Named element as the fallback for the no-tooltip-anywhere case.
            WUX::DependencyObject node = src.try_as<WUX::DependencyObject>();
            WUX::FrameworkElement ttOwner{ nullptr };
            IInspectable existing{ nullptr };
            WUX::FrameworkElement fallback{ nullptr };
            for (int i = 0; i < 24 && node; ++i)
            {
                if (const auto fe = node.try_as<WUX::FrameworkElement>())
                {
                    if (const auto tip = WUXC::ToolTipService::GetToolTip(fe))
                    {
                        ttOwner = fe;
                        existing = tip;
                        break;
                    }
                    if (!fallback && (fe.try_as<WUXC::Control>() || !fe.Name().empty()))
                    {
                        fallback = fe;
                    }
                }
                node = WUX::Media::VisualTreeHelper::GetParent(node);
            }

            const auto target = ttOwner ? ttOwner : fallback;
            if (!target)
            {
                return; // pure layout chrome with no tooltip and nothing nameable — leave it
            }

            // Recover the element's CURRENT tooltip text + the writable handle to it. We support
            // the three shapes used across the app: a boxed-string tip, a ToolTip whose Content
            // is a boxed string (AgentSetTip), and a ToolTip whose Content is a TextBlock (Tab).
            winrt::hstring origText;
            WUXC::ToolTip ttObj{ nullptr };
            WUXC::TextBlock ttText{ nullptr };
            bool stringTip = false;
            if (existing)
            {
                if ((ttObj = existing.try_as<WUXC::ToolTip>()))
                {
                    const auto content = ttObj.Content();
                    if ((ttText = content.try_as<WUXC::TextBlock>()))
                    {
                        origText = ttText.Text();
                    }
                    else
                    {
                        origText = devtip::AsString(content);
                    }
                }
                else
                {
                    origText = devtip::AsString(existing);
                    stringTip = true;
                }
            }

            // Strip a previously-injected first row to recover the TRUE original text.
            std::wstring original{ origText };
            if (devtip::StartsWith(original, devtip::kMarker))
            {
                const auto nl = original.find(L'\n');
                original = (nl == std::wstring::npos) ? std::wstring{} : original.substr(nl + 1);
            }

            std::wstring desired{ devtip::kMarker };
            desired += devtip::ComputeName(target, original);
            if (!original.empty())
            {
                desired += L'\n';
                desired += original;
            }

            if (std::wstring{ origText } == desired)
            {
                return; // already correct — nothing to write
            }

            const winrt::hstring boxed{ desired };
            if (!existing)
            {
                // No tooltip anywhere above: add a name-only tip to the interactive/named element.
                WUXC::ToolTipService::SetToolTip(target, winrt::box_value(boxed));
            }
            else if (ttObj)
            {
                if (ttText)
                {
                    ttText.Text(boxed);
                }
                else if (!ttObj.Content() || ttObj.Content().try_as<IReference<winrt::hstring>>())
                {
                    ttObj.Content(winrt::box_value(boxed));
                }
                // else: a ToolTip with non-text content — leave it untouched (rare).
            }
            else if (stringTip)
            {
                WUXC::ToolTipService::SetToolTip(target, winrt::box_value(boxed));
            }
        });
    }
}
