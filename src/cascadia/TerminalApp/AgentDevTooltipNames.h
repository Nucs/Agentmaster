// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster (DEV ONLY) — append a unique element ID as the FIRST ROW of every tooltip, so the
// user can hover ANY control, read its id, and tell the assistant exactly which control they mean.
//
// GATING: runtime, to the AgentmasterDev package identity ONLY (Profiles::IsDevPackage()). The
// shipped release package is "Agentmaster" (no "Dev" prefix), so InstallDevTooltipNames returns
// immediately there — invisible in release, as required. Matches the project's other dev/release
// seams (e.g. _AgentmasterReopenTarget).
//
// WHAT IT SHOWS (the id of the element you are POINTING AT — not its tooltip text):
//   * x:Name when the element has one      -> e.g.  "⟦id⟧ ReopenButton · Button"   (grep x:Name)
//   * otherwise a deterministic generated id -> e.g. "⟦id⟧ am-7f3a2c · Button"
// The generated id is a stable hash of the element's TYPE + structural tree-path + content +
// automation id (NOT its tooltip text — that was the earlier "duplicate" bug). Because a runtime
// hash isn't greppable, each newly-seen generated id is written ONCE to a dev log in the active
// profile (devtooltip-ids.log) mapping  id -> {type, tooltip, content, automationId, ancestor
// path}. So when the user reads "am-7f3a2c" off a control and names it, the assistant greps that
// log to recover the control's real identity. (Elements that already have an x:Name need no log —
// the name itself is greppable.)
//
// MECHANISM: ONE bubbling PointerMoved handler on the window's Root grid — covers the whole window
// tree (title bar, tab strip, toolbar, Manager tab, Settings editor, terminal panes). On a hover
// we resolve the MEANINGFUL element under the pointer (the nearest Control / x:Named / tooltip-
// bearing ancestor — i.e. "the thing you're pointing at", never an inner TextBlock/Border), then:
//   * augment its existing tooltip with the id first row, or add an id-only tip if it had none.
// Anti-flicker: (a) we throttle on the RESOLVED TARGET, so jiggling within one control does no
// work; (b) every tip we touch/create is made IsHitTestVisible(false), so the pointer never lands
// on the tip popup (the pointer-enters-tip -> element-exits -> tip-hides -> reopens flicker loop).
// Idempotent (a marker sentinel begins our row; we rebuild from the recovered original and skip
// when already correct).
//
// WinRT XAML types, so this lives in TerminalApp (not the plain-C++ AgentMaster/ engine dir) and is
// included from .cpp TUs only — it relies on the pch projections, like AgentTipHelpers.h.

#pragma once

#include <filesystem> // dev-log path
#include <fstream> // dev-log append
#include <memory> // shared_ptr holders (throttle + logged-id set)
#include <mutex> // once_flag for the log header
#include <string>
#include <unordered_set> // log each generated id once per session

#include "AgentMaster/ProfileBootstrap.h" // Profiles::IsDevPackage (gate) + ResolveProfileDir (log) + Fnv1a64 (id hash)

namespace winrt::TerminalApp::implementation
{
    namespace devtip
    {
        // Visible marker that BEGINS our injected first row + the idempotency sentinel ("⟦id⟧ ").
        inline constexpr std::wstring_view kMarker{ L"⟦id⟧ " };

        inline bool StartsWith(std::wstring_view s, std::wstring_view p) noexcept
        {
            return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
        }

        // "Windows.UI.Xaml.Controls.Button" -> "Button".
        inline std::wstring ClassLeaf(const winrt::hstring& full)
        {
            const std::wstring_view s{ full };
            const auto pos = s.rfind(L'.');
            return std::wstring{ pos == std::wstring_view::npos ? s : s.substr(pos + 1) };
        }

