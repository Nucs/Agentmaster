// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster TerminalPage implementation (7 partial files)
// The TerminalPage-side Agentmaster glue -- engine wiring + session lifecycle + observer +
// window-record + the Sessions browser -- connecting the Manager engine to WT's TerminalPage.
// Same class (TerminalPage, declared in TerminalPage.h), split across same-class partial TUs (the
// upstream TabManagement.cpp pattern) so TerminalPage.cpp stays close to upstream.
//
// Partial files in this group (★ marks THIS file):
//   TerminalPage.AgentEngine.cpp               - ~TerminalPage, _InitAgentmasterEngine (consume the process-wide SharedEngine), the Manager tab, _WireAgentManagerContent
//   TerminalPage.AgentSessions.cpp             - spawn/launch/restore/close/adopt for Claude + Codex; tab-title sync; smart naming + per-dir tab color
// ★ TerminalPage.AgentObserver.cpp             - the per-tab overlay/badge bind/reconcile/liveness (incl. managed-Codex) + the Fleet Observer UI lane (_ObserverProbe)
//   TerminalPage.AgentWindowRecord.cpp         - M10 per-window record capture/flush/restore + reopen saved windows (Claude + Codex tab refs)
//   TerminalPage.AgentSessionsPage.cpp         - the Sessions browser (SESSIONS.md): shell + list (search / _RenderSessionsTable / detail + off-thread summary)
//   TerminalPage.AgentSessionsPageActions.cpp  - the Sessions browser row actions: resume/fork, selection nav, hide/unhide/reset, favorite, rename, row filter, overlay registry
//   TerminalPage.AgentSessionsPage.Internal.h  - the Sessions-browser Sess* file-local helpers shared by the two SessionsPage TUs above (anonymous namespace)
// ======================================================================================
//
// Agentmaster — the Fleet Observer's UI lane + bind machinery on this window
// (OBSERVER.md §10/§11d; TAB_OVERLAY.md): publish this window's tab roster, bind
// unbound tabs via the correlation table (_BindClaudeSessionToTab — the shared tail
// of hook adoption AND observer discovery), the per-tab link-badge / observe-badge
// overlays, the scanner-ticked liveness sweep + bind reconcile, and the Explorer
// Tree's force-refresh.
//
// This file implements TerminalPage methods (same class, separate TU — the
// TabManagement.cpp pattern) so the Agentmaster additions live in responsibility-
// grouped files and TerminalPage.cpp stays close to upstream (cheap rebases).

#include "pch.h"
#include "TerminalPage.h"

#include "../../types/inc/utils.hpp" // GuidToPlainString (WT_SESSION keys)

#include "AgentManagerContent.h" // push the External census / RefreshNow
#include "AgentStatusColors.h" // AgentStatusColorFor — the shared state->color palette (tab dot)
#include "AgentTabOverlay.h" // build + own the per-tab overlays (complete com_ptr type)
#include "Tab.h" // get_self<Tab> -> CurrentEffectiveTabBackground (pending-dots contrast)
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog
#include "AgentMaster/Persistence.h" // DeriveSessionTitle / SaveSessions (bind tail)
#include "AgentMaster/ProcessInspect.h" // ResolveClaudeTranscriptPath + AnalyzeSessionTranscript (prompt-nav)
#include "AgentMaster/ProcessObserver.h" // roster publish + Correlation/Activity/External tables
#include "AgentMaster/ProfileBootstrap.h" // Profiles:: profile-aware state paths (engine)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/SessionStore.h" // IsSessionFavorite (the FAVORITE crown on a managed tab's status dot)
#include <winrt/Windows.UI.Xaml.Shapes.h> // Agentmaster (tab tooltip): the header state-dot Ellipse in the summary card

#include <mmsystem.h> // PlaySoundW — the prompt-nav boundary sound (alt+up/down at the ends)
#pragma comment(lib, "winmm.lib")

using namespace winrt;
using namespace winrt::Microsoft::Management::Deployment;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace ::TerminalApp;
using namespace ::Microsoft::Console;
using namespace ::Microsoft::Terminal::Core;
using namespace std::chrono_literals;

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
    using VirtualKeyModifiers = Windows::System::VirtualKeyModifiers;
}

// Agentmaster (tab tooltip): file-local string formatting for _UpdateTabAgentToolTip. The page builds
// the tooltip STRINGS (it owns the SessionInfo + the registry); the Tab renders them (Tab::SetAgentToolTip).
// These mirror the equivalents in AgentManagerContent / AgentTabOverlay (FormatSpan / StateLabel /
// ModeLabel), kept TU-local rather than shared — those are file-statics there too.
namespace
{
    // unix-ms "now" (the registry's clock).
    int64_t TtNowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // A compact span like AgentManagerContent::FormatSpan — months/days/hours/mins, seconds only when
    // under an hour. Always emits at least the floor unit ("0m" / "0s"). Uses "mo" for month (not the
    // adornment's position-dependent "m") so a tooltip is self-explanatory.
    std::wstring TtSpan(int64_t ms, bool allowSeconds)
    {
        if (ms < 0)
        {
            ms = 0;
        }
        int64_t t = ms / 1000;
        const int64_t months = t / (30LL * 24 * 3600);
        t %= (30LL * 24 * 3600);
        const int64_t days = t / (24 * 3600);
        t %= (24 * 3600);
        const int64_t hours = t / 3600;
        t %= 3600;
        const int64_t mins = t / 60;
        const int64_t secs = t % 60;
        const bool showSeconds = allowSeconds && months == 0 && days == 0 && hours == 0;
        std::wstring out;
        const auto add = [&out](int64_t v, const wchar_t* unit) {
            if (v != 0)
            {
                out += std::to_wstring(v);
                out += unit;
            }
        };
        add(months, L"mo");
        add(days, L"d");
        add(hours, L"h");
        add(mins, L"m");
        if (showSeconds)
        {
            add(secs, L"s");
        }
        if (out.empty())
        {
            out = showSeconds ? L"0s" : L"0m";
        }
        return out;
    }

    const wchar_t* TtStateLabel(::Agentmaster::SessionState s)
    {
        using ::Agentmaster::SessionState;
        switch (s)
        {
        case SessionState::Running:
            return L"running";
        case SessionState::WaitingForInput:
            return L"waiting for you";
        case SessionState::NeedsApproval:
            return L"needs approval";
        case SessionState::Error:
            return L"error";
        case SessionState::Done:
            return L"done";
        case SessionState::Idle:
        default:
            return L"idle";
        }
    }

    // Join non-empty parts with sep.
    std::wstring TtJoin(const std::vector<std::wstring>& parts, const wchar_t* sep)
    {
        std::wstring out;
        for (const auto& p : parts)
        {
            if (p.empty())
            {
                continue;
            }
            if (!out.empty())
            {
                out += sep;
            }
            out += p;
        }
        return out;
    }

    // True when the live --permission-mode is the auto-approving bypass mode (worth flagging: this tab
    // is running unsupervised). Case-insensitive contains "bypass".
    bool TtPermIsBypass(const std::wstring& mode)
    {
        std::wstring m = mode;
        for (auto& c : m)
        {
            if (c >= L'A' && c <= L'Z')
            {
                c = static_cast<wchar_t>(c - L'A' + L'a');
            }
        }
        return m.find(L"bypass") != std::wstring::npos;
    }

    // ---- Agentmaster (tab tooltip): the rich, summary-style hover card -------------------------------
    // The tab tooltip is no longer a flat run of text: it is a dark, rounded card (the AgentTabOverlay
    // summary-panel chrome) hosting a header (state dot + title on the left, folder/branch on the right),
    // a state line, a dim kind/model/effort/perm line, then the session-end.js Summary box (NUMBERED
    // messages + files), all in Cascadia Mono. These free helpers build the XAML; the page
    // (_UpdateTabAgentToolTip) feeds them SessionInfo + the cached Summary text and hands the result to
    // Tab::SetAgentToolTip. (No jump buttons / autorunner / queue: a tooltip is read-only.)
    SolidColorBrush TtFill(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
    {
        return SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(a, r, g, b) };
    }

    // Render a Summary-box TEXT (RenderSessionSummaryBox output: a lone kSummarySepMark line = a section
    // divider, " N. " = a numbered user message, "* " = a file row) into the tooltip BODY: a vertical
    // StackPanel of monospace (Cascadia Mono) TextBlocks, each sentinel line a full-width hairline rule
    // (the AgentTabOverlay::_SetSummaryContent display look, minus the jump buttons). Contiguous
    // non-divider lines coalesce into one wrapping TextBlock.
    winrt::Windows::UI::Xaml::Controls::StackPanel TtBuildSummaryBody(const std::wstring& text)
    {
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;
        StackPanel stack;
        stack.Orientation(Orientation::Vertical);
        std::wstring seg;
        const auto flush = [&]() {
            if (seg.empty())
            {
                return;
            }
            TextBlock tb;
            tb.FontFamily(Media::FontFamily{ L"Cascadia Mono" });
            tb.FontSize(11);
            tb.TextWrapping(TextWrapping::Wrap);
            tb.Foreground(TtFill(0xFF, 0xDC, 0xDC, 0xDC));
            tb.Text(winrt::hstring{ seg });
            stack.Children().Append(tb);
            seg.clear();
        };
        size_t i = 0;
        while (i <= text.size())
        {
            const size_t nl = text.find(L'\n', i);
            const size_t end = (nl == std::wstring::npos) ? text.size() : nl;
            const std::wstring lineStr = text.substr(i, end - i);
            if (lineStr.size() == 1 && lineStr[0] == ::Agentmaster::kSummarySepMark)
            {
                flush();
                Border rule;
                rule.Height(1);
                rule.HorizontalAlignment(HorizontalAlignment::Stretch);
                rule.Background(TtFill(0x40, 0xFF, 0xFF, 0xFF));
                rule.Margin(ThicknessHelper::FromLengths(0, 4, 0, 4));
                stack.Children().Append(rule);
            }
            else
            {
                if (!seg.empty())
                {
                    seg += L'\n';
                }
                seg += lineStr;
            }
            if (nl == std::wstring::npos)
            {
                break;
            }
            i = nl + 1;
        }
        flush();
        return stack;
    }

    // Build the whole tab-tooltip card: a dark, rounded Border (summary-panel chrome) holding the header
    // (state dot + title on the left, folder/branch on the right), a state line (colored to match the tab
    // dot), a dim kind/model/effort/perm line, and -- once the Summary body has loaded -- a divider + the
    // numbered Summary box, height-capped by a ScrollViewer so a long conversation can't make a
    // screen-tall tooltip (the full, scrollable view is the pencil-toggled summary panel).
    winrt::Windows::UI::Xaml::Controls::Border TtBuildTooltipCard(winrt::Windows::UI::Color accent,
                                                                  const std::wstring& title,
                                                                  const std::wstring& folderBranch,
                                                                  const std::wstring& stateText,
                                                                  const std::wstring& metaText,
                                                                  const winrt::hstring& bodyText)
    {
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;

        StackPanel col;
        col.Orientation(Orientation::Vertical);
        col.Spacing(1);

        Grid header;
        {
            ColumnDefinition c0;
            c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            ColumnDefinition c1;
            c1.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            header.ColumnDefinitions().Append(c0);
            header.ColumnDefinitions().Append(c1);

            StackPanel left;
            left.Orientation(Orientation::Horizontal);
            left.Spacing(6);
            left.VerticalAlignment(VerticalAlignment::Center);
            winrt::Windows::UI::Xaml::Shapes::Ellipse dot;
            dot.Width(9);
            dot.Height(9);
            dot.Fill(SolidColorBrush{ accent });
            dot.Stroke(TtFill(0xFF, 0x00, 0x00, 0x00));
            dot.StrokeThickness(1);
            dot.VerticalAlignment(VerticalAlignment::Center);
            left.Children().Append(dot);
            TextBlock titleTb;
            titleTb.FontFamily(Media::FontFamily{ L"Cascadia Mono" });
            titleTb.FontSize(13);
            titleTb.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            titleTb.Foreground(TtFill(0xFF, 0xF2, 0xF2, 0xF2));
            titleTb.TextWrapping(TextWrapping::NoWrap);
            titleTb.TextTrimming(TextTrimming::CharacterEllipsis);
            titleTb.VerticalAlignment(VerticalAlignment::Center);
            titleTb.Text(winrt::hstring{ title });
            left.Children().Append(titleTb);
            Grid::SetColumn(left, 0);
            header.Children().Append(left);

            if (!folderBranch.empty())
            {
                TextBlock fb;
                fb.FontFamily(Media::FontFamily{ L"Cascadia Mono" });
                fb.FontSize(11);
                fb.Foreground(TtFill(0xFF, 0xB0, 0xB0, 0xB0));
                fb.TextWrapping(TextWrapping::NoWrap);
                fb.TextTrimming(TextTrimming::CharacterEllipsis);
                fb.VerticalAlignment(VerticalAlignment::Center);
                fb.Margin(ThicknessHelper::FromLengths(12, 0, 0, 0));
                fb.Text(winrt::hstring{ folderBranch });
                Grid::SetColumn(fb, 1);
                header.Children().Append(fb);
            }
            col.Children().Append(header);
        }

        if (!stateText.empty())
        {
            TextBlock st;
            st.FontFamily(Media::FontFamily{ L"Cascadia Mono" });
            st.FontSize(11);
            st.TextWrapping(TextWrapping::Wrap);
            st.Foreground(SolidColorBrush{ accent });
            st.Text(winrt::hstring{ stateText });
            col.Children().Append(st);
        }
        if (!metaText.empty())
        {
            TextBlock mt;
            mt.FontFamily(Media::FontFamily{ L"Cascadia Mono" });
            mt.FontSize(11);
            mt.TextWrapping(TextWrapping::Wrap);
            mt.Foreground(TtFill(0xFF, 0xB0, 0xB0, 0xB0));
            mt.Text(winrt::hstring{ metaText });
            col.Children().Append(mt);
        }
        if (!bodyText.empty())
        {
            Border rule;
            rule.Height(1);
            rule.HorizontalAlignment(HorizontalAlignment::Stretch);
            rule.Background(TtFill(0x40, 0xFF, 0xFF, 0xFF));
            rule.Margin(ThicknessHelper::FromLengths(0, 4, 0, 4));
            col.Children().Append(rule);

            ScrollViewer sv;
            sv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            sv.VerticalScrollMode(ScrollMode::Auto);
            sv.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            sv.MaxHeight(360);
            sv.Content(TtBuildSummaryBody(std::wstring{ bodyText }));
            col.Children().Append(sv);
        }

        Border root;
        root.Background(TtFill(0xFF, 0x20, 0x20, 0x20));
        root.BorderBrush(TtFill(0x40, 0xFF, 0xFF, 0xFF));
        root.BorderThickness(ThicknessHelper::FromUniformLength(1));
        root.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        root.Padding(ThicknessHelper::FromLengths(10, 8, 10, 8));
        root.MaxWidth(460);
        root.Child(col);
        return root;
    }

    // The unmanaged/observe twin: a small dark card -- a "(o) <kind> . unlinked" line over the tab title.
    winrt::Windows::UI::Xaml::Controls::Border TtBuildObserveCard(const std::wstring& kind, const std::wstring& title)
    {
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;
        StackPanel col;
        col.Orientation(Orientation::Vertical);
        col.Spacing(1);
        {
            TextBlock l1;
            l1.FontFamily(Media::FontFamily{ L"Cascadia Mono" });
            l1.FontSize(12);
            l1.Foreground(TtFill(0xFF, 0xB0, 0xB0, 0xB0));
            l1.Text(winrt::hstring{ std::wstring{ L"\x25CB " } + kind + L"  \x00B7  unlinked" });
            col.Children().Append(l1);
        }
        if (!title.empty())
        {
            TextBlock l2;
            l2.FontFamily(Media::FontFamily{ L"Cascadia Mono" });
            l2.FontSize(12);
            l2.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            l2.Foreground(TtFill(0xFF, 0xE6, 0xE6, 0xE6));
            l2.TextWrapping(TextWrapping::Wrap);
            l2.Text(winrt::hstring{ title });
            col.Children().Append(l2);
        }
        Border root;
        root.Background(TtFill(0xFF, 0x20, 0x20, 0x20));
        root.BorderBrush(TtFill(0x40, 0xFF, 0xFF, 0xFF));
        root.BorderThickness(ThicknessHelper::FromUniformLength(1));
        root.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        root.Padding(ThicknessHelper::FromLengths(10, 6, 10, 6));
        root.MaxWidth(420);
        root.Child(col);
        return root;
    }
}

namespace winrt::TerminalApp::implementation
{
    // Agentmaster (tab status dot): show/recolor (or hide, nullopt) the tab-strip status dot —
    // the "[icon] ● <title>" element TabHeaderControl.xaml binds to TerminalTabStatus. The dot is
    // the Explorer Tree's state dot brought onto the tab strip itself: state-colored for a managed
    // session, dim gray for an observed-only tab. It is a SEPARATE visual element, deliberately NOT
    // part of the title string — the one-title invariant (Rule #11: Explorer name == tab title ==
    // persisted SessionInfo.title) must never carry presentation glyphs through renames/persistence.
    // Independent of AppSettings.showTabOverlay (that toggle is the in-terminal HUD). Idempotent on
    // an unchanged color (a new SolidColorBrush per call would re-raise the INPC binding every
    // observer tick), so the per-tick badge path can re-assert it for free. UI thread only.
    void TerminalPage::_SetTabAgentDot(const TerminalApp::Tab& tab, const std::optional<winrt::Windows::UI::Color>& color, bool dormant)
    {
        if (!tab)
        {
            return;
        }
        try
        {
            const auto status = tab.TabStatus();
            if (!status)
            {
                return;
            }
            if (!color)
            {
                status.AgentStatusVisible(false); // WINRT_OBSERVABLE_PROPERTY no-ops when already false
                status.AgentStatusHalfVisible(false);
                return;
            }
            // The full dot (started) and the half-hollow dot (dormant) are MUTUALLY EXCLUSIVE — exactly
            // one is visible. Short-circuit only when the color AND the presentation both already match.
            const bool wantFull = !dormant;
            const bool wantHalf = dormant;
            if (status.AgentStatusVisible() == wantFull && status.AgentStatusHalfVisible() == wantHalf)
            {
                if (const auto cur = status.AgentStatusBrush().try_as<Media::SolidColorBrush>();
                    cur && cur.Color() == *color)
                {
                    return; // already showing exactly this color + presentation — don't churn the binding
                }
            }
            status.AgentStatusBrush(Media::SolidColorBrush{ *color });
            // Clear the off variant FIRST so a started<->dormant swap never momentarily shows both.
            if (dormant)
            {
                status.AgentStatusVisible(false);
                status.AgentStatusHalfVisible(true);
            }
            else
            {
                status.AgentStatusHalfVisible(false);
                status.AgentStatusVisible(true);
            }
        }
        CATCH_LOG();
    }

