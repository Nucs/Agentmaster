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
#include "AgentStatusColors.h" // AgentStatusColorFor — the shared state->color palette (tab dot); TagColorFor (bookmark tags)
#include "AgentTipHelpers.h" // AgentSetTip — the islands-safe hover tooltips on the Tag panel's rows
#include "AgentTabOverlay.h" // build + own the per-tab overlays (complete com_ptr type)
#include "Tab.h" // get_self<Tab> -> CurrentEffectiveTabBackground (pending-dots contrast)
#include "TabHeaderControl.h" // get_self<TabHeaderControl> -> ReserveTitleLines (consistent multi-line tab-row height)
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog
#include "AgentMaster/Engine.h" // ActivateSessionInOtherWindows — a toast click's cross-window jump (System notifications)
#include "AgentToastActivator.h" // ToastActivator::IsRegistered — is the COM activator live (wire the in-process click fallback only if not)?
#include "AgentMaster/Persistence.h" // DeriveSessionTitle / SaveSessions (bind tail)
#include "AgentMaster/ProcessInspect.h" // ResolveClaudeTranscriptPath + AnalyzeSessionTranscript (prompt-nav)
#include "AgentMaster/ProcessObserver.h" // roster publish + Correlation/Activity/External tables
#include "AgentMaster/ProfileBootstrap.h" // Profiles:: profile-aware state paths (engine)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/SessionScanner.h" // PresenceIsWorking + the toast HOLD gates (ShouldHoldCompletionToast / DecideHeldToast / ShouldSuppressDuplicateToast) — System notifications
#include "AgentMaster/SessionStore.h" // IsSessionFavorite (the FAVORITE crown on a managed tab's status dot)
#include "AgentMaster/TranscriptStore.h" // Agentmaster (tab color modes): LoadOrRefreshSessionIndex + InferWorkingDirectory (the inferred-workdir scan)
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
    // (state dot + a wrapping title, which owns the full card width), then one dim line each for the
    // folder/branch and the state (colored to match the tab dot) and the kind/model/effort/perm, and --
    // once the Summary body has loaded -- a divider + the numbered Summary box, height-capped by LINE
    // TRUNCATION + a plain clipping Grid (the full, scrollable view is the pencil-toggled summary panel).
    //
    // NO ScrollViewer -- this is a hard rule, learned from a proven fail-fast (2026-07-02, full-dump stowed
    // backtrace): when the ToolTip popup opens, its content tree ENTERs the live tree, and a ScrollViewer's
    // enter walk activates DirectManipulation (ScrollViewer::OnManipulatabilityAffectingPropertyChanged ->
    // CInputServices::UpdateDirectManipulationManagerActivation -> CDirectManipulationService::
    // ActivateDirectManipulationManager), which under XAML Islands can fail E_INVALIDARG -> a stowed
    // exception -> the 0xC000027B fail-fast that repeatedly crashed the app (see
    // doc/agentmaster/HANDOVER_tab-tooltip.md crash #7). The ScrollViewer was dead weight anyway: the
    // tooltip is IsHitTestVisible(false), so it could NEVER be scrolled -- content past the height cap was
    // already unreachable. Truncating the text loses nothing and removes the DManip surface entirely.
    winrt::Windows::UI::Xaml::Controls::Border TtBuildTooltipCard(winrt::Windows::UI::Color accent,
                                                                  const std::wstring& title,
                                                                  const std::wstring& folderBranch,
                                                                  const std::wstring& stateText,
                                                                  const std::wstring& metaText,
                                                                  const std::wstring& dirDetailLine,
                                                                  const std::vector<std::pair<std::wstring, winrt::Windows::UI::Color>>& tagChips,
                                                                  double tagsOpacity,
                                                                  const winrt::hstring& bodyText)
    {
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;

        StackPanel col;
        col.Orientation(Orientation::Vertical);
        col.Spacing(1);

        // Header: the state dot + the title, which WRAPS -- a title is a few words, sometimes a few
        // lines, so it gets the card's whole width and the folder/branch sits on its own line below.
        //
        // A Grid, NOT a horizontal StackPanel: a StackPanel measures its children with INFINITE width in
        // the stacking direction and arranges them at their DESIRED size, so a TextBlock inside one can
        // never wrap NOR ellipsize -- it just overruns its slot and renders straight THROUGH whatever
        // sits beside it (that, plus an Auto folder/branch column outbidding a 1* title column, is how
        // the title used to collide with the folder/branch). A star column hands the title a finite
        // width, so Wrap + MaxLines + CharacterEllipsis all behave.
        {
            Grid header;
            ColumnDefinition cDot;
            cDot.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            ColumnDefinition cTitle;
            cTitle.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            header.ColumnDefinitions().Append(cDot);
            header.ColumnDefinitions().Append(cTitle);

            winrt::Windows::UI::Xaml::Shapes::Ellipse dot;
            dot.Width(9);
            dot.Height(9);
            dot.Fill(SolidColorBrush{ accent });
            dot.Stroke(TtFill(0xFF, 0x00, 0x00, 0x00));
            dot.StrokeThickness(1);
            // Pinned to the FIRST line's optical center -- centering it against the whole block would
            // drift the dot down the side of a multi-line title.
            dot.VerticalAlignment(VerticalAlignment::Top);
            dot.Margin(ThicknessHelper::FromLengths(0, 4, 0, 0));
            Grid::SetColumn(dot, 0);
            header.Children().Append(dot);

            TextBlock titleTb;
            titleTb.FontFamily(Media::FontFamily{ L"Cascadia Mono" });
            titleTb.FontSize(13);
            titleTb.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            titleTb.Foreground(TtFill(0xFF, 0xF2, 0xF2, 0xF2));
            titleTb.TextWrapping(TextWrapping::Wrap);
            titleTb.MaxLines(3); // a pathological title can't grow a tall header; line 3 ellipsizes
            titleTb.TextTrimming(TextTrimming::CharacterEllipsis);
            titleTb.Margin(ThicknessHelper::FromLengths(6, 0, 0, 0));
            titleTb.Text(winrt::hstring{ title });
            Grid::SetColumn(titleTb, 1);
            header.Children().Append(titleTb);
            col.Children().Append(header);
        }

        // The leaf working-dir folder + "/" + git branch, on its OWN dim line UNDER the title (it used to
        // share the header row, where it starved a multi-word title of room and got overrun by it). It
        // wraps rather than ellipsizing: it is short, and a line break costs less than hiding the branch.
        if (!folderBranch.empty())
        {
            TextBlock fb;
            fb.FontFamily(Media::FontFamily{ L"Cascadia Mono" });
            fb.FontSize(11);
            fb.Foreground(TtFill(0xFF, 0xB0, 0xB0, 0xB0));
            fb.TextWrapping(TextWrapping::Wrap);
            fb.Text(winrt::hstring{ folderBranch });
            col.Children().Append(fb);
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
        // Directory detail (tab color modes / inferred working dir): the SECOND directory the header
        // leaf can't carry, pre-composed by the caller as a full line — under the Inferred mode the
        // header shows the INFERRED dir's leaf, so this reads "launched in → <cwd>" (the full path);
        // in the other modes an existing (dormant) inference still reads "inferred → <inferred dir>".
        // Shown only when the two dirs actually differ (SessionInfo::inferredWorkingDir is stored
        // empty when the inference lands on the cwd).
        if (!dirDetailLine.empty())
        {
            TextBlock inf;
            inf.FontFamily(Media::FontFamily{ L"Cascadia Mono" });
            inf.FontSize(11);
            inf.TextWrapping(TextWrapping::Wrap);
            inf.Foreground(TtFill(0xFF, 0xB0, 0xB0, 0xB0));
            inf.Text(winrt::hstring{ dirDetailLine });
            col.Children().Append(inf);
        }
        // Bookmark TAGS — each tag's name over a 2px UNDERSCORE in its picked color (the badge
        // ribbon's color; user-picked > name-hash, resolved by the spec producer). An underline
        // rather than inked text because XAML can't color a TextDecorations underline separately
        // from its text — a thin Border under the name is the practical colored underscore, and it
        // keeps the names readable in the card's normal foreground. Greedy line-packing by an
        // estimated character budget (no platform WrapPanel; tags are few and short, so the
        // estimate only needs to keep a long tag set from overflowing the card's MaxWidth).
        if (!tagChips.empty())
        {
            StackPanel tagRows;
            tagRows.Orientation(Orientation::Vertical);
            tagRows.Spacing(3);
            tagRows.Margin(ThicknessHelper::FromLengths(0, 3, 0, 0));
            // The user-configurable tag-row opacity (cog TABS slider; default 0.9 = 90% solid). One
            // Opacity on the whole tag-rows panel fades name + colored underscore together.
            tagRows.Opacity(tagsOpacity);
            StackPanel line{ nullptr };
            size_t lineChars = 0;
            constexpr size_t kTagLineBudget = 52; // ~mono-11 chars that fit the 460px card minus padding
            for (const auto& [tagName, tagColor] : tagChips)
            {
                if (!line || (lineChars > 0 && lineChars + tagName.size() > kTagLineBudget))
                {
                    if (line)
                    {
                        tagRows.Children().Append(line);
                    }
                    line = StackPanel{};
                    line.Orientation(Orientation::Horizontal);
                    line.Spacing(10);
                    lineChars = 0;
                }
                StackPanel chip;
                chip.Orientation(Orientation::Vertical);
                TextBlock nameTb;
                nameTb.FontFamily(Media::FontFamily{ L"Cascadia Mono" });
                nameTb.FontSize(11);
                nameTb.Foreground(TtFill(0xFF, 0xDC, 0xDC, 0xDC));
                nameTb.Text(winrt::hstring{ tagName });
                chip.Children().Append(nameTb);
                Border underscore;
                underscore.Height(2);
                underscore.CornerRadius(CornerRadiusHelper::FromUniformRadius(1));
                underscore.HorizontalAlignment(HorizontalAlignment::Stretch);
                underscore.Margin(ThicknessHelper::FromLengths(0, 1, 0, 0));
                underscore.Background(SolidColorBrush{ tagColor });
                chip.Children().Append(underscore);
                line.Children().Append(chip);
                lineChars += tagName.size() + 2;
            }
            if (line && line.Children().Size() > 0)
            {
                tagRows.Children().Append(line);
            }
            col.Children().Append(tagRows);
        }
        if (!bodyText.empty())
        {
            Border rule;
            rule.Height(1);
            rule.HorizontalAlignment(HorizontalAlignment::Stretch);
            rule.Background(TtFill(0x40, 0xFF, 0xFF, 0xFF));
            rule.Margin(ThicknessHelper::FromLengths(0, 4, 0, 4));
            col.Children().Append(rule);

            // Truncate the body to a sane line count (bounds the XAML element count AND the typical
            // height; the old ScrollViewer's 360px cap made anything past ~24 rows invisible anyway).
            constexpr size_t kTtBodyMaxLines = 32;
            std::wstring bodyStr{ bodyText };
            size_t hiddenLines = 0;
            {
                size_t lines = 0, cutAt = std::wstring::npos;
                for (size_t p = 0; p < bodyStr.size(); ++p)
                {
                    if (bodyStr[p] == L'\n' && ++lines == kTtBodyMaxLines)
                    {
                        cutAt = p;
                        break;
                    }
                }
                if (cutAt != std::wstring::npos)
                {
                    for (size_t p = cutAt + 1; p < bodyStr.size(); ++p)
                    {
                        hiddenLines += (bodyStr[p] == L'\n');
                    }
                    ++hiddenLines; // the partial last segment counts too
                    bodyStr.resize(cutAt);
                }
            }

            // A plain Grid as the height cap: UWP layout-clips a child arranged smaller than it wants, so
            // pathological wrapping still can't make a screen-tall tooltip -- WITHOUT a ScrollViewer (whose
            // DirectManipulation activation on popup-enter is the proven 0xC000027B fail-fast; see the
            // function comment). A Grid carries no manipulation machinery.
            Grid bodyClip;
            bodyClip.MaxHeight(360);
            bodyClip.Children().Append(TtBuildSummaryBody(bodyStr));
            col.Children().Append(bodyClip);

            if (hiddenLines > 0)
            {
                TextBlock more;
                more.FontFamily(Media::FontFamily{ L"Cascadia Mono" });
                more.FontSize(10);
                more.Foreground(TtFill(0xFF, 0x8A, 0x8A, 0x8A));
                more.Margin(ThicknessHelper::FromLengths(0, 2, 0, 0));
                more.Text(winrt::hstring{ L"\x2026 +" + std::to_wstring(hiddenLines) + L" more (see the summary panel)" });
                col.Children().Append(more);
            }
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
    // over the dark tab row); the session's MODE-AWARE tab color (ResolveSessionColorHex — the per-dir
    // color in the default mode, the session's own color under Individual, the inferred dir's under
    // InferredWorkingDirectory; the same precedence the board card uses) is the source/fallback. Shared
    // by the per-tick scan and the focus-change refresh so both pick the same way. UI thread.
    winrt::Windows::UI::Color TerminalPage::_PendingDotsColorForTab(const TerminalApp::Tab& tab, const ::Agentmaster::SessionInfo& info)
    {
        const std::wstring hex = ::Agentmaster::ResolveSessionColorHex(_appSettings.tabColorMode, info);
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
                _SetTabPending(hostTab, true, _PendingDotsColorForTab(hostTab, *info));
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

    // ==================================================================================
    // Agentmaster (bookmark tags): the tab-header BOOKMARK badges + the tab context
    // menu's "Tag" panel. A tag is a durable SessionStore "tags" entry (like the favorite
    // star: same-window instant, cross-window catches up on the tab's next bind); the tab
    // strip renders one small bookmark ribbon per tag at the bottom of the header
    // (TabHeaderControl::_UpdateTagBadges, fed through TabStatus.AgentTagsSpec), and the
    // panel — first row a focusable name box with a "+" that appears once you type, below
    // it EVERY global tag sorted by max(session activity) desc, click-toggled on this
    // session — is an islands-safe raw Popup parented into Root() (a Flyout-hosted
    // TextBox gets no keypresses under XAML Islands — the documented text-input trap the
    // Templates row + the Sessions range popup both dodged the same way). The GLOBAL cap
    // (AppSettings::maxTags, default 20, ceiling 40) gates only the creation of a NEW
    // name; applying an existing tag is always allowed.
    // ==================================================================================

    // Low-level: write the session's tag list onto a tab's TabStatus as the '\n'-joined
    // "name\t#AARRGGBB" spec the header control renders from — the COLOR is resolved HERE, once
    // (user-picked > name-hash), so every spec consumer (badges, the rich tab tooltip) reads the
    // same resolution with no store I/O of its own. The observable no-ops when unchanged, so the
    // frequent re-asserts (launch/bind/refresh) are free. UI thread.
    void TerminalPage::_SetTabAgentTags(const TerminalApp::Tab& tab, const std::vector<std::wstring>& tags)
    {
        if (!tab)
        {
            return;
        }
        try
        {
            if (const auto status = tab.TabStatus())
            {
                std::map<std::wstring, std::wstring> stored;
                if (!tags.empty())
                {
                    stored = ::Agentmaster::LoadAllTagColors(); // one small read per (rare) re-assert
                }
                std::wstring spec;
                for (const auto& t : tags)
                {
                    if (t.empty())
                    {
                        continue;
                    }
                    if (!spec.empty())
                    {
                        spec.push_back(L'\n');
                    }
                    spec += t;
                    spec.push_back(L'\t');
                    spec += FormatArgbHexColor(ResolveTagDisplayColor(t, stored));
                }
                status.AgentTagsSpec(winrt::hstring{ spec });
            }
        }
        CATCH_LOG();
    }

    // Re-read the durable store and (re)assert the badges on the tab THIS window hosts for
    // sessionId. A map-miss is a cheap no-op (hosted elsewhere / not open). Called where a
    // managed tab is set up (launch + bind/adopt/re-home) and after every panel toggle.
    void TerminalPage::_RefreshTabTags(const std::wstring& sessionId)
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
            _SetTabAgentTags(tab, ::Agentmaster::GetSessionTags(sessionId));
            _UpdateTabAgentToolTip(tab, sessionId); // the tooltip's tag-chip row rides the spec — reflect a toggle instantly (sig-gated)
        }
    }

    // Re-assert the badges on EVERY tab this window hosts — the recolor fan-out: an explicit
    // swatch pick that recolors an existing tag must repaint each hosted session carrying it
    // (which sessions those are isn't tracked; N small reads over the hosted set is cheap).
    void TerminalPage::_RefreshAllTabTags()
    {
        for (const auto& [sid, weakTab] : _claudeTabs)
        {
            _RefreshTabTags(sid);
        }
    }

    // Agentmaster (bookmark tags): repaint THIS window's Manager content now. A tag add/remove/
    // recolor lives in the SessionStore, never the SessionRegistry — no registry notify fires —
    // so the Triage-Board cards' bookmark ribbons (fed by _RebuildBoard's per-rebuild tag read)
    // would otherwise show stale tags until the next unrelated registry event. Same-window
    // instant, like the tab badges; another window's board catches up on its own next rebuild
    // (the accepted cross-window staleness). Safe when the Manager was never built (null get()).
    void TerminalPage::_RefreshManagerBoardTags()
    {
        if (const auto ipc = _agentManagerContent.get())
        {
            if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
            {
                mgr->RefreshNow();
            }
        }
    }

    // Context-menu "Tags" (WT tab menu): open the panel for this tab's managed session, anchored
    // under its TabViewItem.
    void TerminalPage::_OpenTagEditorForTab(const TerminalApp::Tab& tab)
    {
        const auto sid = _ClaudeSessionForTab(tab);
        if (sid.empty())
        {
            return;
        }
        // Anchor: the tab item's bottom-left, root-relative (the Sessions range-popup recipe —
        // a top/left-aligned parented Popup's offsets are root-relative).
        double x = 8;
        double y = 40;
        try
        {
            if (const auto tvi = tab.TabViewItem())
            {
                const auto pt = tvi.TransformToVisual(Root()).TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 });
                x = pt.X;
                y = pt.Y + tvi.ActualHeight() + 2;
            }
        }
        CATCH_LOG();
        _OpenTagEditorAt(sid, x, y);
    }

    // The Sessions-page row / Triage-Board card / Explorer-tree row "Tags" items: open the panel
    // for ANY session id (SessionStore is id-keyed — an on-disk, never-hosted session tags fine;
    // its badges apply wherever/whenever a tab hosts it), anchored just under the clicked element.
    // The element may be recycled by a re-render between menu open and click — the transform is
    // guarded and the default anchor holds.
    void TerminalPage::_OpenTagEditorForElement(const std::wstring& sessionId, const WUX::FrameworkElement& anchor)
    {
        double x = 8;
        double y = 40;
        try
        {
            if (anchor)
            {
                const auto pt = anchor.TransformToVisual(Root()).TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 });
                x = pt.X;
                y = pt.Y + anchor.ActualHeight() + 2;
            }
        }
        CATCH_LOG();
        _OpenTagEditorAt(sessionId, x, y);
    }

    // The shared open: place + show the panel for `sid` at root-relative (x, y). DEFERRED one
    // dispatcher tick: every caller sits inside a closing context flyout's click, whose Closed
    // handler tosses focus back to its target — that refocus must land BEFORE the panel opens +
    // focuses its name box, or it would steal the box's focus. The color picker is re-randomized
    // on every open (the "prepicked random" pre-pick).
    void TerminalPage::_OpenTagEditorAt(const std::wstring& sid, double x, double y)
    {
        if (sid.empty())
        {
            return;
        }
        const winrt::hstring sidH{ sid };
        Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak = get_weak(), sidH, x, y]() {
            const auto page = weak.get();
            if (!page)
            {
                return;
            }
            page->_tagEditorSessionId = std::wstring{ sidH };
            page->_EnsureTagEditorPopup();
            if (!page->_tagEditorPopup)
            {
                return;
            }
            // Clamp the anchor so the ~280px card never hangs past the window's right edge.
            double cx = x;
            try
            {
                const double rootW = page->Root().ActualWidth();
                if (rootW > 300 && cx > rootW - 300)
                {
                    cx = rootW - 300;
                }
            }
            catch (...)
            {
            }
            page->_tagEditorPopup.HorizontalOffset(std::max(0.0, cx));
            page->_tagEditorPopup.VerticalOffset(std::max(0.0, y));
            page->_tagEditorBox.Text(L"");
            page->_RandomizeTagEditorColor(); // fresh random pre-pick each open
            page->_RebuildTagEditorList();
            page->_UpdateTagEditorAddState();
            page->_tagEditorPopup.IsOpen(true);
            // Focus the name box one tick AFTER the open — on the FIRST open the popup's child has
            // only just been realized, and a same-tick Focus on a not-yet-laid-out TextBox can no-op.
            page->Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak]() {
                if (const auto p = weak.get())
                {
                    if (p->_tagEditorPopup && p->_tagEditorPopup.IsOpen() && p->_tagEditorBox)
                    {
                        p->_tagEditorBox.Focus(FocusState::Programmatic);
                    }
                }
            });
        });
    }

    // Build the panel ONCE and parent it into Root() (row-span across the whole page so the
    // root-relative offsets can place it anywhere). Dismissal is deliberately POINTER-based —
    // Esc, tab switch, or any press OUTSIDE the card (a Root() handledEventsToo hook) — never
    // focus-based: the context flyout's close refocuses the terminal asynchronously, and a
    // focus-loss dismissal would race it and close the panel the instant it opened.
    void TerminalPage::_EnsureTagEditorPopup()
    {
        if (_tagEditorPopup)
        {
            return;
        }

        StackPanel body;
        body.Spacing(6);
        body.Width(260);

        // Row 1 — [ tag name box | + ]: the "+" appears only once the box holds a usable name
        // (_UpdateTagEditorAddState), and Enter commits like clicking it.
        Grid nameRow;
        {
            ColumnDefinition c0;
            c0.Width(GridLength{ 1, GridUnitType::Star });
            ColumnDefinition c1;
            c1.Width(GridLength{ 0, GridUnitType::Auto });
            nameRow.ColumnDefinitions().Append(c0);
            nameRow.ColumnDefinitions().Append(c1);
        }
        _tagEditorBox = TextBox{};
        _tagEditorBox.PlaceholderText(L"tag name");
        _tagEditorBox.VerticalAlignment(VerticalAlignment::Center);
        AgentSetTip(_tagEditorBox, L"Name a new bookmark tag for this session \x2014 press + (or Enter) to add it. Existing tags are toggled from the list below.");
        _tagEditorBox.TextChanged([weak = get_weak()](auto&&, auto&&) {
            if (const auto page = weak.get())
            {
                page->_UpdateTagEditorAddState();
            }
        });
        _tagEditorBox.KeyDown([weak = get_weak()](auto&&, const WUX::Input::KeyRoutedEventArgs& e) {
            const auto page = weak.get();
            if (!page)
            {
                return;
            }
            if (e.Key() == VirtualKey::Enter)
            {
                e.Handled(true);
                // Defer — the commit rebuilds the tag list (a tree mutation under an input event).
                page->Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak]() {
                    if (const auto p = weak.get())
                    {
                        p->_CommitTagEditorAdd();
                    }
                });
            }
            else if (e.Key() == VirtualKey::Escape)
            {
                e.Handled(true);
                page->_CloseTagEditorPopup();
            }
        });
        Grid::SetColumn(_tagEditorBox, 0);
        nameRow.Children().Append(_tagEditorBox);

        _tagEditorAddBtn = Button{};
        _tagEditorAddBtn.Content(winrt::box_value(winrt::hstring{ L"+" }));
        _tagEditorAddBtn.Margin(Thickness{ 6, 0, 0, 0 });
        _tagEditorAddBtn.VerticalAlignment(VerticalAlignment::Center);
        _tagEditorAddBtn.Visibility(Visibility::Collapsed); // appears once the box holds a usable name
        AgentSetTip(_tagEditorAddBtn, L"Add this tag to the session (a new tag counts toward the global tag limit \x2014 Settings cog \x2192 Max bookmark tags)");
        _tagEditorAddBtn.Click([weak = get_weak()](auto&&, auto&&) {
            if (const auto page = weak.get())
            {
                // Defer — the commit rebuilds the tag list (the pointer-handler tree-mutation rule).
                page->Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak]() {
                    if (const auto p = weak.get())
                    {
                        p->_CommitTagEditorAdd();
                    }
                });
            }
        });
        Grid::SetColumn(_tagEditorAddBtn, 1);
        nameRow.Children().Append(_tagEditorAddBtn);
        body.Children().Append(nameRow);

        // Row 2 — the COLOR PICKER: one swatch per palette color (the same mini-palette the
        // name-hash fallback draws from), the picked one ringed white. Pre-picked RANDOM on every
        // open and re-randomized after every added tag (_RandomizeTagEditorColor); clicking a
        // swatch makes the pick EXPLICIT (_tagEditorUserPicked) — a NEW tag always takes the
        // current pick, an EXISTING tag is recolored only by an explicit pick (the one recolor
        // path; the roulette must never silently repaint established tags). The "+" button wears
        // the pick as its background, so what the add will produce is visible at the commit point.
        // Swatch taps only flip brushes (no tree mutation), so the handler is islands-safe inline.
        _tagEditorSwatchRow = StackPanel{};
        _tagEditorSwatchRow.Orientation(Orientation::Horizontal);
        _tagEditorSwatchRow.Spacing(3);
        for (size_t i = 0; i < kTagPaletteSize; ++i)
        {
            Border sw;
            sw.Width(15);
            sw.Height(15);
            sw.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 3, 3, 3, 3 });
            sw.Background(SolidColorBrush{ TagPaletteColor(i) });
            sw.BorderBrush(SolidColorBrush{ winrt::Windows::UI::Colors::Transparent() });
            sw.BorderThickness(Thickness{ 2, 2, 2, 2 });
            const winrt::hstring hexH{ FormatArgbHexColor(TagPaletteColor(i)) };
            sw.Tag(winrt::box_value(hexH)); // the ring update matches swatches by this hex
            AgentSetTip(sw, L"Use this color for the next tag you add (picking one explicitly also recolors an existing tag when you re-add its name)");
            sw.Tapped([weak = get_weak(), hexH](auto&&, const WUX::Input::TappedRoutedEventArgs& e) {
                e.Handled(true); // don't let the press double as an outside/inside-card gesture
                if (const auto page = weak.get())
                {
                    page->_SelectTagEditorColor(std::wstring{ hexH }, true /* explicit user pick */);
                }
            });
            _tagEditorSwatchRow.Children().Append(sw);
        }
        body.Children().Append(_tagEditorSwatchRow);

        // The cap message — shown by _UpdateTagEditorAddState when the typed name is NEW and the
        // universe is already at AppSettings::maxTags.
        _tagEditorHint = TextBlock{};
        _tagEditorHint.FontSize(11);
        _tagEditorHint.Opacity(0.7);
        _tagEditorHint.TextWrapping(TextWrapping::Wrap);
        _tagEditorHint.Visibility(Visibility::Collapsed);
        body.Children().Append(_tagEditorHint);

        // The global tag list (every tag on any session, max-activity-desc), scrollable past ~7 rows.
        ScrollViewer listScroll;
        listScroll.MaxHeight(240);
        listScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        _tagEditorList = StackPanel{};
        _tagEditorList.Spacing(2);
        listScroll.Content(_tagEditorList);
        body.Children().Append(listScroll);

        // The dim "K of N tags" footer (the cap made visible without hunting the cog).
        _tagEditorCount = TextBlock{};
        _tagEditorCount.FontSize(11);
        _tagEditorCount.Opacity(0.55);
        body.Children().Append(_tagEditorCount);

        _tagEditorCard = Border{};
        _tagEditorCard.RequestedTheme(ElementTheme::Dark); // Agentmaster surfaces are always dark
        _tagEditorCard.Background(SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x26, 0x26, 0x26) });
        _tagEditorCard.BorderBrush(SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x3A, 0x3A, 0x3A) });
        _tagEditorCard.BorderThickness(Thickness{ 1, 1, 1, 1 });
        _tagEditorCard.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 6, 6, 6, 6 });
        _tagEditorCard.Padding(Thickness{ 10, 8, 10, 8 });
        _tagEditorCard.Child(body);
        // Esc anywhere in the card (a focused row button, the list, ...) closes; the box's own
        // KeyDown above already handles it while typing (both are idempotent).
        _tagEditorCard.KeyDown([weak = get_weak()](auto&&, const WUX::Input::KeyRoutedEventArgs& e) {
            if (e.Key() == VirtualKey::Escape)
            {
                e.Handled(true);
                if (const auto page = weak.get())
                {
                    page->_CloseTagEditorPopup();
                }
            }
        });

        _tagEditorPopup = Windows::UI::Xaml::Controls::Primitives::Popup{};
        _tagEditorPopup.Child(_tagEditorCard);
        // Top/left aligned so HorizontalOffset/VerticalOffset are root-relative (the path-picker /
        // Sessions range-popup recipe).
        _tagEditorPopup.HorizontalAlignment(HorizontalAlignment::Left);
        _tagEditorPopup.VerticalAlignment(VerticalAlignment::Top);
        Grid::SetRow(_tagEditorPopup, 0);
        Grid::SetRowSpan(_tagEditorPopup, 3); // span the whole Root grid (tab row + infobars + content)
        Root().Children().Append(_tagEditorPopup);

        // Outside-press dismissal: ONE handledEventsToo hook on Root() (registered once, inert while
        // the panel is closed). A press whose source chain reaches the card is INSIDE (keep open);
        // anything else — terminal, tab strip, toolbar — closes it. Popup open/close is not a tree
        // mutation, so the synchronous close inside a pointer handler is safe (AgentTipHelpers doc).
        if (!_tagEditorOutsideHooked)
        {
            _tagEditorOutsideHooked = true;
            Root().AddHandler(
                UIElement::PointerPressedEvent(),
                winrt::box_value(WUX::Input::PointerEventHandler{ [weak = get_weak()](const IInspectable&, const WUX::Input::PointerRoutedEventArgs& e) {
                    const auto page = weak.get();
                    if (!page || !page->_tagEditorPopup || !page->_tagEditorPopup.IsOpen())
                    {
                        return;
                    }
                    auto d = e.OriginalSource().try_as<DependencyObject>();
                    while (d)
                    {
                        if (const auto b = d.try_as<Border>(); b && b == page->_tagEditorCard)
                        {
                            return; // pressed inside the card — keep it open
                        }
                        d = VisualTreeHelper::GetParent(d);
                    }
                    page->_CloseTagEditorPopup();
                } }),
                true /* handledEventsToo — tab items / the terminal handle their presses */);
        }
    }

    // Re-list the GLOBAL tag universe with this session's on/off state per row. Each row =
    // [✓?][bookmark ribbon][name][· sessionCount], sorted by max(session activity) desc
    // (CollectGlobalTags — activity from the shared registry: the max of the hook-driven
    // lastActivityUnixMs and the transcript-derived convLastActivityUnixMs). Click toggles the
    // tag on the session being edited. Also refreshes the cached universe the per-keystroke
    // cap check reads (_tagEditorUniverse — never re-scan the store dir on TextChanged).
    void TerminalPage::_RebuildTagEditorList()
    {
        if (!_tagEditorList)
        {
            return;
        }
        _tagEditorList.Children().Clear();

        std::unordered_map<std::wstring, int64_t> activity;
        if (_sessionRegistry)
        {
            for (const auto& s : _sessionRegistry->Snapshot())
            {
                activity[s.id] = std::max(s.lastActivityUnixMs, s.convLastActivityUnixMs);
            }
        }
        // The universe = every session's tags ∪ the durable KNOWN-TAG registry (tags.json), so a tag
        // whose last carrier was untagged stays listed at ·0 — re-appliable — until its row's ✕
        // explicitly deletes it (tag removal is deliberate, never a side effect of an untag).
        _tagEditorUniverse = ::Agentmaster::CollectGlobalTags(::Agentmaster::LoadAllSessionTags(), activity, ::Agentmaster::LoadKnownTags());
        const auto storedColors = ::Agentmaster::LoadAllTagColors(); // picker-chosen colors (hash fallback per row)

        std::unordered_set<std::wstring> mine; // folded — this session's tags
        for (const auto& t : ::Agentmaster::GetSessionTags(_tagEditorSessionId))
        {
            mine.insert(::Agentmaster::FoldTagName(t));
        }

        if (_tagEditorUniverse.empty())
        {
            TextBlock none;
            none.Text(L"No tags yet \x2014 type a name above and press +.");
            none.FontSize(11);
            none.Opacity(0.6);
            none.TextWrapping(TextWrapping::Wrap);
            _tagEditorList.Children().Append(none);
        }
        for (const auto& info : _tagEditorUniverse)
        {
            const bool on = mine.count(::Agentmaster::FoldTagName(info.name)) > 0;

            Button row;
            row.HorizontalAlignment(HorizontalAlignment::Stretch);
            // Stretch (not Left): the content is a Grid whose star column pins the ✕ delete button
            // to the row's RIGHT edge on a 0-carrier tag; the name stack left-aligns inside col 0.
            row.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            row.Background(SolidColorBrush{ winrt::Windows::UI::Colors::Transparent() });
            row.BorderThickness(Thickness{ 0, 0, 0, 0 });
            row.Padding(Thickness{ 6, 3, 6, 3 });

            StackPanel h;
            h.Orientation(Orientation::Horizontal);
            h.Spacing(6);
            h.HorizontalAlignment(HorizontalAlignment::Left);

            TextBlock check; // reserves its slot either way, so names align on/off
            check.Text(on ? winrt::hstring{ L"\x2713" } : winrt::hstring{ L" " });
            check.Width(14);
            h.Children().Append(check);

            Windows::UI::Xaml::Shapes::Polygon ribbon; // the same 6x9 bookmark the tab header wears
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 0.0f, 0.0f });
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 6.0f, 0.0f });
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 6.0f, 9.0f });
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 3.0f, 6.3f });
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 0.0f, 9.0f });
            ribbon.Fill(SolidColorBrush{ ResolveTagDisplayColor(info.name, storedColors) });
            ribbon.Stroke(SolidColorBrush{ winrt::Windows::UI::Colors::Black() });
            ribbon.StrokeThickness(0.75);
            ribbon.VerticalAlignment(VerticalAlignment::Center);
            h.Children().Append(ribbon);

            TextBlock name;
            name.Text(winrt::hstring{ info.name });
            name.VerticalAlignment(VerticalAlignment::Center);
            name.Opacity(on ? 1.0 : 0.85);
            h.Children().Append(name);

            TextBlock count; // how many sessions carry it — the tag's "weight"
            count.Text(winrt::hstring{ L"\x00B7 " + std::to_wstring(info.sessionCount) });
            count.FontSize(11);
            count.Opacity(0.5);
            count.VerticalAlignment(VerticalAlignment::Center);
            h.Children().Append(count);

            const winrt::hstring tagName{ info.name };

            // Row content: [ check·ribbon·name·count ......... (✕) ] — the ✕ DELETE button appears
            // ONLY on a 0-carrier tag (removal is a big deal: while any session carries the tag it
            // cannot be deleted, only untagged; once nothing carries it, this ✕ is the ONE removal
            // path — an untag alone no longer vanishes it from the list).
            Grid rowGrid;
            {
                ColumnDefinition c0;
                c0.Width(GridLength{ 1, GridUnitType::Star });
                ColumnDefinition c1;
                c1.Width(GridLength{ 0, GridUnitType::Auto });
                rowGrid.ColumnDefinitions().Append(c0);
                rowGrid.ColumnDefinitions().Append(c1);
            }
            Grid::SetColumn(h, 0);
            rowGrid.Children().Append(h);
            if (info.sessionCount == 0)
            {
                Button del; // a Button INSIDE the row Button: the inner click never fires the row toggle
                del.Content(winrt::box_value(winrt::hstring{ L"\x2715" }));
                del.FontSize(10);
                del.Padding(Thickness{ 4, 0, 4, 1 });
                del.Margin(Thickness{ 8, 0, 0, 0 });
                del.Background(SolidColorBrush{ winrt::Windows::UI::Colors::Transparent() });
                del.BorderThickness(Thickness{ 0, 0, 0, 0 });
                del.Opacity(0.55);
                del.VerticalAlignment(VerticalAlignment::Center);
                AgentSetTip(del, winrt::hstring{ L"Delete tag \x201C" + info.name + L"\x201D \x2014 no session carries it anymore. This removes the name from the tag list (its color is remembered if you ever re-create it)." });
                del.Click([weak = get_weak(), tagName](auto&&, auto&&) {
                    if (const auto page = weak.get())
                    {
                        // Defer — the delete rebuilds this very list (a tree mutation under the click).
                        page->Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak, tagName]() {
                            if (const auto p = weak.get())
                            {
                                p->_RemoveGlobalTag(std::wstring{ tagName });
                            }
                        });
                    }
                });
                Grid::SetColumn(del, 1);
                rowGrid.Children().Append(del);
            }

            row.Content(rowGrid);
            AgentSetTip(row, winrt::hstring{ (on ? L"Remove tag \x201C" + info.name + L"\x201D from this session" : L"Tag this session \x201C" + info.name + L"\x201D") });
            row.Click([weak = get_weak(), tagName](auto&&, auto&&) {
                if (const auto page = weak.get())
                {
                    // Defer — the toggle rebuilds this very list (a tree mutation under the click).
                    page->Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak, tagName]() {
                        if (const auto p = weak.get())
                        {
                            p->_ToggleSessionTag(p->_tagEditorSessionId, std::wstring{ tagName });
                        }
                    });
                }
            });
            _tagEditorList.Children().Append(row);
        }

        if (_tagEditorCount)
        {
            const uint32_t cap = ::Agentmaster::ClampMaxTags(_appSettings.maxTags);
            _tagEditorCount.Text(winrt::hstring{ std::to_wstring(_tagEditorUniverse.size()) + L" of " + std::to_wstring(cap) + L" tags" });
        }
    }

    // TextChanged: show the "+" only when the box holds a usable (normalized non-empty) name, and
    // gate it — disabled + a hint — when that name is NEW while the universe already sits at the
    // cap. Reads the CACHED universe (no store-dir scan per keystroke).
    void TerminalPage::_UpdateTagEditorAddState()
    {
        if (!_tagEditorBox || !_tagEditorAddBtn)
        {
            return;
        }
        const std::wstring name = ::Agentmaster::NormalizeTagName(std::wstring{ _tagEditorBox.Text() });
        if (name.empty())
        {
            _tagEditorAddBtn.Visibility(Visibility::Collapsed);
            if (_tagEditorHint)
            {
                _tagEditorHint.Visibility(Visibility::Collapsed);
            }
            return;
        }
        const std::wstring folded = ::Agentmaster::FoldTagName(name);
        bool exists = false;
        for (const auto& info : _tagEditorUniverse)
        {
            if (::Agentmaster::FoldTagName(info.name) == folded)
            {
                exists = true;
                break;
            }
        }
        const uint32_t cap = ::Agentmaster::ClampMaxTags(_appSettings.maxTags);
        const bool blocked = !exists && _tagEditorUniverse.size() >= cap;
        _tagEditorAddBtn.Visibility(Visibility::Visible);
        _tagEditorAddBtn.IsEnabled(!blocked);
        if (_tagEditorHint)
        {
            if (blocked)
            {
                _tagEditorHint.Text(winrt::hstring{ L"Tag limit reached (" + std::to_wstring(cap) + L"). Untag it everywhere to retire a tag, or raise the limit in Settings \x2192 Max bookmark tags." });
            }
            _tagEditorHint.Visibility(blocked ? Visibility::Visible : Visibility::Collapsed);
        }
    }

    // Select a picker color (an explicit swatch tap, or the random pre-pick): remember the hex +
    // whether the USER chose it, ring the matching swatch white, and paint the "+" button with the
    // pick (contrast-inked) so the upcoming add's color is visible right where it commits. Brush
    // flips only — no tree mutation, safe inside pointer dispatch.
    void TerminalPage::_SelectTagEditorColor(const std::wstring& hex, bool userPicked)
    {
        _tagEditorPickedHex = hex;
        _tagEditorUserPicked = userPicked;
        if (_tagEditorSwatchRow)
        {
            for (const auto& child : _tagEditorSwatchRow.Children())
            {
                if (const auto sw = child.try_as<Border>())
                {
                    bool isPicked = false;
                    if (const auto tagged = sw.Tag().try_as<winrt::hstring>())
                    {
                        isPicked = std::wstring{ *tagged } == hex;
                    }
                    sw.BorderBrush(SolidColorBrush{ isPicked ? winrt::Windows::UI::Colors::White() : winrt::Windows::UI::Colors::Transparent() });
                }
            }
        }
        if (_tagEditorAddBtn)
        {
            const auto c = ParseArgbHexColor(hex, TagPaletteColor(0));
            _tagEditorAddBtn.Background(SolidColorBrush{ c });
            _tagEditorAddBtn.Foreground(SolidColorBrush{ BackgroundIsLight(c) ? winrt::Windows::UI::Colors::Black() : winrt::Windows::UI::Colors::White() });
        }
    }

    // Roll a fresh RANDOM pre-pick (panel open + after every added tag — the request's "prepicked
    // random on open and after every new tag added"). Always moves OFF the current pick so a
    // just-consumed color visibly hands over to the next one; the pick is non-explicit
    // (_tagEditorUserPicked=false), so the roulette can never recolor an existing tag.
    void TerminalPage::_RandomizeTagEditorColor()
    {
        static uint64_t rngState = ::GetTickCount64() | 1; // process-wide; quality is irrelevant here
        rngState = rngState * 6364136223846793005ULL + 1442695040888963407ULL;
        size_t idx = static_cast<size_t>((rngState >> 33) % kTagPaletteSize);
        if (!_tagEditorPickedHex.empty() && FormatArgbHexColor(TagPaletteColor(idx)) == _tagEditorPickedHex)
        {
            idx = (idx + 1) % kTagPaletteSize;
        }
        _SelectTagEditorColor(FormatArgbHexColor(TagPaletteColor(idx)), false);
    }

    // The "+" / Enter: add the typed tag to the session being edited. An existing name (case-
    // insensitive) just APPLIES it — reusing the canonical stored casing — while a NEW name is
    // cap-gated (the "+" is already disabled then; this re-check is the belt for a stale cache).
    void TerminalPage::_CommitTagEditorAdd()
    {
        if (_tagEditorSessionId.empty() || !_tagEditorBox)
        {
            return;
        }
        const std::wstring name = ::Agentmaster::NormalizeTagName(std::wstring{ _tagEditorBox.Text() });
        if (name.empty())
        {
            return;
        }
        const std::wstring folded = ::Agentmaster::FoldTagName(name);
        bool exists = false;
        std::wstring canonical = name;
        for (const auto& info : _tagEditorUniverse)
        {
            if (::Agentmaster::FoldTagName(info.name) == folded)
            {
                exists = true;
                canonical = info.name; // reuse the established casing — one tag, one spelling
                break;
            }
        }
        const uint32_t cap = ::Agentmaster::ClampMaxTags(_appSettings.maxTags);
        if (!exists && _tagEditorUniverse.size() >= cap)
        {
            _UpdateTagEditorAddState(); // re-assert the hint; never create past the cap
            return;
        }
        if (::Agentmaster::AddSessionTag(_tagEditorSessionId, canonical))
        {
            // A created tag enters the durable KNOWN-TAG registry (tags.json), so later untagging
            // its last carrier leaves it listed at ·0 (re-appliable) instead of vanishing — only
            // the tag row's ✕ deletes it (removal is deliberate).
            if (!exists)
            {
                ::Agentmaster::RegisterKnownTag(canonical);
            }
            // The picker's color: a NEW tag always takes the current pick (the random pre-pick IS
            // the color shown on the "+" the user just pressed); an EXISTING tag is recolored only
            // by an EXPLICIT swatch pick this open — the deliberate recolor path — never by the
            // roulette. Stored BEFORE the badge refresh so the new spec resolves it.
            const bool recolorExisting = exists && _tagEditorUserPicked;
            if (!_tagEditorPickedHex.empty() && (!exists || recolorExisting))
            {
                ::Agentmaster::SetTagColor(canonical, _tagEditorPickedHex);
                if (recolorExisting)
                {
                    // Nav audit: the deliberate recolor of an EXISTING tag (an explicit swatch pick;
                    // a NEW tag's color is part of its `tag add` creation line).
                    ::Agentmaster::LogNav(L"tag recolor \"" + canonical + L"\" " + _tagEditorPickedHex);
                }
            }
            // Nav audit: the user tagged a session from the Tags panel ((new) == this name just
            // entered the global universe).
            ::Agentmaster::LogNav(L"tag add \"" + canonical + L"\" sid=" + ::Agentmaster::ShortId(_tagEditorSessionId) + (exists ? L"" : L" (new)"));
            if (recolorExisting)
            {
                _RefreshAllTabTags(); // other hosted sessions may carry the recolored tag
            }
            else
            {
                _RefreshTabTags(_tagEditorSessionId);
            }
            // Sessions page live-sync (the _ToggleSessionTag idiom): reflect the new tag into an
            // open page's cache + header chips + table; no-ops when the page was never built.
            _sessionsTags[_tagEditorSessionId] = ::Agentmaster::GetSessionTags(_tagEditorSessionId);
            _RebuildSessionsTagChips();
            _RenderSessionsTable();
            _RefreshManagerBoardTags(); // Triage-Board card ribbons (tags never notify the registry)
            _RandomizeTagEditorColor(); // hand the picker a fresh random color for the NEXT add
        }
        _tagEditorBox.Text(L"");
        _RebuildTagEditorList();
        _UpdateTagEditorAddState();
    }

    // Toggle one tag on a session (the panel's row click): remove when it carries it, else add.
    // Durable (SessionStore) + instant on this window's tab badges + the open panel.
    void TerminalPage::_ToggleSessionTag(const std::wstring& sessionId, const std::wstring& tag)
    {
        if (sessionId.empty() || tag.empty())
        {
            return;
        }
        const std::wstring folded = ::Agentmaster::FoldTagName(::Agentmaster::NormalizeTagName(tag));
        bool has = false;
        for (const auto& t : ::Agentmaster::GetSessionTags(sessionId))
        {
            if (::Agentmaster::FoldTagName(t) == folded)
            {
                has = true;
                break;
            }
        }
        if (has)
        {
            // Untagging must never vanish the tag from the universe (removal is a big deal — only
            // the tag row's ✕ deletes): make sure it's in the durable registry BEFORE the remove,
            // which also self-heals pre-registry tags at the one moment the derived union could
            // lose them (this session might be the last carrier).
            ::Agentmaster::RegisterKnownTag(tag);
        }
        const bool changed = has ? ::Agentmaster::RemoveSessionTag(sessionId, tag) : ::Agentmaster::AddSessionTag(sessionId, tag);
        if (changed)
        {
            // Nav audit: the panel's row toggle (add == applying an existing global tag here).
            ::Agentmaster::LogNav((has ? L"tag remove \"" : L"tag add \"") + tag + L"\" sid=" + ::Agentmaster::ShortId(sessionId));
            _RefreshTabTags(sessionId);
            // Sessions page live-sync (the _ToggleSessionFavorite idiom): keep an open page's tag
            // cache + header chips + table in step without a re-gather. All three are no-ops when
            // the page was never built (null hosts).
            auto tags = ::Agentmaster::GetSessionTags(sessionId);
            if (tags.empty())
            {
                _sessionsTags.erase(sessionId);
            }
            else
            {
                _sessionsTags[sessionId] = std::move(tags);
            }
            _RebuildSessionsTagChips();
            _RenderSessionsTable();
            _RefreshManagerBoardTags(); // Triage-Board card ribbons (tags never notify the registry)
        }
        if (_tagEditorPopup && _tagEditorPopup.IsOpen())
        {
            _RebuildTagEditorList();
            _UpdateTagEditorAddState();
        }
    }

    // The tag-list row's ✕ (rendered only on a 0-carrier tag): EXPLICITLY delete the tag. This is
    // the ONE removal path — untagging a tag's last session keeps it listed (·0) on purpose. Only
    // the registry entry is dropped: if a session gained the tag meanwhile (another window), the
    // derived union keeps it alive — a carried tag can never be deleted. Its tag-colors.json entry
    // is kept (a re-created tag REGAINS its color — the documented feature); the freed name also
    // frees a slot under the AppSettings::maxTags cap.
    void TerminalPage::_RemoveGlobalTag(const std::wstring& tag)
    {
        if (tag.empty())
        {
            return;
        }
        ::Agentmaster::UnregisterKnownTag(tag);
        // Nav audit: the deliberate tag deletion (distinct from a per-session "tag remove").
        ::Agentmaster::LogNav(L"tag delete \"" + tag + L"\"");
        if (_tagEditorPopup && _tagEditorPopup.IsOpen())
        {
            _RebuildTagEditorList();
            _UpdateTagEditorAddState();
        }
    }

    // Dismiss the panel (Esc / a press outside the card / a tab switch). Idempotent.
    void TerminalPage::_CloseTagEditorPopup()
    {
        if (_tagEditorPopup && _tagEditorPopup.IsOpen())
        {
            _tagEditorPopup.IsOpen(false);
        }
        _tagEditorSessionId.clear();
    }

    // ==================================================================================
    // Agentmaster (bookmark tags): the rich TAG HOVER PANEL. Hovering a tab-header
    // bookmark badge opens a popup listing EVERY session carrying that tag — each row a
    // status dot (the Triage palette for a LIVE session; a hollow gray ring for a closed
    // one) + the session's title — and clicking a LIVE row JUMPS to its tab
    // (_ActivateClaudeSession — the cross-window activate). A popup rather than a ToolTip
    // because a tooltip can never take clicks (AgentSetTip's are hit-test-invisible by
    // design). Hover-intent timers give it tooltip ergonomics without the flash: a ~160ms
    // open delay (panning the strip opens nothing) and a ~300ms grace close that
    // panel-enter cancels (the pointer can cross the badge->panel gap) — the Sessions
    // range-popup recipe.
    // ==================================================================================

    // Badge enter (via Tab <- TabHeaderControl): remember what to show, cancel any pending
    // grace close, and (re)arm the open delay. Re-entering another badge mid-delay simply
    // re-targets the pending tag. Runs INSIDE pointer-event dispatch, so it must not touch
    // the visual tree — it only creates the two timers (lazily; not tree work) and arms the
    // open delay; the popup's Root() append happens on the timer's clean dispatcher tick
    // (_ShowTagHoverPanelNow -> _EnsureTagHoverPopup), per the islands defer discipline.
    void TerminalPage::_OnTagBadgeHoverBegin(const winrt::hstring& tag, const WUX::UIElement& anchor)
    {
        if (tag.empty() || !anchor)
        {
            return;
        }
        if (!_tagHoverOpenTimer)
        {
            _tagHoverOpenTimer = WUX::DispatcherTimer{};
            _tagHoverOpenTimer.Interval(std::chrono::milliseconds{ 160 });
            _tagHoverOpenTimer.Tick([weak = get_weak()](auto&&, auto&&) {
                if (const auto page = weak.get())
                {
                    if (page->_tagHoverOpenTimer)
                    {
                        page->_tagHoverOpenTimer.Stop(); // one-shot
                    }
                    page->_ShowTagHoverPanelNow();
                }
            });
        }
        if (!_tagHoverCloseTimer)
        {
            _tagHoverCloseTimer = WUX::DispatcherTimer{};
            _tagHoverCloseTimer.Interval(std::chrono::milliseconds{ 300 });
            _tagHoverCloseTimer.Tick([weak = get_weak()](auto&&, auto&&) {
                if (const auto page = weak.get())
                {
                    if (page->_tagHoverCloseTimer)
                    {
                        page->_tagHoverCloseTimer.Stop(); // one-shot
                    }
                    page->_CloseTagHoverPopup();
                }
            });
        }
        _tagHoverPendingTag = tag;
        _tagHoverPendingAnchor = anchor;
        _tagHoverCloseTimer.Stop();
        _tagHoverOpenTimer.Stop();
        _tagHoverOpenTimer.Start(); // one-shot; restarts the delay on each badge enter
    }

    // Badge exit: a pending (not yet shown) panel is simply cancelled; an OPEN panel gets the
    // grace close — cancelled again if the pointer arrives on the panel itself.
    void TerminalPage::_OnTagBadgeHoverEnd()
    {
        if (_tagHoverOpenTimer)
        {
            _tagHoverOpenTimer.Stop();
        }
        if (_tagHoverPopup && _tagHoverPopup.IsOpen() && _tagHoverCloseTimer)
        {
            _tagHoverCloseTimer.Start();
        }
    }

    // Build the popup + card ONCE and parent it into Root() (the tag editor's recipe;
    // root-relative offsets). The card is the hover-keepalive boundary. Called only from the
    // open timer's tick (a clean dispatcher pass), never from inside pointer dispatch — the
    // Root() append is a tree mutation.
    void TerminalPage::_EnsureTagHoverPopup()
    {
        if (_tagHoverPopup)
        {
            return;
        }

        _tagHoverBody = StackPanel{};
        _tagHoverBody.Spacing(2);
        _tagHoverBody.MinWidth(220);
        _tagHoverBody.MaxWidth(360);

        ScrollViewer scroll; // a heavily-used tag can carry many sessions — bound the height
        scroll.MaxHeight(320);
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll.Content(_tagHoverBody);

        _tagHoverCard = Border{};
        _tagHoverCard.RequestedTheme(ElementTheme::Dark); // Agentmaster surfaces are always dark
        _tagHoverCard.Background(SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x26, 0x26, 0x26) });
        _tagHoverCard.BorderBrush(SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x3A, 0x3A, 0x3A) });
        _tagHoverCard.BorderThickness(Thickness{ 1, 1, 1, 1 });
        _tagHoverCard.CornerRadius(winrt::Windows::UI::Xaml::CornerRadius{ 6, 6, 6, 6 });
        _tagHoverCard.Padding(Thickness{ 8, 6, 8, 6 });
        _tagHoverCard.Child(scroll);
        // The keepalive: entering the card cancels the grace close; leaving it re-arms it.
        _tagHoverCard.PointerEntered([weak = get_weak()](auto&&, auto&&) {
            if (const auto page = weak.get())
            {
                if (page->_tagHoverCloseTimer)
                {
                    page->_tagHoverCloseTimer.Stop();
                }
            }
        });
        _tagHoverCard.PointerExited([weak = get_weak()](auto&&, auto&&) {
            if (const auto page = weak.get())
            {
                if (page->_tagHoverCloseTimer)
                {
                    page->_tagHoverCloseTimer.Start();
                }
            }
        });

        _tagHoverPopup = Windows::UI::Xaml::Controls::Primitives::Popup{};
        _tagHoverPopup.Child(_tagHoverCard);
        _tagHoverPopup.HorizontalAlignment(HorizontalAlignment::Left);
        _tagHoverPopup.VerticalAlignment(VerticalAlignment::Top);
        Grid::SetRow(_tagHoverPopup, 0);
        Grid::SetRowSpan(_tagHoverPopup, 3); // span the whole Root grid
        Root().Children().Append(_tagHoverPopup);
    }

    // The open delay elapsed on a badge: gather every session carrying the pending tag (the
    // durable store — one sparse scan on hover, not per tick), render the rows, and place the
    // panel just under the badge. LIVE sessions first (activity desc), then closed ones.
    void TerminalPage::_ShowTagHoverPanelNow()
    {
        if (_tagHoverPendingTag.empty() || !_tagHoverPendingAnchor)
        {
            return;
        }
        _EnsureTagHoverPopup(); // safe here: the open timer's tick is a clean dispatcher pass
        if (!_tagHoverPopup || !_tagHoverBody)
        {
            return;
        }
        const std::wstring tag{ _tagHoverPendingTag };
        const std::wstring folded = ::Agentmaster::FoldTagName(tag);

        struct HoverRow
        {
            std::wstring sid;
            std::wstring title;
            ::Agentmaster::SessionState state{ ::Agentmaster::SessionState::Idle };
            bool live{ false };
            int64_t activity{ 0 };
        };
        std::vector<HoverRow> rows;
        for (const auto& [sid, tags] : ::Agentmaster::LoadAllSessionTags())
        {
            bool has = false;
            for (const auto& t : tags)
            {
                if (::Agentmaster::FoldTagName(t) == folded)
                {
                    has = true;
                    break;
                }
            }
            if (!has)
            {
                continue;
            }
            HoverRow r;
            r.sid = sid;
            if (_sessionRegistry)
            {
                if (const auto s = _sessionRegistry->Get(sid))
                {
                    r.title = s->title;
                    r.state = s->state;
                    r.live = s->live;
                    r.activity = (std::max)(s->lastActivityUnixMs, s->convLastActivityUnixMs);
                }
            }
            if (r.title.empty())
            {
                r.title = ::Agentmaster::GetStoredSessionTitle(sid); // a closed / never-loaded session's durable title
            }
            if (r.title.empty())
            {
                r.title = ::Agentmaster::ShortId(sid); // last resort: the first-8 id convention
            }
            rows.push_back(std::move(r));
        }
        std::sort(rows.begin(), rows.end(), [](const HoverRow& a, const HoverRow& b) {
            if (a.live != b.live)
            {
                return a.live; // live (jumpable) sessions first
            }
            if (a.activity != b.activity)
            {
                return a.activity > b.activity;
            }
            return a.title < b.title;
        });

        _tagHoverBody.Children().Clear();
        {
            // Header: the tag's bookmark ribbon + its name + the carrier count.
            StackPanel h;
            h.Orientation(Orientation::Horizontal);
            h.Spacing(6);
            Windows::UI::Xaml::Shapes::Polygon ribbon;
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 0.0f, 0.0f });
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 6.0f, 0.0f });
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 6.0f, 9.0f });
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 3.0f, 6.3f });
            ribbon.Points().Append(winrt::Windows::Foundation::Point{ 0.0f, 9.0f });
            ribbon.Fill(SolidColorBrush{ ResolveTagDisplayColor(tag, ::Agentmaster::LoadAllTagColors()) });
            ribbon.Stroke(SolidColorBrush{ winrt::Windows::UI::Colors::Black() });
            ribbon.StrokeThickness(0.75);
            ribbon.VerticalAlignment(VerticalAlignment::Center);
            h.Children().Append(ribbon);
            TextBlock name;
            name.Text(winrt::hstring{ tag });
            name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            name.VerticalAlignment(VerticalAlignment::Center);
            h.Children().Append(name);
            TextBlock count;
            count.Text(winrt::hstring{ L"\x00B7 " + std::to_wstring(rows.size()) + L" session" + (rows.size() == 1 ? L"" : L"s") });
            count.FontSize(11);
            count.Opacity(0.55);
            count.VerticalAlignment(VerticalAlignment::Center);
            h.Children().Append(count);
            _tagHoverBody.Children().Append(h);
        }
        for (const auto& r : rows)
        {
            Button row;
            row.HorizontalAlignment(HorizontalAlignment::Stretch);
            row.HorizontalContentAlignment(HorizontalAlignment::Left);
            row.Background(SolidColorBrush{ winrt::Windows::UI::Colors::Transparent() });
            row.BorderThickness(Thickness{ 0, 0, 0, 0 });
            row.Padding(Thickness{ 6, 3, 6, 3 });

            StackPanel h;
            h.Orientation(Orientation::Horizontal);
            h.Spacing(6);
            // The status indicator: the session's Triage state color when LIVE (the same
            // AgentStatusColorFor palette as its tab dot), a hollow gray ring when closed.
            winrt::Windows::UI::Xaml::Shapes::Ellipse dot;
            dot.Width(9);
            dot.Height(9);
            dot.VerticalAlignment(VerticalAlignment::Center);
            if (r.live)
            {
                dot.Fill(SolidColorBrush{ AgentStatusColorFor(r.state) });
                dot.Stroke(SolidColorBrush{ winrt::Windows::UI::Colors::Black() });
            }
            else
            {
                dot.Fill(SolidColorBrush{ winrt::Windows::UI::Colors::Transparent() });
                dot.Stroke(SolidColorBrush{ winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x80, 0x80, 0x80) });
            }
            dot.StrokeThickness(1);
            h.Children().Append(dot);
            // The title — first line only (titles can be multi-line), display-capped.
            std::wstring title = r.title;
            if (const size_t nl = title.find_first_of(L"\r\n"); nl != std::wstring::npos)
            {
                title.resize(nl);
            }
            if (title.size() > 56)
            {
                title.resize(56);
                title += L"\x2026";
            }
            TextBlock name;
            name.Text(winrt::hstring{ title });
            name.VerticalAlignment(VerticalAlignment::Center);
            name.Opacity(r.live ? 1.0 : 0.6);
            h.Children().Append(name);
            if (!r.live)
            {
                TextBlock closed;
                closed.Text(L"\x00B7 closed");
                closed.FontSize(11);
                closed.Opacity(0.45);
                closed.VerticalAlignment(VerticalAlignment::Center);
                h.Children().Append(closed);
            }
            row.Content(h);
            if (r.live)
            {
                const winrt::hstring sidH{ r.sid };
                row.Click([weak = get_weak(), sidH](auto&&, auto&&) {
                    if (const auto page = weak.get())
                    {
                        // Defer — the jump switches tabs (which also dismisses this panel).
                        page->Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [weak, sidH]() {
                            if (const auto p = weak.get())
                            {
                                p->_CloseTagHoverPopup();
                                p->_ActivateClaudeSession(sidH); // logs [nav] activate; cross-window
                            }
                        });
                    }
                });
            }
            else
            {
                row.IsEnabled(false); // closed sessions have no tab to jump to (resume from Sessions)
            }
            _tagHoverBody.Children().Append(row);
        }

        // Place just under the badge (root-relative — the tag editor's anchor recipe), clamped
        // so the card can't hang past the window's right edge.
        double x = 8;
        double y = 40;
        try
        {
            const auto pt = _tagHoverPendingAnchor.TransformToVisual(Root()).TransformPoint(winrt::Windows::Foundation::Point{ 0, 0 });
            x = pt.X - 8;
            y = pt.Y + 12;
        }
        CATCH_LOG();
        try
        {
            const double rootW = Root().ActualWidth();
            if (rootW > 380 && x > rootW - 380)
            {
                x = rootW - 380;
            }
        }
        catch (...)
        {
        }
        _tagHoverPopup.HorizontalOffset(std::max(0.0, x));
        _tagHoverPopup.VerticalOffset(std::max(0.0, y));
        _tagHoverPopup.IsOpen(true);
    }

    // Dismiss the hover panel (grace timer / a row jump / tab switch). Idempotent.
    void TerminalPage::_CloseTagHoverPopup()
    {
        if (_tagHoverOpenTimer)
        {
            _tagHoverOpenTimer.Stop();
        }
        if (_tagHoverCloseTimer)
        {
            _tagHoverCloseTimer.Stop();
        }
        if (_tagHoverPopup && _tagHoverPopup.IsOpen())
        {
            _tagHoverPopup.IsOpen(false);
        }
        _tagHoverPendingTag = {};
        _tagHoverPendingAnchor = nullptr;
    }

    // Agentmaster (tab status dot): the registry-observer reaction (bounced to this window's UI
    // thread by the engine-init observer). A session state change recolors its hosting tab's dot in
    // place; live=false hides it (the liveness sweep also hides explicitly before it drops the
    // _claudeTabs entry — whichever lands first wins, both are idempotent). A session this window
    // doesn't host is a cheap map-miss no-op (every window's observer sees every fleet event).
    void TerminalPage::_UpdateTabAgentDot(const std::wstring& sessionId, ::Agentmaster::SessionState state, bool live, bool dormant)
    {
        // Agentmaster (System notifications): drop the toast tracker's per-session state on a live=false
        // update BEFORE the host gate below — the archive seams write live=false and then erase
        // _claudeTabs synchronously, so by the time this queued observer hop lands the map lookup
        // misses and the host-gated _EvaluateAgentNotification would never see the drop; a leaked
        // prev==Running would then phantom-fire a "completed" toast on a later restore of the same id.
        if (!live)
        {
            _agentNotifyLastState.erase(sessionId);
            _agentNotifyRunningSinceMs.erase(sessionId);
            _agentToastHeld.erase(sessionId); // an archived session's held toast dies with it (no deferred "completed" off a previous life)
            _agentToastLastShownMs.erase(sessionId);
        }
        const auto it = _claudeTabs.find(sessionId);
        if (it == _claudeTabs.end())
        {
            return;
        }
        if (const auto tab = it->second.get())
        {
            _SetTabAgentDot(tab, live ? std::optional{ AgentStatusColorFor(state) } : std::nullopt, dormant);
            _EvaluateAgentFlash(sessionId, tab, state, live); // start/stop the unvisited "left Running" red flash
            _EvaluateAgentNotification(sessionId, tab, state, live); // Windows toast on the Running -> X edge (the cog's Notifications tab)
            // LAZY tab tooltip (the CPU fix): a LIVE session's card is built on hover, not per notify —
            // with 19+ active sessions the per-notify rebuild here was a steady UI-thread burn (each one
            // a registry deep-copy + string builds + a XAML card). Only the !live edge still routes
            // through: _UpdateTabAgentToolTip's gone/archived branch CLEARS to the default tooltip +
            // drops the summary cache (event-driven — exactly when it changes).
            if (!live)
            {
                _UpdateTabAgentToolTip(tab, sessionId);
            }
        }
    }

    void TerminalPage::_UpdateTabAgentToolTip(const TerminalApp::Tab& tab, const std::wstring& sessionId, bool swapWhileOpen, bool kickSummary)
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
        // The model is the CURRENT one — the transcript truth (SessionDisplayModel: what the last
        // reply actually ran on, so a /model switch shows on its next reply; falls back to the launch
        // `--model`, also where a managed Codex's rollout model lives) — shortened (ShortModelName:
        // "fable-5", not "claude-fable-5"; a Codex id passes through verbatim).
        std::vector<std::wstring> metaParts;
        metaParts.push_back(s.kind == ::Agentmaster::AgentKind::Codex ? std::wstring{ L"codex" } : std::wstring{ L"claude" });
        if (const std::wstring shortModel = ::Agentmaster::ShortModelName(::Agentmaster::SessionDisplayModel(s), ::Agentmaster::ParseModelFamilies(_appSettings.modelFamilies)); !shortModel.empty())
        {
            metaParts.push_back(shortModel);
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

        // The folder/branch line (its own dim line under the title): the leaf working-dir folder + "/" +
        // git branch (the overlay subline) -- the full path is too long to read at a glance, so show only
        // the folder name and the branch. The dir is the session's EFFECTIVE work dir
        // (EffectiveWorkingDir — the INFERRED dir while the session infers: the Inferred tab-color mode,
        // or a home-dir launch in ANY mode; else the launch cwd), matching the overlay subline / board
        // card / tree group; the dim detail line below carries whichever dir this leaf can't.
        const bool inferredActive = !s.inferredWorkingDir.empty() && ::Agentmaster::SessionInfersWorkingDir(_appSettings.tabColorMode, s);
        const std::wstring effDir = ::Agentmaster::EffectiveWorkingDir(_appSettings.tabColorMode, s);
        std::wstring dir = !effDir.empty() ? effDir : s.liveCwd;
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

        // The directory DETAIL line — non-empty only when the scan detected the session working
        // somewhere OTHER than its launch cwd (SessionInfo::inferredWorkingDir is stored empty when
        // they agree), so "present" already means "worth showing". While the inference is SHOWN as
        // the header dir (inferredActive — the Inferred mode, or a home-dir launch in any mode) the
        // detail carries the LAUNCH cwd ("launched in → …");
        // otherwise (a dormant inference) it keeps the classic "inferred → …" full inferred path.
        std::wstring dirDetailLine;
        if (inferredActive && !s.workingDir.empty())
        {
            dirDetailLine = L"launched in \x2192 " + s.workingDir;
        }
        else if (!s.inferredWorkingDir.empty())
        {
            dirDetailLine = L"inferred \x2192 " + s.inferredWorkingDir;
        }

        const std::wstring title = s.title.empty() ? std::wstring{ L"(untitled)" } : s.title;

        // Bookmark TAGS — parsed off the tab's own AgentTagsSpec ("name\t#AARRGGBB" lines; the
        // colors were already resolved by _SetTabAgentTags), so this per-sweep refresh does no
        // store I/O. Rendered as name-over-colored-underscore chips in the card.
        std::wstring tagsSpec;
        std::vector<std::pair<std::wstring, winrt::Windows::UI::Color>> tagChips;
        try
        {
            if (const auto status = tab.TabStatus())
            {
                tagsSpec = std::wstring{ status.AgentTagsSpec() };
            }
        }
        CATCH_LOG();
        {
            std::wstring_view rest{ tagsSpec };
            while (!rest.empty())
            {
                const size_t nl = rest.find(L'\n');
                const std::wstring_view specLine = rest.substr(0, nl);
                rest = (nl == std::wstring_view::npos) ? std::wstring_view{} : rest.substr(nl + 1);
                std::wstring_view tagName = specLine;
                std::wstring_view tagHex{};
                if (const size_t sep = specLine.find(L'\t'); sep != std::wstring_view::npos)
                {
                    tagName = specLine.substr(0, sep);
                    tagHex = specLine.substr(sep + 1);
                }
                if (!tagName.empty())
                {
                    tagChips.emplace_back(std::wstring{ tagName }, ParseArgbHexColor(tagHex, TagColorFor(tagName)));
                }
            }
        }

        // The Summary-box body (numbered messages + files), cached + loaded off-thread (see below).
        winrt::hstring bodyText;
        int64_t bodyMtime = 0;
        if (const auto it = _tabTooltipSummary.find(sessionId); it != _tabTooltipSummary.end())
        {
            bodyText = it->second.body;
            bodyMtime = it->second.mtime;
        }

        // Re-host only on a real content change. The signature folds the header strings + the tags spec +
        // the body's mtime (the body itself changes only when the transcript grows, which bumps the
        // mtime), so an unchanged idle tab is skipped; the "ago" ticking is what re-hosts an
        // otherwise-quiet tab each sweep.
        std::wstring sig = stateText;
        sig += L'\x1f';
        sig += title;
        sig += L'\x1f';
        sig += folderBranch;
        sig += L'\x1f';
        sig += metaText;
        sig += L'\x1f';
        sig += dirDetailLine;
        sig += L'\x1f';
        sig += tagsSpec;
        sig += L'\x1f';
        // The tooltip tag-row opacity rides the sig (rounded to the slider's 1% grain) so a cog
        // change re-hosts an already-built tooltip on the next tick, not only on a content change.
        const double tagsOpacity = ::Agentmaster::ClampTooltipTagsOpacity(_appSettings.tooltipTagsOpacity);
        if (!tagChips.empty())
        {
            sig += std::to_wstring(static_cast<int>(tagsOpacity * 100.0 + 0.5));
            sig += L'\x1f';
        }
        sig += std::to_wstring(bodyMtime);
        if (const auto sit = _tabTooltipSig.find(sessionId); sit == _tabTooltipSig.end() || sit->second != sig)
        {
            impl->SetAgentToolTip(TtBuildTooltipCard(accent, title, folderBranch, stateText, metaText, dirDetailLine, tagChips, tagsOpacity, bodyText), winrt::hstring{ sig }, swapWhileOpen);
            _tabTooltipSig[sessionId] = sig;
        }

        // Keep the Summary body fresh off-thread (throttled + mtime-gated). First sight has no body yet, so
        // this fills it in, then re-hosts the card (a recursive _UpdateTabAgentToolTip on completion).
        // kickSummary=false (the arm/bind pre-host) skips the kick entirely: those builds exist only so
        // ToolTipService has a hosted card before the first hover — the analyze belongs to a REAL hover
        // (else the first sweep after a 60-tab window restore would burst 60 whole-transcript parses onto
        // the pool at the most fragile startup moment). The first hover's build kicks it, and the body
        // lands into the open tip via the completion's swapWhileOpen grant.
        auto& slot = _tabTooltipSummary[sessionId]; // default-creates an empty slot on first sight
        const bool needCheck = kickSummary && (slot.body.empty() || slot.mtime == 0 || (now - slot.lastCheckMs) > 4000);
        if (needCheck && !_tabTooltipSummaryInFlight.count(sessionId))
        {
            slot.lastCheckMs = now;
            const bool codex = (s.kind == ::Agentmaster::AgentKind::Codex);
            _EnsureTabTooltipSummary(tab, winrt::hstring{ sessionId }, codex, winrt::hstring{ s.codexSessionId }, winrt::hstring{ dir });
        }
    }

    // Agentmaster (LAZY tab tooltip — the CPU fix): arm the EVENT-DRIVEN rebuild for a managed tab. The
    // rich card used to be rebuilt for EVERY managed tab on EVERY ~2.5s liveness sweep AND on every
    // registry notify — on a 70-tab strip with active sessions that was a constant stream of registry
    // deep-copies + string builds + XAML card constructions on the UI thread for tooltips nobody was
    // hovering (the measured 65%-of-a-core burn; the seconds-granularity "ago" line busted the content
    // signature every sweep). Now the card is built ONLY when the pointer actually enters the tab:
    // Tab::EnsureAgentToolTipHoverHook wires PointerEntered (fires well before ToolTipService's open
    // delay, so the swap happens while the tip is closed — the safe window), and the callback resolves
    // the tab's CURRENT session at hover time (_ClaudeSessionForTab), so a /resume re-home or fork
    // re-bind never leaves it stale. Idempotent and CHEAP when already armed (one bool probe — safe to
    // call per sweep tick as the self-healing arm for tabs that entered _claudeTabs by any path, incl.
    // window-restored dormant tabs that never pass the bind funnel). The FIRST arm also builds once, so
    // the tooltip is hosted with ToolTipService before the first hover (a tooltip set mid-dwell may not
    // open until the next hover otherwise).
    void TerminalPage::_ArmTabAgentToolTipHover(const TerminalApp::Tab& tab)
    {
        if (!tab)
        {
            return;
        }
        const auto impl = _GetTabImpl(tab);
        if (!impl || impl->AgentToolTipHoverWired())
        {
            return; // steady-state per-tick cost: this bool probe, nothing else
        }
        const bool justWired = impl->EnsureAgentToolTipHoverHook([weakThis = get_weak(), weakTab = winrt::make_weak(tab)]() {
            const auto self = weakThis.get();
            const auto t = weakTab.get();
            if (self && t)
            {
                if (const auto sid = self->_ClaudeSessionForTab(t); !sid.empty())
                {
                    self->_UpdateTabAgentToolTip(t, sid); // hover-time build: fresh state + fresh "ago", kicks the mtime-gated summary load
                }
            }
        });
        if (justWired)
        {
            if (const auto sid = _ClaudeSessionForTab(tab); !sid.empty())
            {
                // Arm-build once so the FIRST hover opens a hosted card — header only (kickSummary=false):
                // the transcript analyze waits for a real hover, so arming a restored 60-tab strip costs
                // 60 small card builds, not 60 whole-transcript parses.
                _UpdateTabAgentToolTip(tab, sid, /* swapWhileOpen */ false, /* kickSummary */ false);
            }
        }
    }

    // Agentmaster (tab tooltip): resolve + stat + analyze the session's transcript OFF the UI thread, and
    // on a transcript growth render the session-end.js Summary box (full=false: numbered messages + files,
    // NO header -- the card already shows state/title/dir) into the per-session cache, then re-host the
    // card. mtime-gated (a quiet tab is one stat) + in-flight-guarded (one load per session at a time);
    // mirrors the AgentTabOverlay summary panel's off-thread caching. Codex uses its rollout analog.
    // LAZY: reached only from a hover/bind/tag-toggle build (_UpdateTabAgentToolTip), never per tick —
    // so the whole-transcript re-analyze runs at human-hover cadence, not 4s x N tabs (the pool burn).
    winrt::fire_and_forget TerminalPage::_EnsureTabTooltipSummary(winrt::TerminalApp::Tab tab, winrt::hstring sessionId, bool codex, winrt::hstring codexId, winrt::hstring cwd)
    {
        const std::wstring id{ sessionId };
        if (id.empty() || _tabTooltipSummaryInFlight.count(id))
        {
            co_return; // already loading this session's summary
        }
        auto strongThis{ get_strong() };
        _tabTooltipSummaryInFlight.insert(id);

        // Agentmaster (extra-safe): this is a fire_and_forget — ANY exception that escapes it
        // std::terminates the app (the _RefreshPromptNavCache / _ScrollAdjacentPrompt idiom). Two throw
        // classes lurk here: (1) the OFF-THREAD transcript resolve/stat/analyze/render — a corrupt or huge
        // .jsonl / rollout, a std::bad_alloc, an out_of_range (AnalyzeSessionTranscript self-contains, but
        // RenderSessionSummaryBox / RenderCodexSummaryBox / ReadCodexRolloutInfo do NOT); and (2) the
        // UI-thread re-host / resume_foreground — a teardown-race resume throw, or _UpdateTabAgentToolTip
        // building XAML. The INNER try contains (1) on the background thread (so it never touches the
        // UI-thread maps and we still reach the resume_foreground cleanup); the OUTER try is the
        // terminate-net for (2). On any failure we simply leave the header-only card — never crash over a
        // tooltip.
        try
        {
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
            int64_t mtime = 0;
            std::wstring body;
            bool rendered = false;
            try
            {
                if (path.empty())
                {
                    path = codex ? ::Agentmaster::ResolveCodexRolloutPathIn(::Agentmaster::CodexDefaultHome(), codexUuid)
                                 : ::Agentmaster::ResolveClaudeTranscriptPath(id);
                }
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
            }
            catch (...)
            {
                // Contained on the BACKGROUND thread (no UI-map access here) — fall through to the
                // resume_foreground cleanup so the in-flight guard is always cleared. Leave the
                // header-only card (body stays empty).
                OutputDebugStringW(L"[Agentmaster] _EnsureTabTooltipSummary: swallowed background analyze/render exception (no crash)\n");
            }

            co_await wil::resume_foreground(Dispatcher());
            _tabTooltipSummaryInFlight.erase(id);
            if (!path.empty())
            {
                auto& slot = _tabTooltipSummary[id];
                slot.path = path;
                slot.mtime = mtime;
                if (rendered)
                {
                    slot.body = winrt::hstring{ body };
                }
                // Re-host the card with the now-loaded / refreshed body. _UpdateTabAgentToolTip recomputes the
                // signature (new mtime) so it re-hosts; the throttle (lastCheckMs, bumped by the caller) keeps it
                // from immediately re-kicking us. swapWhileOpen: the hover that kicked this load is likely still
                // showing the header-only card — the one-shot open-swap grant lets the body land IN the open tip
                // (the single moment the "no Content swap while open" rule is wrong — the user is waiting for it).
                _UpdateTabAgentToolTip(tab, id, /* swapWhileOpen */ true);
            }
            // path empty => no transcript yet (never prompted): leave the header-only card. The in-flight
            // flag was already cleared above, so a later hover retries.
        }
        catch (...)
        {
            // Terminate-net for a resume_foreground teardown-race throw or a _UpdateTabAgentToolTip
            // XAML-build throw. Deliberately does NOT touch the UI-thread maps (we may be off-thread on a
            // resume failure; on a UI-thread throw the in-flight flag was already erased above). A leaked
            // flag can only happen on teardown, where the map dies with the page — moot. Never rethrow.
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[tab-tooltip] _EnsureTabTooltipSummary: swallowed exception (no crash)\n");
        }
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

    // Agentmaster (System notifications — the Settings cog's "Notifications" tab): the Windows-toast
    // twin of _EvaluateAgentFlash, on the SAME registry-observer push but with its OWN edge tracker
    // (deliberately not sharing _agentFlashLastState — the flash holds/erases its map on its own rules,
    // and coupling them would let a change in one silently break the other). The rule: when a hosted
    // session's state leaves RUNNING for anything else, raise a toast
    //     <session title>
    //     Has completed after <2h30m> and is <status>
    // gated by the per-target-state switches (default all ON == "Running to anything else") and the
    // "skip when focused" rule. Because _UpdateTabAgentDot only reaches this for a session hosted in
    // THIS window's _claudeTabs, exactly one window fires per transition. UI thread (the observer hop).
    // Agentmaster (System notifications — spurious-completion hold): the external-work signal at
    // TOAST time, for ONE session — the same two raw inputs the scanner's outlived-turn expression
    // reads (claude's own busy/shell heartbeat; fresh subagent/tool-result side files), evaluated on
    // demand. presenceStatus is the registry's S-lane copy (<= ~2s stale — a real Stop leaves "busy"
    // lingering one survey tick, which is exactly why a held toast costs ~one sweep tick before it
    // fires); the side-file probe (two FindFirstFile enumerations) is paid only when presence alone
    // doesn't decide. `what` receives the human-readable reason for the [notify-hold] log.
    static bool AgentExternalWorkSignal(const ::Agentmaster::SessionInfo& s, bool& presenceWorking, bool& subagentFresh, std::wstring& what)
    {
        presenceWorking = ::Agentmaster::PresenceIsWorking(s.presenceStatus);
        subagentFresh = false;
        if (presenceWorking)
        {
            what = L"presence=" + s.presenceStatus;
            return true;
        }
        if (!s.workingDir.empty() && !s.id.empty())
        {
            const std::wstring path = ::Agentmaster::ClaudeProjectsDir() + L"\\" + ::Agentmaster::EncodeCwdToProjectDir(s.workingDir) + L"\\" + s.id + L".jsonl";
            const int64_t subMs = ::Agentmaster::SubagentActivityUnixMs(path);
            if (subMs > 0 && TtNowMs() - subMs <= ::Agentmaster::kScanSubagentFreshMs)
            {
                subagentFresh = true;
                what = L"side-files";
                return true;
            }
        }
        return false;
    }

    void TerminalPage::_EvaluateAgentNotification(const std::wstring& sessionId, const TerminalApp::Tab& tab, ::Agentmaster::SessionState newState, bool live)
    {
        using ::Agentmaster::SessionState;
        if (!live)
        {
            // Archived/closed: forget the track so a later restore starts fresh — no phantom
            // Running -> X toast off a state recorded in a previous life (the _EvaluateAgentFlash
            // !live rule; also run host-gate-free at the top of _UpdateTabAgentDot, since the archive
            // seams drop _claudeTabs before this queued hop lands).
            _agentNotifyLastState.erase(sessionId);
            _agentNotifyRunningSinceMs.erase(sessionId);
            _agentToastHeld.erase(sessionId);
            _agentToastLastShownMs.erase(sessionId);
            return;
        }

        const auto prevIt = _agentNotifyLastState.find(sessionId);
        const bool hadPrev = (prevIt != _agentNotifyLastState.end());
        const auto prev = hadPrev ? prevIt->second : newState;
        _agentNotifyLastState[sessionId] = newState;

        if (newState == SessionState::Running)
        {
            // Re-entering Running CONFIRMS a held toast as spurious — the outlived-turn promotion
            // (or a genuine new turn) re-lit the session while its completion toast was on hold.
            // This push edge is the PRIMARY drop path (it lands ms after the promotion); the sweep's
            // Drop verdict is the belt.
            if (const auto heldIt = _agentToastHeld.find(sessionId); heldIt != _agentToastHeld.end())
            {
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[notify-drop] " + ::Agentmaster::ShortId(sessionId) + L" re-lit running (held " +
                                                  std::to_wstring((TtNowMs() - heldIt->second.heldAtMs) / 1000) + L"s), spurious completion suppressed\n");
                _agentToastHeld.erase(heldIt);
            }
            // Entering Running: stamp the entry time — but only on a SEEN edge. A session first
            // observed already-Running (adopted / moved in mid-turn) gets no stamp; its completion
            // toast then omits the "after <duration>" clause instead of under-reporting a span
            // measured from first sight.
            if (hadPrev && prev != SessionState::Running)
            {
                _agentNotifyRunningSinceMs[sessionId] = TtNowMs();
            }
            return;
        }

        if (!hadPrev || prev != SessionState::Running)
        {
            return; // at-rest -> at-rest noise (e.g. the Waiting -> Idle decay), or first sight at rest — only the Running -> X edge notifies
        }

        // Left Running. Consume the entry stamp regardless of whether a toast ends up shown — the
        // span belongs to the turn that just ended either way (a re-entry restamps).
        int64_t runningSince = 0;
        if (const auto sinceIt = _agentNotifyRunningSinceMs.find(sessionId); sinceIt != _agentNotifyRunningSinceMs.end())
        {
            runningSince = sinceIt->second;
            _agentNotifyRunningSinceMs.erase(sinceIt);
        }

        if (!_appSettings.notificationsEnabled)
        {
            return;
        }

        // HOLD instead of fire while external work is live (SessionScanner's ShouldHoldCompletionToast):
        // only Idle/WaitingForInput ever hold — exactly the two states the outlived-turn promotion can
        // veto (a NeedsApproval question / an Error / an exit needs you regardless of background work).
        // The liveness-ticked sweep (_SweepAgentPendingToasts) later DROPS the hold (session re-lit
        // Running — the 513d1366 spurious "waiting for you" toast) or FIRES it with the CURRENT state
        // (signal cleared / moved to a hard needs-you state / the kNotifyExternalHoldCapMs backstop).
        if (_sessionRegistry)
        {
            if (const auto info = _sessionRegistry->Get(sessionId); info && info->live)
            {
                bool presenceWorking = false;
                bool subagentFresh = false;
                std::wstring what;
                AgentExternalWorkSignal(*info, presenceWorking, subagentFresh, what);
                if (::Agentmaster::ShouldHoldCompletionToast(newState, presenceWorking, subagentFresh))
                {
                    _agentToastHeld[sessionId] = AgentHeldToast{ newState, runningSince, TtNowMs() };
                    ::Agentmaster::AppendStateLog(L"hooks.log",
                                                  L"[notify-hold] " + ::Agentmaster::ShortId(sessionId) + L" running -> " + TtStateLabel(newState) +
                                                      L" (external work live: " + what + L")\n");
                    return;
                }
            }
        }

        _FireAgentCompletionToast(sessionId, tab, newState, runningSince, 0);
    }

    // Agentmaster (System notifications): the FIRE half of _EvaluateAgentNotification — everything
    // downstream of the edge/hold decision, shared by the immediate path and the sweep's deferred
    // fire so the two can never drift: the per-state cog switch, the focused-tab skip, the
    // per-session double-toast guard, title/body build, the [notify] log (+ "held Ns" when
    // deferred), and the toast itself. `state` is the state at FIRE time (a held toast passes the
    // re-read CURRENT state — the body says what the session is NOW); runningSinceMs is the
    // Running-entry stamp consumed at the edge (0 == entry unseen -> no "after <span>" clause).
    void TerminalPage::_FireAgentCompletionToast(const std::wstring& sessionId, const TerminalApp::Tab& tab, ::Agentmaster::SessionState state, int64_t runningSinceMs, int64_t heldForMs)
    {
        using ::Agentmaster::SessionState;
        if (!_appSettings.notificationsEnabled)
        {
            return; // master toggled off (possibly mid-hold)
        }
        bool wanted = false;
        switch (state)
        {
        case SessionState::WaitingForInput:
            wanted = _appSettings.notifyOnWaiting;
            break;
        case SessionState::NeedsApproval:
            wanted = _appSettings.notifyOnNeedsApproval;
            break;
        case SessionState::Idle:
            wanted = _appSettings.notifyOnIdle;
            break;
        case SessionState::Done:
            wanted = _appSettings.notifyOnDone;
            break;
        case SessionState::Error:
            wanted = _appSettings.notifyOnError;
            break;
        default:
            break; // Running never fires (the edge returns on it; DecideHeldToast Drops on it)
        }
        if (!wanted)
        {
            return; // this target state is muted in the cog (re-checked at fire time — a hold can outlive a cog Save)
        }
        // "Skip when the tab is focused": this window is the ACTIVE one AND the session's tab is its
        // focused tab — you watched it finish (the flash ring's "the current tab is always considered
        // visited" rule, applied to toasts). A focused tab in a BACKGROUND window still notifies.
        // Re-checked at fire time for a held toast: the user may have visited the tab during the hold.
        if (_appSettings.notifySuppressFocused && _activated && tab == _GetFocusedTab())
        {
            return;
        }
        // Double-toast guard: at most one SHOWN toast per session per kNotifyDuplicateToastMs window —
        // a Waiting -> Running -> Waiting flap otherwise toasts on every Waiting entry (the Action
        // Center's Tag+Group replace dedupes the pile, not the interruptions).
        const int64_t now = TtNowMs();
        if (const auto lastIt = _agentToastLastShownMs.find(sessionId);
            lastIt != _agentToastLastShownMs.end() && ::Agentmaster::ShouldSuppressDuplicateToast(now, lastIt->second))
        {
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[notify-dedupe] " + ::Agentmaster::ShortId(sessionId) + L" running -> " + TtStateLabel(state) +
                                              L" suppressed (last toast " + std::to_wstring((now - lastIt->second) / 1000) + L"s ago)\n");
            return;
        }

        // Line 1: the session title (ONE value, Rule #11) — read fresh from the registry, falling back
        // to the tab's text (an unbound rename edge), then a generic label (a title is never empty in
        // practice — DeriveSessionTitle).
        std::wstring title;
        if (_sessionRegistry)
        {
            if (const auto info = _sessionRegistry->Get(sessionId))
            {
                title = info->title;
            }
        }
        if (title.empty() && tab)
        {
            title = std::wstring{ tab.Title() };
        }
        if (title.empty())
        {
            title = L"Agent session";
        }

        // Line 2: "Has completed after 2h30m and is waiting for you" — the Running span via TtSpan
        // (the tooltip's compact form), omitted when the entry wasn't observed.
        std::wstring body = L"Has completed";
        std::wstring span;
        if (runningSinceMs > 0)
        {
            span = TtSpan(now - runningSinceMs, true);
            body += L" after " + span;
        }
        body += L" and is ";
        body += TtStateLabel(state);

        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[notify] " + ::Agentmaster::ShortId(sessionId) + L" running -> " + TtStateLabel(state) +
                                          (span.empty() ? std::wstring{} : (L" (after " + span + L")")) +
                                          (heldForMs > 0 ? (L" (held " + std::to_wstring(heldForMs / 1000) + L"s)") : std::wstring{}) + L"\n");
        _agentToastLastShownMs[sessionId] = now;
        _ShowAgentSessionToast(sessionId, title, body, !_appSettings.notifySound);
    }

    // Agentmaster (System notifications): sweep the HELD completion toasts on the liveness tick
    // (~2.5s cadence). Per held session, re-read the registry + the external-work signal and let the
    // pure DecideHeldToast rule: DROP when the session re-lit Running (the outlived-turn promotion
    // confirmed the completion spurious — the belt behind the push edge's primary drop) or left the
    // fleet; FIRE when the signal cleared (the common one-tick "busy" linger), the session moved to a
    // hard needs-you state, or the kNotifyExternalHoldCapMs backstop elapsed; KEEP otherwise. A
    // deferred fire passes the CURRENT state, and _FireAgentCompletionToast re-applies the cog
    // switches / focused-skip / double-toast guard at fire time. Mirrors _ScanPendingInput's
    // marshal + terminate-net shape (the probe ticks on the scanner thread; the Impl resumes onto
    // the UI thread where the maps live).
    winrt::fire_and_forget TerminalPage::_SweepAgentPendingToasts()
    {
        auto strongThis{ get_strong() };
        try
        {
            co_await _SweepAgentPendingToastsImpl();
        }
        catch (...)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[observer] _SweepAgentPendingToasts: swallowed exception (no crash)\n");
        }
    }

    winrt::Windows::Foundation::IAsyncAction TerminalPage::_SweepAgentPendingToastsImpl()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (_agentToastHeld.empty() || !_sessionRegistry)
        {
            co_return;
        }
        const int64_t now = TtNowMs();
        for (auto it = _agentToastHeld.begin(); it != _agentToastHeld.end();)
        {
            const std::wstring sessionId = it->first; // copy BEFORE any erase (never use a key ref past its node — the re-home UAF lesson)
            const auto info = _sessionRegistry->Get(sessionId);
            TerminalApp::Tab tab{ nullptr };
            if (const auto tabIt = _claudeTabs.find(sessionId); tabIt != _claudeTabs.end())
            {
                tab = tabIt->second.get();
            }
            if (!info || !tab)
            {
                it = _agentToastHeld.erase(it); // archived / moved out / tab torn down — the erase seams usually beat us here
                continue;
            }
            bool presenceWorking = false;
            bool subagentFresh = false;
            std::wstring what;
            AgentExternalWorkSignal(*info, presenceWorking, subagentFresh, what);
            const auto verdict = ::Agentmaster::DecideHeldToast(info->state, info->live, presenceWorking || subagentFresh, now - it->second.heldAtMs);
            if (verdict == ::Agentmaster::HeldToastVerdict::Keep)
            {
                ++it;
                continue;
            }
            const auto held = it->second;
            it = _agentToastHeld.erase(it);
            if (verdict == ::Agentmaster::HeldToastVerdict::Fire)
            {
                _FireAgentCompletionToast(sessionId, tab, info->state, held.runningSinceMs, now - held.heldAtMs);
            }
            else if (info->state == ::Agentmaster::SessionState::Running)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[notify-drop] " + ::Agentmaster::ShortId(sessionId) + L" re-lit running (held " +
                                                  std::to_wstring((now - held.heldAtMs) / 1000) + L"s), spurious completion suppressed\n");
            }
        }
        co_return;
    }

    // Agentmaster (System notifications): raise ONE Windows toast for a session — line 1 the title,
    // line 2 the completion body. The strings go in as DOM TEXT NODES (CreateTextNode), so XML-special
    // characters in a session title are escaped by the DOM, never hand-built markup. Tag+Group make a
    // session's newer toast REPLACE its older one in the Action Center instead of piling up (ShortId
    // fits the legacy 16-char Tag cap).
    //
    // CLICKING IT (NOTIFICATIONS.md §4a) surfaces the session — foreground the hosting window + select
    // its tab — over ONE of two mutually exclusive paths:
    //   * the TOAST COM ACTIVATOR (AgentToastActivator.h), when its class object registered at engine
    //     init. The `launch` attribute below carries the session id, which the shell hands back as
    //     INotificationActivationCallback::Activate's invokedArgs. This is the path that FIXES the
    //     stray window: the shell CoCreateInstances our CLSID and reaches THIS process, instead of
    //     falling back to an AUMID activation that launches a second process (which the running
    //     Emperor turns into a brand-new window with a default tab).
    //   * the legacy in-process ToastNotification.Activated handler, ONLY when the activator did NOT
    //     register (unpackaged, or a package registered before the manifests carried the CLSID — i.e.
    //     the loose layout hasn't been re-registered since). The jump then works exactly as it did —
    //     stray window and all — so a stale registration is never a regression, just un-fixed.
    // Wiring BOTH would double-jump (harmless — same tab, idempotent) and log two [nav] lines, so the
    // handler is wired only as the fallback. With the process gone the OS launches the ExeServer the
    // manifest names (`-ToastActivated`), which starts Agentmaster normally — a fresh window IS right
    // when nothing was open — and the pending activation then jumps if it beats the SCM's timeout.
    //
    // Best-effort by design: CreateToastNotifier throws on a build with no package identity (no AUMID,
    // e.g. unpackaged test hosts) and Show can fail when notifications are disabled system-wide —
    // swallowed, logged once.
    void TerminalPage::_ShowAgentSessionToast(const std::wstring& sessionId, const std::wstring& title, const std::wstring& body, bool silent)
    {
        try
        {
            winrt::Windows::Data::Xml::Dom::XmlDocument doc;
            doc.LoadXml(silent ?
                            LR"(<toast><visual><binding template="ToastGeneric"><text></text><text></text></binding></visual><audio silent="true"/></toast>)" :
                            LR"(<toast><visual><binding template="ToastGeneric"><text></text><text></text></binding></visual></toast>)");
            const auto texts = doc.GetElementsByTagName(L"text");
            texts.Item(0).AppendChild(doc.CreateTextNode(winrt::hstring{ title }));
            texts.Item(1).AppendChild(doc.CreateTextNode(winrt::hstring{ body }));
            // The activation payload: which session to surface. Set as an ATTRIBUTE VALUE via the DOM
            // (like the text nodes) so a session id can never break the markup. It comes back verbatim
            // as Activate()'s invokedArgs — a session id is a plain GUID, so no encoding is needed.
            doc.DocumentElement().SetAttribute(L"launch", winrt::hstring{ sessionId });

            winrt::Windows::UI::Notifications::ToastNotification toast{ doc };
            toast.Tag(winrt::hstring{ ::Agentmaster::ShortId(sessionId) });
            toast.Group(L"agentmaster");
            if (!::Agentmaster::ToastActivator::IsRegistered())
            {
                // FALLBACK ONLY (see above): no COM activator, so the shell will activate the AUMID
                // (launching a second process -> the stray window we can't stop from here). At least
                // make the click do its job: the platform raises Activated on a non-UI thread ->
                // marshal to this window's dispatcher, then surface the session. Deliberately NOT
                // _ActivateClaudeSession — its local path skips the foreground (bringWindowToFront=false
                // is right for an in-window click, which is already foreground), but a toast click
                // arrives from the SHELL with this window possibly minimized / behind other apps, so the
                // local path must run the same restore-if-minimized + SetForegroundWindow +
                // SwitchToThisWindow recipe the cross-window receiver uses. If the tab MOVED to another
                // window since the toast was shown, the fan-out's receiving window foregrounds itself
                // the same way (_FocusClaudeSessionTab(id, /*bringWindowToFront*/ true) on both ends).
                const auto dispatcher = Dispatcher(); // agile — safe to call into from the callback thread
                const auto weakThis = get_weak();
                toast.Activated([weakThis, dispatcher, sessionId](const winrt::Windows::UI::Notifications::ToastNotification&, const winrt::Windows::Foundation::IInspectable&) {
                    dispatcher.RunAsync(CoreDispatcherPriority::Normal, [weakThis, sessionId]() {
                        if (auto self = weakThis.get())
                        {
                            // Nav audit: the user clicked a session's completion toast — the shell-side
                            // jump into the fleet (the toast twin of the board/tree "activate" lines).
                            ::Agentmaster::LogNav(L"notify-click " + ::Agentmaster::ShortId(sessionId) + L" (toast -> foreground window + jump to tab)");
                            if (!self->_FocusClaudeSessionTab(sessionId, /*bringWindowToFront*/ true))
                            {
                                ::Agentmaster::ActivateSessionInOtherWindows(sessionId, self->_windowId);
                            }
                        }
                    });
                });
            }
            winrt::Windows::UI::Notifications::ToastNotificationManager::CreateToastNotifier().Show(toast);
        }
        catch (...)
        {
            if (!_agentToastFailLogged)
            {
                _agentToastFailLogged = true; // once is signal, per-fire is noise (an unpackaged build throws on every Show)
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[notify] toast failed (no package identity / notifications unavailable) - further failures muted\n");
            }
        }
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

    // Agentmaster (Waiting-for-you + Error triage): the tab context-menu's status-adaptive triage move —
    // the tab-menu twin of the Manager board card's "Move to Idle/Done", plus its reverse. EXPLICITLY
    // separate from "Mark Unread": neither direction sets the sticky manualUnread flag or flashes the red
    // ring. The direction is re-derived HERE from the session's LIVE state (the menu label was fixed at
    // flyout-open), and each registry mutation re-checks the state under the lock, so a turn that advanced
    // since the menu opened simply no-ops rather than mis-moving.
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
        else if (info->state == SessionState::Error)
        {
            // Dismiss Error -> Idle/Done. Mirror the board card's mutator: Error is LEVEL-derived (the
            // scanner's recon-error re-asserts it off the unchanged error tail every pass), so besides
            // the state flip this records the errorDismissed ack — the recon-error gate then suppresses
            // the re-derivation until the conversation actually moves (a retry / a NEW error re-fires
            // normally) — and drops the preserved failure reason (the "Empty/0 outside Error"
            // invariant). Also stamp read + clear the unread mark/ring like the Waiting demote: "Move
            // to Idle/Done" is an explicit "I've handled this", acknowledging the attention without a
            // tab visit. The Update guard re-checks the live state, so a race to Running never demotes.
            _ClearSessionUnread(sessionId); // drop the manual mark (+ ring if not auto-flashing)
            _StopAgentFlash(sessionId); // drop any lingering automatic flash too
            _sessionRegistry->Update(sessionId, [](::Agentmaster::SessionInfo& s) {
                if (s.state == SessionState::Error)
                {
                    s.state = SessionState::Idle;
                    s.manualUnread = false;
                    s.readUnixMs = TtNowMs();
                    s.errorDismissed = true;
                    s.errorMessage.clear();
                    s.errorStatus = 0;
                }
            });
            ::Agentmaster::LogNav(L"triage-move " + ::Agentmaster::ShortId(sessionId) + L" -> idle/done (error dismissed)");
        }
        // else: not a triage state (Running / NeedsApproval) — nothing to move (the item is hidden).
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

    // Agentmaster (TAB_OVERLAY.md summary panel): switching TO a managed tab kicks a cheap, mtime-gated
    // content re-read of its summary panel, so the "here-and-now lens" is current the instant you look at
    // it instead of up to ~5 s stale (the panel's own DispatcherTimer backstop cadence). Called from the
    // one tab-switch funnel (_OnTabSelectionChanged), so a user click, Ctrl+Tab, a switchToTab action, or a
    // cross-window Activate all reach it. No-op for a non-session tab (no overlay), when the panel is off,
    // or when the transcript is unchanged (the mtime gate makes it a cheap stat — no re-render / scroll
    // reset). The overlay may not be attached yet during restore (bind attaches it), so the find can miss.
    void TerminalPage::_RefreshFocusedTabSummary(const TerminalApp::Tab& tab)
    {
        if (!tab)
        {
            return;
        }
        const auto id = _ClaudeSessionForTab(tab);
        if (id.empty())
        {
            return; // not a managed Claude/Codex tab
        }
        if (const auto it = _claudeOverlays.find(id); it != _claudeOverlays.end() && it->second)
        {
            it->second->RefreshSummaryContent();
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

        // The pinned lens selection (independent of hover), read live so a restored/seeded selection is
        // honored without caching. Read UNCONDITIONALLY — not just on the Manager tab: the reveal
        // tracking below must follow selection changes that happen while the Manager is HIDDEN (the
        // tab-switch funnel reselects the session you switch to), or merely RETURNING to the Manager
        // would read "selection changed" and surprise-scroll the strip.
        const std::wstring selection = _ManagerSelectedSessionId();

        std::wstring desired;
        if (onManager)
        {
            // Hover wins over the pinned lens selection (a live preview that follows the mouse).
            const std::wstring target = _managerHoverSessionId.empty() ? selection : _managerHoverSessionId;
            // Only pill a session whose tab lives in THIS window (a GLOBAL-scope card can name a
            // session hosted elsewhere — that window pills it, not this one).
            if (!target.empty() && _claudeTabs.find(target) != _claudeTabs.end())
            {
                desired = target;
            }
        }

        // Agentmaster (Linked Lenses — selection follows into view): when the SELECTED session changes
        // WHILE the Manager tab is up (a board card / tree row click), scroll the tab strip so its tab
        // is visible — otherwise the selection pill can sit scrolled off-screen (or hidden under the
        // `<`/`>` overlay buttons). Selection only — a hover preview must NOT scroll (it would jump the
        // strip as the pointer slides across cards) — and only a tab hosted in THIS window. Runs BEFORE
        // the pill's no-change early-return so clicking a card you were already hovering (pill
        // unchanged, selection changed) still scrolls. The tracking updates on EVERY pass (see the
        // selection read above), so only an on-Manager selection CHANGE reveals.
        const bool selectionChanged = selection != _selectionBroughtIntoView;
        _selectionBroughtIntoView = selection;
        if (onManager && selectionChanged && !selection.empty())
        {
            if (const auto it = _claudeTabs.find(selection); it != _claudeTabs.end())
            {
                if (const auto t = it->second.get())
                {
                    _RevealTabInStrip(t);
                }
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

    // Agentmaster (Linked Lenses — reveal the selected tab): scroll the tab strip so this tab's
    // TabViewItem is visible CLEAR of the `<`/`>` scroll RepeatButtons that OVERLAY the strip
    // viewport's edges (~32px each; the `+` new-tab button sits just past the `>` and visually
    // continues it). Two phases, because the strip VIRTUALIZES (ItemsStackPanel — see
    // _UpdateManagerNavButtons: a scrolled-off tab's container is DEREALIZED, its ActualWidth reads 0
    // and it isn't in the visual tree, so StartBringIntoView silently NO-OPS on it — the first
    // version's bug: revealing only worked for nearby, still-realized tabs). Phase 1: a realized item
    // gets one precise ChangeView on the strip's own ScrollViewer (_tabStripScrollViewer — the Home
    // button's instrument) landing it inside [pad, viewport-pad]. Phase 2: a derealized item is
    // realized through the virtualization-aware ListViewBase::ScrollIntoView (the TabListView is the
    // scroller's nearest ListView ancestor), then the precise pass reruns on bounded Low-priority
    // ticks once the container has laid out. Instant scroll (no animation), the Home button idiom —
    // deterministic under the same-tick board rebuild a card click also triggers.
    void TerminalPage::_RevealTabInStrip(const TerminalApp::Tab& tab)
    {
        if (!tab)
        {
            return;
        }
        try
        {
            const auto tvi = tab.TabViewItem();
            if (!tvi)
            {
                return;
            }
            _EnsureTabStripScrollViewer();
            if (_AdjustStripToRevealItem(tvi))
            {
                return; // realized — revealed in one precise pass
            }
            // Derealized (virtualized out). ListViewBase::ScrollIntoView realizes the container and
            // rough-scrolls it into the viewport (WT inserts the TabViewItems directly into TabItems,
            // so the item IS the TabViewItem). Fall back to StartBringIntoView when the template
            // isn't resolvable (it can still help a realized-but-unarranged item).
            ListView tabListView{ nullptr };
            auto node = _tabStripScrollViewer ? VisualTreeHelper::GetParent(_tabStripScrollViewer) : DependencyObject{ nullptr };
            while (node && !tabListView)
            {
                tabListView = node.try_as<ListView>();
                node = VisualTreeHelper::GetParent(node);
            }
            if (tabListView)
            {
                tabListView.ScrollIntoView(tvi);
            }
            else
            {
                tvi.StartBringIntoView();
            }
            _RevealTabRetryAdjust(tvi, 8); // fine-adjust clear of the overlay buttons once realized
        }
        CATCH_LOG();
    }

    // Agentmaster: the precise reveal pass — scroll the strip so `tvi` sits within
    // [pad .. viewport-pad], clear of the `<`/`>` RepeatButtons overlaying the viewport edges. Returns
    // false while the item has no realized layout (virtualized out / pre-arrange), so the caller can
    // realize it first and retry; returning true includes the already-clear no-op case.
    bool TerminalPage::_AdjustStripToRevealItem(const winrt::Microsoft::UI::Xaml::Controls::TabViewItem& tvi)
    {
        const auto& sv = _tabStripScrollViewer;
        if (!sv || !tvi || !tvi.IsLoaded())
        {
            return false;
        }
        const double w = tvi.ActualWidth();
        const double viewport = sv.ViewportWidth();
        if (w <= 0.0 || viewport <= 0.0)
        {
            return false; // derealized container (virtualization) or the strip hasn't laid out yet
        }
        const auto origin = tvi.TransformToVisual(sv).TransformPoint({ 0.0f, 0.0f });
        const double x = origin.X; // the item's left edge in viewport coordinates
        // Clearance past the ~32px overlay scroll buttons. When the strip is too narrow for the tab
        // plus both pads, split the leftover evenly instead of oscillating between the constraints.
        double padL = 40.0;
        double padR = 40.0;
        if (padL + w + padR > viewport)
        {
            padL = padR = std::max(0.0, (viewport - w) / 2.0);
        }
        const double cur = sv.HorizontalOffset();
        double target = cur;
        if (x < padL)
        {
            target = cur + (x - padL); // hidden under (or scrolled past) the `<` — scroll left
        }
        else if (x + w > viewport - padR)
        {
            target = cur + (x + w) - (viewport - padR); // hidden under the `>` (and the adjacent `+`) — scroll right
        }
        target = std::clamp(target, 0.0, std::max(0.0, sv.ScrollableWidth()));
        if (std::abs(target - cur) > 0.5)
        {
            sv.ChangeView(target, nullptr, nullptr, true); // instant — the Home button's ChangeView idiom
        }
        return true;
    }

    // Agentmaster: the deferred fine-adjust behind _RevealTabInStrip's phase 2. ScrollIntoView
    // realizes a virtualized container on a LATER layout pass, never this tick — so retry the precise
    // pass on Low priority (runs post-layout) a bounded number of times. If it never realizes (e.g.
    // the tab closed mid-flight), ScrollIntoView already landed it roughly in view; stop silently.
    void TerminalPage::_RevealTabRetryAdjust(winrt::Microsoft::UI::Xaml::Controls::TabViewItem tvi, int attempts)
    {
        if (!tvi || attempts <= 0)
        {
            return;
        }
        Dispatcher().RunAsync(CoreDispatcherPriority::Low, [weakThis = get_weak(), tvi, attempts]() {
            if (auto page = weakThis.get())
            {
                try
                {
                    if (!page->_AdjustStripToRevealItem(tvi))
                    {
                        page->_RevealTabRetryAdjust(tvi, attempts - 1);
                    }
                }
                CATCH_LOG();
            }
        });
    }

    // Agentmaster (consistent multi-line tab-row height): the tab strip's ListView stretches every tab
    // to the tallest REALIZED tab, so a tab whose title carries embedded newlines (a multi-line rename)
    // grew the WHOLE row only WHILE it was on-screen — scrolling it out (the ListView virtualizes it
    // away) snapped the row shorter, and scrolling back grew it again. Fix: compute the MAX title
    // line-count across ALL tabs (visible OR scrolled-off) and reserve that height on EVERY header's
    // invisible line-shim (TabHeaderControl::ReserveTitleLines), so the row measures that tall no matter
    // which tabs are realized. With no multi-line title the max is 1 and every shim collapses -> the slim
    // single-line strip, exactly as before. Cheap + idempotent (each header no-ops on an unchanged count);
    // called on any tab title change (_UpdateTitle) and on tab add/remove (_OnTabItemsChanged). UI thread.
    void TerminalPage::_UpdateReservedTabTitleLines()
    {
        int32_t maxLines = 1;
        for (const auto& tab : _tabs)
        {
            if (!tab)
            {
                continue;
            }
            // Count RENDERED line breaks. A WinUI TextBox — the rename box, where multi-line titles
            // are born — separates lines with a bare CR ('\r'), NOT LF: counting only '\n' saw every
            // renamed multi-line title as ONE line, so the reserve never engaged and the row still
            // snapped with virtualization (the reported bug). TextBlock breaks on CR, LF, and CRLF
            // alike, so count all three (CRLF as one break).
            int32_t lines = 1;
            const auto title = tab.Title();
            const wchar_t* s = title.c_str();
            for (size_t i = 0; s[i] != L'\0'; ++i)
            {
                if (s[i] == L'\r')
                {
                    ++lines;
                    if (s[i + 1] == L'\n')
                    {
                        ++i; // CRLF is one break
                    }
                }
                else if (s[i] == L'\n')
                {
                    ++lines;
                }
            }
            if (lines > maxLines)
            {
                maxLines = lines;
            }
        }
        for (const auto& tab : _tabs)
        {
            if (!tab)
            {
                continue;
            }
            const auto tvi = tab.TabViewItem();
            if (!tvi)
            {
                continue;
            }
            if (const auto header = tvi.Header().try_as<winrt::TerminalApp::TabHeaderControl>())
            {
                if (const auto impl = winrt::get_self<implementation::TabHeaderControl>(header))
                {
                    impl->ReserveTitleLines(maxLines); // self-guarded: only a real change touches the tree
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
        // Tab-color MODE (inferred working dir): a GLOBAL setting (AppSettings::tabColorMode) — seed
        // this overlay so its subline / Open Path / Copy Path resolve the session's EFFECTIVE work dir
        // (the inferred dir under the Inferred mode); a cog Save re-applies it live via
        // _ReapplyManagedTabColors (the same broadcast that repaints the tabs).
        overlay->SetTabColorMode(static_cast<int>(_appSettings.tabColorMode));
        // Model families (current-model adornment): a GLOBAL setting (AppSettings::modelFamilies) —
        // seed this overlay's row-1 model shortener; a cog Save re-applies it live via the same
        // _ReapplyManagedTabColors broadcast that pushes the tab-color mode.
        overlay->SetModelFamilies(_appSettings.modelFamilies);
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

    // Agentmaster (eager-init / "Activate All Tabs" pacing): the drip cadence. Waking a dormant tab
    // spawns a claude.exe + a swapchain on the UI thread — a burst of N at once freezes the app (and
    // enough of them, the PC) — so wakes are spaced kActivateAllSpacingMs apart, at most
    // kActivateAllBatchSize per kActivateAllBatchPeriodMs window: t=0/0.5/1.0/1.5s, then the next four
    // at t=10/10.5/11/11.5s, ... (the post-batch gap = period − (size−1)·spacing = 8.5s).
    static constexpr int kActivateAllSpacingMs = 500;
    static constexpr int kActivateAllBatchSize = 4;
    static constexpr int kActivateAllBatchPeriodMs = 10000;

    // Agentmaster (eager-init / "Activate All Tabs"): DRIP-FEED-wake every dormant managed tab hosted in
    // THIS window. The local actuator behind the Manager's "Activate All Tabs (N)" button AND the
    // receiving half of the cross-window fan-out (ActivateAllDormantInOtherWindows). NOT a burst: the
    // hosted ids are queued and _ActivateAllDripStep wakes them one at a time on _activateAllTimer's
    // cadence (the k-constants above). Invoked while a drip is already pacing it just MERGES (dedupes)
    // the current ids into the queue — the tail rides the same paced stream, so a re-click / a
    // cross-window echo can never burst. Dormancy is (re)checked at WAKE time, not enqueue time: an id
    // that started/closed meanwhile is skipped for free (never consumes a pacing slot).
    void TerminalPage::_ActivateAllDormantTabsLocal()
    {
        // Merge this window's hosted ids into the drip queue (dedupe against what's still queued).
        for (const auto& [id, weak] : _claudeTabs)
        {
            if (std::find(_activateAllQueue.begin(), _activateAllQueue.end(), id) == _activateAllQueue.end())
            {
                _activateAllQueue.push_back(id);
            }
        }
        if (_activateAllQueue.empty())
        {
            return;
        }
        if (_activateAllTimer && _activateAllTimer.IsEnabled())
        {
            return; // a drip is already pacing — the merged ids extended it, nothing to start
        }
        _activateAllBatchWoken = 0;
        _activateAllWokenTotal = 0;
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[activate-all] window " + _windowId + L" drip-start candidates=" + std::to_wstring(_activateAllQueue.size()) + L" (" + std::to_wstring(kActivateAllSpacingMs) + L"ms apart, " + std::to_wstring(kActivateAllBatchSize) + L" per " + std::to_wstring(kActivateAllBatchPeriodMs / 1000) + L"s)\n");
        _SetActivateAllBusy(true); // the Manager shows "Activating N tabs…" (disabled button + disabled cwd box) until the queue drains
        _ActivateAllDripStep(); // wake the first immediately (the click should feel responsive); the timer paces the rest
    }

    // Agentmaster (eager-init / "Activate All Tabs" pacing): flip THIS window's Manager content into / out
    // of its "busy" state (disabled "Activating N tabs…" button + disabled launch/cwd box) so a paced
    // wake that runs for many seconds is visibly in progress. The Manager tab is non-closable, so the
    // content outlives the drip; a null get() (mid-teardown) simply no-ops. UI thread only.
    void TerminalPage::_SetActivateAllBusy(bool busy)
    {
        if (const auto ipc = _agentManagerContent.get())
        {
            if (auto* const mgr = winrt::get_self<implementation::AgentManagerContent>(ipc))
            {
                mgr->SetActivateAllBusy(busy);
            }
        }
    }

    // Agentmaster (eager-init / "Activate All Tabs" pacing): one drip step — pop queued ids until one
    // actually WAKES (only a real wake, i.e. a claude.exe spawn, consumes a pacing slot), then re-arm
    // the timer for the next step: 500ms within a batch, or the long post-batch gap after the 4th wake
    // so consecutive batches start a full 10s apart. Self-stops when the queue drains (and logs the
    // run's total, matching the old burst log). UI thread only.
    void TerminalPage::_ActivateAllDripStep()
    {
        while (!_activateAllQueue.empty())
        {
            const std::wstring id = std::move(_activateAllQueue.front());
            _activateAllQueue.erase(_activateAllQueue.begin());
            if (_ActivateDormantSession(id))
            {
                ++_activateAllWokenTotal;
                ++_activateAllBatchWoken;
                break;
            }
        }
        if (_activateAllQueue.empty())
        {
            if (_activateAllTimer)
            {
                _activateAllTimer.Stop();
            }
            if (_activateAllWokenTotal > 0)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[activate-all] window " + _windowId + L" woke " + std::to_wstring(_activateAllWokenTotal) + L" dormant tab(s) (paced)\n");
                _activateAllWokenTotal = 0;
            }
            _SetActivateAllBusy(false); // drip done — restore the button's normal label + re-enable the cwd box
            return;
        }
        int delayMs = kActivateAllSpacingMs;
        if (_activateAllBatchWoken >= kActivateAllBatchSize)
        {
            delayMs = kActivateAllBatchPeriodMs - (kActivateAllBatchSize - 1) * kActivateAllSpacingMs; // the next batch starts one full period after this one began
            _activateAllBatchWoken = 0;
        }
        if (!_activateAllTimer)
        {
            _activateAllTimer = WUX::DispatcherTimer{};
            _activateAllTimer.Tick([weak = get_weak()](const IInspectable& sender, const IInspectable&) {
                if (auto self = weak.get())
                {
                    self->_ActivateAllDripStep();
                }
                else if (const auto t = sender.try_as<WUX::DispatcherTimer>())
                {
                    t.Stop(); // page destroyed — stop ticking (UI thread, safe)
                }
            });
        }
        _activateAllTimer.Stop(); // a DispatcherTimer is periodic — Stop→Interval→Start makes each gap exact and one-shot-like
        _activateAllTimer.Interval(std::chrono::milliseconds(delayMs));
        _activateAllTimer.Start();
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

    // Agentmaster (ctrl+home "home / jump-back" toggle): do EXACTLY what the tab-strip nav buttons do —
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
            // Sig PRE-check (the CPU fix): SetAgentToolTip's own guard discards an identical push, but the
            // card argument was still BUILT first — a per-probe-tick XAML construction per observed tab,
            // thrown away every time. Compare the hosted fingerprint before building anything.
            if (std::wstring_view{ impl->AgentToolTipSig() } != sig)
            {
                impl->SetAgentToolTip(TtBuildObserveCard(kind, std::wstring{ impl->Title() }), winrt::hstring{ sig });
            }
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
        _ApplySessionTabColor(hostTab, id, cwd); // mode-aware tab color (per-dir / individual / inferred)
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
        _RefreshTabTags(id); // bookmark tags: show the session's bookmark badges on bind/adopt/re-home
        _UpdateTabAgentToolTip(hostTab, id, /* swapWhileOpen */ false, /* kickSummary */ false); // tab tooltip: replace any "○ … unlinked" observe tooltip with the rich managed HEADER card (the transcript analyze waits for a real hover — a restore binding many tabs must not burst parses)
        _ArmTabAgentToolTipHover(hostTab); // LAZY tooltip: from here on the card rebuilds on hover, not per tick/notify
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
        // Agentmaster (terminate-net): a fire_and_forget must never let an exception escape — that
        // std::terminates the app (the _RefreshPromptNavCache / _ScrollAdjacentPrompt idiom). This UI-lane
        // sweep touches cross-ABI control state (ConnectionState()) + the registry; the body runs as an
        // awaitable IAsyncAction (_SweepClaudeLivenessImpl) whose exceptions PROPAGATE to this co_await
        // (unlike a fire_and_forget's, which terminate), so the catch contains them.
        auto strongThis{ get_strong() };
        try
        {
            co_await _SweepClaudeLivenessImpl();
        }
        catch (...)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[observer] _SweepClaudeLiveness: swallowed exception (no crash)\n");
        }
    }

    winrt::Windows::Foundation::IAsyncAction TerminalPage::_SweepClaudeLivenessImpl()
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
            // LAZY tab tooltip (the CPU fix): the sweep no longer BUILDS anything — it only self-heals the
            // hover ARM for any tab that entered _claudeTabs without passing the bind funnel (a
            // window-restored dormant tab, a re-home). Already-armed = one bool probe per tab per tick.
            // The card itself (and its "ago" line) is built fresh at hover time, which is strictly more
            // current than the old per-sweep rebuild ever was — and costs zero while nobody hovers.
            _ArmTabAgentToolTipHover(tab);
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
        // Agentmaster (terminate-net): the inner per-control ReadPendingInputDraft is already try/caught,
        // but the surrounding UI-thread work (_SetTabPending / _PendingDotsColorForTab / registry) is not —
        // an exception escaping this fire_and_forget would std::terminate. The body is an awaitable
        // IAsyncAction (_ScanPendingInputImpl) whose exceptions propagate to this co_await (see
        // _SweepClaudeLiveness).
        auto strongThis{ get_strong() };
        try
        {
            co_await _ScanPendingInputImpl();
        }
        catch (...)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[observer] _ScanPendingInput: swallowed exception (no crash)\n");
        }
    }

    winrt::Windows::Foundation::IAsyncAction TerminalPage::_ScanPendingInputImpl()
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
                    dotsColor = _PendingDotsColorForTab(hostTab, *info);
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

    // Agentmaster (tab color modes — InferredWorkingDirectory): the inferred-workdir scan.
    // Periodically re-infer each ADMITTED hosted Claude session's ACTUAL working directory from the
    // paths its tool calls touch (files read / edited / created + searched dirs) and re-key its tab
    // color when the inference changes — so a session launched at a repo root that settles into one
    // subtree wears THAT subtree's color, and two sessions sharing a cwd but working in different
    // areas become tellable apart. ADMISSION is the shared SessionInfersWorkingDir predicate: while
    // the GLOBAL tabColorMode is InferredWorkingDirectory every session infers; in EVERY OTHER mode
    // only the sessions LAUNCHED in the user's home dir do (%USERPROFILE% — the default launch dir,
    // a meaningless cwd, so their inference is FORCED on; the rest of the fleet idles exactly as
    // before). The producer and the consumers (EffectiveWorkingDir) gate on the SAME predicate, so
    // an admitted session's inference is always fresh and a non-admitted one's stays dormant.
    //
    // Cost discipline: scanner-ticked (~2s) but each session is throttled to one inference per
    // kInferredScanThrottleMs AND gated on the transcript mtime actually GROWING; the paths come
    // from the Sessions page's per-session SIDECAR index (LoadOrRefreshSessionIndex — (size,mtime)-
    // keyed, resumed incrementally from the stored byte offset, so a steady tick reads only the
    // appended suffix; keeping the sidecar warm also benefits the Sessions browser). File IO runs
    // off-thread; registry/tab work returns to the UI thread. Codex is skipped (its rollout isn't
    // path-parsed — the cwd keys its color, SessionColorKeyDir's fallback). The inference persists
    // on the record (SessionInfo::inferredWorkingDir, sessions.json) so a reopened session wears
    // its inferred color immediately; an inference that lands ON the cwd is stored EMPTY (pure
    // fallback — the common just-launched case stays byte-unchanged on disk).
    winrt::fire_and_forget TerminalPage::_ScanInferredTabColors()
    {
        // Agentmaster (terminate-net): contain any exception so this scanner-ticked lane can never
        // std::terminate the app (the _SweepClaudeLiveness idiom).
        auto strongThis{ get_strong() };
        try
        {
            co_await _ScanInferredTabColorsImpl();
        }
        catch (...)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[observer] _ScanInferredTabColors: swallowed exception (no crash)\n");
        }
    }

    winrt::Windows::Foundation::IAsyncAction TerminalPage::_ScanInferredTabColorsImpl()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry || _claudeTabs.empty())
        {
            co_return;
        }
        // The mode no longer gates the WHOLE scan (home-dir launches infer in every mode — the
        // SessionInfersWorkingDir forcing); each session is admitted per-candidate below, so in a
        // non-Inferred mode with no home-dir session the pass costs a µs map walk and the state
        // map idles empty exactly as before.
        const auto colorMode = _appSettings.tabColorMode;
        constexpr int64_t kInferredScanThrottleMs = 15000; // per-session floor between inferences (~15s latency is plenty for a color)
        const int64_t now = static_cast<int64_t>(::GetTickCount64());

        // Prune scan state whose session left this window (closed / re-homed / moved out).
        for (auto it = _inferredColorScan.begin(); it != _inferredColorScan.end();)
        {
            it = (_claudeTabs.find(it->first) == _claudeTabs.end()) ? _inferredColorScan.erase(it) : std::next(it);
        }

        // Collect the DUE candidates on the UI thread (registry + maps), then hop off for the IO.
        struct InferCand
        {
            std::wstring id;
            std::wstring scanSid; // whose transcript/SIDECAR backs the accumulate: the session's own id, or its fork SOURCE (below)
            std::wstring workingDir;
            std::wstring path;
            int64_t lastMtimeMs{};
        };
        std::vector<InferCand> due;
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            if (!weakTab.get())
            {
                continue;
            }
            const auto info = _sessionRegistry->Get(id);
            if (!info || !info->live || info->kind != ::Agentmaster::AgentKind::Claude)
            {
                continue; // Codex rollouts aren't path-parsed — its color keys on the cwd
            }
            if (!::Agentmaster::SessionInfersWorkingDir(colorMode, *info))
            {
                continue; // not admitted under this mode (only home-dir launches infer outside the Inferred mode) — BEFORE the state map, so non-inferring sessions never accrete entries
            }
            auto& st = _inferredColorScan[id];
            if (st.nextRunMs > now)
            {
                continue; // throttled
            }
            std::wstring scanPath = st.transcriptPath;
            std::wstring scanSid = id;
            if (scanPath.empty())
            {
                scanPath = ::Agentmaster::ResolveClaudeTranscriptPath(id); // globbed while missing, cached once found
                if (!scanPath.empty())
                {
                    st.transcriptPath = scanPath; // the session's own transcript exists now (its first turn happened)
                }
                else if (!info->forkParentId.empty())
                {
                    // Never-messaged FORK: its own transcript doesn't exist until its first turn, but
                    // its content-to-be IS the source's (a fork copies the parent verbatim at fork
                    // point) — so infer from the SOURCE's transcript, against the SOURCE's sidecar
                    // (scanSid: parent stats must never land under the fork's sid — a byte cursor
                    // carried over from the parent's file could silently corrupt the fork's own index
                    // later). Deliberately NOT cached in st.transcriptPath: every pass re-checks for
                    // the fork's OWN transcript first and switches over the moment it exists (the
                    // registry clears forkParentId on the fork's first own-id hook at ~the same time).
                    // This covers the fork whose SOURCE the registry doesn't know — the
                    // adopt-external fork — where the launch-time inheritance can't help but the
                    // transcript glob can; it also backstops a managed fork created before its source
                    // ever had an inference to inherit.
                    scanPath = ::Agentmaster::ResolveClaudeTranscriptPath(info->forkParentId);
                    scanSid = info->forkParentId;
                }
                if (scanPath.empty())
                {
                    st.nextRunMs = now + kInferredScanThrottleMs; // nothing to infer from yet — retry later
                    continue;
                }
            }
            due.push_back({ id, scanSid, info->workingDir, scanPath, st.lastMtimeMs });
        }
        if (due.empty())
        {
            co_return;
        }
        const bool useGit = _appSettings.inferGitRoot; // "Use .git folder to infer" — snapshot on the UI thread

        co_await winrt::resume_background();
        // The machine's temp roots — paths under them NEVER vote (ExcludePathsUnderRoots below): a
        // Claude session scratches under %TEMP% constantly (heredoc scripts, outputs, its per-session
        // scratchpad dir), and a proven live case had a session whose ONLY captured path was one
        // scratchpad pr-body.md — a 1/1 "majority" that inferred the SCRATCHPAD dir and recolored the
        // tab off its repo (no git root above temp, so the git snap couldn't catch it). Collected once
        // per pass; the sidecar keeps the full set (the Sessions 📁/📄 scopes still match temp paths).
        const std::vector<std::wstring> tempRoots = ::Agentmaster::CollectMachineTempRoots();
        // The git-root resolver for the inference's git arm ("Use .git folder to infer"): the real
        // FindGitRootForDir walk, memoized per NormDirKey ACROSS this whole batch — sessions in one
        // pass overwhelmingly share ancestor chains, so the per-pass filesystem cost collapses to a
        // handful of GetFileAttributesW probes (and the pass itself is mtime-gated + ~15s-throttled).
        std::unordered_map<std::wstring, std::wstring> gitMemo; // NormDirKey(dir) -> git root ("" == not in a repo)
        std::function<std::wstring(const std::wstring&)> gitRootOf;
        if (useGit)
        {
            gitRootOf = [&gitMemo](const std::wstring& d) -> std::wstring {
                const std::wstring key = ::Agentmaster::NormDirKey(d);
                if (const auto it = gitMemo.find(key); it != gitMemo.end())
                {
                    return it->second;
                }
                std::wstring root = ::Agentmaster::FindGitRootForDir(d);
                gitMemo.emplace(key, root);
                return root;
            };
        }
        struct InferResult
        {
            std::wstring id;
            std::wstring inferred; // the raw inference (== workingDir for the fallback case)
            std::wstring workingDir;
            int64_t mtimeMs{};
            bool ran{}; // false == the transcript was quiet (mtime unchanged) — just refresh the throttle
        };
        std::vector<InferResult> results;
        results.reserve(due.size());
        for (const auto& c : due)
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!::GetFileAttributesExW(c.path.c_str(), GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                continue; // vanished mid-scan — the liveness/re-home lanes own that story
            }
            // FILETIME -> Unix ms, matching TranscriptStore's own conversion so the TranscriptRef's
            // (size, mtime) key agrees byte-for-byte with the sidecar the Sessions page writes.
            const auto toUnixMs = [](const FILETIME& ft) -> int64_t {
                ULARGE_INTEGER u{};
                u.HighPart = ft.dwHighDateTime;
                u.LowPart = ft.dwLowDateTime;
                if (u.QuadPart < 116444736000000000ULL)
                {
                    return 0;
                }
                return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
            };
            const int64_t mtimeMs = toUnixMs(fad.ftLastWriteTime);
            if (mtimeMs == c.lastMtimeMs)
            {
                results.push_back({ c.id, std::wstring{}, c.workingDir, mtimeMs, false });
                continue; // quiet transcript — nothing new to infer from
            }
            ::Agentmaster::TranscriptRef ref;
            ref.sessionId = c.scanSid; // the OWNER of the scanned transcript — a fork borrowing its source's file refreshes the SOURCE's sidecar, never its own
            ref.path = c.path;
            ref.sizeBytes = (static_cast<int64_t>(fad.nFileSizeHigh) << 32) | static_cast<int64_t>(fad.nFileSizeLow);
            ref.mtimeMs = mtimeMs;
            ref.birthMs = toUnixMs(fad.ftCreationTime);
            const auto entry = ::Agentmaster::LoadOrRefreshSessionIndex(ref); // sidecar-cached; reads only the appended suffix
            std::wstring inferred = c.workingDir;
            if (entry.valid)
            {
                auto votePaths = entry.stats.pathsAccessed; // copy — the sidecar keeps the FULL set for the search scopes
                ::Agentmaster::ExcludePathsUnderRoots(votePaths, tempRoots); // temp scratch never votes (may empty the corpus -> honest cwd fallback)
                inferred = ::Agentmaster::InferWorkingDirectory(votePaths, c.workingDir, gitRootOf);
            }
            results.push_back({ c.id, inferred, c.workingDir, mtimeMs, true });
        }

        co_await wil::resume_foreground(Dispatcher());
        const int64_t after = static_cast<int64_t>(::GetTickCount64());
        for (const auto& r : results)
        {
            auto& st = _inferredColorScan[r.id];
            st.lastMtimeMs = r.mtimeMs;
            st.nextRunMs = after + kInferredScanThrottleMs;
            if (!r.ran)
            {
                continue;
            }
            const auto info = _sessionRegistry->Get(r.id);
            if (!info || !info->live)
            {
                continue; // archived/re-homed while we were off-thread
            }
            // An inference that lands ON the cwd is stored EMPTY: SessionColorKeyDir then falls back
            // to workingDir naturally, and the common case never dirties sessions.json.
            const std::wstring store = (::Agentmaster::NormDirKey(r.inferred) == ::Agentmaster::NormDirKey(r.workingDir)) ? std::wstring{} : r.inferred;
            if (info->inferredWorkingDir == store)
            {
                continue; // settled — no churn (the usual steady-state outcome)
            }
            _sessionRegistry->Update(r.id, [&store](::Agentmaster::SessionInfo& s) { s.inferredWorkingDir = store; });
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[infer-dir] " + ::Agentmaster::ShortId(r.id) + L" -> " + (store.empty() ? (L"(cwd) " + r.workingDir) : store) + L"\n");
            TerminalApp::Tab tab{ nullptr };
            if (const auto it = _claudeTabs.find(r.id); it != _claudeTabs.end())
            {
                tab = it->second.get();
            }
            if (tab)
            {
                _ApplySessionTabColor(tab, r.id, r.workingDir); // re-key the paint onto the new inferred dir
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
        // Agentmaster (terminate-net): contain any exception so this scanner-ticked bind/re-home lane can
        // never std::terminate the app. The body is an awaitable IAsyncAction (_ReconcileClaudeTabsImpl)
        // whose exceptions propagate to this co_await (see _SweepClaudeLiveness).
        auto strongThis{ get_strong() };
        try
        {
            co_await _ReconcileClaudeTabsImpl();
        }
        catch (...)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[observer] _ReconcileClaudeTabs: swallowed exception (no crash)\n");
        }
    }

    winrt::Windows::Foundation::IAsyncAction TerminalPage::_ReconcileClaudeTabsImpl()
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
        // Agentmaster (terminate-net): the Fleet Observer UI lane touches many cross-ABI control reads
        // (SessionId() / RootProcessHandle()) + XAML bind work every ~2s tick; an exception escaping this
        // fire_and_forget would std::terminate the app. The body is an awaitable IAsyncAction
        // (_ObserverProbeImpl) whose exceptions propagate to this co_await (see _SweepClaudeLiveness).
        auto strongThis{ get_strong() };
        try
        {
            co_await _ObserverProbeImpl();
        }
        catch (...)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[observer] _ObserverProbe: swallowed exception (no crash)\n");
        }
    }

    winrt::Windows::Foundation::IAsyncAction TerminalPage::_ObserverProbeImpl()
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
        // Agentmaster (terminate-net): the Explorer-tree refresh button's lane — contain any exception so
        // it can never std::terminate the app. The body is an awaitable IAsyncAction
        // (_RefreshObserverDataImpl) whose exceptions propagate to this co_await (see _SweepClaudeLiveness).
        auto strongThis{ get_strong() };
        try
        {
            co_await _RefreshObserverDataImpl();
        }
        catch (...)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[observer] _RefreshObserverData: swallowed exception (no crash)\n");
        }
    }

    winrt::Windows::Foundation::IAsyncAction TerminalPage::_RefreshObserverDataImpl()
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