        // Collapse whitespace runs to single spaces, trim, cap at n (…-elided). For log tidiness.
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
                out.push_back(L'…');
            }
            return out;
        }

        // 6 lowercase-hex chars of a 64-bit value (low 24 bits) — the generated-id suffix.
        inline std::wstring Hex6(uint64_t h)
        {
            static constexpr wchar_t d[] = L"0123456789abcdef";
            std::wstring s(6, L'0');
            for (int i = 5; i >= 0; --i)
            {
                s[static_cast<size_t>(i)] = d[h & 0xF];
                h >>= 4;
            }
            return s;
        }

        inline std::string Utf8(const std::wstring& w)
        {
            if (w.empty())
            {
                return {};
            }
            const int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(len), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
            return s;
        }

        // Boxed-string tooltip value -> its text, else "".
        inline winrt::hstring AsString(const winrt::Windows::Foundation::IInspectable& o)
        {
            return o ? winrt::unbox_value_or<winrt::hstring>(o, L"") : winrt::hstring{};
        }

        // The element's OWN content/text (NOT its tooltip) — a hash + log input, never displayed
        // as the id (that was the duplicate bug).
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

        // The child-index path from a stable point down to the element ("3/1/0/2") — makes the
        // generated id unique per tree position even for content-identical siblings (board cards).
        inline std::wstring StructuralPath(winrt::Windows::UI::Xaml::DependencyObject node)
        {
            using VTH = winrt::Windows::UI::Xaml::Media::VisualTreeHelper;
            std::wstring path;
            for (int i = 0; i < 24 && node; ++i)
            {
                const auto parent = VTH::GetParent(node);
                if (!parent)
                {
                    break;
                }
                int idx = -1;
                const int n = VTH::GetChildrenCount(parent);
                for (int c = 0; c < n; ++c)
                {
                    if (VTH::GetChild(parent, c) == node)
                    {
                        idx = c;
                        break;
                    }
                }
                path = std::to_wstring(idx) + (path.empty() ? std::wstring{} : (L"/" + path));
                node = parent;
            }
            return path;
        }

        // Human-readable ancestor chain ("Root>TabContent>Grid>Button") for the dev log, so the
        // assistant can place a generated id without an x:Name.
        inline std::wstring AncestorChain(winrt::Windows::UI::Xaml::DependencyObject node)
        {
            namespace WUX = winrt::Windows::UI::Xaml;
            using VTH = WUX::Media::VisualTreeHelper;
            std::wstring chain;
            for (int i = 0; i < 24 && node; ++i)
            {
                if (const auto fe = node.try_as<WUX::FrameworkElement>())
                {
                    const auto nm = fe.Name();
                    const auto seg = nm.empty() ? ClassLeaf(winrt::get_class_name(fe)) : std::wstring{ nm };
                    chain = chain.empty() ? seg : (seg + L">" + chain);
                }
                node = VTH::GetParent(node);
            }
            return chain;
        }

        inline const std::wstring& LogPath()
        {
            static const std::wstring p = ::Agentmaster::Profiles::ResolveProfileDir() + L"\\devtooltip-ids.log";
            return p;
        }

        // Truncate + header ONCE per process, so the log reflects the current run's id assignments.
        inline void EnsureLogHeader()
        {
            static std::once_flag once;
            std::call_once(once, []() {
                try
                {
                    std::ofstream o(std::filesystem::path{ LogPath() }, std::ios::trunc | std::ios::binary);
                    o << "# Agentmaster dev tooltip id-map: id\\ttype\\ttip\\tcontent\\taid\\tpath\n";
                }
                catch (...)
                {
                }
            });
        }

        inline void LogId(const std::wstring& id, const std::wstring& type, const std::wstring& tip, const std::wstring& content, const std::wstring& aid, const std::wstring& path)
        {
            try
            {
                std::ofstream o(std::filesystem::path{ LogPath() }, std::ios::app | std::ios::binary);
                o << Utf8(id) << '\t' << Utf8(type) << "\ttip=" << Utf8(Tidy(tip, 200)) << "\tcontent=" << Utf8(Tidy(content, 120)) << "\taid=" << Utf8(aid) << "\tpath=" << Utf8(path) << '\n';
            }
            catch (...)
            {
            }
        }
    }

    // Install the dev-only tooltip-id injector on a window's Root element. No-op outside the
    // AgentmasterDev package / on a null root.
    inline void InstallDevTooltipNames(const winrt::Windows::UI::Xaml::FrameworkElement& root)
    {
        if (!::Agentmaster::Profiles::IsDevPackage() || !root)
        {
            return;
        }

        namespace WUX = winrt::Windows::UI::Xaml;
        namespace WUXC = WUX::Controls;
        namespace WUXA = WUX::Automation;
        using winrt::Windows::Foundation::IInspectable;
        using winrt::Windows::Foundation::IReference;

        devtip::EnsureLogHeader();

        // Throttle on the RESOLVED TARGET (not the raw leaf), so jiggling within one control does
        // nothing. Plus a per-session set so each generated id is logged once.
        auto lastTarget = std::make_shared<WUX::FrameworkElement>(nullptr);
        auto logged = std::make_shared<std::unordered_set<std::wstring>>();

        root.PointerMoved([lastTarget, logged](const IInspectable& /*sender*/, const WUX::Input::PointerRoutedEventArgs& args) {
            const auto src = args.OriginalSource();
            if (!src)
            {
                return;
            }

            // Resolve the meaningful element being pointed at: the nearest ancestor (incl. the leaf)
            // that is a Control, OR is x:Named, OR already owns a tooltip — i.e. the control, never
            // an inner TextBlock/Border. Bail if we somehow land inside a ToolTip popup.
            WUX::DependencyObject node = src.try_as<WUX::DependencyObject>();
            WUX::FrameworkElement target{ nullptr };
            IInspectable existing{ nullptr };
            for (int i = 0; i < 24 && node; ++i)
            {
                if (node.try_as<WUXC::ToolTip>())
                {
                    return;
                }
                if (const auto fe = node.try_as<WUX::FrameworkElement>())
                {
                    const auto tip = WUXC::ToolTipService::GetToolTip(fe);
                    if (tip || fe.try_as<WUXC::Control>() || !fe.Name().empty())
                    {
                        target = fe;
                        existing = tip;
                        break;
                    }
                }
                node = WUX::Media::VisualTreeHelper::GetParent(node);
            }
            if (!target)
            {
                return;
            }
            if (*lastTarget && *lastTarget == target)
            {
                return; // same control as last move
            }
            *lastTarget = target;

            // Recover the target's CURRENT tooltip text + the writable handle. Three shapes:
            // a boxed-string tip, a ToolTip with boxed-string Content (AgentSetTip), a ToolTip with
            // a TextBlock Content (Tab).
            winrt::hstring origText;
            WUXC::ToolTip ttObj{ nullptr };
            WUXC::TextBlock ttText{ nullptr };
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
                }
            }

            // Strip a previously-injected id row to recover the TRUE original tooltip text.
            std::wstring original{ origText };
            if (devtip::StartsWith(original, devtip::kMarker))
            {
                const auto nl = original.find(L'\n');
                original = (nl == std::wstring::npos) ? std::wstring{} : original.substr(nl + 1);
            }

            const auto type = devtip::ClassLeaf(winrt::get_class_name(target));

            // The id: real x:Name (greppable) OR a deterministic generated id (logged for lookup).
            std::wstring id;
            if (const auto nm = target.Name(); !nm.empty())
            {
                id = std::wstring{ nm };
            }
            else
            {
                const std::wstring aid{ WUXA::AutomationProperties::GetAutomationId(target) };
                const auto content = devtip::TextHint(target);
                const auto path = devtip::StructuralPath(target);
                const auto sig = type + L"|" + aid + L"|" + content + L"|" + original + L"|" + path;
                id = L"am-" + devtip::Hex6(::Agentmaster::Profiles::detail::Fnv1a64(sig));
                if (logged->insert(id).second)
                {
                    devtip::LogId(id, type, original, content, aid, devtip::AncestorChain(target));
                }
            }

            std::wstring desired{ devtip::kMarker };
            desired += id;
            desired += L" · ";
            desired += type;
            if (!original.empty())
            {
                desired += L'\n';
                desired += original;
            }

            if (std::wstring{ origText } == desired)
            {
                return; // already correct
            }

            const winrt::hstring boxed{ desired };
            if (ttObj)
            {
                ttObj.IsHitTestVisible(false); // anti-flicker: the tip popup never eats the pointer
                if (ttText)
                {
                    ttText.Text(boxed);
                }
                else if (!ttObj.Content() || ttObj.Content().try_as<IReference<winrt::hstring>>())
                {
                    ttObj.Content(winrt::box_value(boxed));
                }
                // else: a ToolTip with non-text content — leave content, the hit-test fix still applies
            }
            else
            {
                // No tooltip object (a boxed-string tip, or none at all): give the target a fresh
                // hit-test-invisible ToolTip so it shows the id without flicker (theme inherited).
                WUXC::ToolTip tt;
                tt.Content(winrt::box_value(boxed));
                tt.IsHitTestVisible(false);
                WUXC::ToolTipService::SetToolTip(target, tt);
            }
        });
    }
}