    // Agentmaster (PENDING_INPUT.md): show/hide the unsent-draft "3 dots" pulse below a tab's status dot
    // (TabStatus.AgentPendingVisible; TabHeaderControl starts/stops the pulse storyboard from it), painting
    // them dotsColor — the contrast-picked pending color (LIGHT on a dark tab / DARK on a light one, from
    // the session's per-dir color so they're never invisible). The brush is set BEFORE the visibility flips
    // true (so the dots never show with a null/stale brush) and only when the color actually changed; the
    // WINRT_OBSERVABLE_PROPERTY no-ops when unchanged, so re-asserting the same state every scan tick is
    // free. _ScanPendingInput passes the computed color when on; off leaves the (hidden) brush as-is. UI thread.
    void TerminalPage::_SetTabPending(const TerminalApp::Tab& tab, bool on, const std::optional<winrt::Windows::UI::Color>& dotsColor)
    {
        if (!tab)
        {
            return;
        }
        try
        {
            const auto status = tab.TabStatus();
            if (!status)
            {
                return;
            }
            if (on && dotsColor)
            {
                // Re-point the dots' brush only on a genuine color change (don't churn the binding every tick).
                const auto cur = status.AgentPendingBrush().try_as<Media::SolidColorBrush>();
                if (!(cur && cur.Color() == *dotsColor))
                {
                    status.AgentPendingBrush(Media::SolidColorBrush{ *dotsColor });
                }
            }
            status.AgentPendingVisible(on);
        }
        CATCH_LOG();
    }

    // Agentmaster (PENDING_INPUT.md): the contrast-picked "3 dots" color for a tab — the LIGHT pending
    // color on a dark effective background, the DARK one on a light one, so the dots are never invisible.
    // The background is the tab's CURRENT effective header background (Tab::CurrentEffectiveTabBackground),
    // which accounts for the selected/unselected light/dark shift (a deselected colored tab renders at 30%
    // over the dark tab row); the session's per-dir color (Rule #12, the same precedence the board card
    // uses) is the source/fallback. Shared by the per-tick scan and the focus-change refresh so both pick
    // the same way. UI thread.
    winrt::Windows::UI::Color TerminalPage::_PendingDotsColorForTab(const TerminalApp::Tab& tab, const std::wstring& workingDir)
    {
        const auto dirHex = ::Agentmaster::GetDirColor(workingDir);
        const std::wstring hex = dirHex ? *dirHex : ::Agentmaster::AutoDirColorHex(workingDir);
        const auto dirColor = ParseArgbHexColor(hex, winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x2E, 0x2E, 0x2E));
        const auto bg = winrt::get_self<Tab>(tab)->CurrentEffectiveTabBackground(dirColor);
        return PendingDotsColorFor(bg, _appSettings.pendingDotsLightColor, _appSettings.pendingDotsDarkColor);
    }

    // Agentmaster (PENDING_INPUT.md): re-pick the "3 dots" color for every tab currently showing a draft,
    // WITHOUT a buffer re-read. A tab's effective background — and so the dots' best-contrast color — SHIFTS
    // when it goes selected<->unselected (WT renders a deselected colored tab at 30% over the dark tab row),
    // so on a tab switch the now-deselected and now-selected pending tabs must re-contrast immediately
    // instead of waiting for the next ~2s scan tick. Called from _OnTabSelectionChanged. Light: only
    // iterates tabs whose session already holds a non-empty pendingInput (those actually showing the dots).
    void TerminalPage::_RefreshPendingDotsContrast()
    {
        if (!_sessionRegistry || _claudeTabs.empty())
        {
            return;
        }
        std::vector<std::wstring> ids;
        ids.reserve(_claudeTabs.size());
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            ids.push_back(id);
        }
        for (const auto& id : ids)
        {
            const auto info = _sessionRegistry->Get(id);
            if (!info || info->kind != ::Agentmaster::AgentKind::Claude || info->pendingInput.empty())
            {
                continue; // only tabs actually showing the dots need re-contrast
            }
            TerminalApp::Tab hostTab{ nullptr };
            if (const auto it = _claudeTabs.find(id); it != _claudeTabs.end())
            {
                hostTab = it->second.get();
            }
            if (hostTab)
            {
                _SetTabPending(hostTab, true, _PendingDotsColorForTab(hostTab, info->workingDir));
            }
        }
    }

    // Agentmaster (FAVORITES.md §5a): show/hide the FAVORITE marker over a tab's status dot — the CROWN
    // or the STAR, per the GLOBAL AppSettings::favoriteIcon. Low-level setter (mirrors _SetTabAgentDot):
    // the WINRT_OBSERVABLE_PROPERTY no-ops when the value is unchanged, so re-asserting the same state is
    // free. The two markers are MUTUALLY EXCLUSIVE — at most one is ever visible — so a switch between
    // them (or to unfavorited) always drives both observables. UI thread only.
    void TerminalPage::_SetTabAgentFavorite(const TerminalApp::Tab& tab, bool on)
    {
        if (!tab)
        {
            return;
        }
        try
        {
            if (const auto status = tab.TabStatus())
            {
                const bool star = on && (_appSettings.favoriteIcon == ::Agentmaster::FavoriteIcon::Star);
                status.AgentFavoriteVisible(on && !star); // Crown is the default/else marker
                status.AgentFavoriteStarVisible(star);
            }
        }
        CATCH_LOG();
    }

    // Agentmaster (FAVORITES.md): (re)assert the crown on the tab THIS window hosts for sessionId from
    // the durable on-disk truth (IsSessionFavorite). A map-miss is a cheap no-op (the session is hosted
    // by another window, or not open). Called where a managed tab is set up (launch + adopt/bind) so a
    // favorited session shows its crown the instant its tab appears, and from _ToggleSessionFavorite so a
    // same-window toggle updates instantly. (Cross-window LIVE toggles aren't pushed — favorite lives in
    // SessionStore, not the registry, so there's no observer fan-out; the hosting window picks it up on
    // the tab's next bind. A small, documented gap; see FAVORITES.md.)
    void TerminalPage::_RefreshTabFavoriteCrown(const std::wstring& sessionId)
    {
        if (sessionId.empty())
        {
            return;
        }
        const auto it = _claudeTabs.find(sessionId);
        if (it == _claudeTabs.end())
        {
            return;
        }
        if (const auto tab = it->second.get())
        {
            _SetTabAgentFavorite(tab, ::Agentmaster::IsSessionFavorite(sessionId));
        }
    }

    // Agentmaster (FAVORITES.md §5a): re-assert the favorite marker on EVERY tab this window hosts —
    // used when the GLOBAL AppSettings::favoriteIcon flips (Crown <-> Star) via the cog Save or a
    // cross-window broadcast, since IsSessionFavorite is unchanged but the GLYPH must switch live.
    // _SetTabAgentFavorite re-reads _appSettings.favoriteIcon and drives both observables, so this just
    // re-derives the on/off from the durable store for each hosted session. Cheap + idempotent (the
    // observable no-ops when unchanged); a non-favorite tab is re-cleared harmlessly.
    void TerminalPage::_RefreshAllFavoriteIcons()
    {
        for (const auto& [sessionId, weakTab] : _claudeTabs)
        {
            if (const auto tab = weakTab.get())
            {
                _SetTabAgentFavorite(tab, ::Agentmaster::IsSessionFavorite(sessionId));
            }
        }
    }

    // Agentmaster (tab status dot): the registry-observer reaction (bounced to this window's UI
    // thread by the engine-init observer). A session state change recolors its hosting tab's dot in
    // place; live=false hides it (the liveness sweep also hides explicitly before it drops the
    // _claudeTabs entry — whichever lands first wins, both are idempotent). A session this window
    // doesn't host is a cheap map-miss no-op (every window's observer sees every fleet event).
    void TerminalPage::_UpdateTabAgentDot(const std::wstring& sessionId, ::Agentmaster::SessionState state, bool live, bool dormant)
    {
        const auto it = _claudeTabs.find(sessionId);
        if (it == _claudeTabs.end())
        {
            return;
        }
        if (const auto tab = it->second.get())
        {
            _SetTabAgentDot(tab, live ? std::optional{ AgentStatusColorFor(state) } : std::nullopt, dormant);
            _EvaluateAgentFlash(sessionId, tab, state, live); // start/stop the unvisited "left Running" red flash
            _UpdateTabAgentToolTip(tab, sessionId); // refresh the rich hover tooltip (reverts to default when !live)
        }
    }

    void TerminalPage::_UpdateTabAgentToolTip(const TerminalApp::Tab& tab, const std::wstring& sessionId)
    {
        if (!tab || !_sessionRegistry)
        {
            return;
        }
        const auto impl = _GetTabImpl(tab);
        if (!impl)
        {
            return;
        }
        const auto info = _sessionRegistry->Get(sessionId);
        if (!info || !info->live)
        {
            impl->ClearAgentToolTip(); // archived / gone -> the default title+keychord tooltip
            _tabTooltipSummary.erase(sessionId); // drop the cached summary + sig so a later relaunch reloads fresh
            _tabTooltipSig.erase(sessionId);
            return;
        }
        const auto& s = *info;
        using ::Agentmaster::SessionState;
        const int64_t now = TtNowMs();
        const auto accent = AgentStatusColorFor(s.state);

        // Header line 2: the state, its last-activity age, and WHY it needs you (colored to match the dot).
        std::wstring stateText = TtStateLabel(s.state);
        const int64_t lastAct = s.convLastActivityUnixMs ? s.convLastActivityUnixMs : s.lastActivityUnixMs;
        if (lastAct > 0)
        {
            stateText += L"  \x00B7  " + TtSpan(now - lastAct, true);
        }
        std::wstring why;
        if (s.state == SessionState::NeedsApproval)
        {
            why = L"approval required";
        }
        else if (s.state == SessionState::WaitingForInput && s.lastMessageWasQuestion)
        {
            why = L"answer needed";
        }
        else if (!s.pendingConfirmPromptId.empty())
        {
            why = L"awaiting your confirm";
        }
        if (!why.empty())
        {
            stateText += L"  \x00B7  " + why;
        }
        if (_flashingSessions.count(sessionId) || _manualUnreadSessions.count(sessionId))
        {
            stateText += L"  \x00B7  \x26A0 unread"; // changed since you were last here
        }

        // Header line 3: agent kind, model, effort, and the permission mode (a bypass tab is unsupervised).
        std::vector<std::wstring> metaParts;
        metaParts.push_back(s.kind == ::Agentmaster::AgentKind::Codex ? std::wstring{ L"codex" } : std::wstring{ L"claude" });
        if (!s.model.empty())
        {
            metaParts.push_back(s.model);
        }
        if (!s.effort.empty())
        {
            metaParts.push_back(s.effort);
        }
        if (TtPermIsBypass(s.permissionMode))
        {
            metaParts.push_back(L"\x26A1 bypass"); // auto-approving everything
        }
        else if (!s.permissionMode.empty() && s.permissionMode != L"default")
        {
            metaParts.push_back(s.permissionMode);
        }
        const std::wstring metaText = TtJoin(metaParts, L"  \x00B7  ");

        // Header (right side): the leaf working-dir folder + "/" + git branch (the overlay subline) -- the
        // full path is too long to read at a glance, so show only the folder name and the branch.
        std::wstring dir = !s.workingDir.empty() ? s.workingDir : s.liveCwd;
        while (!dir.empty() && (dir.back() == L'/' || dir.back() == L'\\'))
        {
            dir.pop_back();
        }
        std::wstring leaf = dir;
        if (const auto pos = dir.find_last_of(L"/\\"); pos != std::wstring::npos)
        {
            leaf = dir.substr(pos + 1);
        }
        std::wstring folderBranch = leaf;
        if (!s.branch.empty())
        {
            folderBranch = folderBranch.empty() ? s.branch : (folderBranch + L"/" + s.branch);
        }

        const std::wstring title = s.title.empty() ? std::wstring{ L"(untitled)" } : s.title;

        // The Summary-box body (numbered messages + files), cached + loaded off-thread (see below).
        winrt::hstring bodyText;
        int64_t bodyMtime = 0;
        if (const auto it = _tabTooltipSummary.find(sessionId); it != _tabTooltipSummary.end())
        {
            bodyText = it->second.body;
            bodyMtime = it->second.mtime;
        }

        // Re-host only on a real content change. The signature folds the header strings + the body's mtime
        // (the body itself changes only when the transcript grows, which bumps the mtime), so an unchanged
        // idle tab is skipped; the "ago" ticking is what re-hosts an otherwise-quiet tab each sweep.
        std::wstring sig = stateText;
        sig += L'\x1f';
        sig += title;
        sig += L'\x1f';
        sig += folderBranch;
        sig += L'\x1f';
        sig += metaText;
        sig += L'\x1f';
        sig += std::to_wstring(bodyMtime);
        if (const auto sit = _tabTooltipSig.find(sessionId); sit == _tabTooltipSig.end() || sit->second != sig)
        {
            impl->SetAgentToolTip(TtBuildTooltipCard(accent, title, folderBranch, stateText, metaText, bodyText), winrt::hstring{ sig });
            _tabTooltipSig[sessionId] = sig;
        }

        // Keep the Summary body fresh off-thread (throttled + mtime-gated). First sight has no body yet, so
        // this fills it in, then re-hosts the card (a recursive _UpdateTabAgentToolTip on completion).
        auto& slot = _tabTooltipSummary[sessionId]; // default-creates an empty slot on first sight
        const bool needCheck = slot.body.empty() || slot.mtime == 0 || (now - slot.lastCheckMs) > 4000;
        if (needCheck && !_tabTooltipSummaryInFlight.count(sessionId))
        {
            slot.lastCheckMs = now;
            const bool codex = (s.kind == ::Agentmaster::AgentKind::Codex);
            _EnsureTabTooltipSummary(tab, winrt::hstring{ sessionId }, codex, winrt::hstring{ s.codexSessionId }, winrt::hstring{ dir });
        }
    }

    // Agentmaster (tab tooltip): resolve + stat + analyze the session's transcript OFF the UI thread, and
    // on a transcript growth render the session-end.js Summary box (full=false: numbered messages + files,
    // NO header -- the card already shows state/title/dir) into the per-session cache, then re-host the
    // card. mtime-gated (a quiet tab is one stat) + in-flight-guarded (one load per session at a time);
    // mirrors the AgentTabOverlay summary panel's off-thread caching. Codex uses its rollout analog.
    winrt::fire_and_forget TerminalPage::_EnsureTabTooltipSummary(winrt::TerminalApp::Tab tab, winrt::hstring sessionId, bool codex, winrt::hstring codexId, winrt::hstring cwd)
    {
        const std::wstring id{ sessionId };
        if (id.empty() || _tabTooltipSummaryInFlight.count(id))
        {
            co_return; // already loading this session's summary
        }
        auto strongThis{ get_strong() };
        _tabTooltipSummaryInFlight.insert(id);

        // Snapshot the cached path/mtime + the display toggles on the UI thread, before going background.
        std::wstring cachedPath;
        int64_t cachedMtime = 0;
        if (const auto it = _tabTooltipSummary.find(id); it != _tabTooltipSummary.end())
        {
            cachedPath = it->second.path;
            cachedMtime = it->second.mtime;
        }
        const bool wrapNewlines = _appSettings.summaryPanelWrapNewlines;
        const bool truncate = _appSettings.summaryPanelTruncate;
        const std::wstring codexUuid{ codexId };
        const std::wstring dir{ cwd };

        co_await winrt::resume_background();

        std::wstring path = cachedPath;
        if (path.empty())
        {
            path = codex ? ::Agentmaster::ResolveCodexRolloutPathIn(::Agentmaster::CodexDefaultHome(), codexUuid)
                         : ::Agentmaster::ResolveClaudeTranscriptPath(id);
        }
        int64_t mtime = 0;
        if (!path.empty())
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
            {
                ULARGE_INTEGER li{};
                li.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                li.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                mtime = static_cast<int64_t>(li.QuadPart);
            }
        }
        std::wstring body;
        bool rendered = false;
        if (!path.empty() && (mtime != cachedMtime || cachedMtime == 0))
        {
            if (codex)
            {
                const auto cinfo = ::Agentmaster::ReadCodexRolloutInfo(path, 0 /* whole file */, 200 /* prompts */);
                body = ::Agentmaster::RenderCodexSummaryBox(cinfo, std::wstring{}, dir, path, std::wstring{}, std::wstring{}, std::wstring{}, /*full*/ false);
            }
            else
            {
                auto a = ::Agentmaster::AnalyzeSessionTranscript(path, 0 /* whole file */);
                body = ::Agentmaster::RenderSessionSummaryBox(a, std::wstring{}, dir, path, std::wstring{}, std::wstring{}, std::wstring{}, a.planFilePath, /*full*/ false, wrapNewlines, truncate);
            }
            rendered = true;
        }

        co_await wil::resume_foreground(Dispatcher());
        _tabTooltipSummaryInFlight.erase(id);
        if (path.empty())
        {
            co_return; // no transcript yet (never prompted) -- leave the header-only card
        }
        auto& slot = _tabTooltipSummary[id];
        slot.path = path;
        slot.mtime = mtime;
        if (rendered)
        {
            slot.body = winrt::hstring{ body };
        }
        // Re-host the card with the now-loaded / refreshed body. _UpdateTabAgentToolTip recomputes the
        // signature (new mtime) so it re-hosts; the throttle (lastCheckMs, bumped by the caller) keeps it
        // from immediately re-kicking us.
        _UpdateTabAgentToolTip(tab, id);
    }

    // Agentmaster (tab status-dot RED FLASH): the attention cue. When a hosted session goes from
    // Running to one of the "it's on you / at rest" states — Idle, WaitingForInput (waiting-for-you),
    // or NeedsApproval (approval-required) — while its tab is NOT the one you're looking at, that tab's
    // status-dot OUTLINE flashes red until you switch to the tab. "The current tab is always considered
    // visited", so the active tab never flashes. The trigger is precisely the Running -> {Idle /
    // WaitingForInput / NeedsApproval} edge (NOT -> Done or -> Error), so we remember each hosted
    // session's last state. Any move OUT of a target state — back to Running (working again), or to
    // Done / Error — clears the flash; a target -> target move (e.g. the WaitingForInput -> Idle cache
    // decay) keeps it flashing until you visit. UI thread (the observer hop).
    void TerminalPage::_EvaluateAgentFlash(const std::wstring& sessionId, const TerminalApp::Tab& tab, ::Agentmaster::SessionState newState, bool live)
    {
        using ::Agentmaster::SessionState;
        if (!live)
        {
            // Archived/closed: stop any flash AND forget the last state, so if this session is later
            // RESTORED its first update starts a fresh track (no phantom Running -> other edge that
            // would spuriously flash a freshly resumed tab opened in the background). A manual Mark
            // Unread also does not survive archive/close.
            _StopAgentFlash(sessionId);
            _ClearSessionUnread(sessionId);
            _agentFlashLastState.erase(sessionId);
            return;
        }

        // Agentmaster (Waiting-for-you "unread" model): if this hosted session's tab is the one you're
        // looking at (the window's focused tab), it counts as READ — so a turn that completes while you
        // sit in the tab is read immediately (and may decay after the timeout, rather than waiting for a
        // switch-away-and-back). Stamped on every update for the focused tab; cheap + quiet + monotonic.
        if (tab == _GetFocusedTab())
        {
            _MarkSessionRead(sessionId);
        }

        const auto prevIt = _agentFlashLastState.find(sessionId);
        const bool hadPrev = (prevIt != _agentFlashLastState.end());
        const auto prev = hadPrev ? prevIt->second : SessionState::Idle;
        _agentFlashLastState[sessionId] = newState;

        // We flash ONLY for the "now it's on you / at rest" states: Idle, WaitingForInput
        // (waiting-for-you), NeedsApproval (approval-required). Any other state — Running (working
        // again), Done (finished clean), or Error — is NOT one we flash for, so a move there stops any
        // existing flash. (This is also how "back to Running" clears it.)
        const bool isTarget = (newState == SessionState::Idle ||
                               newState == SessionState::WaitingForInput ||
                               newState == SessionState::NeedsApproval);
        if (!isTarget)
        {
            _StopAgentFlash(sessionId);
            return;
        }

        // newState is a target state. START the flash only on the Running -> target EDGE, and only on
        // an UNVISITED tab. A target -> target move (e.g. the WaitingForInput -> Idle cache decay)
        // leaves an existing flash alone — still on you until you visit the tab.
        if (hadPrev && prev == SessionState::Running)
        {
            if (tab == _GetFocusedTab())
            {
                _StopAgentFlash(sessionId); // the current tab is always considered visited
            }
            else
            {
                _StartAgentFlash(sessionId);
            }
        }
        // else: target -> target (or first sight) — leave any existing flash as-is.
    }

    // Agentmaster (tab status-dot red flash): add this session to the flashing set + ensure the shared
    // timer runs, then paint its outline at the CURRENT phase so it blinks in lockstep with any tabs
    // already flashing (no per-tab phase drift). No-op if already flashing.
    void TerminalPage::_StartAgentFlash(const std::wstring& sessionId)
    {
        if (!_flashingSessions.insert(sessionId).second)
        {
            return; // already flashing
        }
        _EnsureAgentFlashTimer();
        _ApplyAgentFlashRingForSession(sessionId);
    }

    // Agentmaster (tab status-dot red flash): stop this session's AUTOMATIC flash + hide its red ring;
    // when no flashing tabs remain (auto OR manual), stop the shared timer. No-op if it wasn't auto-
    // flashing. Does NOT hide the ring if the session is ALSO manually marked unread — a Mark Unread is
    // sticky (only a visit/archive clears it, via _ClearSessionUnread).
    void TerminalPage::_StopAgentFlash(const std::wstring& sessionId)
    {
        if (_flashingSessions.erase(sessionId) == 0)
        {
            return; // wasn't auto-flashing
        }
        if (!_manualUnreadSessions.count(sessionId))
        {
            if (const auto it = _claudeTabs.find(sessionId); it != _claudeTabs.end())
            {
                if (const auto tab = it->second.get())
                {
                    _SetTabFlashRing(tab, false); // hide the red ring (the dot's own black stroke + fill stay)
                }
            }
        }
        if (_flashingSessions.empty() && _manualUnreadSessions.empty())
        {
            _StopAgentFlashTimer();
        }
    }

    // Agentmaster (Mark Unread): the tab context-menu action. Force the red ring to flash on this
    // session's tab until the user VISITS it (switches to it). Unlike the automatic flash, it fires even
    // when this session's tab is the CURRENTLY-FOCUSED one (no active-tab skip) — marking the tab you're
    // on flashes it, and only a leave-then-return (a switch back, via _VisitTabClearFlash) clears it.
    // Sticky: the automatic state logic (_EvaluateAgentFlash / _StopAgentFlash) never clears a manual mark.
    void TerminalPage::_MarkSessionUnread(const std::wstring& sessionId)
    {
        if (sessionId.empty())
        {
            return;
        }
        _manualUnreadSessions.insert(sessionId); // idempotent
        // Nav audit: the user manually marked this session UNREAD (the tab right-click "Mark Unread") — a
        // deliberate "remind me" that flashes the ring until visited + promotes an at-rest card to Waiting.
        // (Its clear twin is automatic — a tab VISIT, already covered by tab-focus — so it stays unlogged.)
        ::Agentmaster::LogNav(L"mark-unread " + ::Agentmaster::ShortId(sessionId));
        _EnsureAgentFlashTimer();
        _ApplyAgentFlashRingForSession(sessionId); // flash at the current shared phase NOW, even if focused
        // Agentmaster (Waiting-for-you "unread" model): the manual mark is also an ENGINE fact, so the
        // Triage Board (in EVERY window) reads it — set the sticky manualUnread flag (the time-decay
        // never demotes it) and PROMOTE an at-rest session into Waiting-for-you (the user asked to be
        // reminded). Notifying Update so the card moves columns immediately.
        if (_sessionRegistry)
        {
            _sessionRegistry->Update(sessionId, [](::Agentmaster::SessionInfo& s) {
                s.manualUnread = true;
                if (s.state == ::Agentmaster::SessionState::Idle || s.state == ::Agentmaster::SessionState::Done)
                {
                    s.state = ::Agentmaster::SessionState::WaitingForInput;
                }
            });
        }
    }

    // Agentmaster (Waiting-for-you triage): the tab context-menu's status-adaptive triage move — the
    // tab-menu twin of the Manager board card's "Move to Idle/Done", plus its reverse. EXPLICITLY separate
    // from "Mark Unread": neither direction sets the sticky manualUnread flag or flashes the red ring. The
    // direction is re-derived HERE from the session's LIVE state (the menu label was fixed at flyout-open),
    // and each registry mutation re-checks the state under the lock, so a turn that advanced since the menu
    // opened simply no-ops rather than mis-moving.
    void TerminalPage::_MoveSessionTriageState(const std::wstring& sessionId)
    {
        if (sessionId.empty() || !_sessionRegistry)
        {
            return;
        }
        const auto info = _sessionRegistry->Get(sessionId);
        if (!info)
        {
            return;
        }
        using ::Agentmaster::SessionState;
        if (info->state == SessionState::WaitingForInput)
        {
            // Demote Waiting-for-you -> Idle/Done. Mirror the board card's mutator (stamp read + clear the
            // sticky manualUnread) and ALSO drop this tab's flash ring (manual + automatic) — "Move to
            // Idle/Done" is an explicit "I've handled this", so it acknowledges the attention without a tab
            // visit. The Update guard re-checks the live state, so a race to Running never demotes.
            _ClearSessionUnread(sessionId); // drop the manual mark (+ ring if not auto-flashing)
            _StopAgentFlash(sessionId); // drop the automatic "left Running" flash too
            _sessionRegistry->Update(sessionId, [](::Agentmaster::SessionInfo& s) {
                if (s.state == SessionState::WaitingForInput)
                {
                    s.state = SessionState::Idle;
                    s.manualUnread = false;
                    s.readUnixMs = TtNowMs();
                }
            });
            ::Agentmaster::LogNav(L"triage-move " + ::Agentmaster::ShortId(sessionId) + L" -> idle/done");
        }
        else if (info->state == SessionState::Idle || info->state == SessionState::Done)
        {
            // Promote Idle/Done -> Waiting-for-you. A PLAIN move (the whole point of keeping it separate
            // from Mark Unread): manualUnread stays false (so the normal time-decay still applies) and no
            // ring flashes (Idle->Waiting is a target->target edge, which _EvaluateAgentFlash never flashes).
            // Refresh the decay anchor + leave it unread so it behaves like a fresh turn-complete (waits the
            // FULL Waiting-for-you timeout, then decays once read) instead of instantly decaying off an
            // ancient lastActivity.
            _sessionRegistry->Update(sessionId, [](::Agentmaster::SessionInfo& s) {
                if (s.state == SessionState::Idle || s.state == SessionState::Done)
                {
                    s.state = SessionState::WaitingForInput;
                    s.manualUnread = false;
                    s.lastActivityUnixMs = TtNowMs(); // restart the Waiting-for-you window (don't instant-decay)
                    s.readUnixMs = 0; // unread for this "turn", like a real turn-complete
                }
            });
            ::Agentmaster::LogNav(L"triage-move " + ::Agentmaster::ShortId(sessionId) + L" -> waiting");
        }
        // else: not a triage state (Running / NeedsApproval / Error) — nothing to move (the item is hidden).
    }

    // Agentmaster (Mark Unread): drop a session's manual unread mark + hide its ring UNLESS the automatic
    // flash is also active for it. Called from _VisitTabClearFlash (a visit) and the archive path.
    void TerminalPage::_ClearSessionUnread(const std::wstring& sessionId)
    {
        if (_manualUnreadSessions.erase(sessionId) == 0)
        {
            return; // wasn't marked
        }
        // Agentmaster (Waiting-for-you "unread" model): drop the engine's sticky manualUnread too, so a
        // promoted/held WaitingForInput card is free to time-decay again (the SessionScanner does the
        // visible demote on its next tick, gated on read-state). Quiet: clearing the flag alone changes
        // nothing the board shows until the decay actually fires.
        if (_sessionRegistry)
        {
            _sessionRegistry->UpdateQuiet(sessionId, [](::Agentmaster::SessionInfo& s) { s.manualUnread = false; });
        }
        if (!_flashingSessions.count(sessionId))
        {
            if (const auto it = _claudeTabs.find(sessionId); it != _claudeTabs.end())
            {
                if (const auto tab = it->second.get())
                {
                    _SetTabFlashRing(tab, false);
                }
            }
        }
        if (_flashingSessions.empty() && _manualUnreadSessions.empty())
        {
            _StopAgentFlashTimer();
        }
    }

    // Agentmaster (tab status-dot red flash): visiting a tab marks it seen — switching to a flashing
    // session's tab stops its flash. Called from the one tab-switch funnel (_OnTabSelectionChanged), so
    // a user click, Ctrl+Tab, a switchToTab action, or a cross-window Activate all clear it. Cheap
    // no-op for a non-session tab or a tab that isn't flashing.
    void TerminalPage::_VisitTabClearFlash(const TerminalApp::Tab& tab)
    {
        if (!tab)
        {
            return;
        }
        const auto id = _ClaudeSessionForTab(tab);
        if (!id.empty())
        {
            _MarkSessionRead(id); // Agentmaster (unread model): visiting the tab = reading it (stamp readUnixMs, so a past-timeout Waiting-for-you card may now decay)
            _StopAgentFlash(id); // clear the automatic flash...
            _ClearSessionUnread(id); // ...AND any manual Mark Unread — a visit clears both (the leave-then-return that ends a marked tab)
        }
    }

    // Agentmaster (Waiting-for-you "unread" model): stamp this session READ now. The engine gate
    // (ShouldDecayWaitingToIdle) then permits a past-timeout WaitingForInput card to demote to Idle —
    // an unread session keeps waiting until this lands. Quiet (no observer churn): the visible demote
    // is the SessionScanner's own notifying state change. Monotonic (never moves readUnixMs backward).
    void TerminalPage::_MarkSessionRead(const std::wstring& sessionId)
    {
        if (sessionId.empty() || !_sessionRegistry)
        {
            return;
        }
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        _sessionRegistry->UpdateQuiet(sessionId, [now](::Agentmaster::SessionInfo& s) {
            if (s.readUnixMs < now)
            {
                s.readUnixMs = now;
            }
        });
    }

    // Agentmaster (tab status-dot red flash): lazily build the ONE shared per-window flash timer and
    // (re)start it. One timer + one phase drive every flashing tab, so they blink together. A fresh
    // burst begins on the red ("on") phase. The Tick self-stops if the page is gone (weak), so a closed
    // window never leaks a ticking timer.
    void TerminalPage::_EnsureAgentFlashTimer()
    {
        if (!_agentFlashTimer)
        {
            _agentFlashTimer = WUX::DispatcherTimer{};
            _agentFlashTimer.Interval(std::chrono::milliseconds(600));
            _agentFlashTimer.Tick([weak = get_weak()](const IInspectable& sender, const IInspectable&) {
                if (auto self = weak.get())
                {
                    self->_OnAgentFlashTick();
                }
                else if (const auto t = sender.try_as<WUX::DispatcherTimer>())
                {
                    t.Stop(); // page destroyed — stop ticking (UI thread, safe)
                }
            });
        }
        if (!_agentFlashTimer.IsEnabled())
        {
            _agentFlashPhase = true; // a fresh burst begins on the red "on" phase
            _agentFlashTimer.Start();
        }
    }

    // Agentmaster (tab status-dot red flash): stop the shared timer (called when the flashing set empties).
    void TerminalPage::_StopAgentFlashTimer()
    {
        if (_agentFlashTimer)
        {
            _agentFlashTimer.Stop();
        }
        _agentFlashPhase = false;
    }

    // Agentmaster (tab status-dot red flash): the shared-timer tick. Toggle the one phase and show/hide
    // EVERY flashing tab's red ring together (ring ON on the "on" phase, OFF otherwise) — this is what
    // keeps multiple flashing tabs synchronized. Prunes any session whose tab has gone away, and stops
    // the timer once none remain.
    void TerminalPage::_OnAgentFlashTick()
    {
        _agentFlashPhase = !_agentFlashPhase;
        // Apply the current phase to EVERY flashing tab — the automatic set AND the manual Mark-Unread
        // set (a session in both is set twice, idempotently). Prune any whose tab has gone away.
        const auto applyAndPrune = [this](std::unordered_set<std::wstring>& set) {
            for (auto it = set.begin(); it != set.end();)
            {
                TerminalApp::Tab tab{ nullptr };
                if (const auto tabIt = _claudeTabs.find(*it); tabIt != _claudeTabs.end())
                {
                    tab = tabIt->second.get();
                }
                if (!tab)
                {
                    it = set.erase(it); // tab closed / re-homed — drop it
                    continue;
                }
                _SetTabFlashRing(tab, _agentFlashPhase); // ring ON on the red phase, OFF otherwise
                ++it;
            }
        };
        applyAndPrune(_flashingSessions);
        applyAndPrune(_manualUnreadSessions);
        if (_flashingSessions.empty() && _manualUnreadSessions.empty())
        {
            _StopAgentFlashTimer();
        }
    }

    // Agentmaster (tab status-dot red flash): show/hide one flashing session's red ring at the current
    // shared phase. Used when a tab joins an in-progress flash so it lands on the same on/off beat as the rest.
    void TerminalPage::_ApplyAgentFlashRingForSession(const std::wstring& sessionId)
    {
        if (const auto it = _claudeTabs.find(sessionId); it != _claudeTabs.end())
        {
            if (const auto tab = it->second.get())
            {
                _SetTabFlashRing(tab, _agentFlashPhase);
            }
        }
    }

    // Agentmaster (tab status-dot flash): show/hide a tab's FLASH RING — the ellipse behind the dot
    // (TabHeaderControl.xaml, bound to AgentFlashRingVisible) whose edge peeks out as a colored ring
    // around the dot's black stroke. The dot's own stroke + fill are untouched. When SHOWING it, the
    // ring is painted with this window's shared flash-ring brush — the user-configurable "status
    // flashing color" (Settings cog -> AppSettings::flashRingColor; its alpha = opacity) — BEFORE the
    // ring becomes visible, so it is never shown with a null Fill; the same brush instance is reused
    // across tabs + the 600ms blink, so the observable no-ops after the first set (no binding churn).
    // Idempotent (the WINRT_OBSERVABLE_PROPERTY no-ops on an unchanged value). UI thread only.
    void TerminalPage::_SetTabFlashRing(const TerminalApp::Tab& tab, bool on)
    {
        if (!tab)
        {
            return;
        }
        try
        {
            const auto status = tab.TabStatus();
            if (!status)
            {
                return;
            }
            if (on)
            {
                _EnsureFlashRingBrush();
                status.AgentFlashRingBrush(_flashRingBrush);
            }
            status.AgentFlashRingVisible(on);
        }
        CATCH_LOG();
    }

    // Agentmaster (status-dot flash-ring color): parse the GLOBAL AppSettings::flashRingColor
    // ("#AARRGGBB"; the leading alpha byte is the ring opacity) into a Color, falling back to the
    // default red at 80% opacity (alpha 0xCC) on a malformed / empty value. ParseArgbHexColor
    // (AgentStatusColors.h) is the ONE parser shared with the Settings cog's color picker, so the
    // on-disk string and the rendered ring can never drift.
    winrt::Windows::UI::Color TerminalPage::_FlashRingColorFromSettings() const
    {
        return ParseArgbHexColor(_appSettings.flashRingColor,
                                 winrt::Windows::UI::ColorHelper::FromArgb(0xCC, 0xFF, 0x00, 0x00));
    }

    // Agentmaster (status-dot flash-ring color): lazily build this window's ONE shared flash-ring brush
    // from the current setting, on the first flash. Every flashing tab in the window is then pointed at
    // this instance (see _SetTabFlashRing), so a later color change updates them all at once.
    void TerminalPage::_EnsureFlashRingBrush()
    {
        if (!_flashRingBrush)
        {
            _flashRingBrush = Media::SolidColorBrush{ _FlashRingColorFromSettings() };
        }
    }

    // Agentmaster (status-dot flash-ring color): re-point the shared brush at the (possibly changed)
    // AppSettings::flashRingColor. Mutating the existing instance's Color live-updates EVERY tab whose
    // ring Fill is bound to it — no per-tab re-assert needed — so a settings change recolors the live
    // flash instantly; builds the brush if no tab has flashed yet. Called from the Settings cog Save
    // handler and the cross-window settings broadcast (each window owns its own brush).
    void TerminalPage::_RefreshFlashRingBrush()
    {
        const auto color = _FlashRingColorFromSettings();
        if (!_flashRingBrush)
        {
            _flashRingBrush = Media::SolidColorBrush{ color };
        }
        else
        {
            _flashRingBrush.Color(color);
        }
    }

    // Agentmaster (TAB_OVERLAY.md): apply the GLOBAL per-tab overlay rest/hover opacities
    // (AppSettings::tabOverlayRestOpacity / tabOverlayHoverOpacity) to every overlay this window hosts —
    // the LINKED badges (_claudeOverlays) AND the registry-less observe badges (_pendingOverlays), so a
    // pwsh/cmd/unprompted-claude badge dims/brightens the same as a managed one. Called on a cog Save +
    // the cross-window broadcast (the _RefreshFlashRingBrush idiom). UI thread only.
    void TerminalPage::_RefreshOverlayOpacities()
    {
        const double rest = _appSettings.tabOverlayRestOpacity;
        const double hover = _appSettings.tabOverlayHoverOpacity;
        for (const auto& [id, ov] : _claudeOverlays)
        {
            if (ov)
            {
                ov->SetOverlayOpacities(rest, hover);
            }
        }
        for (const auto& [wt, ov] : _pendingOverlays)
        {
            if (ov)
            {
                ov->SetOverlayOpacities(rest, hover);
            }
        }
    }

    // Agentmaster (Linked Lenses): show/hide the "selected/active" pill behind a tab's header — the
    // filled accent background TabHeaderControl.xaml binds to TerminalTabStatus.AgentSelectionVisible
    // / AgentSelectionBrush. It makes a session's tab read as the active tab while you stay on the
    // Manager tab, WITHOUT changing the real TabView selection or the per-dir tab color (Rule #12) —
    // a separate presentation layer, exactly like the status dot. UI thread only.
    void TerminalPage::_SetTabSelectionPill(const TerminalApp::Tab& tab, bool on)
    {
        if (!tab)
        {
            return;
        }
        try
        {
            const auto status = tab.TabStatus();
            if (!status)
            {
                return;
            }
            if (!on)
            {
                status.AgentSelectionVisible(false); // WINRT_OBSERVABLE_PROPERTY no-ops when already false
                return;
            }
            // The "selected" fill: the system accent at partial alpha, so the title + dot stay legible
            // on top (and it reads as one consistent "this is the picked tab" tint regardless of the
            // tab's own dir color). Resolved the same way WT picks the active-pane border accent
            // (_updatePaneResources) — guard with HasKey, with a sane WT-blue fallback.
            auto accent = winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x00, 0x78, 0xD4);
            const auto res = Application::Current().Resources();
            if (const auto accentKey = winrt::box_value(L"SystemAccentColor"); res.HasKey(accentKey))
            {
                accent = winrt::unbox_value_or<winrt::Windows::UI::Color>(res.Lookup(accentKey), accent);
            }
            accent.A = 0x66; // ~40% — present enough to read "selected" without washing out the title
            status.AgentSelectionBrush(SolidColorBrush{ accent });
            status.AgentSelectionVisible(true);
        }
        CATCH_LOG();
    }

    // Agentmaster (Linked Lenses): re-evaluate which tab (if any) wears the "selected/active" pill.
    // The target is the managed session HOVERED in the Manager (a live preview that follows the
    // mouse), else the SELECTED session — but ONLY while the Agent Manager tab is the active tab and
    // that session actually has a tab in THIS window. Otherwise nothing is pilled. Idempotent: a
    // no-op when the target is unchanged; on a change it clears the old tab's pill and sets the new.
    // Called on a Manager lens change, a hover push, and a tab switch (so it clears when you leave
    // the Manager tab and re-applies when you return). UI thread only.
    void TerminalPage::_UpdateManagerSelectionHighlight()
    {
        // Gate: the Manager tab must be the active (selected) tab.
        bool onManager = false;
        if (_managerTab && _tabView)
        {
            const auto idx = _tabView.SelectedIndex();
            if (idx >= 0 && idx < static_cast<int32_t>(_tabs.Size()))
            {
                onManager = (_tabs.GetAt(idx) == _managerTab);
            }
        }
        if (!onManager)
        {
            // Off the Manager tab, hover is meaningless. Clear it so returning to the Manager falls
            // back to the SELECTION until a genuine PointerEntered re-arms hover — otherwise a hover
            // left dangling by a keyboard tab-switch (no PointerExited) would wrongly pill on return.
            _managerHoverSessionId.clear();
        }

        std::wstring desired;
        if (onManager)
        {
            // Hover wins over the pinned lens selection (a preview); fall back to the selection,
            // read live from the content so a restored/seeded selection is honored without caching.
            std::wstring target = _managerHoverSessionId;
            if (target.empty())
            {
                if (const auto ipc = _agentManagerContent.get())
                {
                    if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                    {
                        target = std::wstring{ mgr->SelectedSessionId() };
                    }
                }
            }
            // Only pill a session whose tab lives in THIS window (a GLOBAL-scope card can name a
            // session hosted elsewhere — that window pills it, not this one).
            if (!target.empty() && _claudeTabs.find(target) != _claudeTabs.end())
            {
                desired = target;
            }
        }

        if (desired == _pilledSessionId)
        {
            return; // no change
        }
        // Clear the previously-pilled tab.
        if (!_pilledSessionId.empty())
        {
            if (const auto it = _claudeTabs.find(_pilledSessionId); it != _claudeTabs.end())
            {
                if (const auto t = it->second.get())
                {
                    _SetTabSelectionPill(t, false);
                }
            }
        }
        _pilledSessionId = desired;
        if (!desired.empty())
        {
            if (const auto it = _claudeTabs.find(desired); it != _claudeTabs.end())
            {
                if (const auto t = it->second.get())
                {
                    _SetTabSelectionPill(t, true);
                }
            }
        }
    }

    // Agentmaster (TAB_OVERLAY.md): build the per-tab "link badge" overlay for a Claude session and
    // install it into its terminal pane's top-right slot. Gated on AppSettings.showTabOverlay. A
    // re-attach replaces the prior overlay for that id (the old com_ptr's release detaches its
    // observer). Best-effort — a tab with no TerminalPaneContent is left alone.
    void TerminalPage::_AttachClaudeOverlay(const TerminalApp::Tab& tab, const std::wstring& sessionId)
    {
        if (!_appSettings.showTabOverlay || !_sessionRegistry || !tab || sessionId.empty())
        {
            return;
        }
        const auto tabImpl = _GetTabImpl(tab);
        if (!tabImpl)
        {
            return;
        }
        TerminalApp::TerminalPaneContent termContent{ nullptr };
        if (const auto rootPane = tabImpl->GetRootPane())
        {
            rootPane->WalkTree([&](auto&& pane) {
                if (termContent)
                {
                    return;
                }
                if (const auto content = pane->GetContent())
                {
                    if (const auto term = content.try_as<TerminalApp::TerminalPaneContent>())
                    {
                        termContent = term;
                    }
                }
            });
        }
        if (!termContent)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[overlay] " + sessionId + L" NOT attached (no TerminalPaneContent in tab)\n");
            return;
        }
        auto overlay = winrt::make_self<implementation::AgentTabOverlay>();
        overlay->Initialize(sessionId, _sessionRegistry);
        // Summary panel (TAB_OVERLAY.md): its show/hide is the GLOBAL AppSettings::showSummaryPanel, not
        // per-session — seed this overlay with the current value, and wire the pencil to flip the global
        // setting (freshest-disk RMW) + broadcast live to every linked overlay in this window.
        overlay->SetSummaryEnabled(_appSettings.showSummaryPanel);
        // Summary panel SIZE (TAB_OVERLAY.md resize): a GLOBAL setting
        // (AppSettings::summaryPanelWidthFraction/HeightFraction) stored as fractions of the pane so it
        // scales with the window. Seed this overlay with the current fractions; the grips persist a new
        // size on drag release via the resize handler below.
        overlay->SetSummarySize(_appSettings.summaryPanelWidthFraction, _appSettings.summaryPanelHeightFraction);
        // Summary panel newline-wrap mode (TAB_OVERLAY.md): a GLOBAL setting
        // (AppSettings::summaryPanelWrapNewlines) — seed this overlay with the current value; the wrap-line
        // toggle at the right of the panel's times bar flips it via the handler below.
        overlay->SetSummaryWrapNewlines(_appSettings.summaryPanelWrapNewlines);
        // Summary panel truncate mode (TAB_OVERLAY.md): also a GLOBAL setting
        // (AppSettings::summaryPanelTruncate) — seed this overlay; the truncate toggle (left of the wrap
        // toggle) flips it via the handler below.
        overlay->SetSummaryTruncate(_appSettings.summaryPanelTruncate);
        // Summary panel show-previous mode (conversation lineage): a GLOBAL setting
        // (AppSettings::summaryPanelShowPrevious) — seed this overlay; the previous-session toggle
        // (leftmost in the times bar, shown only when /compact'ed) flips it via the handler below.
        overlay->SetSummaryShowPrevious(_appSettings.summaryPanelShowPrevious);
        // Per-tab overlay REST/HOVER opacities (TAB_OVERLAY.md): GLOBAL settings
        // (AppSettings::tabOverlayRestOpacity / tabOverlayHoverOpacity) — seed this overlay; the cog's
        // "Overlay opacity" slider re-applies them live via _RefreshOverlayOpacities (Save + broadcast).
        overlay->SetOverlayOpacities(_appSettings.tabOverlayRestOpacity, _appSettings.tabOverlayHoverOpacity);
        {
            auto weakThis = get_weak();
            overlay->SetSummaryToggleHandler([weakThis]() {
                if (auto self = weakThis.get())
                {
                    self->_ToggleSummaryPanel();
                }
            });
            overlay->SetSummaryWrapToggleHandler([weakThis]() {
                if (auto self = weakThis.get())
                {
                    self->_ToggleSummaryWrap();
                }
            });
            overlay->SetSummaryTruncateToggleHandler([weakThis]() {
                if (auto self = weakThis.get())
                {
                    self->_ToggleSummaryTruncate();
                }
            });
            overlay->SetSummaryPreviousToggleHandler([weakThis]() {
                if (auto self = weakThis.get())
                {
                    self->_ToggleSummaryPrevious();
                }
            });
            // A grip drag persists the new size GLOBALLY (the treeSort / archiveSplitFraction idiom): a
            // freshest-disk read-modify-write of just these two fields, keep this window's in-memory copy
            // in step (so a later cog Save can't regress it), then apply it LIVE to every linked overlay
            // in this window. Other already-open windows adopt it on their next launch.
            overlay->SetSummaryResizeHandler([weakThis](double wf, double hf) {
                auto self = weakThis.get();
                if (!self)
                {
                    return;
                }
                auto s = ::Agentmaster::LoadAppSettings();
                s.summaryPanelWidthFraction = wf;
                s.summaryPanelHeightFraction = hf;
                ::Agentmaster::SaveAppSettings(s);
                self->_appSettings.summaryPanelWidthFraction = wf;
                self->_appSettings.summaryPanelHeightFraction = hf;
                for (const auto& [id, ov] : self->_claudeOverlays)
                {
                    if (ov)
                    {
                        ov->SetSummarySize(wf, hf);
                    }
                }
            });
            // Agentmaster (SUMMARY_JUMP.md): a numbered prompt's jump button -> center this session's
            // terminal view on where the prompt is rendered. The overlay has no control reference, so it
            // hands us the session's prompt list + the clicked index; we resolve the tab's TermControl by
            // sessionId at click time (robust to a pane restart) and call JumpToConversationPrompt.
            overlay->SetJumpHandler([weakThis, sessionId](const std::vector<std::wstring>& msgs, int index) -> int {
                auto self = weakThis.get();
                return self ? self->_JumpToPromptInSession(sessionId, msgs, index) : -1;
            });
            // Agentmaster (SUMMARY_JUMP.md): per-icon eligibility — resolve every prompt to a row (-1 ==
            // not on screen) so the overlay can dim the jump buttons that currently won't work. One
            // linearize+resolve; the overlay calls it on (re)build, on a 5 s tick, and after a click.
            overlay->SetEligibilityHandler([weakThis, sessionId](const std::vector<std::wstring>& msgs) -> std::vector<int> {
                auto self = weakThis.get();
                return self ? self->_JumpEligibilityInSession(sessionId, msgs) : std::vector<int>{};
            });
            // Agentmaster (SUMMARY_JUMP.md §7): the overlay's row-2 ↑/↓ buttons run the SAME nav as
            // alt+up/down for THIS session — scroll to the prev/next off-screen sent prompt, highlight it,
            // and play the boundary sound at the ends (_ScrollAdjacentPrompt does all three).
            overlay->SetAdjacentPromptHandler([weakThis, sessionId](bool up) {
                if (auto self = weakThis.get())
                {
                    self->_ScrollAdjacentPrompt(sessionId, up);
                }
            });
        }
        if (const auto impl = winrt::get_self<implementation::TerminalPaneContent>(termContent))
        {
            impl->SetAgentOverlay(overlay->Root());
            impl->SetAgentSummaryOverlay(overlay->SummaryRoot()); // 2nd slot: the pencil-toggled summary panel (TAB_OVERLAY.md)
            {
                // Push the live pane size into the overlay (on the wrapper's SizeChanged + once now) so it
                // can size the summary panel as a fraction of the pane (TAB_OVERLAY.md resize). Weak so the
                // pane handler can't keep the overlay alive past tab teardown.
                auto weakOverlay = overlay->get_weak();
                impl->SetSummaryPaneSizeHandler([weakOverlay](double w, double h) {
                    if (const auto ov = weakOverlay.get())
                    {
                        ov->OnSummaryPaneSize(w, h);
                    }
                });
            }
            impl->SetAgentManaged(true); // exclude this managed-session pane from broadcast input (item 2)
            _claudeOverlays[sessionId] = overlay; // replaces any prior overlay for this id
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[overlay] " + sessionId + L" attached\n");
        }
    }

    // Agentmaster (SUMMARY_JUMP.md): center the terminal view of `sessionId`'s tab on where the i-th
    // conversation prompt is rendered. Resolves the tab's TermControl by sessionId (live, so a pane
    // restart can't strand a captured stale control), hands it the prompt list, and lets
    // TermControl::JumpToConversationPrompt do the resolve + center. Returns the buffer row, or -1 (no
    // tab / no control / prompt not on screen). UI thread.
    // Agentmaster (SUMMARY_JUMP.md): the live TermControl hosting a managed session's tab (or null). Walks
    // the tab's pane tree at call time, so a pane restart can't strand a captured stale control.
    Microsoft::Terminal::Control::TermControl TerminalPage::_ControlForSession(const std::wstring& sessionId)
    {
        const auto it = _claudeTabs.find(sessionId);
        if (it == _claudeTabs.end())
        {
            return nullptr;
        }
        const auto tab = it->second.get();
        if (!tab)
        {
            return nullptr;
        }
        const auto tabImpl = _GetTabImpl(tab);
        if (!tabImpl)
        {
            return nullptr;
        }
        Microsoft::Terminal::Control::TermControl control{ nullptr };
        if (const auto rootPane = tabImpl->GetRootPane())
        {
            rootPane->WalkTree([&](auto&& pane) {
                if (control)
                {
                    return;
                }
                if (const auto content = pane->GetContent())
                {
                    if (const auto term = content.try_as<TerminalApp::TerminalPaneContent>())
                    {
                        if (const auto impl = winrt::get_self<implementation::TerminalPaneContent>(term))
                        {
                            control = impl->GetTermControl();
                        }
                    }
                }
            });
        }
        return control;
    }

    // Agentmaster (eager-init / "Activate Tab"): start a DORMANT session's claude IN PLACE — without
    // switching to its tab. A WT background/restored tab spawns its child lazily, only on the
    // SwapChainPanel's first non-zero layout (when first SHOWN), so a window-restored / re-homed managed
    // tab the user never clicked never resumes (no claude, no hooks, no autorunner). TermControl::
    // InitializeWithSize forces the AV-safe Initialize()->Start() with a placeholder size (the laid-out
    // tab-content area; it self-corrects when the tab is later shown). Returns true if it woke one (false:
    // not hosted here / no terminal / already started). UI thread only.
    bool TerminalPage::_ActivateDormantSession(const std::wstring& sessionId)
    {
        const auto control = _ControlForSession(sessionId);
        if (!control)
        {
            return false; // not hosted in this window (or no terminal pane) — nothing to wake here
        }
        if (control.ConnectionState() != TerminalConnection::ConnectionState::NotConnected)
        {
            return false; // already started (Connecting/Connected) or ended (Closed) — not dormant
        }
        // Placeholder size = the laid-out shared tab-content area (the Manager tab is showing, so this is
        // the size a terminal tab gets). A non-positive size would make InitializeWithSize no-op, so fall
        // back to a sane default; the control resizes to its true size via _SwapChainSizeChanged when shown.
        double w = _tabContent ? _tabContent.ActualWidth() : 0.0;
        double h = _tabContent ? _tabContent.ActualHeight() : 0.0;
        if (w <= 0.0 || h <= 0.0)
        {
            w = 1200.0;
            h = 800.0;
        }
        float scale = 1.0f;
        try
        {
            if (const auto xr = _tabContent ? _tabContent.XamlRoot() : nullptr)
            {
                if (const auto rs = xr.RasterizationScale(); rs > 0)
                {
                    scale = static_cast<float>(rs);
                }
            }
        }
        CATCH_LOG();
        bool ok = false;
        try
        {
            ok = control.InitializeWithSize(w, h, scale);
        }
        CATCH_LOG();
        if (ok && _sessionRegistry)
        {
            _sessionRegistry->SetStarted(sessionId, true); // instant: the half-hollow dot fills + the "Activate" count drops (the liveness tick would also reconcile)
            ::Agentmaster::LogNav(L"activate-dormant " + ::Agentmaster::ShortId(sessionId));
        }
        return ok;
    }

    // Agentmaster (eager-init / "Activate All Tabs"): eager-init every dormant managed tab hosted in THIS
    // window. Returns the count woken. The local actuator behind the Manager's "Activate All Tabs (N)"
    // button AND the receiving half of the cross-window fan-out (ActivateAllDormantInOtherWindows).
    int TerminalPage::_ActivateAllDormantTabsLocal()
    {
        if (_claudeTabs.empty())
        {
            return 0;
        }
        // Snapshot the ids first — _ActivateDormantSession doesn't mutate _claudeTabs, but iterate a copy
        // so a concurrent bind/erase can't invalidate the iterator.
        std::vector<std::wstring> ids;
        ids.reserve(_claudeTabs.size());
        for (const auto& [id, weak] : _claudeTabs)
        {
            ids.push_back(id);
        }
        int woke = 0;
        for (const auto& id : ids)
        {
            if (_ActivateDormantSession(id))
            {
                ++woke;
            }
        }
        if (woke > 0)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[activate-all] window " + _windowId + L" woke " + std::to_wstring(woke) + L" dormant tab(s)\n");
        }
        return woke;
    }

    // Agentmaster (eager-init): flip SessionInfo::started true the moment this session's control STARTS its
    // connection (leaves NotConnected). A just-created tab's control is NotConnected, so the dot/board seed
    // it DORMANT (half-hollow); a FOCUSED tab initializes within a frame, and the ~2s liveness sweep is too
    // slow to clear the half-hollow before the user notices. TermControl.Initialized fires once, exactly
    // when _InitializeTerminal completes (right after conn.Start()), so a one-shot handler clears it
    // promptly. A background (never-shown) tab never initializes, so it correctly STAYS dormant until
    // activated. The handler holds a WEAK page ref (leak-free; the control owns the subscription for its
    // lifetime). The liveness sweep remains the backstop if the event is somehow missed.
    void TerminalPage::_TrackSessionStarted(const std::wstring& sessionId)
    {
        if (sessionId.empty() || !_sessionRegistry)
        {
            return;
        }
        const auto control = _ControlForSession(sessionId);
        if (!control)
        {
            return;
        }
        if (control.ConnectionState() != TerminalConnection::ConnectionState::NotConnected)
        {
            _sessionRegistry->SetStarted(sessionId, true); // already started (e.g. a focused tab that laid out before we got here)
            return;
        }
        auto weakThis = get_weak();
        const std::wstring id = sessionId;
        control.Initialized([weakThis, id](const auto& /*sender*/, const auto& /*args*/) {
            if (auto self = weakThis.get())
            {
                if (self->_sessionRegistry)
                {
                    self->_sessionRegistry->SetStarted(id, true);
                }
            }
        });
    }

    static winrt::Windows::Foundation::Collections::IVector<winrt::hstring> _PromptsToVector(const std::vector<std::wstring>& msgs)
    {
        std::vector<winrt::hstring> hv;
        hv.reserve(msgs.size());
        for (const auto& m : msgs)
        {
            hv.emplace_back(m);
        }
        return winrt::single_threaded_vector<winrt::hstring>(std::move(hv));
    }

    int TerminalPage::_JumpToPromptInSession(const std::wstring& sessionId, const std::vector<std::wstring>& msgs, int index)
    {
        if (index < 0 || msgs.empty())
        {
            return -1;
        }
        const auto control = _ControlForSession(sessionId);
        if (!control)
        {
            return -1;
        }
        // Nav audit: the user clicked a summary-panel ▸ to scroll the session's terminal to where that
        // prompt is rendered (SUMMARY_JUMP.md). row >= 0 == centered on that buffer row; -1 == the
        // prompt couldn't be located in the live scrollback. (The per-render eligibility probe
        // _JumpEligibilityInSession is deliberately NOT logged — it isn't a user action.)
        const int row = control.JumpToConversationPrompt(_PromptsToVector(msgs), static_cast<uint32_t>(index));
        ::Agentmaster::LogNav(L"jump-to-prompt " + ::Agentmaster::ShortId(sessionId) + L" prompt#" + std::to_wstring(index) + (row >= 0 ? (L" -> buffer row " + std::to_wstring(row)) : std::wstring{ L" (not found in scrollback)" }));
        return row;
    }

    // Agentmaster (SUMMARY_JUMP.md): a row per prompt (>=0 == resolvable/on-screen, -1 == not) for the
    // summary panel's per-icon eligibility dimming. ONE linearize+resolve in ControlCore. Empty on no control.
    std::vector<int> TerminalPage::_JumpEligibilityInSession(const std::wstring& sessionId, const std::vector<std::wstring>& msgs)
    {
        std::vector<int> rows;
        if (msgs.empty())
        {
            return rows;
        }
        const auto control = _ControlForSession(sessionId);
        if (!control)
        {
            return rows;
        }
        // Defense-in-depth + efficiency (SUMMARY_JUMP.md §4a): a tab restored on relaunch but never
        // activated has a live control whose ControlCore has not Initialize()d yet (its TextBuffer is
        // null) — reading it AVs. ControlCore::ResolveConversationPromptRows guards this internally, but
        // skip the cross-ABI no-op entirely for a NotConnected control (the connection starts on the same
        // first-layout gate as Initialize, so NotConnected == not-yet-initialized). Empty => all icons dim.
        // Only NotConnected is skipped: a Closed (ended) session keeps its scrollback, so jumping still works.
        if (control.ConnectionState() == winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::NotConnected)
        {
            return rows;
        }
        const auto resolved = control.ResolveConversationPromptRows(_PromptsToVector(msgs));
        rows.reserve(resolved.Size());
        for (const auto r : resolved)
        {
            rows.push_back(r);
        }
        return rows;
    }

    // Agentmaster (alt+up / alt+down prompt nav): the focused tab's managed CLAUDE sessionId, or empty.
    // Prompt nav resolves the conversation's SENT prompts from the transcript and finds them in the live
    // buffer (SUMMARY_JUMP.md), so it applies only to a managed Claude session; a Codex tab (no in-buffer
    // prompt resolve in v1), a shell, the Manager tab, or an external all return empty, so the action falls
    // back to WT's default MoveFocus. UI thread.
    std::wstring TerminalPage::_FocusedPromptNavSession()
    {
        const auto focused = _GetFocusedTab();
        if (!focused)
        {
            return {};
        }
        const auto sid = _ClaudeSessionForTab(focused);
        if (sid.empty() || !_sessionRegistry)
        {
            return {};
        }
        const auto info = _sessionRegistry->Get(sid);
        if (!info || info->kind != ::Agentmaster::AgentKind::Claude)
        {
            return {};
        }
        return sid;
    }

    // Agentmaster (alt+up / alt+down prompt nav): WORKER-thread helper — (re)read a session's sent prompts
    // when its transcript GREW. `path` / `mtime` are in/out: `path` is resolved when empty, `mtime` is
    // advanced to the file's current write time. Returns true (and fills `outFresh`) when it re-read, false
    // when the cached list is still current. Shared by _ScrollAdjacentPrompt (a jump) and
    // _RefreshPromptNavCache (the 30 s focused refresh) so the two stay byte-for-byte consistent.
    static bool ReadPromptNavIfGrown(const std::wstring& sessionId, std::wstring& path, int64_t& mtime, bool hadPrompts, std::vector<std::wstring>& outFresh)
    {
        // Agentmaster (extra-safe): this runs on a BACKGROUND thread inside a fire_and_forget coroutine,
        // so ANY exception escaping here (a transcript-parse throw from AnalyzeSessionTranscript, a
        // std::bad_alloc on a huge transcript, etc.) would unwind out of the coroutine with no caller to
        // catch it and std::terminate the whole app -- killing EVERY session at once. Contain it: degrade
        // to "no refresh" (the caller then navigates with the cached/empty prompt list, which is always
        // safe). Logged, never silently hidden, so a genuine parse bug stays discoverable.
        try
        {
            if (path.empty())
            {
                path = ::Agentmaster::ResolveClaudeTranscriptPath(sessionId);
            }
            if (path.empty())
            {
                return false;
            }
            const int64_t prevMtime = mtime;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
            {
                ULARGE_INTEGER li{};
                li.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                li.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                mtime = static_cast<int64_t>(li.QuadPart);
            }
            if (!hadPrompts || mtime != prevMtime)
            {
                outFresh = ::Agentmaster::AnalyzeSessionTranscript(path, 0 /* whole file */).userMsgs;
                return true;
            }
        }
        catch (...)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[prompt-nav] ReadPromptNavIfGrown: swallowed exception (no crash)\n");
        }
        return false;
    }

    // Agentmaster (alt+up / alt+down prompt nav): center the view on the nearest SENT prompt that is
    // currently OFF-SCREEN in the given direction. To NEVER lose sync with the live buffer, EACH press
    // re-reads the transcript's sent prompts (mtime-gated — a stat every press, a full re-read only when it
    // GREW) BEFORE navigating, so the needle list is current; the buffer-position resolve in
    // TermControl::ScrollToAdjacentConversationPrompt is already from-scratch on every call. (Previously a
    // warm cache navigated FIRST and refreshed AFTER, so a just-sent prompt wasn't catchable until the next
    // press — the "captures not caught / not refreshed" report.) fire_and_forget: UI -> worker -> UI.
    winrt::fire_and_forget TerminalPage::_ScrollAdjacentPrompt(std::wstring sessionId, bool up)
    {
        auto strongThis{ get_strong() }; // keep the page alive across the co_awaits

        // Agentmaster (extra-safe): this is a fire_and_forget — ANY exception that escapes it (a std::bad_alloc
        // from the cache map/vector ops, or a teardown-race throw from a co_await resume when the dispatcher is
        // shutting down) calls std::terminate and kills the whole app. The heavy steps already self-contain
        // (ReadPromptNavIfGrown + _NavigateAdjacentPrompt each catch internally); this wraps the remaining
        // synchronous map writes AND the suspension points so nothing on this path can ever crash the process.
        try
        {
            // (UI thread) snapshot the cache KEY (path, mtime, whether we have prompts yet) — not the list,
            // since we always navigate AFTER the refresh below.
            std::wstring path;
            int64_t mtime = 0;
            bool hadPrompts = false;
            if (const auto it = _promptNavCache.find(sessionId); it != _promptNavCache.end())
            {
                path = it->second.path;
                mtime = it->second.mtime;
                hadPrompts = !it->second.prompts.empty();
            }

            // (worker) re-read the prompt list when the transcript grew — BEFORE we navigate.
            co_await winrt::resume_background();
            std::vector<std::wstring> fresh;
            const bool refreshed = ReadPromptNavIfGrown(sessionId, path, mtime, hadPrompts, fresh);

            // (UI thread) store any refresh, then navigate with the FRESHEST prompt list (a fresh
            // position resolve happens inside _NavigateAdjacentPrompt regardless).
            co_await wil::resume_foreground(Dispatcher());
            std::vector<std::wstring> prompts;
            if (refreshed)
            {
                auto& e = _promptNavCache[sessionId];
                e.path = path;
                e.mtime = mtime;
                e.prompts = fresh;
                prompts = std::move(fresh);
            }
            else if (const auto it = _promptNavCache.find(sessionId); it != _promptNavCache.end())
            {
                prompts = it->second.prompts; // transcript unchanged -> the cached list is already current
            }
            _NavigateAdjacentPrompt(sessionId, prompts, up);
        }
        catch (...)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[prompt-nav] _ScrollAdjacentPrompt: swallowed exception (no crash)\n");
        }
    }

    // Agentmaster (alt+up / alt+down prompt nav, SUMMARY_JUMP.md §7): the 30 s focused refresh — re-read the
    // session's sent prompts (mtime-gated) into _promptNavCache and re-resolve the summary panel's jump-icon
    // eligibility, WITHOUT navigating. Keeps the jump data in sync with a buffer that scrolls under an
    // idle-but-focused session, so the next jump (and the panel's icon dimming) is already current.
    winrt::fire_and_forget TerminalPage::_RefreshPromptNavCache(std::wstring sessionId)
    {
        auto strongThis{ get_strong() };

        // Agentmaster (extra-safe): fire_and_forget — contain any escaping exception (a cache-map bad_alloc, a
        // teardown-race resume throw; see _ScrollAdjacentPrompt) so a background refresh can never
        // std::terminate the app. RefreshJumpData re-resolves the buffer, which is already init-guarded.
        try
        {
            std::wstring path;
            int64_t mtime = 0;
            bool hadPrompts = false;
            if (const auto it = _promptNavCache.find(sessionId); it != _promptNavCache.end())
            {
                path = it->second.path;
                mtime = it->second.mtime;
                hadPrompts = !it->second.prompts.empty();
            }

            co_await winrt::resume_background();
            std::vector<std::wstring> fresh;
            const bool refreshed = ReadPromptNavIfGrown(sessionId, path, mtime, hadPrompts, fresh);

            co_await wil::resume_foreground(Dispatcher());
            if (refreshed)
            {
                auto& e = _promptNavCache[sessionId];
                e.path = path;
                e.mtime = mtime;
                e.prompts = std::move(fresh);
            }
            // Re-resolve the summary panel's jump-icon eligibility against the live buffer (a no-op if the panel
            // is closed / has no jump buttons). The position resolve is fresh; this only re-dims stale icons.
            if (const auto it = _claudeOverlays.find(sessionId); it != _claudeOverlays.end() && it->second)
            {
                it->second->RefreshJumpData();
            }
        }
        catch (...)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[prompt-nav] _RefreshPromptNavCache: swallowed exception (no crash)\n");
        }
    }

    // Agentmaster (alt+up / alt+down prompt nav): the UI-thread half — resolve the session's live control
    // and scroll it to the nearest off-screen prompt up/down, else play the boundary sound (no prompts /
    // no control / nothing off-screen that way).
    void TerminalPage::_NavigateAdjacentPrompt(const std::wstring& sessionId, const std::vector<std::wstring>& prompts, bool up)
    {
        if (prompts.empty())
        {
            _PlayPromptNavLimitSound();
            return;
        }
        const auto control = _ControlForSession(sessionId);
        if (!control || control.ConnectionState() == winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::NotConnected)
        {
            _PlayPromptNavLimitSound();
            return;
        }
        // Agentmaster (extra-safe): the cross-ABI scroll call reads the live viewport and touches the
        // ScrollBar XAML part; an unexpected winrt::hresult_error (e.g. a not-yet-applied template's null
        // ScrollBar, or a teardown race) would otherwise unwind into the fire_and_forget caller and
        // std::terminate the app. Contain it here -- a failed navigation just plays the boundary sound.
        // (The AV path itself is already guarded inside ScrollToAdjacentConversationPrompt; this catches
        // the THROWN-exception cousins.) Logged, never silently hidden.
        try
        {
            const int idx = control.ScrollToAdjacentConversationPrompt(_PromptsToVector(prompts), up);
            // -2: stepping DOWN past the last prompt scrolled to the BOTTOM (live tail). We DID move, so no
            // boundary sound; clear any stale summary highlight (we're past the numbered prompts now).
            if (idx == -2)
            {
                if (const auto it = _claudeOverlays.find(sessionId); it != _claudeOverlays.end() && it->second)
                {
                    it->second->HighlightSummaryMessage(-1);
                }
                return;
            }
            if (idx < 0)
            {
                _PlayPromptNavLimitSound(); // no further sent prompt off-screen in that direction
                return;
            }
            // Highlight the message we landed on in this session's summary panel (if the panel is open).
            if (const auto it = _claudeOverlays.find(sessionId); it != _claudeOverlays.end() && it->second)
            {
                it->second->HighlightSummaryMessage(idx);
            }
        }
        catch (...)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[prompt-nav] _NavigateAdjacentPrompt: scroll/highlight threw (no crash)\n");
            _PlayPromptNavLimitSound();
        }
    }

    // Agentmaster (alt+up / alt+down prompt nav): boundary feedback — distinct from the jump chime
    // (SystemAsterisk) so "you've hit the end" reads differently. Async; winmm is already in the link.
    void TerminalPage::_PlayPromptNavLimitSound()
    {
        ::PlaySoundW(L"SystemExclamation", nullptr, SND_ALIAS | SND_ASYNC);
    }

    // Agentmaster (alt+up / alt+down prompt nav): step the focused session's view to the previous / next
    // SENT prompt that is currently off-screen. On a managed Claude session this overrides WT's default
    // alt+up/down pane-focus move; on any other tab it falls back to MoveFocus (preserving upstream
    // behavior + the GH#6129 keychord propagation when no pane is there to move to).
    void TerminalPage::_HandleAgentScrollToPrevPrompt(const winrt::Windows::Foundation::IInspectable& /*sender*/, const ActionEventArgs& args)
    {
        if (const auto sid = _FocusedPromptNavSession(); !sid.empty())
        {
            _ScrollAdjacentPrompt(sid, /*up*/ true);
            args.Handled(true);
        }
        else
        {
            args.Handled(_MoveFocus(FocusDirection::Up));
        }
    }

    void TerminalPage::_HandleAgentScrollToNextPrompt(const winrt::Windows::Foundation::IInspectable& /*sender*/, const ActionEventArgs& args)
    {
        if (const auto sid = _FocusedPromptNavSession(); !sid.empty())
        {
            _ScrollAdjacentPrompt(sid, /*up*/ false);
            args.Handled(true);
        }
        else
        {
            args.Handled(_MoveFocus(FocusDirection::Down));
        }
    }

    // Agentmaster (shift+home "home / jump-back" toggle): do EXACTLY what the tab-strip nav buttons do —
    // they are the SOURCE OF TRUTH, so this routes through their click handlers instead of reinventing the
    // logic (an earlier version used _mruTabs, which ignored the selected card). Off the Manager tab -> the
    // "Home" button (_OnManagerHomeButtonClick): jump to the pinned Manager tab. On the Manager tab -> the
    // "Jump Back" button (_OnManagerJumpBackButtonClick): return to the tab of the session SELECTED in the
    // Manager lens (the card auto-selected when you came from it) via _ActivateClaudeSession — respecting
    // the selected card, cross-window-safe, and a no-op when nothing is selected. Works everywhere: over a
    // focused terminal the chord is intercepted by TermControl's ActionMap lookup, and on the Manager tab's
    // own text boxes it is tunneled in by _ManagerPaneNavPreviewKeyDown (which lists AgentToggleManagerTab
    // in its allow-list). UI thread. (nullptr for the RoutedEventArgs is idiomatic here — the handlers
    // ignore both args, like TitleChanged.raise(*this, nullptr).)
    void TerminalPage::_HandleAgentToggleManagerTab(const winrt::Windows::Foundation::IInspectable& sender, const ActionEventArgs& args)
    {
        // The Manager tab is pinned + non-closable, but it is null very early in startup and after a
        // window's last teardown — be defensive so the chord just no-ops then.
        if (!_managerTab)
        {
            args.Handled(false);
            return;
        }

        if (_GetFocusedTab() == _managerTab)
        {
            // On the Manager tab -> the "Jump Back" button's action: return to the SELECTED session's tab.
            _OnManagerJumpBackButtonClick(sender, nullptr);
        }
        else
        {
            // Off the Manager tab -> the "Home" button's action: jump to the pinned Manager tab.
            _OnManagerHomeButtonClick(sender, nullptr);
        }
        // Consume the chord either way so it never falls through to the terminal's select-to-line-start.
        args.Handled(true);
    }

    // Agentmaster (TAB_OVERLAY.md summary panel): the per-tab pencil button toggles the summary panel's
    // visibility, which is a GLOBAL setting (AppSettings::showSummaryPanel) so the choice is shared across
    // windows and survives restart. Mirror the treeSort / archiveSplitFraction pattern: a freshest-disk
    // read-modify-write of just this field (so a concurrent cog Save / another window can't be clobbered),
    // keep this window's in-memory copy in step, then apply it LIVE to every linked overlay in this window
    // (other already-open windows adopt it on their next launch, like treeSort).
    void TerminalPage::_ToggleSummaryPanel()
    {
        auto s = ::Agentmaster::LoadAppSettings();
        const bool next = !s.showSummaryPanel;
        s.showSummaryPanel = next;
        ::Agentmaster::SaveAppSettings(s);
        _appSettings.showSummaryPanel = next;
        for (const auto& [id, ov] : _claudeOverlays)
        {
            if (ov)
            {
                ov->SetSummaryEnabled(next);
            }
        }
    }

    // Agentmaster (TAB_OVERLAY.md summary panel): the wrap-line toggle in the panel's times bar flips
    // whether messages keep their real newlines (multi-line) or collapse to a literal "\n". Like the
    // pencil/showSummaryPanel, it is a GLOBAL setting (AppSettings::summaryPanelWrapNewlines) shared
    // across windows and persisted: a freshest-disk read-modify-write of just this field (so a concurrent
    // cog Save / another window can't be clobbered), keep this window's in-memory copy in step, then apply
    // it LIVE to every linked overlay in this window (other already-open windows adopt it on next launch).
    void TerminalPage::_ToggleSummaryWrap()
    {
        auto s = ::Agentmaster::LoadAppSettings();
        const bool next = !s.summaryPanelWrapNewlines;
        s.summaryPanelWrapNewlines = next;
        ::Agentmaster::SaveAppSettings(s);
        _appSettings.summaryPanelWrapNewlines = next;
        for (const auto& [id, ov] : _claudeOverlays)
        {
            if (ov)
            {
                ov->SetSummaryWrapNewlines(next);
            }
        }
        _InvalidateSessionsSummaryForToggle(); // the Sessions detail renders the same box, honoring this flag
    }

    // Agentmaster (TAB_OVERLAY.md summary panel): the truncate toggle (left of the wrap toggle in the
    // panel's times bar) flips whether each message is capped — 6 lines when wrapped (7th+ -> "..."),
    // else 500 chars — or shown in full (the default). Same GLOBAL setting + freshest-disk RMW + live
    // broadcast idiom as _ToggleSummaryWrap (AppSettings::summaryPanelTruncate).
    void TerminalPage::_ToggleSummaryTruncate()
    {
        auto s = ::Agentmaster::LoadAppSettings();
        const bool next = !s.summaryPanelTruncate;
        s.summaryPanelTruncate = next;
        ::Agentmaster::SaveAppSettings(s);
        _appSettings.summaryPanelTruncate = next;
        for (const auto& [id, ov] : _claudeOverlays)
        {
            if (ov)
            {
                ov->SetSummaryTruncate(next);
            }
        }
        _InvalidateSessionsSummaryForToggle(); // the Sessions detail renders the same box, honoring this flag
    }

    // Agentmaster: a wrap/truncate toggle changed how the summary box renders, but the Sessions page caches
    // the RENDERED text per (id, mtime) — which now bakes the wrap/truncate flags. Drop the cache so the
    // next view re-renders with the new flags, and re-render the open detail in place if the page is up.
    // (Cheap: the cache is per-session rendered text; the re-analyze is off-thread + only for the viewed row.)
    void TerminalPage::_InvalidateSessionsSummaryForToggle()
    {
        _sessionsSummaryCache.clear();
        if (_sessionsPageVisible.load(std::memory_order_relaxed) && !_sessionsSelectedId.empty())
        {
            _ShowSessionsDetail(_sessionsSelectedId);
        }
    }

    // Agentmaster (conversation lineage): the previous-session toggle (leftmost in the panel's times bar,
    // shown only when the session was /compact'ed) flips whether the pre-compaction segment(s) render
    // above the current Messages. Same GLOBAL setting + freshest-disk RMW + live broadcast idiom as
    // _ToggleSummaryWrap / _ToggleSummaryTruncate (AppSettings::summaryPanelShowPrevious).
    void TerminalPage::_ToggleSummaryPrevious()
    {
        auto s = ::Agentmaster::LoadAppSettings();
        const bool next = !s.summaryPanelShowPrevious;
        s.summaryPanelShowPrevious = next;
        ::Agentmaster::SaveAppSettings(s);
        _appSettings.summaryPanelShowPrevious = next;
        for (const auto& [id, ov] : _claudeOverlays)
        {
            if (ov)
            {
                ov->SetSummaryShowPrevious(next);
            }
        }
    }

    // Agentmaster (OBSERVER.md §4/§11d): attach-or-update a registry-LESS "○ <kind> · unlinked" badge
    // on a NON-bound tab the observer classified — a shell ("pwsh" / "cmd"), a never-prompted claude
    // ("claude": correlated but no transcript id yet, §11d), or codex. Keyed by WT_SESSION (there is no
    // sessionId). If a badge already exists for the tab its kind is updated in place (so a pwsh tab
    // that becomes a claude flips pwsh -> claude); the real _AttachClaudeOverlay replaces it once a
    // claude resolves an id. Idempotent — ShowActivity skips a re-render when the kind is unchanged.
    void TerminalPage::_SetTabActivityBadge(const TerminalApp::Tab& tab, const std::wstring& wtSession, const std::wstring& kind)
    {
        if (!tab || wtSession.empty())
        {
            return;
        }
        // Tab status dot: an OBSERVED-but-unmanaged tab (pwsh / cmd / unprompted claude / codex)
        // carries a dim gray dot — same visual language as the tree/board (state color = managed,
        // dim = merely observed). Set BEFORE the overlay gate below: the dot is tab-strip chrome,
        // independent of the in-terminal HUD toggle. Re-asserted every probe tick; _SetTabAgentDot
        // is idempotent on the unchanged color. (A tab that becomes managed gets its state-colored
        // dot from the bind tail, which runs after this badge is dropped.)
        _SetTabAgentDot(tab, winrt::Windows::UI::ColorHelper::FromArgb(0x70, 0x80, 0x80, 0x80));
        // Tab tooltip: the unmanaged/observe twin of the managed session tooltip — "○ <kind> · unlinked"
        // over the tab's OWN title (kept, so we never regress the default tooltip's title for shell tabs).
        // Tab-strip chrome like the dot, so set BEFORE the in-terminal HUD gate below. Idempotent on the
        // Tab side (signature guard), so the per-tick probe re-assert is free.
        if (const auto impl = _GetTabImpl(tab))
        {
            const std::wstring sig = std::wstring{ L"observe\x1f" } + kind + L"\x1f" + std::wstring{ impl->Title() };
            impl->SetAgentToolTip(TtBuildObserveCard(kind, std::wstring{ impl->Title() }), winrt::hstring{ sig });
        }
        if (!_appSettings.showTabOverlay)
        {
            return;
        }
        if (const auto existing = _pendingOverlays.find(wtSession); existing != _pendingOverlays.end())
        {
            if (existing->second)
            {
                existing->second->ShowActivity(kind); // update the kind in place (no-op if unchanged)
            }
            return;
        }
        const auto tabImpl = _GetTabImpl(tab);
        if (!tabImpl)
        {
            return;
        }
        TerminalApp::TerminalPaneContent termContent{ nullptr };
        if (const auto rootPane = tabImpl->GetRootPane())
        {
            rootPane->WalkTree([&](auto&& pane) {
                if (termContent)
                {
                    return;
                }
                if (const auto content = pane->GetContent())
                {
                    if (const auto term = content.try_as<TerminalApp::TerminalPaneContent>())
                    {
                        termContent = term;
                    }
                }
            });
        }
        if (!termContent)
        {
            return;
        }
        auto overlay = winrt::make_self<implementation::AgentTabOverlay>();
        overlay->ShowActivity(kind);
        overlay->SetOverlayOpacities(_appSettings.tabOverlayRestOpacity, _appSettings.tabOverlayHoverOpacity); // honor the GLOBAL overlay opacities (TAB_OVERLAY.md) like a linked badge
        if (const auto impl = winrt::get_self<implementation::TerminalPaneContent>(termContent))
        {
            impl->SetAgentOverlay(overlay->Root());
            _pendingOverlays[wtSession] = overlay;
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[overlay] observe badge kind=" + kind + L" wt=" + wtSession + L"\n");
        }
    }

    // Agentmaster: reconcile a session's tab binding on a SessionStart, keyed by the STABLE
    // WT_SESSION `tabToken` (the ConPTY identity, which — unlike the Claude session id — never
    // changes for the life of a tab). Now fired for EVERY SessionStart, this one path handles:
    //   * ADOPT  — a `claude` the user typed into a `+` tab (a new, unknown id wired by the PATH
    //              shim): find the live ConPTY whose WT_SESSION == tabToken and bind a stdin
    //              injector to it, promoting it to full observe+control (Rule #3).
    //   * RE-HOME — a tab whose claude changed conversation id via the in-session `/resume` (the id
    //              changes, the tabToken does not). The same tab is found bound to an OLD id; that
    //              old id is archived (queue kept restorable) and the tab is re-pointed to the new
    //              id — even if the new id is a previously-known/archived one (which never creates a
    //              record here, so the old new-record-only adoption gate missed it).
    // Idempotent: a SessionStart for an already-bound session (every Manager-launched one) fast-
    // returns. Best-effort: a claude with no matching connection in this window stays observe-only
    // and the registry is left untouched. Runs on the UI thread (XAML walk).
    winrt::fire_and_forget TerminalPage::_AdoptExternalSession(winrt::hstring sessionId, winrt::hstring cwd, winrt::hstring tabToken)
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());

        if (!_sessionRegistry)
        {
            co_return;
        }

        const std::wstring id{ sessionId };

        const auto lower = [](std::wstring s) {
            for (auto& c : s)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return s;
        };
        const std::wstring token = lower(std::wstring{ tabToken });

        // Idempotent fast path: a SessionStart for a session already bound to a live tab (and
        // carrying its overlay, when overlays are enabled) needs nothing. This is the common case —
        // every Manager-launched session re-emits SessionStart and lands here, so the reconcile is
        // a cheap no-op for them (no walk, no registry writes, no cross-window notify spam).
        if (const auto it = _claudeTabs.find(id); it != _claudeTabs.end())
        {
            if (const auto t = it->second.get())
            {
                const bool overlayOk = (_claudeOverlays.find(id) != _claudeOverlays.end()) || !_appSettings.showTabOverlay;
                if (overlayOk)
                {
                    co_return;
                }
            }
        }

        // Diagnostic: we are actually going to try to (re)bind this session to a tab (not a fast
        // no-op). Shows how many terminal tabs this window has, so a mismatch is visible in the log.
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[bind-try] " + id + L" token=" + token + L" tabs=" + std::to_wstring(_tabs.Size()) + L"\n");

        if (token.empty())
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[adopt] " + id + L" observe-only (no tabToken)\n");
            co_return;
        }

        // Find the live ConPTY in THIS window whose WT_SESSION == token (the STABLE tab identity).
        TerminalApp::Tab hostTab{ nullptr };
        TerminalConnection::ITerminalConnection match{ nullptr };
        for (const auto& projectedTab : _tabs)
        {
            const auto tabImpl = _GetTabImpl(projectedTab);
            if (!tabImpl)
            {
                continue;
            }
            TerminalConnection::ITerminalConnection found{ nullptr };
            tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                if (found)
                {
                    return;
                }
                const auto content = pane->GetContent();
                if (!content)
                {
                    return;
                }
                const auto term = content.try_as<TerminalApp::TerminalPaneContent>();
                if (!term)
                {
                    return;
                }
                const auto ctrl = term.GetTermControl();
                if (!ctrl)
                {
                    return;
                }
                const auto conn = ctrl.Connection();
                if (!conn)
                {
                    return;
                }
                if (lower(::Microsoft::Console::Utils::GuidToPlainString(conn.SessionId())) == token)
                {
                    found = conn;
                }
            });
            if (found)
            {
                hostTab = projectedTab;
                match = found;
                break;
            }
        }

        // No connection in this window hosts that WT_SESSION — a claude hosted outside this app (or
        // in another window). Stay observe-only; do NOT touch the registry (avoids notify spam on
        // every other window for a session it doesn't host).
        if (!match || !hostTab)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[adopt] " + id + L" observe-only (no connection for " + token + L")\n");
            co_return;
        }

        _BindClaudeSessionToTab(hostTab, match, id, std::wstring{ cwd }, L"WT_SESSION " + token);
        co_return;
    }

    // Agentmaster: bind a Claude session id to a specific live tab + ConPTY connection — the shared
    // tail of BOTH correlation paths: WT_SESSION-tabToken hook adoption (_AdoptExternalSession) and
    // the Fleet Observer's PEB correlation table (_ObserverProbe). Runs on the UI thread. Handles the in-
    // session /resume RE-HOME (a different id already bound to this tab is archived, its Auto Testing
    // kept restorable), derives/pins the title (one-title rule, Rule #11), binds the stdin injector
    // (Rule #3: inject by sessionId), marks the session live, colors the tab per working dir (Rule
    // #12), attaches the per-tab overlay, and persists. `origin` is a human tag for the log only.
    void TerminalPage::_BindClaudeSessionToTab(const TerminalApp::Tab& hostTab,
                                               const TerminalConnection::ITerminalConnection& conn,
                                               const std::wstring& id,
                                               const std::wstring& cwd,
                                               const std::wstring& origin)
    {
        if (!_sessionRegistry || !hostTab || !conn || id.empty())
        {
            return;
        }

        // RE-HOME (in-session `/resume`): if this SAME tab is currently bound to a DIFFERENT session
        // id, the conversation switched ids on a stable ConPTY. Supersede the old id — archive it
        // (its Auto Testing stays restorable) and drop its tab/overlay binding — then re-point below.
        // reHomedFromOtherId records that we did so, so the title block below does NOT let the old
        // (now-archived) conversation's still-pinned tab title bleed onto the new conversation
        // (Rule #11: each session owns its title; the new id must show ITS name, not the archived one's).
        bool reHomedFromOtherId = false;
        for (const auto& [oldId, weakOld] : _claudeTabs)
        {
            if (const auto t = weakOld.get(); t && t == hostTab && oldId != id)
            {
                // Agentmaster (--fork-session source-id echo BACKSTOP): if the session already bound to
                // this tab was FORKED FROM the incoming id, this "new conversation id" is actually the
                // fork's SOURCE — a `--fork-session` claude fires its first SessionStart under the source
                // id (see SessionInfo::forkParentId). It is NOT an in-session /resume; the fork keeps
                // writing its own id and every later hook lands there. Do NOT re-home (that would track
                // the inactive source and orphan the fork); keep the fork bound and retire the transient
                // source-id record. The registry guard (SessionRegistry::OnHookEvent) normally absorbs
                // this echo earlier via the eagerly-stamped tabToken; this is the defense-in-depth catch
                // for any echo that reaches the bind path anyway. (ids are canonical lowercase GUIDs.)
                if (const auto oldInfo = _sessionRegistry->Get(oldId); oldInfo && !oldInfo->forkParentId.empty() && oldInfo->forkParentId == id)
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[fork-echo] kept " + oldId + L" bound; ignored source-id " + id + L" on its tab\n");
                    _sessionRegistry->Update(id, [](::Agentmaster::SessionInfo& s) { s.live = false; }); // retire the transient source-id record
                    return; // leave the fork (oldId) as the sole binding for this tab
                }
                // COPY the key first: `oldId` is a reference INTO the _claudeTabs node, and the
                // erase below frees that node — using `oldId` afterwards (the _claudeOverlays erase)
                // would hash freed memory -> AV. (Latent use-after-free; the observer's more frequent
                // re-homes exposed it.)
                const std::wstring superseded = oldId;
                _sessionRegistry->SetInjector(superseded, nullptr);
                _sessionRegistry->Update(superseded, [](::Agentmaster::SessionInfo& s) {
                    s.live = false;
                    s.pendingConfirmPromptId.clear();
                });
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[rehome] " + superseded + L" -> " + id + L" (same tab, new conversation id)\n");
                // Nav audit: a TAB SWAPPED its bound session id (claude switched conversation in place
                // — /clear, /resume into another conversation, or /compact). Surface it in the [nav]
                // trail so "my tab is suddenly a different session" is followable, not just inferable
                // from the deeper [rehome] line. The old id is archived (its Auto Testing stays restorable).
                ::Agentmaster::LogNav(L"tab-swap " + ::Agentmaster::ShortId(superseded) + L" -> " + ::Agentmaster::ShortId(id) + L" (same tab; claude switched conversation \x2014 /clear, /resume or /compact)");
                _claudeTabs.erase(superseded);
                _claudeOverlays.erase(superseded);
                reHomedFromOtherId = true; // the tab's pinned text is `superseded`'s title — don't bleed it onto `id`
                break; // one tab hosts one session
            }
        }

        // Give the card a readable title from its working dir if it has none.
        const std::wstring ttl = ::Agentmaster::DeriveSessionTitle(cwd);
        _sessionRegistry->Update(id, [&ttl](::Agentmaster::SessionInfo& s) {
            if (s.title.empty())
            {
                s.title = ttl;
            }
        });

        // Bind the id to this tab — full observe+control (Rule #3: inject by sessionId).
        const auto connection = conn;
        _sessionRegistry->SetInjector(id, [connection](const std::wstring& text) {
            const auto* begin = reinterpret_cast<const char16_t*>(text.data());
            connection.WriteInput(winrt::array_view<const char16_t>{ begin, begin + text.size() });
        });
        _claudeTabs[id] = winrt::make_weak(hostTab);
        _sessionRegistry->Update(id, [](::Agentmaster::SessionInfo& s) {
            s.live = true; // running in a tab we control now (covers /resume to a previously-archived id)
        });

        // One-title rule (Rule #11) — give the tab THIS session's own title:
        //   * Normal adopt/move bind: a name the user already gave the tab wins — mirror the tab's
        //     runtime text into the registry; otherwise pin the tab to the managed/derived name.
        //   * In-session `/resume` re-home (reHomedFromOtherId): the tab's runtime text is still the
        //     SUPERSEDED (now-archived) conversation's pinned title. It must NOT bleed onto the new
        //     conversation — that one owns its identity: its prior managed title if it had one, else
        //     the cwd-derived ttl set above (DeriveSessionTitle is never empty, so the else-branch pin
        //     always fires). Skip the tab-text-wins mirror and pin the tab to the new id's title,
        //     flipping the strip from the archived name to the new one.
        // Either way `id` is now in _claudeTabs, so later renames sync via _SyncClaudeTitleFromTab.
        if (const auto impl = _GetTabImpl(hostTab))
        {
            const std::wstring tabText{ impl->GetTabText() };
            if (!tabText.empty() && !reHomedFromOtherId)
            {
                _sessionRegistry->Update(id, [&tabText](::Agentmaster::SessionInfo& s) { s.title = tabText; });
            }
            else if (const auto s = _sessionRegistry->Get(id); s && !s->title.empty())
            {
                _SetClaudeTabTextPinned(impl, winrt::hstring{ s->title }); // pinned: registry->tab, no write-back
            }
        }
        _ApplyDirColorToTab(hostTab, cwd); // per-directory tab color
        _AttachClaudeOverlay(hostTab, id); // per-tab "link badge" overlay (TAB_OVERLAY.md)
        // Tab status dot: managed now — seed the strip dot from the session's current state (the
        // engine-init registry observer keeps it live from here on). A re-homed restored tab whose
        // claude hasn't started yet (started=false, not external) seeds the half-hollow dormant dot;
        // the liveness tick reconciles started from ConnectionState() within one cadence.
        if (const auto s = _sessionRegistry->Get(id))
        {
            _SetTabAgentDot(hostTab, AgentStatusColorFor(s->state), !s->started && !s->external);
        }
        _TrackSessionStarted(id); // Agentmaster (eager-init): clear the dormant flag once this control's connection starts
        _RefreshTabFavoriteCrown(id); // FAVORITES.md: show the gold crown if this session is starred
        _UpdateTabAgentToolTip(hostTab, id); // tab tooltip: replace any "○ … unlinked" observe tooltip with the rich managed one
        ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[adopt] " + id + L" bound via " + origin + L"\n");
    }

    // Agentmaster: the scanner's interval liveness sweep, marshaled onto THIS window's UI thread
    // (the ConnectionState read + XAML tab walk are UI-thread-only, so the plain-C++ scanner can't
    // do it itself — it just TICKS this probe on its slow cadence). Walk this window's claude tabs;
    // any whose hosting ConPTY connection has reached Closed — the claude.exe exited (crashed with
    // no SessionEnd, ran `/exit`, or a clean SessionEnd that only set state=Done) — is archived in
    // place: flip the record to Archived (live=false), unbind its stdin injector, drop the
    // sessionId->tab mapping, and persist. The (now-dead) tab is LEFT for the user to read/close;
    // the session leaves the Triage Board and lists under "Archived", restorable via `claude
    // --resume`. No confirm dialog — the process is already gone (unlike the user-initiated archive
    // seam). Best-effort + idempotent: a tab with no terminal, or any live terminal, is left alone.
    winrt::fire_and_forget TerminalPage::_SweepClaudeLiveness()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());

        if (!_sessionRegistry || _claudeTabs.empty())
        {
            co_return;
        }

        // Collect first, mutate after — never erase from _claudeTabs while iterating it.
        std::vector<std::wstring> dead;
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            const auto tab = weakTab.get();
            if (!tab)
            {
                continue; // weak_ref already lapsed (tab fully torn down) — the close path owns it
            }
            const auto tabImpl = _GetTabImpl(tab);
            if (!tabImpl)
            {
                continue;
            }
            // Tab tooltip: refresh on the slow sweep cadence so the relative "ago"/timing stays honest
            // for a quiet (Idle/Waiting) session that emits no registry change between turns. Cheap —
            // the Tab signature-guards the rebuild, so the XAML changes only when a displayed value does.
            _UpdateTabAgentToolTip(tab, id);
            // The session's OWN connection WT_SESSION (== its tabToken, kept current by hooks + the
            // observer). When known, judge liveness by THIS session's connection specifically — NOT "any
            // terminal in the tab" — so a Claude pane closed/dead beside a still-live shell sibling (a
            // user split) is archived instead of lingering live=true (the sibling kept anyAlive true,
            // so the old rule never fired -> the card lingered until the WHOLE tab died).
            const auto info = _sessionRegistry->Get(id);
            std::wstring sessionWt = info ? info->tabToken : std::wstring{};
            for (auto& c : sessionWt)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            bool sawTerminal = false;
            bool anyAlive = false;
            bool foundSessionConn = false;
            bool sessionConnAlive = false;
            // Agentmaster (eager-init / "started" reconcile): has this session's control left
            // ConnectionState::NotConnected (claude actually running) vs. live-but-dormant? Prefer the
            // session's OWN connection (tabToken match); fall back to "any control started" when we
            // can't pinpoint it. Reconciled into SessionInfo::started below (change-gated, so a steady
            // re-assert is free) — the half-hollow dot + "Activate" gates read it.
            bool anyStarted = false;
            bool sessionStarted = false;
            tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                const auto content = pane->GetContent();
                if (!content)
                {
                    return;
                }
                const auto term = content.try_as<TerminalApp::TerminalPaneContent>();
                if (!term)
                {
                    return;
                }
                const auto ctrl = term.GetTermControl();
                if (!ctrl)
                {
                    return;
                }
                sawTerminal = true;
                // < Closed == NotConnected / Connecting / Connected / Closing -> still alive.
                const auto connState = ctrl.ConnectionState();
                const bool alive = connState < TerminalConnection::ConnectionState::Closed;
                const bool startedThis = connState != TerminalConnection::ConnectionState::NotConnected;
                anyAlive = anyAlive || alive;
                anyStarted = anyStarted || startedThis;
                if (!sessionWt.empty())
                {
                    if (const auto cc = ctrl.Connection())
                    {
                        std::wstring wt = ::Microsoft::Console::Utils::GuidToPlainString(cc.SessionId());
                        for (auto& c : wt)
                        {
                            if (c >= L'A' && c <= L'Z')
                            {
                                c = static_cast<wchar_t>(c - L'A' + L'a');
                            }
                        }
                        if (wt == sessionWt)
                        {
                            foundSessionConn = true;
                            sessionConnAlive = alive;
                            sessionStarted = startedThis;
                        }
                    }
                }
            });
            // Reconcile the dormant/started flag. Prefer the pinpointed session connection; else use
            // "any control started". Skip (leave the flag as-is) when we couldn't read any terminal, so
            // an unreadable tab never spuriously flips a started session back to dormant.
            if (foundSessionConn)
            {
                _sessionRegistry->SetStarted(id, sessionStarted);
            }
            else if (sawTerminal)
            {
                _sessionRegistry->SetStarted(id, anyStarted);
            }
            bool isDead;
            if (!sessionWt.empty() && sawTerminal)
            {
                // We can pinpoint THIS session's connection: dead iff its pane is GONE from the tab
                // (closed — e.g. a closePane on one pane of a split) or its connection reached Closed
                // (claude exited). A live sibling pane no longer protects it.
                isDead = !foundSessionConn || !sessionConnAlive;
            }
            else
            {
                // No tabToken yet (a just-launched session before its first hook/observe) -> fall back to
                // the original rule: archive only a tab we positively saw a dead terminal in, with no
                // still-live terminal. (Never archive a tab whose content we couldn't read.)
                isDead = sawTerminal && !anyAlive;
            }
            if (isDead)
            {
                dead.push_back(id);
            }
        }

        if (dead.empty())
        {
            co_return;
        }
        for (const auto& id : dead)
        {
            // Tab status dot: clear it BEFORE dropping the _claudeTabs entry — the sweep leaves the
            // dead tab open for the user to read, and the registry-observer path can't reach it
            // after the map erase (its Update bounce would land on a map miss).
            if (const auto deadIt = _claudeTabs.find(id); deadIt != _claudeTabs.end())
            {
                if (const auto t = deadIt->second.get())
                {
                    _SetTabAgentDot(t, std::nullopt);
                    if (const auto ti = _GetTabImpl(t))
                    {
                        ti->ClearAgentToolTip(); // revert the dead tab to the default tooltip
                    }
                }
            }
            _sessionRegistry->Update(id, [](::Agentmaster::SessionInfo& s) {
                s.live = false;
                s.pendingConfirmPromptId.clear();
                s.pendingInput.clear(); // a dead session holds no live draft (PENDING_INPUT.md)
            });
            _sessionRegistry->SetInjector(id, nullptr);
            _claudeTabs.erase(id);
            _claudeOverlays.erase(id); // drop the per-tab overlay (detaches its registry observer)
            _pendingClearStreak.erase(id); // PENDING_INPUT.md: drop the debounce counter with the tab
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[liveness] dead -> archived " + id + L"\n");
        }
        ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
        co_return;
    }

    // Agentmaster (PENDING_INPUT.md): once per scanner tick, read the BOTTOM of each bound, started
    // CLAUDE session's terminal buffer and detect an UNSENT draft in Claude's input box — the
    // bottom-most ❯ line wrapped by ── rules (PendingInput.h). This is the ONE session fact hooks can
    // never carry: they fire on SUBMIT, but a draft is by definition not yet submitted, so the only
    // way to know a tab holds an unsent message is to read the rendered buffer. Strictly READ-ONLY —
    // a transient draft FACT, never SessionState (Rule #7/#13). BACKGROUND (unfocused) tabs are scanned
    // too — the whole point is to notice a draft left in a tab the user switched away from.
    //
    // The (debounced) draft is committed via SetPendingInput, which NOTIFIES on the empty<->non-empty
    // FLIP — that drives the Triage-Board card's "3 dots" pulse (it rebuilds on the notify) and, cross-
    // window, every other window's board. THIS window's tab-strip pulse is driven directly here
    // (_SetTabPending — we hold the tab), every tick + idempotently, so a re-homed tab re-asserts. The
    // flip is logged ([pending]). CLEAR DEBOUNCE (eager show / lazy hide): a non-empty read shows the
    // dots immediately; an empty read only CLEARS after kPendingClearConfirmTicks consecutive empty
    // scans, so a single mid-repaint frame (Claude's Ink TUI redraws the box constantly) can't flicker
    // the indicator off. UI thread (the only place a control's buffer is readable).
    winrt::fire_and_forget TerminalPage::_ScanPendingInput()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry || _claudeTabs.empty())
        {
            co_return;
        }
        // Snapshot the ids first — never read controls while iterating _claudeTabs.
        std::vector<std::wstring> ids;
        ids.reserve(_claudeTabs.size());
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            ids.push_back(id);
        }
        constexpr int kPendingClearConfirmTicks = 2; // consecutive empty reads required to CLEAR (anti-flicker)
        for (const auto& id : ids)
        {
            const auto info = _sessionRegistry->Get(id);
            if (!info || info->kind != ::Agentmaster::AgentKind::Claude)
            {
                _pendingClearStreak.erase(id);
                continue; // Codex's TUI has no ❯ input box — only Claude is monitored in v1
            }
            // This session's tab (for the local tab-strip pulse) — a weak_ref in _claudeTabs.
            TerminalApp::Tab hostTab{ nullptr };
            if (const auto it = _claudeTabs.find(id); it != _claudeTabs.end())
            {
                hostTab = it->second.get();
            }
            const auto control = _ControlForSession(id);
            // The buffer exists only once the control has STARTED (left NotConnected). A dormant
            // window-restored tab has no claude running -> no draft possible (and ControlCore guards the
            // null buffer internally, but skip the no-op cross-ABI call here). Leave the stored draft +
            // streak untouched so a momentarily-unreadable tab doesn't drop a real pending state.
            if (!control || control.ConnectionState() == TerminalConnection::ConnectionState::NotConnected)
            {
                continue;
            }
            std::wstring draft;
            try
            {
                const auto h = control.ReadPendingInputDraft();
                draft.assign(h.c_str(), h.size());
            }
            catch (...)
            {
                continue; // a control torn down mid-tick — skip it
            }

            // Clear debounce: decide what the registry should hold THIS tick.
            std::wstring effectiveDraft;
            if (!draft.empty())
            {
                _pendingClearStreak.erase(id);
                effectiveDraft = draft; // a real draft takes effect immediately
            }
            else if (info->pendingInput.empty())
            {
                effectiveDraft.clear(); // already clear; nothing to do
            }
            else if (++_pendingClearStreak[id] >= kPendingClearConfirmTicks)
            {
                _pendingClearStreak.erase(id);
                effectiveDraft.clear(); // empty confirmed over consecutive scans -> CLEAR
            }
            else
            {
                effectiveDraft = info->pendingInput; // keep showing during the confirm window
            }

            // Commit (SetPendingInput notifies only on the boolean flip — appear/clear) + drive the
            // local tab-strip pulse every tick (idempotent).
            const bool flipped = _sessionRegistry->SetPendingInput(id, effectiveDraft);
            const bool hasPending = !effectiveDraft.empty();
            if (hostTab)
            {
                // Contrast-pick the "3 dots" color from the tab's CURRENT effective header background
                // (_PendingDotsColorForTab), so the dots are never invisible: the LIGHT pending color on a
                // dark background, the DARK one on a light one. The background is the per-dir tab color
                // (Rule #12) AS RENDERED — which SHIFTS with the selected/unselected state (an unfocused
                // colored tab draws at 30% over the dark tab row, much darker than its full color), so the
                // dots re-pick on a focus change too (also refreshed eagerly from _OnTabSelectionChanged).
                // Only computed when actually showing (cleared tabs don't need it).
                std::optional<winrt::Windows::UI::Color> dotsColor;
                if (hasPending)
                {
                    dotsColor = _PendingDotsColorForTab(hostTab, info->workingDir);
                }
                _SetTabPending(hostTab, hasPending, dotsColor);
            }
            if (flipped)
            {
                if (!hasPending)
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[pending] " + ::Agentmaster::ShortId(id) + L" cleared\n");
                }
                else
                {
                    auto firstLine = effectiveDraft.substr(0, effectiveDraft.find(L'\n'));
                    if (firstLine.size() > 80)
                    {
                        firstLine = firstLine.substr(0, 80) + L"...";
                    }
                    // Diagnostic: the leading code points (hex) of the draft. If an EMPTY box ever still
                    // appears as "pending" (a cursor/placeholder glyph the detector didn't strip), this
                    // names the exact culprit char from the log — no extra deploy needed to diagnose.
                    std::wstring cp;
                    for (size_t i = 0; i < firstLine.size() && i < 6; ++i)
                    {
                        wchar_t b[8];
                        ::swprintf(b, 8, L"%04X ", static_cast<unsigned>(firstLine[i]));
                        cp += b;
                    }
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[pending] " + ::Agentmaster::ShortId(id) + L" draft (chars=" + std::to_wstring(effectiveDraft.size()) + L" cp: " + cp + L"): " + firstLine + L"\n");
                }
            }
        }
        co_return;
    }

    // Agentmaster (TAB_OVERLAY.md): the periodic tab<->session BIND reconcile — the POLL backstop to
    // event-driven adoption. Ticked by the shared SessionScanner alongside the liveness sweep. For
    // every LIVE session that has reported a hosting WT_SESSION (tabToken), re-run the idempotent
    // bind (_AdoptExternalSession), UNLESS it is already fully set up in THIS window (bound tab +
    // overlay) — that skip keeps the common case free of work and log noise. This catches: a session
    // whose SessionStart adoption was missed; a missed overlay attach; and a tab whose claude changed
    // its session id via an in-session /resume (the id changed; the tabToken — the ConPTY — did not,
    // and ANY later hook refreshes s.tabToken, so the poll re-homes even with no fresh SessionStart).
    // Only LIVE sessions are reconciled: a re-home archives the superseded id (live=false), so two
    // ids sharing one ConPTY can't ping-pong over the tab.
    winrt::fire_and_forget TerminalPage::_ReconcileClaudeTabs()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry)
        {
            co_return;
        }
        // Storm guard (multi-window): the registry is a process-wide singleton (M9), so Snapshot()
        // returns the ENTIRE fleet — sessions hosted in OTHER windows included. A session can only be
        // bound where its ConPTY physically lives, so collect THIS window's live WT_SESSION tokens
        // (== ITerminalConnection::SessionId(), the tabToken) up front and reconcile ONLY sessions whose
        // tabToken is in that set. Without this, every window re-ran _AdoptExternalSession for every
        // OTHER window's sessions on EVERY scanner tick, and each call queues a fire_and_forget UI-thread
        // coroutine — so the dispatcher floods faster than the UI thread can drain it. Observed live: 2-3
        // windows over a 44-session fleet drove a ~100 line/s [bind-try]/[adopt] storm, a 7.4 GB working
        // set, and a Responding=False window that still processed input but NEVER rendered (transparent/
        // black). Each window now reconciles only its own tabs (a Manager-only window does zero attempts).
        const auto lower = [](std::wstring s) {
            for (auto& c : s)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return s;
        };
        std::unordered_set<std::wstring> windowTokens;
        for (const auto& projectedTab : _tabs)
        {
            if (projectedTab == _managerTab)
            {
                continue; // the Manager tab hosts no ConPTY
            }
            const auto tabImpl = _GetTabImpl(projectedTab);
            if (!tabImpl)
            {
                continue;
            }
            TerminalConnection::ITerminalConnection conn{ nullptr };
            tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                if (conn)
                {
                    return;
                }
                const auto content = pane->GetContent();
                if (!content)
                {
                    return;
                }
                const auto term = content.try_as<TerminalApp::TerminalPaneContent>();
                if (!term)
                {
                    return;
                }
                const auto ctrl = term.GetTermControl();
                if (!ctrl)
                {
                    return;
                }
                conn = ctrl.Connection();
            });
            if (conn)
            {
                // The exact tab id: WT_SESSION == ITerminalConnection::SessionId() (the correlation key).
                windowTokens.insert(lower(::Microsoft::Console::Utils::GuidToPlainString(conn.SessionId())));
            }
        }

        const auto sessions = _sessionRegistry->Snapshot();
        size_t attempts = 0;
        for (const auto& s : sessions)
        {
            if (!s.live || s.tabToken.empty())
            {
                continue; // archived, or no hook has revealed a hosting ConPTY yet
            }
            // Only a session whose hosting ConPTY lives in THIS window is bindable here — skip the rest
            // of the (process-wide) fleet so a window never re-attempts another window's sessions (the
            // dispatcher-flood fix above). A window that doesn't host this tabToken isn't its reconciler.
            if (windowTokens.find(lower(s.tabToken)) == windowTokens.end())
            {
                continue;
            }
            // Skip sessions already fully set up in THIS window (bound tab + overlay) — no work, no log.
            const auto it = _claudeTabs.find(s.id);
            const bool boundHere = (it != _claudeTabs.end() && it->second.get() != nullptr);
            const bool overlayOk = (_claudeOverlays.find(s.id) != _claudeOverlays.end()) || !_appSettings.showTabOverlay;
            if (boundHere && overlayOk)
            {
                continue;
            }
            ++attempts;
            _AdoptExternalSession(winrt::hstring{ s.id }, winrt::hstring{ s.workingDir }, winrt::hstring{ s.tabToken });
        }
        if (attempts > 0)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[reconcile] sessions=" + std::to_wstring(sessions.size()) +
                                              L" windowTabs=" + std::to_wstring(windowTokens.size()) +
                                              L" attempts=" + std::to_wstring(attempts) +
                                              L" boundTabs=" + std::to_wstring(_claudeTabs.size()) + L"\n");
        }
        co_return;
    }

    // Agentmaster (Fleet Observer, OBSERVER.md §10): the UI lane of the PULL observer — the one
    // WinRT thread that touches XAML. Replaces _DiscoverClaudeTabsByCwd (the per-tab Toolhelp walk).
    // Two halves, each scanner tick (alongside the reconcile + liveness sweep):
    //   PUBLISH — build THIS window's tab roster {WT_SESSION, shell PID, already-bound} (both reads
    //     are µs: ITerminalConnection::SessionId() + RootProcessHandle()->GetProcessId) and hand it
    //     to the process-wide ProcessObserver, which surveys ALL claude PEBs off-thread in ONE
    //     snapshot and keys correlation on the exact WT_SESSION.
    //   READ + BIND — read the observer's correlation table and, for each of our UNBOUND tabs whose
    //     claude the observer resolved to an OURS conversation id, run the shared
    //     _BindClaudeSessionToTab (injector + overlay + title + color) — full observe+control with
    //     NO hooks / shim / settings, so a hand-typed `claude` after a `cd` still binds to the right
    //     tab + id. A short settle between publish and read lets the publish-triggered survey land,
    //     so a freshly-typed claude binds this tick (≤ ~one cadence) rather than next.
    // External (WindowsTerminal) claudes are observe-only (runningApp != Agentmaster) and never bind.
    winrt::fire_and_forget TerminalPage::_ObserverProbe()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry || !_observer)
        {
            co_return;
        }

        // Lowercase a GUID-plain string to match the observer's roster key form (it lowercases too).
        const auto lower = [](std::wstring s) {
            for (auto& c : s)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return s;
        };

        // --- PASS 1 (UI thread): build this window's roster + remember each tab's conn for the bind. ---
        struct ProbeTab
        {
            winrt::weak_ref<TerminalApp::Tab> tab; // weak so the 300ms settle below never delays a tab teardown
            TerminalConnection::ITerminalConnection conn{ nullptr };
            std::wstring wt;
        };
        std::vector<ProbeTab> probeTabs;
        std::vector<::Agentmaster::TabRosterEntry> roster;
        for (const auto& projectedTab : _tabs)
        {
            if (projectedTab == _managerTab)
            {
                continue;
            }
            const auto tabImpl = _GetTabImpl(projectedTab);
            if (!tabImpl)
            {
                continue;
            }
            TerminalConnection::ITerminalConnection conn{ nullptr };
            tabImpl->GetRootPane()->WalkTree([&](auto&& pane) {
                if (conn)
                {
                    return;
                }
                const auto content = pane->GetContent();
                if (!content)
                {
                    return;
                }
                const auto term = content.try_as<TerminalApp::TerminalPaneContent>();
                if (!term)
                {
                    return;
                }
                const auto ctrl = term.GetTermControl();
                if (!ctrl)
                {
                    return;
                }
                conn = ctrl.Connection();
            });
            if (!conn)
            {
                continue; // not a terminal tab
            }
            // The exact tab id: WT_SESSION == ITerminalConnection::SessionId() (the correlation key).
            const std::wstring wt = lower(::Microsoft::Console::Utils::GuidToPlainString(conn.SessionId()));
            // The tab's shell PID, from the ConPTY root process HANDLE (the observer reads the claude
            // DESCENDANT's PEB; we hand it the shell so its tree walk is anchored to this exact tab).
            uint32_t shellPid = 0;
            if (const auto cpc = conn.try_as<TerminalConnection::ConptyConnection>())
            {
                if (const auto h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(cpc.RootProcessHandle())))
                {
                    shellPid = ::GetProcessId(h);
                }
            }
            bool bound = false;
            for (const auto& [boundId, weakBound] : _claudeTabs)
            {
                if (const auto t = weakBound.get(); t && t == projectedTab)
                {
                    bound = true;
                    break;
                }
            }
            ::Agentmaster::TabRosterEntry e;
            e.wtSession = wt;
            e.shellPid = shellPid;
            e.bound = bound;
            roster.push_back(std::move(e));
            probeTabs.push_back({ winrt::make_weak(projectedTab), conn, wt });
        }

        // PUBLISH (the observer diffs + Wake()s its survey when this roster changed).
        _observer->PublishRoster(_windowId, std::move(roster));

        // Let a publish-triggered survey land before we read, so a just-appeared claude binds THIS
        // tick rather than next. resume_after resumes on the threadpool — hop back to the UI thread.
        co_await winrt::resume_after(std::chrono::milliseconds(300));
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry || !_observer)
        {
            co_return;
        }

        // --- PASS 2 (UI thread): push the External census to the Manager, then bind our tabs. ---
        // External (WindowsTerminal) claudes aren't in any roster, so push them regardless of corr
        // (a window with zero OUR tabs can still surface the external group). The setter diffs.
        if (const auto ipc = _agentManagerContent.get())
        {
            if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
            {
                mgr->SetExternalClaudes(_observer->External());
            }
        }

        const auto corr = _observer->Correlation();
        const auto act = _observer->Activity(); // every tab's foreground activity (pwsh / cmd / claude / codex)
        std::unordered_map<std::wstring, ::Agentmaster::CorrelationRow> byWt;
        for (const auto& c : corr)
        {
            byWt[c.wtSession] = c;
        }
        std::unordered_map<std::wstring, ::Agentmaster::TabActivityRow> actByWt;
        for (const auto& a : act)
        {
            actByWt[a.wtSession] = a; // the whole row (Codex carries its model, to enrich the badge)
        }
        std::unordered_set<std::wstring> rosterWts; // this window's tabs this tick (for observe-badge pruning)
        for (const auto& pt : probeTabs)
        {
            rosterWts.insert(pt.wt);
            const auto hostTab = pt.tab.get();
            if (!hostTab)
            {
                continue; // tab torn down during the settle
            }
            // One session per tab; is this tab already bound (has a real overlay)?
            std::wstring boundId;
            for (const auto& [bid, weakBound] : _claudeTabs)
            {
                if (const auto t = weakBound.get(); t && t == hostTab)
                {
                    boundId = bid;
                    break;
                }
            }
            if (!boundId.empty())
            {
                _DropPendingOverlay(pt.wt); // bound -> the real overlay owns the slot now
                // Managed Codex (Codex-launch, lifecycle + state): a Codex record has NO hook/scanner
                // feed, so the C2 rollout-tail is its state authority. Pull the resolved rollout uuid +
                // the turn-state from the observer's activity row onto the record (cheap no-op when
                // unchanged); also stamps tabToken so the census dedups it out of the External group.
                if (const auto ait = actByWt.find(pt.wt); ait != actByWt.end() && ait->second.activity == ::Agentmaster::TabActivity::Codex)
                {
                    _ReconcileManagedCodex(boundId, ait->second);
                }
                continue;
            }

            const auto it = byWt.find(pt.wt);
            const bool oursClaude = (it != byWt.end()) && it->second.runningApp == ::Agentmaster::RunningApp::Agentmaster;

            if (oursClaude && !it->second.sessionId.empty())
            {
                // This window hosts the connection NOW (it's in OUR roster, walked from _tabs), so we are
                // its rightful local owner. Bind via the shared tail — keyed on the exact WT_SESSION, so a
                // hand-typed claude after a `cd` binds to the right id even with no hook; the bound overlay
                // replaces any pending badge in the slot.
                //
                // HasInjector here does NOT mean "skip". Reaching this point means the tab is NOT in our
                // _claudeTabs (the alreadyBound check above already `continue`d if it were). If the session
                // ALSO already has an injector, it was bound by ANOTHER window and the tab just arrived
                // here via a cross-window tear-out / move-to-window: WT moves the live ConPTY between
                // TerminalPages, the origin evicted its stale entry in _DetachClaudeTabForMove, and
                // WT_SESSION is stable across the move. RE-HOME it — a connection lives in exactly one
                // window at a time, so this window provably owns it now and binding can't double-bind.
                // _BindClaudeSessionToTab re-points the injector to THIS tab's surviving connection and
                // recreates the local _claudeTabs entry + overlay, giving Activate / Archive / Rename a
                // handle. (The old behavior — skipping on HasInjector — left a moved session headless in
                // the destination: a visible card with no local control.)
                const bool moved = _sessionRegistry->HasInjector(it->second.sessionId);
                _DropPendingOverlay(pt.wt);
                const std::wstring why = (moved ? L"observer re-home (moved) wt=" : L"observer wt=") + pt.wt;
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[discover] " + it->second.sessionId + L" cwd=" + it->second.cwd + L" (" + why + L")\n");
                _BindClaudeSessionToTab(hostTab, pt.conn, it->second.sessionId, it->second.cwd, why);
                continue;
            }

            // Non-bound and not a resolved claude -> show an "observe" badge for whatever the observer
            // classified this tab as: an unresolved claude (no transcript id yet, §11d), or a shell /
            // codex from the activity table. So a pwsh / cmd tab carries "○ pwsh · unlinked" too, and
            // it flips to "claude" the instant the user runs claude (then to the linked overlay on the
            // first prompt).
            std::wstring kind;
            if (oursClaude)
            {
                kind = L"claude"; // correlated to a claude, but no conversation id yet
            }
            else if (const auto ait = actByWt.find(pt.wt); ait != actByWt.end())
            {
                const auto& arow = ait->second;
                switch (arow.activity)
                {
                case ::Agentmaster::TabActivity::Powershell:
                    kind = L"pwsh";
                    break;
                case ::Agentmaster::TabActivity::Cmd:
                    kind = L"cmd";
                    break;
                case ::Agentmaster::TabActivity::Codex:
                {
                    // Codex is observe-only — enrich the badge with the model + turn state (Phase C2):
                    // "○ codex · gpt-5.5 · running · unlinked" (the kind string is the badge's
                    // idempotency key, so it re-renders as the model lands / the turn flips).
                    std::wstring k = arow.model.empty() ? std::wstring{ L"codex" } : (std::wstring{ L"codex  \x00B7  " } + arow.model);
                    if (arow.codexState == ::Agentmaster::CodexState::Running)
                    {
                        k += L"  \x00B7  running";
                    }
                    else if (arow.codexState == ::Agentmaster::CodexState::Waiting)
                    {
                        k += L"  \x00B7  waiting";
                    }
                    kind = std::move(k);
                    break;
                }
                case ::Agentmaster::TabActivity::ClaudeCode:
                    kind = L"claude"; // activity caught the claude before correlation did
                    break;
                default:
                    break; // Other / Unknown -> no badge
                }
            }
            if (!kind.empty())
            {
                _SetTabActivityBadge(hostTab, pt.wt, kind);
            }
            else if (!corr.empty() || !act.empty())
            {
                // A fresh survey says this tab is nothing we badge (or its claude exited) -> drop the badge.
                _DropPendingOverlay(pt.wt);
            }
        }
        // Tabs that left THIS window's roster (closed / moved) -> drop their pending badge.
        for (auto pit = _pendingOverlays.begin(); pit != _pendingOverlays.end();)
        {
            if (rosterWts.count(pit->first))
            {
                ++pit;
            }
            else
            {
                if (pit->second)
                {
                    pit->second->Root().Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
                }
                pit = _pendingOverlays.erase(pit);
            }
        }
        co_return;
    }

    // Agentmaster (Codex-launch): reconcile a MANAGED Codex record from the Fleet Observer's per-tab
    // activity row — the Codex analog of the hook/scanner state feed (Codex has neither). Fills the
    // resolved rollout uuid (the `codex resume` target, needed for archive/restore), stamps tabToken
    // (the census-dedup key so a managed codex never ALSO shows as an External row), and maps the C2
    // rollout-tail turn-state onto SessionState. A cheap no-op when nothing changed (no registry/UI
    // churn each probe). Codex exposes no NeedsApproval/Error via PULL, so an Unknown turn-state leaves
    // the current state untouched.
    void TerminalPage::_ReconcileManagedCodex(const std::wstring& sessionId, const ::Agentmaster::TabActivityRow& act)
    {
        if (!_sessionRegistry)
        {
            return;
        }
        const auto s = _sessionRegistry->Get(sessionId);
        if (!s || s->kind != ::Agentmaster::AgentKind::Codex)
        {
            return; // only managed Codex records
        }
        std::optional<::Agentmaster::SessionState> mapped;
        switch (act.codexState)
        {
        case ::Agentmaster::CodexState::Running:
            mapped = ::Agentmaster::SessionState::Running;
            break;
        case ::Agentmaster::CodexState::Waiting:
            mapped = ::Agentmaster::SessionState::WaitingForInput;
            break;
        case ::Agentmaster::CodexState::Idle:
            mapped = ::Agentmaster::SessionState::Idle;
            break;
        default:
            break; // Unknown -> leave the current state
        }
        const bool uuidChanged = !act.sessionId.empty() && s->codexSessionId != act.sessionId;
        const bool tokenChanged = !act.wtSession.empty() && s->tabToken != act.wtSession;
        const bool stateChanged = mapped.has_value() && s->state != *mapped;
        if (!uuidChanged && !tokenChanged && !stateChanged)
        {
            return; // steady state -> no churn
        }
        _sessionRegistry->Update(sessionId, [&](::Agentmaster::SessionInfo& si) {
            if (uuidChanged)
            {
                si.codexSessionId = act.sessionId; // the `codex resume` target (persisted for archive/restore)
            }
            if (tokenChanged)
            {
                si.tabToken = act.wtSession; // census-dedup key (managed codex excluded from External)
            }
            if (stateChanged)
            {
                si.state = *mapped;
            }
        });
    }

    // Agentmaster: the Explorer Tree "refresh" button's action — force a fresh reload NOW instead of
    // waiting for the next observer/scanner tick. Wake() kicks an immediate FULL survey (re-reads each
    // claude's PEB + transcript -> re-enriches the registry and recomputes the External / Correlation /
    // Activity tables, bypassing the O7 steady-state debounce); _ObserverProbe() re-publishes this
    // window's roster, lets the survey settle, then re-pushes the External census + binds. The survey
    // enriches the registry SILENTLY (no change event, so it never drives churn — Rule #13), so once it
    // lands we force ONE Manager redraw so the refreshed LOCAL/GLOBAL timing + the EXTERNAL census both
    // show even when nothing structurally "changed". Covers every displayed scope.
    winrt::fire_and_forget TerminalPage::_RefreshObserverData()
    {
        auto weakThis = get_weak();
        if (_observer)
        {
            _observer->Wake(); // force an immediate full survey
        }
        _ObserverProbe(); // re-publish roster -> (settle) -> re-push External census + bind

        // Let the survey + probe land (the probe itself settles ~300 ms), then force a redraw so the
        // freshly enriched registry + recomputed census are reflected even when nothing "changed".
        co_await winrt::resume_after(std::chrono::milliseconds(450));
        co_await wil::resume_foreground(Dispatcher());
        if (auto self = weakThis.get())
        {
            if (const auto ipc = self->_agentManagerContent.get())
            {
                if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
                {
                    mgr->RefreshNow();
                }
            }
        }
        co_return;
    }

    // Agentmaster (OBSERVER.md §11d): drop this window's pending "claude · unlinked" badge for a tab
    // (by WT_SESSION) — collapse its element (we hold the overlay, not the pane) and release it. Used
    // when the tab binds a real session, the claude exits, or the tab leaves the roster.
    void TerminalPage::_DropPendingOverlay(const std::wstring& wtSession)
    {
        const auto it = _pendingOverlays.find(wtSession);
        if (it == _pendingOverlays.end())
        {
            return;
        }
        if (it->second)
        {
            it->second->Root().Visibility(winrt::Windows::UI::Xaml::Visibility::Collapsed);
        }
        _pendingOverlays.erase(it);
    }
}
