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

#include "AgentCatchLog.h" // AgentLogCaughtException — full-detail swallowed-exception forensics (type/hr/msg + throw stacks)
#include "AgentManagerContent.h" // push the External census / RefreshNow
#include "AgentStatusColors.h" // AgentStatusColorFor — the shared state->color palette (tab dot); TagColorFor (bookmark tags)
#include "AgentTipHelpers.h" // AgentSetTip — the islands-safe hover tooltips on the Tag panel's rows
#include "AgentTabOverlay.h" // build + own the per-tab overlays (complete com_ptr type)
#include "Tab.h" // get_self<Tab> -> CurrentEffectiveTabBackground (pending-dots contrast)
#include "TabHeaderControl.h" // get_self<TabHeaderControl> -> ReserveTitleLines (consistent multi-line tab-row height)
#include "AgentMaster/ClaudeSpawn.h" // AppendStateLog; ResolvePendingPasteRefs (the paste-cache adapter)
#include "AgentMaster/Engine.h" // ActivateSessionInOtherWindows — a toast click's cross-window jump (System notifications)
#include "AgentMaster/PendingInput.h" // PickCurrentPromptText + the DRAFT SWAP ladder (DecideDraftClear / BuildInputKill / BuildInputYank / BuildBackspaces) — PENDING_INPUT.md §9
#include "AgentMaster/PendingPaste.h" // FindPasteMarkers — the cheap pure pre-check before the off-thread resolve (PENDING_INPUT.md §2b)
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

    // The tab-tooltip card's SIZE budget.
    //
    // Width is a flat cap: the card is anchored under a tab and centered on it, so every extra pixel of
    // width spends half to each side. 660 = the historical 460 + 200 (100 per side) -- enough that a
    // real session title and a numbered prompt line stop wrapping so hard, without the card sprawling
    // across the strip.
    //
    // Height is a WINDOW FRACTION, because height is the dimension that actually stretches: the card
    // grows downward with the Summary body (a chatty session has dozens of numbered prompts + files),
    // and the old flat 360px body clip topped the whole card out near half the screen. The caller
    // measures the XAML island root -- the window client area, which IS the screen when maximized, the
    // normal case -- and passes kTtCardHeightFraction of it; the tooltip renders in the island's popup
    // root, so the window is the true bound regardless. A non-positive budget (root not laid out yet)
    // falls back to the historical fixed size, so an unmeasured build is never worse than before.
    constexpr double kTtCardMaxWidth = 660.0; // 460 + 200 (100 per side)
    constexpr double kTtCardHeightFraction = 0.80; // "up to 80% of the screen"
    constexpr double kTtCardFallbackHeight = 460.0; // ~= the pre-budget card: the 360px body clip + its chrome
    // Chrome the body does NOT get: header (a title wraps to <=3 lines) + folder/branch + state + meta +
    // dir-detail + up to a couple of tag rows + the divider + the card padding + the "+N more" line.
    // An estimate -- nothing is laid out at build time -- so it is deliberately generous; overshooting
    // only costs the body a line or two, undershooting would let the card exceed the fraction.
    constexpr double kTtChromeReserve = 170.0;
    constexpr double kTtMinBodyHeight = 120.0; // a tiny window still shows a few body lines

    // ---- WHEEL SCROLL: the body is one screenful of a much taller document (Agentmaster) ----------
    // A long conversation's Summary body is taller than the card's height budget, and everything past
    // the cap used to be simply unreachable (the "+K more" marker was the whole story). The body is now
    // a scrollable VIEWPORT driven by the mouse wheel alone -- no ScrollViewer, no drag, no keyboard:
    // see TtBuildTooltipCard's viewport block for how (and HANDOVER_tab-tooltip.md invariant 6a for
    // why a ScrollViewer is permanently off the table here).
    constexpr double kTtScrollStepPx = 48.0; // per wheel notch (120 units) ~= the platform's 3 lines
    constexpr double kTtScrollBarWidth = 3.0; // the slim, collapsed-ScrollBar look
    constexpr double kTtScrollBarGutterPx = 5.0; // parked THIS far into the card's right padding, so it never covers text
    constexpr double kTtScrollThumbMinPx = 24.0; // a very deep body still leaves a visible thumb
    // The bar exists ONLY while you are scrolling: hidden at rest (a card that merely CAN scroll shows
    // nothing), lit on the first notch, gone again a moment after the last one. It is feedback for the
    // gesture, not chrome — an overlay bar sitting permanently on a hover card is just noise.
    constexpr double kTtScrollBarRestOpacity = 0.0;
    constexpr double kTtScrollBarActiveOpacity = 1.0;
    constexpr int kTtScrollBarHoldMs = 1800; // ... then, after the last notch, it fades out entirely
    constexpr int kTtScrollBarFadeStepMs = 40;
    constexpr double kTtScrollBarFadeStep = 0.15;
    // With the body SCROLLABLE the line cap changes meaning: it is no longer "one screenful" (the clip
    // is that) but how much of the summary we are willing to host in the card AT ALL -- the element /
    // text-measure backstop. TtBuildSummaryBody coalesces each contiguous run into ONE wrapping
    // TextBlock, so depth costs measure, not element count; this many screenfuls is the reach, and
    // anything past it still reports itself in the "+K more" marker (the summary panel has the rest).
    constexpr size_t kTtScrollBodyScreenfuls = 8;
    constexpr size_t kTtBodyMaxLinesFloor = 192;
    constexpr size_t kTtBodyMaxLinesCeil = 1000;

    // The scrollable pieces of a freshly built card, handed back to the page (which keeps WEAK refs to
    // them, so a wheel notch over the tab header can shift the body -- TerminalPage::_ScrollTabAgentToolTip).
    struct TtCardScrollParts
    {
        winrt::Windows::UI::Xaml::FrameworkElement viewport{ nullptr }; // the clipping host (one screenful)
        winrt::Windows::UI::Xaml::FrameworkElement content{ nullptr }; // the FULL body (render-shifted by contentShift)
        winrt::Windows::UI::Xaml::FrameworkElement thumb{ nullptr }; // the slim auto-hiding scrollbar
        winrt::Windows::UI::Xaml::Media::TranslateTransform contentShift{ nullptr };
        winrt::Windows::UI::Xaml::Media::ScaleTransform thumbScale{ nullptr }; // thumb LENGTH (viewport/content share)
        winrt::Windows::UI::Xaml::Media::TranslateTransform thumbShift{ nullptr }; // thumb POSITION (scrolled fraction)
    };

    // Build the whole tab-tooltip card: a dark, rounded Border (summary-panel chrome) holding the header
    // (state dot + a wrapping title, which owns the full card width), then one dim line each for the
    // folder/branch and the state (colored to match the tab dot) and the kind/model/effort/perm, and --
    // once the Summary body has loaded -- a divider + the numbered Summary box inside a WHEEL-SCROLLABLE
    // viewport (a clipping Grid + a render-transformed body + a slim auto-hiding bar; `scrollOut` hands
    // the page the pieces it drives). maxCardHeight is the caller's window-derived budget for the WHOLE
    // card (see the constants above); <= 0 means "not measured" -> the historical fixed size.
    //
    // NO ScrollViewer -- this is a hard rule, learned from a proven fail-fast (2026-07-02, full-dump stowed
    // backtrace): when the ToolTip popup opens, its content tree ENTERs the live tree, and a ScrollViewer's
    // enter walk activates DirectManipulation (ScrollViewer::OnManipulatabilityAffectingPropertyChanged ->
    // CInputServices::UpdateDirectManipulationManagerActivation -> CDirectManipulationService::
    // ActivateDirectManipulationManager), which under XAML Islands can fail E_INVALIDARG -> a stowed
    // exception -> the 0xC000027B fail-fast that repeatedly crashed the app (see
    // doc/agentmaster/HANDOVER_tab-tooltip.md crash #7 + invariant 6a). That rule STILL STANDS, and the
    // scrolling below deliberately does not bend it: the card stays inert (panels, TextBlocks, Borders,
    // transforms -- nothing manipulation-capable) and stays IsHitTestVisible(false). The wheel is read on
    // the TAB HEADER instead -- a real pointer target that is under the cursor the whole time the tip is
    // up -- and the page merely shifts a RenderTransform (Tab::EnsureAgentToolTipHoverHook ->
    // TerminalPage::_ScrollTabAgentToolTip). Wheel-only is also the whole interaction budget: no drag, no
    // keyboard, no manipulation.
    winrt::Windows::UI::Xaml::Controls::Border TtBuildTooltipCard(winrt::Windows::UI::Color accent,
                                                                  const std::wstring& title,
                                                                  const std::wstring& folderBranch,
                                                                  const std::wstring& stateText,
                                                                  const std::wstring& metaText,
                                                                  const std::wstring& dirDetailLine,
                                                                  const std::vector<std::pair<std::wstring, winrt::Windows::UI::Color>>& tagChips,
                                                                  double tagsOpacity,
                                                                  const winrt::hstring& bodyText,
                                                                  double maxCardHeight,
                                                                  TtCardScrollParts& scrollOut)
    {
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;

        const double cardHeight = maxCardHeight > 0.0 ? maxCardHeight : kTtCardFallbackHeight;
        const double bodyMaxHeight = std::max(kTtMinBodyHeight, cardHeight - kTtChromeReserve);

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
            // ~mono-11 chars that fit the card minus its padding. Derived from kTtCardMaxWidth so it can't
            // drift stale beside it (it was a hand-tuned 52 back when the card was a hardcoded 460 wide --
            // the ratio, kept here, already allows for the 10px inter-chip gaps).
            const size_t kTagLineBudget = static_cast<size_t>(52.0 * kTtCardMaxWidth / 460.0);
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

            // Truncate the body to a sane line count: a BACKSTOP that bounds the XAML element count, not
            // the visual cut (the clipping Grid below is that). Derived from the height budget with a
            // deliberately UNDER-estimated per-line height -- but now times kTtScrollBodyScreenfuls,
            // because the viewport SCROLLS: the cap has to reach well past one screenful or the wheel
            // would run out of document immediately (the whole point is getting to the bottom).
            const size_t kTtBodyMaxLines = std::clamp<size_t>(static_cast<size_t>(bodyMaxHeight / 11.0) * kTtScrollBodyScreenfuls, kTtBodyMaxLinesFloor, kTtBodyMaxLinesCeil);
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

            // The body VIEWPORT -- one screenful of a taller document, scrolled by the wheel. Three
            // nested elements, each load-bearing (and all of them inert: Grids, a StackPanel, a Border
            // and transforms -- no manipulation machinery, see the function comment):
            //
            //   bodyArea (Grid)          hosts the viewport AND the overlay scrollbar. The bar is a
            //                            SIBLING of the clip, so its negative right margin can park it in
            //                            the card's padding gutter instead of being cut by the clip.
            //   bodyClip (Grid)          the height cap and the visual cut. MaxHeight bounds it; the page
            //                            wires its SizeChanged to set an explicit Clip rect once the popup
            //                            actually lays it out (nothing is measured at build time).
            //   measureHost (StackPanel) measures its child with INFINITE height. That is what makes the
            //                            body's ActualHeight the true scroll extent, and -- more subtly --
            //                            what keeps the body free of a LAYOUT clip of its own: a layout
            //                            clip lives in the element's own coordinate space, so it would
            //                            travel WITH the render transform below and reveal nothing. The
            //                            clips that matter therefore sit on elements we never transform.
            //
            // Scrolling is a RenderTransform on the body, so a wheel notch costs no layout pass and can
            // never resize or move the popup the user is reading.
            Grid bodyArea;
            Grid bodyClip;
            bodyClip.MaxHeight(bodyMaxHeight);
            StackPanel measureHost;
            measureHost.Orientation(Orientation::Vertical);
            measureHost.VerticalAlignment(VerticalAlignment::Top);
            auto bodyStack = TtBuildSummaryBody(bodyStr);
            Media::TranslateTransform bodyShift;
            bodyStack.RenderTransform(bodyShift);
            measureHost.Children().Append(bodyStack);
            bodyClip.Children().Append(measureHost);
            bodyArea.Children().Append(bodyClip);

            // The slim overlay scrollbar (the collapsed-ScrollBar look): a 3px rounded bar in the card's
            // right padding gutter. Hidden until there is something to scroll, then dim at rest and fully
            // lit while you scroll -- the page owns that (see _SyncTabTooltipScrollBar / the fade timer).
            // Its LENGTH and POSITION are a ScaleTransform + a TranslateTransform rather than Height and
            // Margin, so every update stays render-only (a layout write from the SizeChanged path would
            // be the popup layout-cycle hazard the tag badges hit).
            Border thumb;
            thumb.Width(kTtScrollBarWidth);
            thumb.HorizontalAlignment(HorizontalAlignment::Right);
            thumb.VerticalAlignment(VerticalAlignment::Stretch);
            thumb.Margin(ThicknessHelper::FromLengths(0, 1, -kTtScrollBarGutterPx, 1));
            thumb.CornerRadius(CornerRadiusHelper::FromUniformRadius(kTtScrollBarWidth / 2.0));
            thumb.Background(TtFill(0xFF, 0xC8, 0xC8, 0xC8));
            thumb.Opacity(0.0); // the first layout decides: dim hint if it overflows, stay hidden if not
            Media::ScaleTransform thumbScale;
            thumbScale.ScaleY(1.0);
            Media::TranslateTransform thumbShift;
            Media::TransformGroup thumbXform;
            thumbXform.Children().Append(thumbScale); // scale from the top (CenterY 0) ...
            thumbXform.Children().Append(thumbShift); // ... then slide, in unscaled pixels
            thumb.RenderTransform(thumbXform);
            bodyArea.Children().Append(thumb);

            col.Children().Append(bodyArea);

            scrollOut.viewport = bodyClip;
            scrollOut.content = bodyStack;
            scrollOut.thumb = thumb;
            scrollOut.contentShift = bodyShift;
            scrollOut.thumbScale = thumbScale;
            scrollOut.thumbShift = thumbShift;

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
        root.MaxWidth(kTtCardMaxWidth);
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

    // Agentmaster (SPARK CROWN — the cache-warm hint): show/hide a tab's amber glow + rising embers. The
    // status dot is never touched, so the Triage state color reads exactly as before. Idempotent: the
    // observable no-ops on an unchanged bool, and the brush is re-pointed only on a genuine color change,
    // so re-asserting the same warmth every 2.5s costs nothing. UI thread.
    void TerminalPage::_SetTabCacheWarm(const TerminalApp::Tab& tab, bool on, const std::optional<winrt::Windows::UI::Color>& sparkColor)
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
            if (on && sparkColor)
            {
                const auto cur = status.AgentCacheWarmBrush().try_as<Media::SolidColorBrush>();
                if (!(cur && cur.Color() == *sparkColor))
                {
                    status.AgentCacheWarmBrush(Media::SolidColorBrush{ *sparkColor });
                }
            }
            status.AgentCacheWarmVisible(on);
        }
        CATCH_LOG();
    }

    // Agentmaster (SPARK CROWN): the ember/glow color for a tab — the SAME recipe the pending "3 dots"
    // use (the session's mode-aware tab color, resolved to how the header is CURRENTLY rendered, since an
    // unfocused colored tab draws much darker than its full color), fed through the fire palette instead
    // of the pending pair. UI thread.
    winrt::Windows::UI::Color TerminalPage::_CacheWarmSparkColorForTab(const TerminalApp::Tab& tab, const ::Agentmaster::SessionInfo& info)
    {
        const std::wstring hex = ::Agentmaster::ResolveSessionColorHex(_appSettings.tabColorMode, info);
        const auto dirColor = ParseArgbHexColor(hex, winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0x2E, 0x2E, 0x2E));
        const auto bg = winrt::get_self<Tab>(tab)->CurrentEffectiveTabBackground(dirColor);
        return CacheWarmSparkColorFor(bg);
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
                ::Agentmaster::AgentLogCaughtException(L"tag-editor popup position");
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
            ::Agentmaster::AgentLogCaughtException(L"tag-hover popup position");
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
            _tabTooltipScroll.erase(sessionId); // and the (now dangling) scroll pieces of the card we just dropped
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
        // The card's height budget: 80% of the window (== the screen when maximized, the normal case;
        // the tooltip renders in the island's popup root, so the window bounds it regardless). Measured
        // here because the builder is a free helper with no page. An unmeasured root (0) tells it to use
        // its fixed fallback. Like tagsOpacity above it rides the sig -- coarsened to a 50px grain so a
        // resize re-hosts an already-built card, without a drag re-hosting on every pixel.
        double cardMaxHeight = 0.0;
        try
        {
            if (const auto root = Root())
            {
                cardMaxHeight = root.ActualHeight() * ::kTtCardHeightFraction;
            }
        }
        CATCH_LOG();
        sig += L'\x1f';
        sig += std::to_wstring(static_cast<int>(cardMaxHeight / 50.0));
        // WHEEL SCROLL: a card built while the tip is OPEN cannot be hosted (Tab::_UpdateAgentToolTip
        // swaps Content only while closed — invariant 3), and adopting its scroll pieces would leave the
        // wheel driving an off-screen tree while the reader stares at the hosted one. So skip the whole
        // rebuild in that window; nothing is lost, because the next PointerEntered rebuilds it fresh
        // anyway (the card is hover-built, and its "ago" line would have gone stale by then regardless).
        // The one exception is the push that IS allowed to swap while open: the async summary arrival.
        const auto sit = _tabTooltipSig.find(sessionId);
        const bool sigChanged = (sit == _tabTooltipSig.end() || sit->second != sig);
        // ...or the tab has NO tooltip attached right now (an owner recycle detached it). Attachment is
        // what lets the framework open the card at all — ToolTipService only starts watching the owner's
        // hover when the tooltip is attached — so a same-signature tab with nothing attached must still
        // push, or it stays permanently tooltip-less until some unrelated field changes.
        if ((sigChanged || !impl->AgentToolTipAttached()) && (!impl->AgentToolTipOpen() || swapWhileOpen))
        {
            TtCardScrollParts scrollParts{};
            impl->SetAgentToolTip(TtBuildTooltipCard(accent, title, folderBranch, stateText, metaText, dirDetailLine, tagChips, tagsOpacity, bodyText, cardMaxHeight, scrollParts), winrt::hstring{ sig }, swapWhileOpen);
            _tabTooltipSig[sessionId] = sig;

            // Agentmaster (tab tooltip -- WHEEL SCROLL): adopt the fresh card's scroll pieces, replacing
            // the previous card's (a rebuild is a whole new tree, so the old refs are dead). WEAK refs to
            // the elements + strong refs to the three transforms: a transform is a leaf DependencyObject
            // that holds nothing back, while a strong element ref here would pin every superseded card
            // for as long as the session lives (the per-element-handler leak class AgentTipHelpers
            // exists to avoid). A card with no body has no viewport -- the entry then simply reports
            // "nothing to scroll".
            auto& st = _tabTooltipScroll[sessionId];
            st = {};
            if (scrollParts.viewport && scrollParts.content)
            {
                st.viewport = winrt::make_weak(scrollParts.viewport);
                st.content = winrt::make_weak(scrollParts.content);
                if (scrollParts.thumb)
                {
                    st.thumb = winrt::make_weak(scrollParts.thumb);
                }
                st.contentShift = scrollParts.contentShift;
                st.thumbScale = scrollParts.thumbScale;
                st.thumbShift = scrollParts.thumbShift;

                // The card is built BEFORE it is ever shown (ToolTipService needs it hosted before the
                // first hover), so nothing is measured yet. Its own first layout INSIDE the popup is
                // therefore where the viewport clips itself and the bar decides whether to show the dim
                // "there is more" hint. Both writes are render-only (Clip / transforms / Opacity) -- a
                // layout write from inside a layout pass is the popup layout-cycle fail-fast the tag
                // badges hit. The handler captures only a WEAK page + the session id, never an element,
                // so this per-change-rebuilt tree can't leak through its own handler.
                scrollParts.viewport.SizeChanged([weak = get_weak(), sid = sessionId](const IInspectable& sender, const auto&) {
                    const auto self = weak.get();
                    if (!self)
                    {
                        return;
                    }
                    try
                    {
                        if (const auto fe = sender.try_as<FrameworkElement>())
                        {
                            // An explicit Clip on the (never-transformed) viewport is what cuts the
                            // shifted body at the viewport edge — belt to the layout clip that already
                            // bounds the unscrolled case.
                            RectangleGeometry clip;
                            clip.Rect(winrt::Windows::Foundation::Rect{ 0, 0, static_cast<float>(fe.ActualWidth()), static_cast<float>(fe.ActualHeight()) });
                            fe.Clip(clip);
                        }
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"tab tooltip scroll viewport clip");
                    }
                    // Keep whatever offset the reader is at (a resize mid-read must not yank them back
                    // to the top); a fresh card is at 0 anyway. The bar is sized here but stays INVISIBLE
                    // — it appears only under the wheel.
                    self->_SyncTabTooltipScrollBar(sid, std::nullopt, ::kTtScrollBarRestOpacity);
                });
            }
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

    // Agentmaster (tab tooltip — WHEEL SCROLL): push `offset` (nullopt = keep the current one) onto the
    // hovered card's body and re-fit the slim scrollbar to it, painting the bar at `opacity`. Returns the
    // MAXIMUM scrollable offset — 0 means the body fits, so there is nothing to scroll and the bar stays
    // hidden (which is also how the wheel handler decides whether it consumed the notch).
    //
    // Every write here is RENDER-only — a transform value or Opacity — never a layout property. That is
    // deliberate and load-bearing: this also runs from the viewport's own SizeChanged, i.e. from inside a
    // layout pass, and mutating layout from a layout-driven handler inside a popup is exactly the
    // E_LAYOUTCYCLE fail-fast the tag badges had to be rewritten around (TabHeaderControl's
    // _PositionTagBadges coalescing scheduler). Sizing the thumb by ScaleY instead of Height, and moving
    // it by a TranslateTransform instead of Margin, keeps this path structurally incapable of it.
    double TerminalPage::_SyncTabTooltipScrollBar(const std::wstring& sessionId, std::optional<double> offset, double opacity)
    {
        const auto it = _tabTooltipScroll.find(sessionId);
        if (it == _tabTooltipScroll.end())
        {
            return 0.0;
        }
        auto& st = it->second;
        try
        {
            const auto viewport = st.viewport.get();
            const auto content = st.content.get();
            if (!viewport || !content)
            {
                return 0.0; // no body on this card (or the card is gone) — nothing to scroll
            }
            const double viewH = viewport.ActualHeight();
            const double contentH = content.ActualHeight();
            // +1px of slack: a sub-pixel overshoot is not "scrollable", it is rounding.
            const double maxOff = (viewH > 0.0 && contentH > viewH + 1.0) ? (contentH - viewH) : 0.0;
            st.offset = std::clamp(offset.value_or(st.offset), 0.0, maxOff);
            if (st.contentShift)
            {
                st.contentShift.Y(-st.offset); // scroll DOWN = shift the body UP
            }
            if (const auto thumb = st.thumb.get())
            {
                const double barH = thumb.ActualHeight();
                if (maxOff > 0.0 && barH > 0.0 && st.thumbScale && st.thumbShift)
                {
                    // Thumb length = the visible share of the document (floored, so a very deep body
                    // still leaves a readable thumb); position = the scrolled fraction of the travel.
                    const double scale = std::clamp(std::max(viewH / contentH, ::kTtScrollThumbMinPx / barH), 0.05, 1.0);
                    st.thumbScale.ScaleY(scale);
                    st.thumbShift.Y((barH - barH * scale) * (st.offset / maxOff));
                }
                thumb.Opacity(maxOff > 0.0 ? std::clamp(opacity, 0.0, 1.0) : 0.0);
            }
            return maxOff;
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SyncTabTooltipScrollBar");
            return 0.0;
        }
    }

    // Agentmaster (tab tooltip — WHEEL SCROLL): a wheel notch arrived on a tab whose rich card is OPEN
    // (Tab's PointerWheelChanged already checked that, and only ever READS IsOpen). Move the body and
    // report whether we consumed the notch — false leaves it to the tab strip's own horizontal scroll,
    // which is the right answer whenever this card has nothing to scroll.
    //
    // The tab's CURRENT session is resolved here rather than captured at wiring time, exactly like the
    // hover-build hook: a /resume re-home or a fork re-bind swaps the tab's session underneath us.
    bool TerminalPage::_ScrollTabAgentToolTip(const TerminalApp::Tab& tab, int wheelDelta, bool fromTabItem)
    {
        _tooltipWheelClaimed = true; // tell the tab-row belt below that this notch is already spoken for
        if (!tab || wheelDelta == 0)
        {
            return false;
        }
        const auto sessionId = _ClaudeSessionForTab(tab);
        if (sessionId.empty())
        {
            return false;
        }
        const auto it = _tabTooltipScroll.find(sessionId);
        if (it == _tabTooltipScroll.end())
        {
            return false;
        }
        // A notch is 120 units; wheel DOWN is negative and moves us further down the document.
        const double before = it->second.offset;
        const double next = before - (static_cast<double>(wheelDelta) / 120.0) * ::kTtScrollStepPx;
        const double maxOff = _SyncTabTooltipScrollBar(sessionId, next, ::kTtScrollBarActiveOpacity);
        // One log line per notch, only for a session whose card is actually up: this is the ONE step of
        // the chain that cannot be seen from the outside (the bar's own behavior proves the geometry).
        // A hover+scroll that produces NO line here means the wheel never reached us at all.
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[tooltip-wheel] " + ::Agentmaster::ShortId(sessionId) + L" src=" + (fromTabItem ? L"item" : L"row") +
                                          L" delta=" + std::to_wstring(wheelDelta) +
                                          L" off=" + std::to_wstring(static_cast<int>(before)) + L"->" + std::to_wstring(static_cast<int>(it->second.offset)) +
                                          L" max=" + std::to_wstring(static_cast<int>(maxOff)));
        if (maxOff <= 0.0)
        {
            return false; // the whole body already fits — don't steal the wheel from the strip
        }
        _ArmTabTooltipScrollBarFade(sessionId);
        return true; // consumed even at an end stop: this card IS the scroller under the pointer
    }

    // Agentmaster (tab tooltip — WHEEL SCROLL): the BELT under the per-tab handler. The notch is supposed
    // to arrive on the TabViewItem (the element the pointer is actually over), but that path depends on
    // MUX's tab-strip internals not swallowing it first; this one sits on the whole tab ROW with
    // handledEventsToo, so it sees the event no matter who handled it on the way up. It scrolls whichever
    // tab currently has its card open — which, since the framework closes the tip the moment you leave the
    // tab, is always the tab you are hovering.
    //
    // `_tooltipWheelClaimed` is the de-dupe: the per-tab handler runs FIRST (it is deeper in the bubble)
    // and every attempt it makes sets the flag, so this one consumes-and-skips. Both handlers being ours
    // makes that ordering guaranteed rather than hopeful.
    void TerminalPage::_WireTabStripTooltipWheel()
    {
        if (_tabStripWheelWired)
        {
            return;
        }
        const auto row = TabRow();
        if (!row)
        {
            return; // not laid out yet — the next arm tries again
        }
        _tabStripWheelWired = true;
        row.AddHandler(
            UIElement::PointerWheelChangedEvent(),
            winrt::box_value(WUX::Input::PointerEventHandler{ [weak = get_weak()](const IInspectable&, const WUX::Input::PointerRoutedEventArgs& e) {
                const auto page = weak.get();
                if (!page)
                {
                    return;
                }
                if (std::exchange(page->_tooltipWheelClaimed, false))
                {
                    return; // the tab's own handler already had this notch
                }
                try
                {
                    // Whose card is up? Exactly one can be (the tip dies on pointer-exit).
                    bool found = false;
                    for (const auto& [sid, weakTab] : page->_claudeTabs)
                    {
                        const auto tab = weakTab.get();
                        if (!tab)
                        {
                            continue;
                        }
                        const auto impl = page->_GetTabImpl(tab);
                        if (!impl || !impl->AgentToolTipOpen())
                        {
                            continue;
                        }
                        found = true;
                        const auto pt = e.GetCurrentPoint(nullptr);
                        if (const auto delta = pt ? pt.Properties().MouseWheelDelta() : 0; delta != 0 && page->_ScrollTabAgentToolTip(tab, delta, /* fromTabItem */ false))
                        {
                            e.Handled(true);
                        }
                        break;
                    }
                    // The belt's own throttled trace: a notch that reached the ROW but found no open card
                    // is a DIFFERENT failure from one that never arrived at all (see the item handler).
                    if (const auto nowTick = ::GetTickCount64(); !found && nowTick - page->_tooltipWheelLogTick > 400)
                    {
                        page->_tooltipWheelLogTick = nowTick;
                        ::Agentmaster::AppendStateLog(L"hooks.log", L"[tooltip-wheel] row: no open card");
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"tab-strip tooltip wheel belt");
                }
            } }),
            true /* handledEventsToo — the strip's own scroller may have taken it first */);
    }

    // Agentmaster (tab tooltip — WHEEL SCROLL): a fresh hover starts at the top of the body. Called from
    // the PointerEntered build hook, so it covers the case the rebuild does not: an UNCHANGED signature
    // re-hosts nothing, so the very same card element (still shifted where you left it last time) is what
    // the next hover would show.
    void TerminalPage::_ResetTabAgentToolTipScroll(const std::wstring& sessionId)
    {
        if (_tabTooltipScroll.find(sessionId) == _tabTooltipScroll.end())
        {
            return;
        }
        if (_tabTooltipScrollFadeTimer)
        {
            _tabTooltipScrollFadeTimer.Stop(); // a pending fade would erase the hint we are about to show
        }
        _tabTooltipScrollFadeSession.clear();
        _SyncTabTooltipScrollBar(sessionId, 0.0, ::kTtScrollBarRestOpacity);
    }

    // Agentmaster (tab tooltip — WHEEL SCROLL): (re)start the scrollbar's auto-hide. Every notch restarts
    // the hold, so the bar stays lit while you are scrolling and only fades once you stop — the modern
    // overlay-scrollbar behavior, hand-rolled because the card can host no ScrollViewer to get it for free.
    // ONE timer per window is enough: exactly one tooltip is ever on screen.
    void TerminalPage::_ArmTabTooltipScrollBarFade(const std::wstring& sessionId)
    {
        _tabTooltipScrollFadeSession = sessionId;
        _tabTooltipScrollFadeHolding = true;
        if (!_tabTooltipScrollFadeTimer)
        {
            _tabTooltipScrollFadeTimer = WUX::DispatcherTimer{};
            _tabTooltipScrollFadeTimer.Tick([weak = get_weak()](const IInspectable& sender, const IInspectable&) {
                if (auto self = weak.get())
                {
                    self->_OnTabTooltipScrollBarFadeTick();
                }
                else if (const auto t = sender.try_as<WUX::DispatcherTimer>())
                {
                    t.Stop(); // page destroyed — stop ticking (UI thread, safe)
                }
            });
        }
        _tabTooltipScrollFadeTimer.Stop();
        _tabTooltipScrollFadeTimer.Interval(std::chrono::milliseconds(::kTtScrollBarHoldMs));
        _tabTooltipScrollFadeTimer.Start();
    }

    // Agentmaster (tab tooltip — WHEEL SCROLL): the auto-hide tick, in two phases on one timer — first
    // the long hold after the last notch, then a short step-down fade to fully invisible (a hand-rolled
    // fade: an animation inside ToolTip content is more moving parts than this surface has earned).
    void TerminalPage::_OnTabTooltipScrollBarFadeTick()
    {
        if (!_tabTooltipScrollFadeTimer)
        {
            return;
        }
        if (_tabTooltipScrollFadeHolding)
        {
            _tabTooltipScrollFadeHolding = false; // the hold elapsed — switch to the fade cadence
            _tabTooltipScrollFadeTimer.Stop();
            _tabTooltipScrollFadeTimer.Interval(std::chrono::milliseconds(::kTtScrollBarFadeStepMs));
            _tabTooltipScrollFadeTimer.Start();
            return;
        }
        double left = 0.0;
        try
        {
            if (const auto it = _tabTooltipScroll.find(_tabTooltipScrollFadeSession); it != _tabTooltipScroll.end())
            {
                if (const auto thumb = it->second.thumb.get())
                {
                    left = std::max(0.0, thumb.Opacity() - ::kTtScrollBarFadeStep);
                    thumb.Opacity(left);
                }
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_OnTabTooltipScrollBarFadeTick");
            left = 0.0; // give up on this bar rather than tick forever
        }
        if (left <= 0.0)
        {
            _tabTooltipScrollFadeTimer.Stop();
            _tabTooltipScrollFadeSession.clear();
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
        _WireTabStripTooltipWheel(); // WHEEL SCROLL: the once-per-window belt (a bool probe once wired; retries while TabRow isn't up)
        const auto impl = _GetTabImpl(tab);
        if (!impl || impl->AgentToolTipHoverWired())
        {
            return; // steady-state per-tick cost: this bool probe, nothing else
        }
        const bool justWired = impl->EnsureAgentToolTipHoverHook(
            [weakThis = get_weak(), weakTab = winrt::make_weak(tab)]() {
                const auto self = weakThis.get();
                const auto t = weakTab.get();
                if (self && t)
                {
                    if (const auto sid = self->_ClaudeSessionForTab(t); !sid.empty())
                    {
                        // PointerEntered BUBBLES, so it re-fires as the pointer crosses the header's own
                        // inner elements (icon -> title -> close button) — i.e. repeatedly, mid-hover,
                        // while the tip is already up. The build is sig-gated and harmless there, but the
                        // scroll reset is NOT: it would snap a reader back to the top of the body every
                        // time they nudged the mouse. So reset only on a hover that starts CLOSED.
                        const auto impl = self->_GetTabImpl(t);
                        const bool tipOpen = impl && impl->AgentToolTipOpen();
                        self->_UpdateTabAgentToolTip(t, sid); // hover-time build: fresh state + fresh "ago", kicks the mtime-gated summary load
                        if (!tipOpen)
                        {
                            self->_ResetTabAgentToolTipScroll(sid); // WHEEL SCROLL: a fresh hover starts at the top of the body
                        }
                    }
                }
            },
            // WHEEL SCROLL: the same hook wires the tab header's PointerWheelChanged, because the header
            // is the only real pointer target in this whole feature — the card is hit-test-invisible by
            // design and can never receive input itself.
            [weakThis = get_weak(), weakTab = winrt::make_weak(tab)](int delta) -> bool {
                const auto self = weakThis.get();
                const auto t = weakTab.get();
                return (self && t) ? self->_ScrollTabAgentToolTip(t, delta, /* fromTabItem */ true) : false;
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
            // Agentmaster (tab tooltip): the hover card ALWAYS trims its messages, independent of the
            // summary PANEL's global "Truncate long messages" toggle (AppSettings::summaryPanelTruncate).
            // The tooltip is a compact, read-only card with NO button to control trimming — so turning
            // truncation OFF in the panel (which owns the toggle) must not bloat the tooltip into a
            // screen-tall wall of full messages. This also aligns the Claude branch with the Codex branch
            // below, which already renders via SummaryEscapeMsg's truncate=true default. (Wrap still
            // follows the global toggle; only trimming is pinned ON here.)
            const bool truncate = true;
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
                ::Agentmaster::AgentLogCaughtException(L"_EnsureTabTooltipSummary analyze (contained)");
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
            ::Agentmaster::AgentLogCaughtException(L"_EnsureTabTooltipSummary");
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
            _agentFlashRunSpan.erase(sessionId); // forget the Running span with the state (same rule)
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

        // Agentmaster: track the CURRENT Running span for the answer-blip flash floor below. Stamped on
        // the ENTRY edge only (X -> Running), so the span measures this run, and remembering whether it
        // came from NeedsApproval (the answer release) is what keeps the floor off every other edge.
        if (newState == SessionState::Running)
        {
            if (prev != SessionState::Running)
            {
                _agentFlashRunSpan[sessionId] = AgentRunSpan{ TtNowMs(), hadPrev && prev == SessionState::NeedsApproval };
            }
        }

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
            // Agentmaster: suppress ONLY the post-answer blip — a Running that was entered from
            // NeedsApproval (the user answered) and ended within kFlashMinRunSpanAfterAnswerMs. Before
            // the answer-release synths this shape was NeedsApproval -> WaitingForInput, a target ->
            // target move that never flashed; without this floor every answered question would newly
            // flash its tab. A longer post-answer run still flashes (it genuinely needs you again), and
            // no other edge is touched. See ShouldSuppressAnswerBlipFlash.
            int64_t runSpanMs = 0;
            bool fromNeedsApproval = false;
            if (const auto rit = _agentFlashRunSpan.find(sessionId); rit != _agentFlashRunSpan.end())
            {
                runSpanMs = TtNowMs() - rit->second.sinceMs;
                fromNeedsApproval = rit->second.fromNeedsApproval;
            }
            _agentFlashRunSpan.erase(sessionId); // the run ended; the next entry edge re-stamps it
            if (::Agentmaster::ShouldSuppressAnswerBlipFlash(newState, fromNeedsApproval, runSpanMs))
            {
                return; // leave any existing flash as-is, exactly like the old target -> target path
            }
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

        // MINIMUM COMPLETION SPAN (SessionScanner's ShouldSuppressShortCompletionToast): a Running span
        // under kNotifyMinCompletionSpanMs yields NO toast at all — it is either the "0 seconds complete
        // but still running" flicker (a slow-path Stop landing ~20ms after the NEXT turn's prompt, then
        // an immediate re-light: 6e2d0b48 / 532dc9ac / e7fa7fcc / 09e226cb) or a turn too brief to be
        // worth interrupting for. Deliberately checked BEFORE the hold below, for two reasons: a doomed
        // toast is never parked, and — decisively — a HELD toast's span keeps growing, so parking a
        // short one would let it cross the floor and fire late, turning "no notification" into "a
        // delayed notification". Idle/Waiting only: NeedsApproval / Error / Done need you regardless of
        // how briefly the turn ran (an approval request routinely blocks seconds in). A span of 0 is an
        // UNOBSERVED entry (adopted mid-turn), not a short one — it is never floored.
        const int64_t runningSpan = (runningSince > 0) ? (TtNowMs() - runningSince) : 0;
        if (::Agentmaster::ShouldSuppressShortCompletionToast(newState, runningSpan))
        {
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[notify-skip] " + ::Agentmaster::ShortId(sessionId) + L" running -> " + TtStateLabel(newState) +
                                              L" after " + TtSpan(runningSpan, true) + L" (under the " +
                                              std::to_wstring(::Agentmaster::kNotifyMinCompletionSpanMs / 1000) + L"s minimum)\n");
            return;
        }

        // HOLD instead of fire while external work is live (SessionScanner's ShouldHoldCompletionToast):
        // only Idle/WaitingForInput ever hold — exactly the two states the outlived-turn promotion can
        // veto (a NeedsApproval question / an Error / an exit needs you regardless of background work).
        // The liveness-ticked sweep (_SweepAgentPendingToasts) later DROPS the hold (session re-lit
        // Running — the 513d1366 spurious "waiting for you" toast) or FIRES it with the CURRENT state
        // (signal cleared / moved to a hard needs-you state / the kNotifyExternalHoldCapMs backstop).
        // Every held toast is already past the minimum-span floor above and a hold only GROWS the span,
        // so the sweep's deferred fire can never dip back under the floor.
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

        // Line 3: WHICH request just came back — a one-line truncation of the prompt whose turn just
        // finished, quoted. Lines 1+2 identify the session and its outcome but never what it was
        // DOING, which is the missing half when several sessions finish while you are elsewhere: a
        // title tells you the folder, not the task. Sourced from the registry queue's newest Sent
        // entry (::Agentmaster::LastDeliveredPrompt — the same record the Auto Testing's SENT summary
        // renders, so it covers a prompt typed straight into the ConPTY as well as one we injected).
        // OMITTED ENTIRELY when there is no delivered prompt (a never-prompted launch, a managed
        // Codex — no prompt records — or a session adopted after its last turn): the toast then reads
        // exactly as it did before, two lines. Prefer the prompt BODY, falling back to its short
        // label, mirroring the overlay's row-3 choice.
        std::wstring prompt;
        if (_sessionRegistry)
        {
            if (const auto info = _sessionRegistry->Get(sessionId))
            {
                if (const auto* p = ::Agentmaster::LastDeliveredPrompt(info->queue))
                {
                    prompt = ::Agentmaster::PromptPreviewLine(!p->text.empty() ? p->text : p->label,
                                                              ::Agentmaster::kNotifyPromptPreviewChars);
                }
            }
        }
        // Typographic quotes mark it as the user's own words rather than more status prose (the same
        // convention the cog's tooltip uses when it quotes a toast). PromptPreviewLine already
        // returned "" for an all-whitespace prompt, so an empty quote can never render.
        const std::wstring line3 = prompt.empty() ? std::wstring{} : (L"\x201C" + prompt + L"\x201D");

        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[notify] " + ::Agentmaster::ShortId(sessionId) + L" running -> " + TtStateLabel(state) +
                                          (span.empty() ? std::wstring{} : (L" (after " + span + L")")) +
                                          (heldForMs > 0 ? (L" (held " + std::to_wstring(heldForMs / 1000) + L"s)") : std::wstring{}) +
                                          (line3.empty() ? L" (no prompt line)" : L"") + L"\n");
        _agentToastLastShownMs[sessionId] = now;
        _ShowAgentSessionToast(sessionId, title, body, line3, !_appSettings.notifySound);
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
            ::Agentmaster::AgentLogCaughtException(L"_SweepAgentPendingToasts");
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
    // line 2 the completion body, and OPTIONAL line 3 the just-finished prompt (`detail`, omitted when
    // empty). The strings go in as DOM TEXT NODES (CreateTextNode), so XML-special characters in a
    // session title — or in a PROMPT, which is arbitrary user text and the likeliest source of them —
    // are escaped by the DOM, never hand-built markup. Tag+Group make a session's newer toast REPLACE
    // its older one in the Action Center instead of piling up (ShortId fits the legacy 16-char Tag cap).
    //
    // ON-SCREEN TIME: `duration="long"` (~25s) rather than the default short (~5-7s, whatever the user's
    // "Show notifications for" ease-of-access setting says). The Windows toast schema has NO arbitrary
    // duration -- short and long are the only two values -- so "keep it up longer" has exactly one lever.
    // Long is the right one here: a completion toast exists to be caught while you are looking at ANOTHER
    // window, and the short default routinely expires before you glance over. It also only affects the
    // BANNER; the toast persists in the Action Center either way (and Tag+Group still de-dupes it there).
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
    void TerminalPage::_ShowAgentSessionToast(const std::wstring& sessionId, const std::wstring& title, const std::wstring& body, const std::wstring& detail, bool silent)
    {
        try
        {
            // The template is COMPOSED rather than picked from literals: two independent options (the
            // silent audio element, and whether there is a 3rd line at all) would otherwise need four
            // hand-maintained strings. `detail` empty => only two <text> elements are emitted, so a
            // session with no delivered prompt produces byte-for-byte the toast it always did. Note
            // ToastGeneric caps at 4 text elements and a BANNER renders title + ~2 body lines, so
            // three is the most that shows without the user expanding it.
            std::wstring xml = LR"(<toast duration="long"><visual><binding template="ToastGeneric"><text></text><text></text>)";
            if (!detail.empty())
            {
                xml += L"<text></text>";
            }
            xml += L"</binding></visual>";
            if (silent)
            {
                xml += LR"(<audio silent="true"/>)";
            }
            xml += L"</toast>";

            winrt::Windows::Data::Xml::Dom::XmlDocument doc;
            doc.LoadXml(winrt::hstring{ xml });
            const auto texts = doc.GetElementsByTagName(L"text");
            texts.Item(0).AppendChild(doc.CreateTextNode(winrt::hstring{ title }));
            texts.Item(1).AppendChild(doc.CreateTextNode(winrt::hstring{ body }));
            if (!detail.empty())
            {
                texts.Item(2).AppendChild(doc.CreateTextNode(winrt::hstring{ detail }));
            }
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
                ::Agentmaster::AgentLogCaughtException(L"notify toast Show"); // full detail (hr/msg/stack) for the ONE logged failure
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

    // Agentmaster (SUMMARY_JUMP.md §4, perf): mirror "which tab is selected" onto every linked overlay.
    // The summary panel's jump-eligibility resolve is O(prompts x the 1.2M-char buffer window) and its only
    // effect is the icon opacities of a panel you can see, but it used to run on a 5 s timer for EVERY
    // linked overlay in the window — with ~19 live Claude tabs and 200+-prompt conversations that
    // saturated the UI thread outright (the 2026-07-19 release freeze). AgentTabOverlay::SetTabFocused
    // is the gate: the focused overlay resolves (once, on the transition, then on its interval floor) and
    // the rest go quiet. `tab` may be the Manager tab / a shell tab / null — then NOTHING is focused,
    // which is exactly right. Cheap: one map walk over the window's overlays per tab switch.
    void TerminalPage::_SyncOverlayFocusToTab(const TerminalApp::Tab& tab)
    {
        const auto focusedId = tab ? _ClaudeSessionForTab(tab) : std::wstring{};
        for (auto& [id, overlay] : _claudeOverlays)
        {
            if (overlay)
            {
                overlay->SetTabFocused(!focusedId.empty() && id == focusedId);
            }
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
            // Agentmaster (PENDING_INPUT.md §8): the copy menu's "Copy Current Prompt" reads the UNSENT
            // draft LIVE out of this session's buffer. The overlay has no control reference (same reason
            // as the jump handler), so the page does the read — resolved by sessionId at click time, so a
            // pane restart can't strand a captured stale control — and hands back "" if it isn't
            // readable, which makes the copy fall back to the observer's recorded draft.
            overlay->SetLiveDraftHandler([weakThis, sessionId]() -> std::wstring {
                auto self = weakThis.get();
                return self ? self->_ReadLiveDraftForSession(sessionId) : std::wstring{};
            });
            // Agentmaster (PENDING_INPUT.md §8d): the MAIL button's default click REMOVES the draft from
            // the input box after queueing it (a move); Shift+Click keeps it. The overlay can't inject, so
            // the page runs the verified clear (self-marshaling; a no-op if the box isn't ours / is empty).
            overlay->SetClearDraftHandler([weakThis, sessionId]() {
                if (auto self = weakThis.get())
                {
                    self->_ClearLiveDraftForSession(sessionId);
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
            // Agentmaster (SUMMARY_JUMP.md §4, perf): seed the focus flag — a fresh overlay defaults to
            // NOT focused, so a session bound while its own tab is already selected (a launch focuses the
            // new tab, then the observer binds it a tick later — no further tab switch follows) would
            // otherwise never run its jump-eligibility resolve. The tab-switch funnel maintains it after.
            if (const auto focused = _GetFocusedTab())
            {
                overlay->SetTabFocused(_ClaudeSessionForTab(focused) == sessionId);
            }
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

    // Agentmaster (PENDING_INPUT.md §8): read a session's UNSENT input-box draft out of its LIVE
    // terminal buffer, right now — the primary source behind every copy menu's "Copy Current Prompt".
    // This is the same read the scan lane performs each liveness tick (ControlCore::ReadPendingInputDraft
    // → the pure DetectPendingInput), just on demand, so the copy is the box as rendered at click time
    // rather than up to one tick stale.
    //
    // WRAPPED, because every one of its failure modes is ORDINARY and none may cost the user a copy:
    // the session's tab may live in ANOTHER window (a different UI thread — _ControlForSession answers
    // null, which is exactly why the caller has a fallback), the tab may be dormant (window-restored,
    // its claude never started — no buffer to read), the control may be torn down mid-click, or the
    // buffer may not be initialized yet. All of those return "" here, and CopySessionField then falls
    // back to the observer's recorded SessionInfo::pendingInput. UI thread only.
    std::wstring TerminalPage::_ReadLiveDraftForSession(const std::wstring& sessionId)
    {
        try
        {
            const auto control = _ControlForSession(sessionId);
            if (!control || control.ConnectionState() == TerminalConnection::ConnectionState::NotConnected)
            {
                return {}; // not hosted here, or dormant (no started buffer) — the remembered draft answers
            }
            const auto h = control.ReadPendingInputDraft();
            return std::wstring{ h.c_str(), h.size() };
        }
        catch (...)
        {
            // Rule #18: log what threw + where; the recovery (an empty read → the remembered draft)
            // is unchanged. Same shape as the scan lane's per-control guard.
            ::Agentmaster::AgentLogCaughtException(L"_ReadLiveDraftForSession");
            return {};
        }
    }

    // Agentmaster (DELIVERY.md §11 / DELIVERY_PLAN.md R4+R5): ONE fresh tri-state read of a session's
    // input box — {InputBoxState as int32, draft text}, decoded from TermControl::ReadInputBoxProbe's
    // "<state digit><text>" encoding (one call, so a buffer mutation can never split verdict from
    // text). `maxRows` raises the read window past the scan's 120 rows for verification reads (a
    // filled prompt can render taller). Every ordinary failure — unhosted, dormant, torn down,
    // pre-initialized — answers {Unknown, ""}: "no information", which no consumer treats as safe.
    std::pair<int32_t, std::wstring> TerminalPage::_ReadInputBoxProbeForSession(const std::wstring& sessionId, int32_t maxRows)
    {
        constexpr auto unknown = static_cast<int32_t>(::Agentmaster::InputBoxState::Unknown);
        try
        {
            const auto control = _ControlForSession(sessionId);
            if (!control || control.ConnectionState() == TerminalConnection::ConnectionState::NotConnected)
            {
                return { unknown, std::wstring{} };
            }
            const auto h = control.ReadInputBoxProbe(maxRows);
            if (h.empty())
            {
                return { unknown, std::wstring{} }; // terminal not initialized yet
            }
            const std::wstring encoded{ h.c_str(), h.size() };
            const int32_t state = static_cast<int32_t>(encoded[0] - L'0');
            if (state < static_cast<int32_t>(::Agentmaster::InputBoxState::Unknown) ||
                state > static_cast<int32_t>(::Agentmaster::InputBoxState::MenuOpen))
            {
                return { unknown, std::wstring{} }; // malformed encoding — treat as no information
            }
            return { state, encoded.substr(1) };
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ReadInputBoxProbeForSession");
            return { unknown, std::wstring{} };
        }
    }

    // ---- The DRAFT SWAP (PENDING_INPUT.md §9) ------------------------------------------------
    //
    // THE PROBLEM. A prompt is delivered as a bracketed paste plus a submit CR. A paste lands AT THE
    // CURSOR, so if the box already holds the user's UNSENT draft, the CR submits draft + prompt as
    // ONE message the user never wrote and never pressed Enter on — and the draft is gone. The state
    // machine cannot prevent this on its own: a draft is deliberately a transient FACT, never
    // SessionState (Rule #7/#13), so a session holding one still reads Idle / WaitingForInput, which
    // is exactly the "ready to send" set.
    //
    // THE SWAP. Take the draft out of the way, send, put it back — every step VERIFIED by re-reading
    // the box, never assumed:
    //
    //   1. READ    the box (live buffer; the observer's remembered draft is the payload fallback —
    //              PickCurrentPromptText, the same rule "Copy Current Prompt" uses, §8).
    //   2. LOCK    the control read-only, so the user's own keystrokes cannot interleave with the
    //              swap. They still SEE everything happening in the tab; they just cannot type into
    //              it for the ~1s it takes. WT short-circuits the read-only check for key events, so
    //              this is silent (no warning dialog per keystroke).
    //   3. CLEAR   with Ctrl+U (into the TUI's kill-ring), re-reading until the box is CONFIRMED
    //              empty; DecideDraftClear escalates to backspaces and finally gives up.
    //   4. ABORT   if it never confirms empty: send NOTHING, roll the prompt back to Pending, restore
    //              whatever is in the box, unlock. A prompt sent late is recoverable; a mangled
    //              message is not (Rule #16's spirit — never lose what the user wrote).
    //   5. SEND    the prompt through the unchanged recipe, so the echo dedup, the pickup guard and
    //              the Enter-retry watchdog all still apply to it.
    //   6. AWAIT   the prompt actually LEAVING the box (a turn-started signal + an empty box). If it
    //              is still sitting there un-submitted we do NOT restore — yanking then would merge
    //              into it, the very bug this exists to prevent — and the draft stays in the kill-ring
    //              (a manual Ctrl+Y recovers it) and in the registry's memory, both logged.
    //   7. RESTORE with Ctrl+Y — the kill-ring hands back the user's EXACT bytes, round-tripped by the
    //              TUI rather than re-typed by us — verified; if the yank puts nothing back, the draft
    //              we READ is re-pasted with BuildPromptFill (no submit CR). Never both: the paste
    //              runs only on a box VERIFIED still empty after the yank.
    //   8. UNLOCK  (only the read-only WE took) and re-record the draft so the "3 dots" stay honest.
    //
    // Off => step 2 onward is skipped and the injection is byte-identical to the historical behavior
    // (AppSettings::preserveDraftOnSend). A session with an EMPTY box takes the same fast path: there
    // is nothing a paste could merge into.

    // Pacing. The poll is what makes every step VERIFIED rather than assumed: inject, wait one beat
    // for Claude's TUI to repaint, re-read the box. 60ms is well under a frame budget yet far above
    // the ConPTY round-trip, so a normal swap costs ~2-4 polls end to end. The budgets bound each
    // phase independently: clearing is quick or it is broken; the SUBMIT wait is long because it must
    // outlast the Enter-retry watchdog's presses (3s + 6s + 6s) before we conclude the prompt is
    // stuck in the box; restoring is quick again.
    static constexpr int64_t kDraftSwapPollMs = 60;
    static constexpr int64_t kDraftSwapActionSettleMs = 300; // how long one keystroke gets to land + repaint before we judge it
    static constexpr int64_t kDraftSwapClearBudgetMs = 2400;
    static constexpr int64_t kDraftDiscardBudgetMs = 4000; // the mail-move DISCARD ladder walks ~a line per round (the swap's stash clears whole-box in one press), so it gets a larger wall-clock; a single-line clear still finishes in ~150ms
    static constexpr int64_t kDraftSwapSubmitBudgetMs = 18000;
    static constexpr int64_t kDraftSwapRestoreBudgetMs = 1200;
    // Agentmaster (DELIVERY.md §11 / DELIVERY_PLAN.md R4 — VERIFIED PLACEMENT):
    static constexpr int32_t kSendVerifyProbeRows = 1000; // the raised read window for verification reads — a filled prompt can render taller than the scan's fixed 120 rows
    static constexpr int64_t kSendVerifySettleMs = 1500; // per fill: how long the read-back may keep settling while the box is still CHANGING (progress-extended)
    static constexpr int64_t kSendVerifySettleHardCapMs = 4000; // absolute cap on one fill's settle, however long the box keeps growing
    static constexpr int32_t kSendVerifyMaxFills = 3; // fill attempts per delivery before giving up (was 2; raised per the confirm-and-retry directive, DELIVERY.md §12 — inner re-fills are livelock-free: they never touch the advance trigger)
    static constexpr int64_t kSendUndoBudgetMs = 5000; // wall-clock cap on the Foreign undo ladder
    static constexpr int64_t kDraftSwapRestoreHardCapMs = 4000; // R6a: the restore settle extends while the box is still CHANGING (a large stash pop's repaint outruns the fixed 1.2s), up to this
    static constexpr int32_t kSendVerifyStrikeLimit = 2; // unverifiable deliveries per prompt before it FAILS terminally (a plain rollback would re-fire the same doomed verify forever — the RC2 livelock shape). §12: the terminal strike no longer pauses the autorunner — the prompt Fails (bounded forward progress, never the same doomed verify twice more) but the MODE stays the user's; a persistent wall is held upstream by the pre-flight decline + the recorded box state, which never strike.

    // Do two box reads describe the same draft? Both sides come out of the SAME detector on the same
    // box, so a plain comparison is fair; only trailing whitespace is normalized away (the box
    // reserves vertical slack, and a focused empty line can render as padding).
    static std::wstring DraftSwapNormalize(std::wstring_view s)
    {
        size_t end = s.size();
        while (end > 0 && (s[end - 1] == L' ' || s[end - 1] == L'\t' || s[end - 1] == L'\n' || s[end - 1] == L'\r'))
        {
            --end;
        }
        return std::wstring{ s.substr(0, end) };
    }

    // Agentmaster (DELIVERY.md RC4): newline-fold for the submit-await's box-vs-sent-text compare —
    // CRLF and a lone CR both read as LF, the same fold BuildPromptFill applies at inject (a
    // compose-sourced submission carries \r; the box detector emits \n — without the fold the two
    // could never compare equal).
    static std::wstring DraftSwapFoldCr(std::wstring_view s)
    {
        std::wstring out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == L'\r')
            {
                out.push_back(L'\n');
                if (i + 1 < s.size() && s[i + 1] == L'\n')
                {
                    ++i; // collapse CRLF -> one LF
                }
            }
            else
            {
                out.push_back(s[i]);
            }
        }
        return out;
    }

    // The submitter this window registers next to its injector. Called on the SENDING thread — the
    // scheduler worker, or another window's Manager UI thread for a Send-now — so it must stay cheap
    // and thread-safe: nothing here touches _appSettings, the controls, or the in-flight map (all
    // UI-thread state). HasInjector is the honest accept test; false reproduces exactly the old
    // "no injector bound yet" contract, and the caller performs its usual Rule-#4 rollback.
    bool TerminalPage::_AcceptPromptSubmission(const ::Agentmaster::PromptSubmission& submission)
    {
        if (!_sessionRegistry || !_sessionRegistry->HasInjector(submission.sessionId))
        {
            return false;
        }
        _SubmitPromptWithDraftSwap(submission); // self-marshals; owns the outcome (incl. any rollback)
        return true;
    }

    winrt::fire_and_forget TerminalPage::_SubmitPromptWithDraftSwap(::Agentmaster::PromptSubmission submission)
    {
        // Terminate-net (the _SweepClaudeLiveness idiom): an exception escaping a fire_and_forget is
        // std::terminate, so the body is an awaitable IAsyncAction whose exceptions land on this
        // co_await. The Impl additionally unlocks the control on its own failure paths — releasing
        // read-only is UI-thread-affine, so it can never ride a scope_exit that a pool-thread frame
        // teardown could run (the documented ~TerminalPage destructor-cascade crash class).
        auto strongThis{ get_strong() };
        try
        {
            co_await _SubmitPromptWithDraftSwapImpl(submission);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SubmitPromptWithDraftSwap");
        }
    }

    // One VERIFIED read of a session's input box. The live buffer read is authoritative, but the
    // detector can miss a single mid-repaint frame (Claude's Ink TUI redraws constantly — the same
    // reason _ScanPendingInput debounces its CLEAR over two ticks), so an empty result is re-read
    // once before it is believed. `remembered` (the observer's SessionInfo::pendingInput) is the
    // payload FALLBACK for a read that cannot see the box at all, per PickCurrentPromptText (§8).
    // NOT a coroutine on purpose: the caller controls the pacing between reads.
    std::wstring TerminalPage::_ReadDraftForSwap(const std::wstring& sessionId, const std::wstring& remembered)
    {
        const auto live = _ReadLiveDraftForSession(sessionId);
        return ::Agentmaster::PickCurrentPromptText(live, remembered).text;
    }

    // Release the read-only window (ONLY when we were the ones who took it — a user who had toggled
    // read-only themselves keeps it) and drop the in-flight latch. Called on EVERY exit path, never
    // from a destructor or a scope_exit, so it always runs on the UI thread. Idempotent.
    void TerminalPage::_EndDraftSwap(const std::wstring& sessionId, bool restoreInteractive)
    {
        const auto it = _draftSwapsInFlight.find(sessionId);
        const bool wasAlreadyReadOnly = it != _draftSwapsInFlight.end() ? it->second : true;
        _draftSwapsInFlight.erase(sessionId);
        if (!restoreInteractive || wasAlreadyReadOnly)
        {
            return;
        }
        try
        {
            if (const auto control = _ControlForSession(sessionId))
            {
                control.SetReadOnly(false);
            }
        }
        catch (...)
        {
            // Rule #18: the recovery (leave it as-is) is unchanged, but a control that throws while
            // being handed back to the user is exactly the kind of thing that must not vanish — the
            // symptom would be a permanently un-typeable tab with no explanation anywhere.
            ::Agentmaster::AgentLogCaughtException(L"_EndDraftSwap unlock");
        }
    }

    // Put `draft` back in the box and VERIFY it against that ground truth, returning what the box
    // finally reads. This is the one place the restore is decided, so the abort path and the normal
    // path cannot drift.
    //
    // The TUI's OWN channel is tried first, because it is the only one that returns Claude's internal
    // state rather than re-typed characters — crucially, a draft holding a "[Pasted text #N +M lines]"
    // placeholder still refers to the real paste-cache content afterwards. Which channel that is
    // depends on how the box was emptied, which is what `viaStash` carries:
    //
    //   viaStash == true   -> Ctrl+S again. The stash is a TOGGLE over one slot: pressed on an EMPTY
    //                         box it restores. That empty-box precondition is not an optimization but
    //                         a correctness gate — pressing it on a box with text would STASH that
    //                         text instead, so the box is re-read and the press is skipped unless it
    //                         is verified empty.
    //   viaStash == false  -> Ctrl+Y, yanking the kill-ring back.
    //
    // Either way the result is COMPARED against what we read before clearing, because neither channel
    // is guaranteed: a multi-press or mixed clear, or a submit that flushed the ring, can hand back
    // the wrong text. On a mismatch the box is emptied again and the draft is re-pasted verbatim
    // (BuildPromptFill — the /handover-standby channel: a bracketed paste with NO submit CR, so a
    // multi-line draft lands as one multi-line draft and nothing runs).
    //
    // `allowPaste` is false when the draft contains a paste PLACEHOLDER: re-typing
    // "[Pasted text #1 +50 lines]" would put that label in the box as literal text and silently lose
    // the content behind it — worse than leaving the box empty with the draft preserved in memory
    // (and logged).
    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> TerminalPage::_RestoreDraftAfterSwap(std::wstring sessionId, std::wstring draft, bool allowPaste, bool viaStash)
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry)
        {
            co_return winrt::hstring{};
        }
        const auto wanted = DraftSwapNormalize(draft);
        const auto sid8 = ::Agentmaster::ShortId(sessionId);
        bool stashPressedHere = false; // R6d: the disposition ledger — was the slot consumed by THIS restore?

        // A small verified read loop: poll until the box settles or the budget runs out.
        // Agentmaster (DELIVERY.md §11 / R6a — PROGRESS EXTENDS): the fixed 1.2s budget was measured
        // too short for a large stash pop's repaint (~2.4K drafts), which turned three SLOW SUCCESSES
        // into recorded failures — and every "failed" restore left the slot state unknowable, the
        // fuel of the §11 delayed detonation. While the box is still CHANGING between polls the
        // deadline extends (the clear ladder's change-detection idiom), hard-capped.
        const auto settle = [&](auto&& predicate) -> winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> {
            std::wstring seen;
            std::wstring lastSeen;
            const int64_t start = TtNowMs();
            int64_t deadline = start + kDraftSwapRestoreBudgetMs;
            while (TtNowMs() < deadline)
            {
                co_await winrt::resume_after(std::chrono::milliseconds(kDraftSwapPollMs));
                co_await wil::resume_foreground(Dispatcher());
                if (!_sessionRegistry)
                {
                    co_return winrt::hstring{};
                }
                seen = _ReadLiveDraftForSession(sessionId);
                if (predicate(seen))
                {
                    break;
                }
                if (seen != lastSeen)
                {
                    lastSeen = seen; // the box is still repainting toward something — extend, capped
                    const int64_t extended = TtNowMs() + kDraftSwapRestoreBudgetMs;
                    const int64_t hardCap = start + kDraftSwapRestoreHardCapMs;
                    deadline = extended < hardCap ? extended : hardCap;
                }
            }
            co_return winrt::hstring{ seen };
        };

        // 1. ask the TUI for it back. The stash press is GATED on the box being verified empty (see
        //    the header comment: on a non-empty box that same key stashes instead of restoring, which
        //    would hide the text rather than return it).
        std::wstring box = _ReadLiveDraftForSession(sessionId);
        // Agentmaster (DELIVERY.md RC4): VERIFY-FIRST — the TUI may have restored the draft ITSELF
        // (a stash pops back at turn end on some builds; the 07:26:59 incident's box re-population).
        // If the box already reads the draft, any press could only disturb it — a stash press on a
        // non-empty box STASHES it away again, a yank would append — so: already there ⇒ done.
        if (DraftSwapNormalize(box) == wanted)
        {
            co_return winrt::hstring{ box };
        }
        if (viaStash)
        {
            if (box.empty())
            {
                _sessionRegistry->Inject(sessionId, ::Agentmaster::BuildInputStash());
                stashPressedHere = true;
            }
        }
        else
        {
            _sessionRegistry->Inject(sessionId, ::Agentmaster::BuildInputYank());
        }
        const auto afterUndo = co_await settle([&](const std::wstring& s) { return DraftSwapNormalize(s) == wanted; });
        box.assign(afterUndo.c_str(), afterUndo.size());
        if (DraftSwapNormalize(box) == wanted)
        {
            co_return winrt::hstring{ box }; // exact — the user's own bytes, TUI-round-tripped
        }

        // 2. it came back empty, partial, or wrong. Clear whatever it left before re-pasting, so the
        //    fallback can never CONCATENATE onto a bad restore. Ctrl+U (not Ctrl+S) even in the stash
        //    case: a stash press here would put this leftover into the slot, and the slot is the one
        //    place the user's original may still be sitting.
        if (!box.empty())
        {
            _sessionRegistry->Inject(sessionId, ::Agentmaster::BuildInputKill());
            const auto afterKill = co_await settle([](const std::wstring& s) { return s.empty(); });
            box.assign(afterKill.c_str(), afterKill.size());
        }
        // Agentmaster (DELIVERY.md §11 / R6b — never end a swap with the slot believed-LOADED): a
        // stash-cleared draft whose restore reached here WITHOUT ever pressing the stash (the box
        // held junk at entry, killed above) still has the user's draft sitting in the slot — and a
        // loaded slot is a delayed detonation: claude AUTO-POPS it at a later submit, straight into
        // a future delivery's read→write window (the §11 mechanism). One un-stash press on the
        // now-VERIFIED-EMPTY box converts the landmine into a visible draft: whatever pops stays in
        // the box, is recorded by the caller, and the next swap handles it in the open. A no-op when
        // the slot was empty.
        if (viaStash && !stashPressedHere && box.empty())
        {
            _sessionRegistry->Inject(sessionId, ::Agentmaster::BuildInputStash());
            stashPressedHere = true;
            const auto popped = co_await settle([&](const std::wstring& s) { return !s.empty(); });
            box.assign(popped.c_str(), popped.size());
            if (DraftSwapNormalize(box) == wanted)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-swap] " + sid8 + L" restore: the un-stash rung recovered the draft (slot=consumed)\n");
                co_return winrt::hstring{ box }; // the slot held the draft after all — recovered late
            }
            if (!box.empty())
            {
                // Something ELSE popped (an older stash). Leave it VISIBLE — the caller records it,
                // the pending scan tracks it, and no future submit can detonate it invisibly.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-swap] " + sid8 + L" restore: the un-stash rung popped DIFFERENT content (chars=" + std::to_wstring(box.size()) + L") - left visible in the box (slot=consumed), the draft stays in memory\n");
                co_return winrt::hstring{ box };
            }
        }
        if (!allowPaste || !box.empty())
        {
            // Either re-typing would corrupt a paste placeholder, or we could not get the box back to
            // a known state. Stop touching it and report what is actually there; the caller keeps the
            // draft in memory (and the kill-ring still holds it for a manual Ctrl+Y).
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[draft-swap] " + sid8 + L" restore gave up (box chars=" + std::to_wstring(box.size()) +
                                              L", slot=" + (viaStash ? (stashPressedHere ? L"consumed" : L"maybe-loaded") : L"n/a") + L")\n");
            co_return winrt::hstring{ box };
        }
        _sessionRegistry->Inject(sessionId, ::Agentmaster::BuildPromptFill(draft));
        const auto afterPaste = co_await settle([&](const std::wstring& s) { return DraftSwapNormalize(s) == wanted; });
        co_return afterPaste;
    }

    // Agentmaster (DELIVERY.md §11 / DELIVERY_PLAN.md R4 — the VERIFIED SEND core): the closed-loop
    // replacement for the blind paste+CR. FILL (bracketed paste, NO CR) → settle → READ THE BOX BACK
    // (the deep probe — a filled prompt can render taller than the scan window) → commit the lone CR
    // only on a read that verifies as exactly-the-prompt (whitespace-tolerant) or its collapsed
    // placeholder render (claude re-collapses a big pasted fill — the handover paste tier). Recovery
    // per verdict:
    //   Eaten    → re-fill (≤ kSendVerifyMaxFills), then give up (strike).
    //   Partial  → OUR text, incomplete: discard it (the mail-clear's End+Ctrl+U ladder — a true,
    //              mid-turn-safe discard; nothing foreign is at risk) and re-fill (counts as a fill).
    //   Foreign  → the merge, caught BEFORE the commit: undo our insertion in verified backspace
    //              batches (budget == the folded prompt length EXACTLY — backspaces delete editor
    //              characters, soft wrap is render-only, and over-deletion would eat the foreign
    //              text, which is the user's until proven otherwise), record what remains as
    //              pendingInput (the dots + memory now show what the reads had missed), strike.
    //   NoBox/MenuOpen/Unknown post-fill → cannot verify: no CR, no blind undo (an unreadable box
    //              must not be blind-backspaced), record the box state, strike.
    // A STRIKE rolls the prompt back (caller-side) — but the SECOND strike for one prompt marks it
    // Failed instead (return 2): a rollback would re-fire the same doomed verify at advance cadence
    // forever, the RC2 mark/decline/rollback livelock in a new coat. §12: the terminal strike no
    // longer flips the autorunner mode to Off — the Failed prompt is the bounded resolution and the
    // MODE stays the user's (the walls that could burn a whole queue are held upstream by the
    // strike-free pre-flight decline + the recorded box state).
    // `gateTag` is THIS delivery attempt's gate claim (DeliveryGateTagFor over the nonce-stamped
    // submission): it is REVALIDATED at each fill and immediately before the irreversible CR, so a
    // delivery that lost its claim (expired + reclaimed by a newer attempt while this one starved
    // in a backlogged dispatcher) ABORTS instead of typing into a box another carrier now owns.
    // Returns 1 == delivered (CR fired), 0 == not delivered (caller rolls back), 2 == not delivered
    // AND terminally Failed here (caller must NOT roll back), 3 == aborted stale — the gate claim
    // was lost mid-flight (caller must NOT roll back either: the prompt now belongs to whichever
    // newer attempt took the claim, or to the reclaim verdict that lapsed it). Caller holds the box
    // (read-only lock + _draftSwapsInFlight) and has cleared/verified it empty-ish; UI thread.
    winrt::Windows::Foundation::IAsyncOperation<int32_t> TerminalPage::_InjectPromptVerified(std::wstring sessionId, std::wstring sid8, std::wstring promptId, std::wstring text, std::wstring gateTag)
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry)
        {
            co_return 0;
        }
        constexpr auto stNoBox = static_cast<int32_t>(::Agentmaster::InputBoxState::NoBox);
        constexpr auto stEmpty = static_cast<int32_t>(::Agentmaster::InputBoxState::Empty);
        constexpr auto stMenu = static_cast<int32_t>(::Agentmaster::InputBoxState::MenuOpen);
        constexpr auto stUnknown = static_cast<int32_t>(::Agentmaster::InputBoxState::Unknown);

        // The strike ledger: an unverifiable delivery rolls back ONCE; the second one for the same
        // prompt resolves terminally (Failed, the lost-send idiom) so the advance can never livelock
        // re-trying a delivery that provably cannot verify. §12: the terminal strike keeps the
        // autorunner MODE — Failed already stops THIS prompt from re-firing, the queue proceeds
        // (bounded: ≤ kSendVerifyStrikeLimit deliveries per prompt), and a persistent wall is held
        // upstream by the strike-free pre-flight decline + recorded box state. Only a detected
        // POST-COMMIT merge (an actually-mangled message on the transcript — the R7 classifier) and
        // the give-up ladder still pause; a REFUSED merge is the protection working, not a fault to
        // punish the user's mode for.
        const auto strike = [&](const std::wstring& why) -> int32_t {
            const int32_t strikes = ++_sendVerifyStrikes[promptId];
            if (strikes < kSendVerifyStrikeLimit)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[send-verify] " + sid8 + L" prompt " + ::Agentmaster::ShortId(promptId) + L" NOT delivered: " + why +
                                                  L" (strike " + std::to_wstring(strikes) + L"/" + std::to_wstring(kSendVerifyStrikeLimit) + L" - prompt rolls back to Pending)\n");
                return 0;
            }
            _sendVerifyStrikes.erase(promptId);
            _sessionRegistry->Update(sessionId, [&](::Agentmaster::SessionInfo& ss) {
                for (auto& p : ss.queue)
                {
                    if (p.id == promptId && p.status == ::Agentmaster::PromptStatus::Sent && !p.echoed)
                    {
                        p.status = ::Agentmaster::PromptStatus::Failed;
                        break;
                    }
                }
            });
            const std::wstring line = L"[send-verify] " + sid8 + L" prompt " + ::Agentmaster::ShortId(promptId) +
                                      L" FAILED after " + std::to_wstring(kSendVerifyStrikeLimit) + L" unverifiable deliveries (" + why +
                                      L") - not resent; the autorunner keeps its mode and continues with the next prompt\n";
            ::Agentmaster::AppendStateLog(L"hooks.log", line);
            ::Agentmaster::AppendStateLog(L"autorunner.log", line);
            return 2;
        };

        int32_t fills = 0;
        while (fills < kSendVerifyMaxFills)
        {
            // §12: re-assert THIS attempt's gate claim before typing anything (and refresh its
            // expiry anchor — a slow verify must never expire out from under itself). A lost claim
            // means a newer attempt (or the reclaim verdict) owns this prompt now: abort, type
            // nothing, and do not touch the queue.
            if (!_sessionRegistry->RevalidateDeliveryGate(sessionId, gateTag))
            {
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[send-verify] " + sid8 + L" prompt " + ::Agentmaster::ShortId(promptId) +
                                                  L" stale delivery aborted before fill " + std::to_wstring(fills + 1) +
                                                  L" (the gate claim was lost - a newer attempt owns this prompt)\n");
                co_return 3;
            }
            if (!_sessionRegistry->Inject(sessionId, ::Agentmaster::BuildPromptFill(text)))
            {
                co_return 0; // injector vanished — nothing typed; the caller rolls back
            }
            ++fills;

            // Settle: poll the deep probe until the box verifies, progress-extending while it is
            // still changing (a large paste renders over several frames), hard-capped.
            const int64_t settleStart = TtNowMs();
            int64_t deadline = settleStart + kSendVerifySettleMs;
            int32_t boxState = stUnknown;
            std::wstring box;
            std::wstring lastSeen;
            auto verdict = ::Agentmaster::FillVerify::Eaten;
            while (TtNowMs() < deadline)
            {
                co_await winrt::resume_after(std::chrono::milliseconds(kDraftSwapPollMs));
                co_await wil::resume_foreground(Dispatcher());
                if (!_sessionRegistry)
                {
                    co_return 0;
                }
                std::tie(boxState, box) = _ReadInputBoxProbeForSession(sessionId, kSendVerifyProbeRows);
                if (boxState == stEmpty || boxState == static_cast<int32_t>(::Agentmaster::InputBoxState::Draft))
                {
                    verdict = ::Agentmaster::VerifyFillAgainstPrompt(box, text);
                    if (verdict == ::Agentmaster::FillVerify::Verified || verdict == ::Agentmaster::FillVerify::VerifiedCollapsed)
                    {
                        break;
                    }
                }
                if (box != lastSeen)
                {
                    lastSeen = box; // progress — extend, capped
                    const int64_t extended = TtNowMs() + kSendVerifySettleMs;
                    const int64_t hardCap = settleStart + kSendVerifySettleHardCapMs;
                    deadline = extended < hardCap ? extended : hardCap;
                }
            }

            // ---- the COMMIT, or the recovery ----
            if (boxState == stEmpty || boxState == static_cast<int32_t>(::Agentmaster::InputBoxState::Draft))
            {
                verdict = ::Agentmaster::VerifyFillAgainstPrompt(box, text);
            }
            else
            {
                // Unreadable after our own fill (NoBox: the render outgrew even the deep window, or
                // drifted; MenuOpen: a menu popped mid-fill; Unknown: the control tore down). No CR
                // — a blind commit is the §11 bug — and no blind undo either (backspacing a box we
                // cannot read could eat anything). Record the state so the advance holds.
                if (boxState == stNoBox || boxState == stMenu)
                {
                    _sessionRegistry->SetPendingBoxState(sessionId, static_cast<::Agentmaster::InputBoxState>(boxState));
                }
                co_return strike(boxState == stMenu ? L"a menu opened over the input box after the fill" :
                                                      L"the input box was unreadable after the fill");
            }

            if (verdict == ::Agentmaster::FillVerify::Verified || verdict == ::Agentmaster::FillVerify::VerifiedCollapsed)
            {
                // §12: the LAST check before the one irreversible step — the CR commits whatever the
                // box holds, so it must only ever fire while this attempt still owns the box. The
                // settle above co_awaited (a starved dispatcher can stall there arbitrarily long), so
                // the claim is re-asserted at the commit itself, not just at the fill.
                if (!_sessionRegistry->RevalidateDeliveryGate(sessionId, gateTag))
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log",
                                                  L"[send-verify] " + sid8 + L" prompt " + ::Agentmaster::ShortId(promptId) +
                                                      L" stale delivery aborted at the CR commit (the gate claim was lost; the verified fill stays visible as a draft)\n");
                    co_return 3;
                }
                if (!_sessionRegistry->Inject(sessionId, std::wstring(1, L'\r')))
                {
                    co_return 0; // injector vanished between fill and CR — the fill stays visible as a draft; caller rolls back
                }
                _sendVerifyStrikes.erase(promptId);
                _sessionRegistry->MarkPromptInjected(sessionId, promptId); // §12: injection evidence for the lost-send verdict
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[delivered] " + sid8 + L" prompt " + ::Agentmaster::ShortId(promptId) +
                                                  L" (chars=" + std::to_wstring(text.size()) +
                                                  L", verified=" + (verdict == ::Agentmaster::FillVerify::Verified ? L"exact" : L"collapsed") +
                                                  (fills > 1 ? L", fills=" + std::to_wstring(fills) : L"") + L")\n");
                co_return 1;
            }

            if (verdict == ::Agentmaster::FillVerify::Eaten)
            {
                // Verified-empty past the settle: the TUI ate the paste pre-raw-mode (the standby
                // lane's measured race). Nothing of ours is in the box — safe to re-fill.
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[send-verify] " + sid8 + L" fill " + std::to_wstring(fills) + L"/" + std::to_wstring(kSendVerifyMaxFills) +
                                                  L" was eaten (box verified empty) - " + (fills < kSendVerifyMaxFills ? L"re-filling" : L"giving up") + L"\n");
                continue;
            }

            if (verdict == ::Agentmaster::FillVerify::Partial)
            {
                // OUR text, incomplete (the paste's tail was eaten / never finished rendering).
                // Discard it with the true-discard ladder (End+Ctrl+U — never the stash: this text
                // must NOT come back) and re-fill; the attempt is spent either way.
                ::Agentmaster::DraftDiscardProgress spent;
                std::wstring cur = box;
                const int64_t discardDeadline = TtNowMs() + kDraftDiscardBudgetMs;
                while (TtNowMs() < discardDeadline)
                {
                    const auto plan = ::Agentmaster::DecideDraftDiscard(cur, spent);
                    if (plan.action == ::Agentmaster::DraftDiscardAction::Done || plan.action == ::Agentmaster::DraftDiscardAction::GiveUp)
                    {
                        break;
                    }
                    if (plan.action == ::Agentmaster::DraftDiscardAction::EndKill)
                    {
                        _sessionRegistry->Inject(sessionId, ::Agentmaster::BuildInputEnd() + ::Agentmaster::BuildInputKill());
                        ++spent.killRounds;
                    }
                    else
                    {
                        _sessionRegistry->Inject(sessionId, ::Agentmaster::BuildInputEnd() + ::Agentmaster::BuildBackspaces(plan.backspaces));
                        ++spent.backspaceRounds;
                    }
                    const std::wstring before = cur;
                    for (int64_t settled = 0; settled <= kDraftSwapActionSettleMs; settled += kDraftSwapPollMs)
                    {
                        co_await winrt::resume_after(std::chrono::milliseconds(kDraftSwapPollMs));
                        co_await wil::resume_foreground(Dispatcher());
                        if (!_sessionRegistry)
                        {
                            co_return 0;
                        }
                        cur = _ReadInputBoxProbeForSession(sessionId, kSendVerifyProbeRows).second;
                        if (cur != before)
                        {
                            break;
                        }
                    }
                    if (cur == before)
                    {
                        (plan.action == ::Agentmaster::DraftDiscardAction::EndKill ? spent.killStalls : spent.backspaceStalls) += 1;
                    }
                    else
                    {
                        (plan.action == ::Agentmaster::DraftDiscardAction::EndKill ? spent.killStalls : spent.backspaceStalls) = 0;
                    }
                }
                if (!cur.empty())
                {
                    co_return strike(L"a partial fill would not clear for the re-fill");
                }
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[send-verify] " + sid8 + L" fill " + std::to_wstring(fills) + L"/" + std::to_wstring(kSendVerifyMaxFills) +
                                                  L" landed PARTIAL - discarded, " + (fills < kSendVerifyMaxFills ? L"re-filling" : L"giving up") + L"\n");
                continue;
            }

            // ---- Foreign: the merge, caught pre-commit ----
            {
                std::wstring head = box.substr(0, 48);
                for (auto& ch : head)
                {
                    if (ch == L'\n' || ch == L'\r' || ch == L'\t')
                    {
                        ch = L' ';
                    }
                }
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[send-verify] " + sid8 + L" FOREIGN read-back after the fill (box chars=" + std::to_wstring(box.size()) +
                                                  L" head=\"" + head + L"\") - the merge this would have submitted is refused; undoing our insertion\n");
                // Undo our insertion in verified batches. Budget == the FOLDED prompt length exactly:
                // a backspace deletes one editor character (soft wrap is render-only), so pressing
                // more could only eat the foreign text.
                size_t budget = ::Agentmaster::FoldCrToLf(text).size();
                int32_t stalls = 0;
                std::wstring cur = box;
                const int64_t undoDeadline = TtNowMs() + kSendUndoBudgetMs;
                while (budget > 0 && stalls < 2 && TtNowMs() < undoDeadline)
                {
                    const auto plan = ::Agentmaster::DecideSendUndo(cur, text);
                    if (plan.action != ::Agentmaster::SendUndoAction::Press || plan.presses == 0)
                    {
                        break; // Done (our tail is gone) or Stop (ambiguous — never press further)
                    }
                    size_t press = plan.presses < budget ? plan.presses : budget;
                    if (press > 512)
                    {
                        press = 512; // batch: re-verify between chunks
                    }
                    _sessionRegistry->Inject(sessionId, ::Agentmaster::BuildBackspaces(press));
                    budget -= press;
                    const std::wstring before = cur;
                    for (int64_t settled = 0; settled <= kDraftSwapActionSettleMs; settled += kDraftSwapPollMs)
                    {
                        co_await winrt::resume_after(std::chrono::milliseconds(kDraftSwapPollMs));
                        co_await wil::resume_foreground(Dispatcher());
                        if (!_sessionRegistry)
                        {
                            co_return 0;
                        }
                        cur = _ReadInputBoxProbeForSession(sessionId, kSendVerifyProbeRows).second;
                        if (cur != before)
                        {
                            break;
                        }
                    }
                    stalls = (cur == before) ? stalls + 1 : 0;
                }
                // Record what the box actually holds now — the content every read had missed. The
                // dots/memory go honest, the next swap's clear handles it, and the pending scan
                // keeps it fresh from here.
                if (!cur.empty())
                {
                    _sessionRegistry->SetPendingInput(sessionId, cur);
                }
                co_return strike(L"foreign content in the box (merge refused, our insertion undone)");
            }
        }
        co_return strike(L"the fill never appeared in the box (eaten x" + std::to_wstring(kSendVerifyMaxFills) + L")");
    }

    winrt::Windows::Foundation::IAsyncAction TerminalPage::_SubmitPromptWithDraftSwapImpl(::Agentmaster::PromptSubmission submission)
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());

        const auto& id = submission.sessionId;
        const auto sid8 = ::Agentmaster::ShortId(id);
        if (!_sessionRegistry)
        {
            co_return; // registry gone == process teardown; the gate it owned is gone with it
        }
        // Agentmaster (DELIVERY.md): the registry opened this session's delivery gate when it
        // ACCEPTED this submission; WE own closing it — on EVERY exit, which is what un-holds the
        // scheduler's advance (the close notifies) and re-admits the Enter-retry watch. A scope
        // guard is legal here, unlike the read-only release: CloseDeliveryGate is registry-only
        // work (thread-safe, no UI affinity), so running it on whatever thread unwinds this frame
        // is safe — the UI-affine unlock stays an explicit call on every path (_EndDraftSwap). A
        // close after an expiry-reclaim / an Upsert re-key is an owner-mismatch no-op in the
        // registry, so this guard can never clear a younger delivery's claim.
        auto gateCloser = wil::scope_exit([gateRegistry = _sessionRegistry, gateId = submission.sessionId, gateTag = ::Agentmaster::DeliveryGateTagFor(submission)]() noexcept {
            try
            {
                gateRegistry->CloseDeliveryGate(gateId, gateTag);
            }
            catch (...)
            {
                // Rule #18 — and a noexcept-destructor context: a throw escaping here would be
                // std::terminate, so the containment is load-bearing, not just forensics.
                ::Agentmaster::AgentLogCaughtException(L"_SubmitPromptWithDraftSwap gate close");
            }
        });

        // ---- the fast paths: no swap needed, inject exactly as before ----
        const auto plainSend = [&]() -> bool {
            const bool ok = _sessionRegistry->Inject(id, ::Agentmaster::BuildPromptSubmission(submission.text));
            if (ok)
            {
                // DELIVERY.md RC5: [send] in autorunner.log is "accepted for delivery"; THIS is the
                // injection actually reaching the ConPTY — the line that separates "8 sends logged,
                // 0 delivered" from reality.
                _sessionRegistry->MarkPromptInjected(id, submission.promptId); // §12: injection evidence
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[delivered] " + sid8 + L" prompt " + ::Agentmaster::ShortId(submission.promptId) +
                                                  L" (chars=" + std::to_wstring(submission.text.size()) + L")\n");
            }
            return ok;
        };

        // ---- STALE-DELIVERY GUARD (DELIVERY.md §12) ----
        // The resume_foreground above is the delivery's FIRST dispatcher hop, and it can starve for
        // MINUTES on a backlogged UI thread (measured 83s during a 51-tab window restore). By the
        // time this body runs, the world may have moved on: the gate expired and the reclaim verdict
        // rolled the prompt back to Pending (a fresh attempt may already carry it — with its OWN
        // nonce tag), or a verdict resolved it otherwise. Typing now would deliver a prompt the
        // system no longer accounts as in-flight — the recorded incident's duplicate "Typed" row.
        // So: the prompt must still be Sent, and THIS attempt's gate claim must still stand
        // (RevalidateDeliveryGate re-stamps an expired-but-unreclaimed claim — the holder was alive
        // all along, merely starved). Either failing aborts WITHOUT a rollback: a no-longer-Sent
        // prompt was resolved by someone with fresher evidence, and a lost claim means a newer
        // attempt owns it.
        const auto gateTag = ::Agentmaster::DeliveryGateTagFor(submission);
        {
            bool stillSent = false;
            if (const auto pre = _sessionRegistry->Get(id))
            {
                for (const auto& p : pre->queue)
                {
                    if (p.id == submission.promptId && p.status == ::Agentmaster::PromptStatus::Sent)
                    {
                        stillSent = true;
                        break;
                    }
                }
            }
            if (!stillSent || !_sessionRegistry->RevalidateDeliveryGate(id, gateTag))
            {
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[send-verify] " + sid8 + L" prompt " + ::Agentmaster::ShortId(submission.promptId) +
                                                  L" stale delivery aborted at the top guard (" +
                                                  (stillSent ? L"the gate claim was lost to a newer attempt" : L"the prompt is no longer Sent") +
                                                  L") - nothing typed\n");
                co_return; // the gateCloser releases our claim iff we still hold it (owner-matched)
            }
        }
        const auto info = _sessionRegistry->Get(id);
        const auto control = _ControlForSession(id);
        const bool verifyOn = _appSettings.verifySendBeforeSubmit;
        if (!info || !control ||
            control.ConnectionState() == TerminalConnection::ConnectionState::NotConnected)
        {
            // Session gone, or no readable buffer (dormant / not hosted here) — there is no box to
            // protect OR to verify against, so this is the historical injection verbatim.
            if (!plainSend())
            {
                _sessionRegistry->RollbackPromptToPending(id, submission.promptId, submission.refundAutoSend);
            }
            co_return;
        }
        if (!_appSettings.preserveDraftOnSend && !verifyOn)
        {
            // Both protections off — the historical one-write paste+CR, byte-identical.
            if (!plainSend())
            {
                _sessionRegistry->RollbackPromptToPending(id, submission.promptId, submission.refundAutoSend);
            }
            co_return;
        }
        if (_draftSwapsInFlight.count(id) != 0)
        {
            // A swap is already holding this session's box. Two concurrent sends into one session
            // should not happen (the pickup guard keeps the queue to one prompt per turn), and
            // injecting alongside a swap is precisely the merge we are preventing — so decline and
            // let the prompt re-fire on the next advance.
            _sessionRegistry->RollbackPromptToPending(id, submission.promptId, submission.refundAutoSend);
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-swap] " + sid8 + L" declined: a swap is already in flight (prompt stays Pending)\n");
            co_return;
        }

        // ---- 0. PRE-FLIGHT (DELIVERY_PLAN.md R5) ----
        // One fresh tri-state probe BEFORE anything is typed: a MENU on screen would EAT the paste
        // (the §9 dialog that silently consumed a delivered prompt), and a bare NoBox is exactly
        // where the §11 invisible-content merge lived. A single settle re-read absorbs a mid-repaint
        // frame; a persisting NoBox/MenuOpen DECLINES the delivery — the recorded state makes
        // DecideAdvance hold (no mark/decline livelock), the scan releases the hold the moment a box
        // renders again, and a NoBox that persists with queued work escalates via the scanner
        // (ShouldWarnOnBoxNotVisible - a once-per-episode warning since §12, never a mode change).
        constexpr auto stNoBox = static_cast<int32_t>(::Agentmaster::InputBoxState::NoBox);
        constexpr auto stMenu = static_cast<int32_t>(::Agentmaster::InputBoxState::MenuOpen);
        std::wstring probeText;
        if (verifyOn)
        {
            auto [probeState, probeRead] = _ReadInputBoxProbeForSession(id, kSendVerifyProbeRows);
            if (probeState == stNoBox || probeState == stMenu)
            {
                co_await winrt::resume_after(std::chrono::milliseconds(kDraftSwapActionSettleMs));
                co_await wil::resume_foreground(Dispatcher());
                if (!_sessionRegistry)
                {
                    co_return;
                }
                std::tie(probeState, probeRead) = _ReadInputBoxProbeForSession(id, kSendVerifyProbeRows);
            }
            if (probeState == stNoBox || probeState == stMenu)
            {
                _sessionRegistry->SetPendingBoxState(id, static_cast<::Agentmaster::InputBoxState>(probeState));
                _sessionRegistry->RollbackPromptToPending(id, submission.promptId, submission.refundAutoSend);
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[send-verify] " + sid8 + L" declined: no parseable input box (" +
                                                  (probeState == stMenu ? L"a menu/dialog is open - a paste would feed the MENU" : L"box not visible") +
                                                  L") - prompt stays Pending, the advance holds until a box renders\n");
                co_return;
            }
            probeText = std::move(probeRead);
        }

        // ---- 1. READ ----
        // With the probe in hand its text IS the live read (fresh + deep); otherwise the classic
        // shallow read. Either way the observer's remembered draft is the payload fallback.
        std::wstring draft = verifyOn ? ::Agentmaster::PickCurrentPromptText(probeText, info->pendingInput).text :
                                        _ReadDraftForSwap(id, info->pendingInput);
        if (draft.empty())
        {
            // The box may simply be repainting. One short re-read before we believe "nothing to
            // protect" — cheap, and the alternative (a missed draft) is the whole bug.
            co_await winrt::resume_after(std::chrono::milliseconds(kDraftSwapPollMs));
            co_await wil::resume_foreground(Dispatcher());
            if (!_sessionRegistry)
            {
                co_return;
            }
            draft = _ReadDraftForSwap(id, info->pendingInput);
        }
        if (draft.empty())
        {
            if (!verifyOn)
            {
                if (!plainSend()) // empty box — a paste cannot merge into anything
                {
                    _sessionRegistry->RollbackPromptToPending(id, submission.promptId, submission.refundAutoSend);
                }
                co_return;
            }
            // VERIFIED fast path (R4): even an empty-box send is closed-loop now — Incident 3's
            // merge went through exactly this branch ("nothing to protect" ⇒ blind paste+CR, while
            // TUI-internal content materialized into the ≤60ms read→write window). LOCK the control
            // so the user's keystrokes can't interleave either (the TUI's own inserts are what the
            // read-back catches), fill → verify → CR, hand the tab back.
            bool fastWasReadOnly = true;
            try
            {
                fastWasReadOnly = control.ReadOnly();
                if (!fastWasReadOnly)
                {
                    control.SetReadOnly(true);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_SubmitPromptWithDraftSwap fast-path lock");
                fastWasReadOnly = true; // never hand back a read-only we did not take
            }
            _draftSwapsInFlight[id] = fastWasReadOnly;
            const int32_t delivered = co_await _InjectPromptVerified(id, sid8, submission.promptId, submission.text, gateTag);
            if (!_sessionRegistry)
            {
                _EndDraftSwap(id, true);
                co_return;
            }
            if (delivered == 0)
            {
                _sessionRegistry->RollbackPromptToPending(id, submission.promptId, submission.refundAutoSend);
            }
            _EndDraftSwap(id, true);
            co_return;
        }
        if (!_appSettings.preserveDraftOnSend)
        {
            // Preservation OFF but verify ON, and the box HOLDS a draft: the historical behavior
            // merged here. The verified send refuses the merge instead — the fill reads back
            // Foreign, our insertion is undone, and the prompt rolls back (twice ⇒ Failed + paused,
            // loudly). The user chose not to have drafts rescued; they did not choose mangled
            // messages.
            bool offWasReadOnly = true;
            try
            {
                offWasReadOnly = control.ReadOnly();
                if (!offWasReadOnly)
                {
                    control.SetReadOnly(true);
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_SubmitPromptWithDraftSwap preserve-off lock");
                offWasReadOnly = true;
            }
            _draftSwapsInFlight[id] = offWasReadOnly;
            const int32_t delivered = co_await _InjectPromptVerified(id, sid8, submission.promptId, submission.text, gateTag);
            if (!_sessionRegistry)
            {
                _EndDraftSwap(id, true);
                co_return;
            }
            if (delivered == 0)
            {
                _sessionRegistry->RollbackPromptToPending(id, submission.promptId, submission.refundAutoSend);
            }
            _EndDraftSwap(id, true);
            co_return;
        }

        // A draft holding a "[Pasted text #N +M lines]" placeholder can only be restored by the TUI's
        // own kill-ring: the placeholder is a LABEL for content living in Claude's paste-cache, so
        // re-typing those characters would put the label in the box as literal text and silently drop
        // the content behind it. When one is present the paste fallback is therefore refused, and a
        // failed yank leaves the box empty with the draft preserved in memory + the ring (both logged)
        // rather than corrupted in place. (PENDING_INPUT.md §2b owns the placeholder format.)
        const bool allowPasteRestore = ::Agentmaster::FindPasteMarkers(draft).empty();

        // ---- 2. LOCK ----
        bool wasAlreadyReadOnly = true;
        try
        {
            wasAlreadyReadOnly = control.ReadOnly();
            if (!wasAlreadyReadOnly)
            {
                control.SetReadOnly(true);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SubmitPromptWithDraftSwap lock");
            wasAlreadyReadOnly = true; // never try to hand back a read-only we did not take
        }
        _draftSwapsInFlight[id] = wasAlreadyReadOnly;
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-swap] " + sid8 + L" holding an unsent draft (chars=" + std::to_wstring(draft.size()) + L") - input locked, clearing the box via " + std::wstring(_appSettings.draftSwapUseCtrlS ? L"Ctrl+S stash" : L"the kill-ring") + L"\n");

        // ---- 3. CLEAR (verified) ----
        // Each rung is followed by a SETTLE, not a single poll: one 60ms read can easily outrun the
        // TUI's repaint, and reading the pre-repaint box would look like "the keystroke changed
        // nothing" — which DecideDraftClear rightly treats as "this binding is not ours" and would
        // abandon a Ctrl+U that was in fact about to work. So after every injection we wait for the
        // box to actually CHANGE (bounded by kDraftSwapActionSettleMs) before judging the rung.
        ::Agentmaster::DraftClearProgress spent;
        spent.useStash = _appSettings.draftSwapUseCtrlS;
        std::wstring box = draft;
        bool cleared = false;
        const int64_t clearDeadline = TtNowMs() + kDraftSwapClearBudgetMs;
        while (TtNowMs() < clearDeadline)
        {
            const auto plan = ::Agentmaster::DecideDraftClear(box, spent);
            if (plan.action == ::Agentmaster::DraftClearAction::Done)
            {
                cleared = true;
                break;
            }
            if (plan.action == ::Agentmaster::DraftClearAction::GiveUp)
            {
                break;
            }
            if (plan.action == ::Agentmaster::DraftClearAction::Stash)
            {
                _sessionRegistry->Inject(id, ::Agentmaster::BuildInputStash());
                ++spent.stashPresses;
            }
            else if (plan.action == ::Agentmaster::DraftClearAction::Kill)
            {
                _sessionRegistry->Inject(id, ::Agentmaster::BuildInputKill());
                ++spent.killPresses;
            }
            else
            {
                _sessionRegistry->Inject(id, ::Agentmaster::BuildBackspaces(plan.backspaces));
                ++spent.backspaceRounds;
            }
            const std::wstring before = box;
            for (int64_t settled = 0; settled <= kDraftSwapActionSettleMs; settled += kDraftSwapPollMs)
            {
                co_await winrt::resume_after(std::chrono::milliseconds(kDraftSwapPollMs));
                co_await wil::resume_foreground(Dispatcher());
                if (!_sessionRegistry)
                {
                    // Defensive only (_sessionRegistry is assigned once at engine init and never cleared), but a
                    // bail after the latch MUST still hand the tab back — a stranded read-only control is an
                    // un-typeable terminal with nothing on screen to explain it.
                    _EndDraftSwap(id, true);
                    co_return;
                }
                // Deliberately NOT PickCurrentPromptText here: mid-swap the remembered value is the draft
                // we are trying to erase, so falling back to it would report the box as never-empty.
                box = _ReadLiveDraftForSession(id);
                if (box != before)
                {
                    break; // the keystroke landed and repainted — judge the rung on this
                }
            }
            spent.shrank = box.size() < before.size();
        }
        // Which rung actually emptied the box decides how it is put back: a stash is restored by the
        // SAME Ctrl+S toggle, a kill by Ctrl+Y. Getting this wrong is not a cosmetic mistake — pressing
        // Ctrl+S on a box that was never stashed would STASH whatever is in it.
        const bool clearedByStash = cleared && spent.stashPresses > 0 && spent.killPresses == 0 && spent.backspaceRounds == 0;

        // ---- 4. ABORT ----
        if (!cleared)
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-swap] " + sid8 + L" ABORTED: the input box would not clear after " + std::to_wstring(spent.stashPresses) + L" stash + " + std::to_wstring(spent.killPresses) + L" kill press(es) + " + std::to_wstring(spent.backspaceRounds) + L" backspace round(s) - nothing sent, prompt stays Pending\n");
            _sessionRegistry->RollbackPromptToPending(id, submission.promptId, submission.refundAutoSend);
            // An abort must be a no-op for the USER too. The ladder may have erased PART of the draft
            // (a backspace round that ran out of budget), so put it back through the same verified
            // restore the success path uses rather than assuming the box was left untouched.
            if (DraftSwapNormalize(box) != DraftSwapNormalize(draft))
            {
                const auto recovered = co_await _RestoreDraftAfterSwap(id, draft, allowPasteRestore, clearedByStash);
                if (!_sessionRegistry)
                {
                    _EndDraftSwap(id, true);
                    co_return;
                }
                box.assign(recovered.c_str(), recovered.size());
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-swap] " + sid8 + (DraftSwapNormalize(box) == DraftSwapNormalize(draft) ? L" draft recovered after the aborted clear\n" : L" draft only PARTLY recovered after the aborted clear - the full text is kept in memory (Copy Current Prompt)\n"));
            }
            _sessionRegistry->SetPendingInput(id, box.empty() ? draft : box);
            _EndDraftSwap(id, true);
            co_return;
        }

        // ---- 5. SEND (VERIFIED when the R4 switch is on: fill → read-back → CR) ----
        const int64_t sentAtMs = TtNowMs();
        bool sendOk = false;
        int32_t verifiedResult = -1; // -1 == the legacy path ran; 0/1/2 == _InjectPromptVerified's verdict
        if (verifyOn)
        {
            verifiedResult = co_await _InjectPromptVerified(id, sid8, submission.promptId, submission.text, gateTag);
            if (!_sessionRegistry)
            {
                _EndDraftSwap(id, true);
                co_return;
            }
            sendOk = (verifiedResult == 1);
        }
        else
        {
            sendOk = plainSend();
        }
        if (!sendOk)
        {
            // _InjectPromptVerified already logged its own reason (and on its terminal verdict, 2,
            // already marked the prompt Failed — never roll THAT back to Pending, it would resurrect
            // a resolved prompt; a stale abort, 3, means a NEWER attempt owns the prompt — never
            // touch it from this superseded carrier either); the legacy path logs here as before.
            if (verifiedResult == -1)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-swap] " + sid8 + L" send failed after clearing - restoring the draft, prompt stays Pending\n");
            }
            if (verifiedResult != 2 && verifiedResult != 3)
            {
                _sessionRegistry->RollbackPromptToPending(id, submission.promptId, submission.refundAutoSend);
            }
            const auto recovered = co_await _RestoreDraftAfterSwap(id, draft, allowPasteRestore, clearedByStash);
            if (!_sessionRegistry)
            {
                _EndDraftSwap(id, true);
                co_return;
            }
            _sessionRegistry->SetPendingInput(id, recovered.empty() ? draft : std::wstring{ recovered.c_str(), recovered.size() });
            _EndDraftSwap(id, true);
            co_return;
        }

        // ---- 6. AWAIT the prompt leaving the box ----
        // "Submitted" is a turn-started signal (the same three DecideEnterRetry trusts: the state left
        // the ready set, a UserPromptSubmit stamped turns.lastPromptUnixMs, or the transcript advanced)
        // AND a box that no longer holds THE PROMPT. The box gate exists to prevent restoring into a
        // box that still holds the un-submitted prompt — the merge this feature exists to prevent —
        // so the test is "the box does not read the sent text": empty, OR reading something else.
        //
        // ⚠ Deliberately NOT "box empty" (DELIVERY.md RC4 — the recorded 07:26:59 incident): claude
        // can repopulate the box right after a successful submit (a Ctrl+S stash popping back at turn
        // end, a repaint the detector half-reads), and the empty-box demand held this await for its
        // FULL 18s budget on a send that had demonstrably submitted (echo at +2s, clean Stop at +8s)
        // — keeping the gate + latch held, deferring the restore, and feeding the advance livelock.
        const auto sentNorm = DraftSwapNormalize(DraftSwapFoldCr(submission.text));
        bool submitted = false;
        for (int64_t waited = 0; waited <= kDraftSwapSubmitBudgetMs; waited += kDraftSwapPollMs)
        {
            co_await winrt::resume_after(std::chrono::milliseconds(kDraftSwapPollMs));
            co_await wil::resume_foreground(Dispatcher());
            if (!_sessionRegistry)
            {
                _EndDraftSwap(id, true);
                co_return;
            }
            box = _ReadLiveDraftForSession(id);
            const auto now = _sessionRegistry->Get(id);
            const bool turnStarted = now &&
                                     ((now->state != ::Agentmaster::SessionState::Idle && now->state != ::Agentmaster::SessionState::WaitingForInput) ||
                                      now->turns.lastPromptUnixMs > sentAtMs ||
                                      now->convLastActivityUnixMs > sentAtMs);
            const bool promptGone = box.empty() || DraftSwapNormalize(DraftSwapFoldCr(box)) != sentNorm;
            if (promptGone && turnStarted)
            {
                submitted = true;
                break;
            }
        }
        if (!submitted && !box.empty())
        {
            // The box still holds the PROMPT (the classic eaten submit CR — with the promptGone
            // test above, a non-empty box can only strand us here by reading the sent text, or by
            // the turn never signaling at all). The Enter-retry watchdog owns the rescue once the
            // gate closes; we must not add to the box. The draft is NOT lost: it is in the TUI's
            // kill-ring (one Ctrl+Y away) and stays in the registry's memory below. Log WHAT the
            // box reads (DELIVERY.md observability — the 07:26:59 forensics could not tell).
            std::wstring head = box.substr(0, 48);
            for (auto& ch : head)
            {
                if (ch == L'\n' || ch == L'\r' || ch == L'\t')
                {
                    ch = L' ';
                }
            }
            const bool boxIsPrompt = DraftSwapNormalize(DraftSwapFoldCr(box)) == sentNorm;
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[draft-swap] " + sid8 + L" restore DEFERRED: the prompt has not left the input box (box chars=" +
                                              std::to_wstring(box.size()) + L" matchesPrompt=" + (boxIsPrompt ? L"yes" : L"no") +
                                              L" head=\"" + head + L"\") - draft kept in the kill-ring (Ctrl+Y) and in memory, not re-typed\n");
            _sessionRegistry->SetPendingInput(id, draft);
            _EndDraftSwap(id, true);
            co_return;
        }

        // ---- 7. RESTORE (yank, verified against the draft we read; paste fallback) ----
        const auto restoredH = co_await _RestoreDraftAfterSwap(id, draft, allowPasteRestore, clearedByStash);
        if (!_sessionRegistry)
        {
            _EndDraftSwap(id, true);
            co_return;
        }
        const std::wstring restored{ restoredH.c_str(), restoredH.size() };
        const bool exact = DraftSwapNormalize(restored) == DraftSwapNormalize(draft);
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[draft-swap] " + sid8 +
                                          (exact ? L" prompt sent and the draft is back in the box, unsent (via " + std::wstring(clearedByStash ? L"Ctrl+S stash" : L"the kill-ring") + L", chars=" + std::to_wstring(restored.size()) + L")\n" :
                                                   L" prompt sent but the draft could NOT be put back verbatim" + std::wstring(allowPasteRestore ? L"" : L" (it holds a paste placeholder, which must never be re-typed as literal text)") + L" - kept in memory (Copy Current Prompt) and in the kill-ring (Ctrl+Y)\n"));

        // ---- 8. UNLOCK + keep the "3 dots" honest ----
        // Record what the box ACTUALLY reads now, falling back to what we took — so even a failed
        // restore leaves the draft remembered, shown by the pending indicator, and copyable.
        _sessionRegistry->SetPendingInput(id, restored.empty() ? draft : restored);
        _EndDraftSwap(id, true);
    }

    // Agentmaster (DELIVERY_PLAN.md R8 — the VERIFIED PRESSER): the Enter-retry watchdog's lone
    // Enter, routed through a LIVE box read at press time. The scheduler's pure draft guard reads
    // the scan-stale SessionInfo::pendingInput (≤ ~one liveness tick old — the acknowledged R2
    // residual), and worse, an AskUserQuestion MENU with the state still Waiting/Idle (recon-block
    // can lag the unwritten tool_use line by minutes) passes every scan-side guard while a lone
    // Enter into it SELECTS the highlighted option — an answer fabricated by the rescue mechanism.
    // So the press reads the box RIGHT NOW:
    //   Draft == the watched prompt (fold-matched)  → press: the eaten-CR rescue, unchanged.
    //   Empty (verified)                            → press: a lone Enter into an empty box is a
    //                                                 no-op for claude.
    //   Draft ≠ the prompt (foreign)                → REFUSE + refresh pendingInput with the live
    //                                                 text (the next DecideEnterRetry's guard then
    //                                                 sees truth, not a tick-old echo).
    //   NoBox / MenuOpen                            → REFUSE + refresh pendingBoxState (the advance
    //                                                 holds too). Never press into a menu.
    //   Unknown (dormant / torn down)               → REFUSE (nothing to press into).
    // Invoked by SessionRegistry::PressEnterVerified on the scheduler worker; self-marshals. The
    // attempt was already spent by the scheduler — a refusal surfaces through the give-up ladder
    // (Failed + autorunner paused), never a silent forever-retry.
    winrt::fire_and_forget TerminalPage::_PressEnterForWatchedPrompt(std::wstring sessionId, std::wstring promptId, std::wstring promptText)
    {
        auto strongThis{ get_strong() };
        try
        {
            co_await wil::resume_foreground(Dispatcher());
            if (!_sessionRegistry)
            {
                co_return;
            }
            const auto sid8 = ::Agentmaster::ShortId(sessionId);
            if (_draftSwapsInFlight.count(sessionId) != 0)
            {
                // A swap/clear owns the box — the delivery gate should have held the watchdog off,
                // but the box mutex is the belt (a lone Enter mid-swap is the RC3 merge).
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[enter-retry] " + sid8 + L" press refused: a swap/clear holds the box\n");
                co_return;
            }
            const auto [state, live] = _ReadInputBoxProbeForSession(sessionId, kSendVerifyProbeRows);
            const auto st = static_cast<::Agentmaster::InputBoxState>(state);
            if (st == ::Agentmaster::InputBoxState::Empty ||
                (st == ::Agentmaster::InputBoxState::Draft && ::Agentmaster::DraftMatchesPromptText(live, promptText)))
            {
                _sessionRegistry->Inject(sessionId, std::wstring(1, L'\r'));
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[enter-retry] " + sid8 + L" prompt " + ::Agentmaster::ShortId(promptId) +
                                                  (st == ::Agentmaster::InputBoxState::Empty ? L" verified press (box empty)\n" : L" verified press (box holds the watched prompt)\n"));
                co_return;
            }
            if (st == ::Agentmaster::InputBoxState::Draft)
            {
                _sessionRegistry->SetPendingInput(sessionId, live); // the guard now sees the live truth
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[enter-retry] " + sid8 + L" press refused: the box holds a FOREIGN draft (chars=" +
                                                  std::to_wstring(live.size()) + L") - a lone Enter would submit it\n");
                co_return;
            }
            if (st == ::Agentmaster::InputBoxState::NoBox || st == ::Agentmaster::InputBoxState::MenuOpen)
            {
                _sessionRegistry->SetPendingBoxState(sessionId, st);
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[enter-retry] " + sid8 + L" press refused: " +
                                                  (st == ::Agentmaster::InputBoxState::MenuOpen ? L"a menu/dialog is open (Enter would SELECT an option)\n" : L"no parseable input box\n"));
                co_return;
            }
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[enter-retry] " + sid8 + L" press refused: box unreadable (dormant / torn down)\n");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_PressEnterForWatchedPrompt");
        }
    }

    // ---- CLEAR the box after a MAIL-button queue (PENDING_INPUT.md §8d, the "move" mode) ------------
    //
    // The per-tab overlay's MAIL button queues the session's unsent draft into its Auto-Testing queue.
    // A PLAIN click now also REMOVES it from Claude's input box (the draft is MOVED, not copied); a
    // Shift+Click keeps it (the historical copy). This is the removal half — invoked from the overlay's
    // page-wired clear handler AFTER the queue append succeeded.
    //
    // ⚠ It deliberately does NOT reuse the swap's clear ladder: that ladder's first rung is Ctrl+S,
    // and Claude's stash is not a discard — a stashed draft is AUTO-RESTORED into the input box ~0.4s
    // after the NEXT message submission (measured live, PendingInput.h). The swap is safe because its
    // restore always CONSUMES the slot; this clear has NO restore by design, so a stash here planted a
    // scheduled re-paste — the moved draft reappeared in the box right after its own queued copy was
    // delivered (the "Second pass please" incident, session d373b992). It runs the DISCARD ladder
    // instead (DecideDraftDiscard: per-round End+Ctrl+U, then End+Backspaces — true discards, measured
    // mid-turn-safe), each round judged by re-reading the LIVE box (ControlCore::ReadPendingInputDraft
    // via _ReadLiveDraftForSession — the direct buffer read on a 60ms poll, never the scanner's cached
    // copy) so the loop exits within one poll tick of the box actually emptying. NO send and NO restore
    // — the draft is being deliberately removed (it is already safe in the queue; the kill-ring keeps a
    // manually recoverable copy, Ctrl+Y). It shares the swap's box-mutex (_draftSwapsInFlight) so a
    // concurrent send declines rather than injecting alongside it, and it LOCKS the control read-only
    // for the clear so the user's own keystrokes can't interleave — the lock is handed back the INSTANT
    // the outcome is known (unlock precedes every piece of bookkeeping; unlocked on EVERY path via
    // _EndDraftSwap — a stranded read-only is an un-typeable tab). fire_and_forget => a terminate-net
    // wrapper (an escaped exception there is std::terminate), the body an awaitable Impl.
    winrt::fire_and_forget TerminalPage::_ClearLiveDraftForSession(std::wstring sessionId)
    {
        auto strongThis{ get_strong() };
        try
        {
            co_await _ClearLiveDraftForSessionImpl(sessionId);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ClearLiveDraftForSession");
        }
    }

    winrt::Windows::Foundation::IAsyncAction TerminalPage::_ClearLiveDraftForSessionImpl(std::wstring sessionId)
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry)
        {
            co_return;
        }
        const auto sid8 = ::Agentmaster::ShortId(sessionId);

        const auto control = _ControlForSession(sessionId);
        if (!control || control.ConnectionState() == TerminalConnection::ConnectionState::NotConnected)
        {
            // Not hosted here / dormant — there is no live box to clear. The draft is already queued;
            // a box we cannot see is left untouched (nothing to move out).
            co_return;
        }
        if (_draftSwapsInFlight.count(sessionId) != 0)
        {
            // A send-swap (or another clear) already holds this session's box — do not inject alongside
            // it (the very merge the swap exists to prevent). Leave the draft; the box is being managed.
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-clear] " + sid8 + L" declined: the box is already held (a swap/clear is in flight)\n");
            co_return;
        }

        // READ — confirm there is actually something to clear (the box may have emptied since the click).
        std::wstring box = _ReadLiveDraftForSession(sessionId);
        if (DraftSwapNormalize(box).empty())
        {
            // Already empty: the draft moved to the queue, so make the registry agree (drops the "3 dots"
            // + disables the MAIL button now) and stop.
            _sessionRegistry->SetPendingInput(sessionId, L"");
            co_return;
        }

        // Agentmaster (DELIVERY.md): own the box through the ENGINE gate too — a concurrent
        // SubmitPrompt is then declined SYNCHRONOUSLY (before marking anything Sent) and the
        // scheduler's DecideAdvance HOLDS instead of mark/decline/rollback-flapping against this
        // clear (the queue-append that precedes a mail-move is exactly what wakes the autorunner).
        // The gate's close (the scope guard below) notifies, which re-fires the held advance the
        // moment the box is free.
        if (!_sessionRegistry->TryOpenDeliveryGate(sessionId, std::wstring{ ::Agentmaster::kDeliveryGateClearTag }))
        {
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-clear] " + sid8 + L" declined: a delivery owns the box (gate held) - draft left in place (it is already queued)\n");
            co_return;
        }
        auto clearGateCloser = wil::scope_exit([gateRegistry = _sessionRegistry, sessionId]() noexcept {
            try
            {
                gateRegistry->CloseDeliveryGate(sessionId, std::wstring{ ::Agentmaster::kDeliveryGateClearTag });
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_ClearLiveDraftForSession gate close");
            }
        });

        // LOCK — like the swap: keep the user's keystrokes from interleaving during the clear.
        bool wasAlreadyReadOnly = true;
        try
        {
            wasAlreadyReadOnly = control.ReadOnly();
            if (!wasAlreadyReadOnly)
            {
                control.SetReadOnly(true);
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ClearLiveDraftForSession lock");
            wasAlreadyReadOnly = true; // never hand back a read-only we did not take
        }
        _draftSwapsInFlight[sessionId] = wasAlreadyReadOnly;
        ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-clear] " + sid8 + L" clearing the box after queueing (chars=" + std::to_wstring(box.size()) + L") via End+Ctrl+U discard\n");

        // CLEAR (verified) — the DISCARD ladder (PendingInput.h), NOT the swap's: a MOVE must
        // destroy the box copy, and the swap's Ctrl+S rung only relocates it into the stash slot,
        // which claude auto-restores at the next submit (the header-comment caution). Each round
        // is ONE inject — End + the eat, processed in order by the TUI — then the settle re-read
        // against the LIVE box; the per-rung stall counters implement the measured tolerance for
        // a no-op kill round between lines.
        ::Agentmaster::DraftDiscardProgress spent;
        bool cleared = false;
        const int64_t clearDeadline = TtNowMs() + kDraftDiscardBudgetMs;
        while (TtNowMs() < clearDeadline)
        {
            const auto plan = ::Agentmaster::DecideDraftDiscard(box, spent);
            if (plan.action == ::Agentmaster::DraftDiscardAction::Done)
            {
                cleared = true;
                break;
            }
            if (plan.action == ::Agentmaster::DraftDiscardAction::GiveUp)
            {
                break;
            }
            const bool killRung = plan.action == ::Agentmaster::DraftDiscardAction::EndKill;
            _sessionRegistry->Inject(sessionId,
                                     killRung ? ::Agentmaster::BuildInputEnd() + ::Agentmaster::BuildInputKill() :
                                                ::Agentmaster::BuildInputEnd() + ::Agentmaster::BuildBackspaces(plan.backspaces));
            const std::wstring before = box;
            for (int64_t settled = 0; settled <= kDraftSwapActionSettleMs; settled += kDraftSwapPollMs)
            {
                co_await winrt::resume_after(std::chrono::milliseconds(kDraftSwapPollMs));
                co_await wil::resume_foreground(Dispatcher());
                if (!_sessionRegistry)
                {
                    _EndDraftSwap(sessionId, true); // hand the tab back even on a defensive bail
                    co_return;
                }
                box = _ReadLiveDraftForSession(sessionId);
                if (box != before)
                {
                    break; // the keystrokes landed + repainted — judge the round on this
                }
            }
            const bool changed = box != before;
            if (killRung)
            {
                ++spent.killRounds;
                spent.killStalls = changed ? 0 : spent.killStalls + 1;
            }
            else
            {
                ++spent.backspaceRounds;
                spent.backspaceStalls = changed ? 0 : spent.backspaceStalls + 1;
            }
        }

        // UNLOCK FIRST — the instant the outcome is known the keyboard is the user's again;
        // nothing below (registry writes, log appends) may sit between the verdict and the
        // hand-back. Then the delivery gate, whose close-notify re-fires a held advance — the
        // box-mutex is already down, so that delivery is not declined by our own leftovers.
        _EndDraftSwap(sessionId, true); // UNLOCK (only the read-only WE took)
        clearGateCloser.reset(); // close the gate NOW (guarded lambda), not at co_return after the bookkeeping

        if (cleared)
        {
            // The draft is out of the box and already in the queue, so it is no longer an unsent draft:
            // tell the registry NOW (drops the "3 dots" + disables the MAIL button immediately, instead of
            // waiting out the scanner's ~2-tick clear debounce — which also closes the window where a fast
            // re-click would re-queue the still-remembered draft). Honest: the box is VERIFIED empty.
            _sessionRegistry->SetPendingInput(sessionId, L"");
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-clear] " + sid8 + L" cleared (moved to the queue)\n");
        }
        else
        {
            // Best-effort: the box would not empty. The prompt is already queued, so the only cost is that
            // the draft is ALSO still in the box (== a Shift+Click). Leave pendingInput alone (the draft is
            // genuinely still there) and let the user clear it by hand.
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-clear] " + sid8 + L" NOT cleared (the box would not empty after " + std::to_wstring(spent.killRounds) + L" End+kill + " + std::to_wstring(spent.backspaceRounds) + L" End+backspace round(s)) - draft left in place (it is still queued)\n");
        }
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
            ::Agentmaster::AgentLogCaughtException(L"ReadPromptNavIfGrown");
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
            ::Agentmaster::AgentLogCaughtException(L"_ScrollAdjacentPrompt");
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
            ::Agentmaster::AgentLogCaughtException(L"_RefreshPromptNavCache");
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
            ::Agentmaster::AgentLogCaughtException(L"_NavigateAdjacentPrompt scroll/highlight");
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
        // Agentmaster (terminate-net): the adoption lane — fired from the engine's adoption fan-out
        // (bridge/registry threads) on every SessionStart, it hops to the UI thread and does XAML bind
        // work (_BindClaudeSessionToTab: injector + overlay + title + color). An exception escaping this
        // fire_and_forget (incl. resume_foreground on a dying dispatcher at window close) would
        // std::terminate the app. The body is an awaitable IAsyncAction (_AdoptExternalSessionImpl)
        // whose exceptions propagate to this co_await (see _SweepClaudeLiveness).
        auto strongThis{ get_strong() };
        try
        {
            co_await _AdoptExternalSessionImpl(sessionId, cwd, tabToken);
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_AdoptExternalSession");
        }
    }

    winrt::Windows::Foundation::IAsyncAction TerminalPage::_AdoptExternalSessionImpl(winrt::hstring sessionId, winrt::hstring cwd, winrt::hstring tabToken)
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
                _sessionRegistry->SetPromptSubmitter(superseded, nullptr); // same lifetime as the injector (the new id re-registers below)
                _sessionRegistry->SetEnterPresser(superseded, nullptr); // DELIVERY_PLAN.md R8
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
        // Agentmaster (DELIVERY.md §11 / PENDING_INPUT.md §9): the SUBMITTER + the VERIFIED PRESSER,
        // in lockstep with the injector — this window owns the control, so only it can read the box
        // or block the keyboard. The bind path used to register the injector ALONE, which silently
        // routed every adopted/re-homed session's sends through the registry's raw no-submitter
        // fallback: no draft swap AND (post-R4) no fill→verify→CR — exactly the unprotected shape
        // Incident 3 proved fatal. One protection contract for launched and adopted sessions alike.
        _sessionRegistry->SetPromptSubmitter(id, [weakThis{ get_weak() }](const ::Agentmaster::PromptSubmission& submission) -> bool {
            const auto self = weakThis.get();
            return self ? self->_AcceptPromptSubmission(submission) : false; // gone => not accepted; the caller rolls back (Rule #4)
        });
        _sessionRegistry->SetEnterPresser(id, [weakThis{ get_weak() }](const std::wstring& pressId, const std::wstring& promptId, const std::wstring& promptText) {
            if (const auto self = weakThis.get())
            {
                self->_PressEnterForWatchedPrompt(pressId, promptId, promptText);
            }
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
            ::Agentmaster::AgentLogCaughtException(L"_SweepClaudeLiveness");
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
                // PENDING_INPUT.md §5: the draft is deliberately KEPT. The dead claude's input box is
                // gone, so this record is the ONLY copy of an unsent message — archiving must not be
                // the erasure ("persist and load on startup the message"; Rule #16's never-lose
                // spirit). The frozen pendingInputUnixMs marks it a stale MEMORY for display, and a
                // later resume revalidates: the new claude's empty box debounce-clears it honestly.
            });
            _sessionRegistry->SetInjector(id, nullptr);
            _sessionRegistry->SetPromptSubmitter(id, nullptr); // PENDING_INPUT.md §9 — same lifetime as the injector
            _sessionRegistry->SetEnterPresser(id, nullptr); // DELIVERY_PLAN.md R8 — same lifetime as the submitter
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
    // Agentmaster (SPARK CROWN — the cache-warm hint): re-evaluate ServerCacheStillWarm for every session
    // this window hosts and drive its tab-strip glow + embers. This has to be POLLED rather than pushed:
    // warmth BEGINS on an event (a turn's hook / the observer's transcript enrichment) but ENDS on a
    // CLOCK — nothing fires when the ~5-minute window lapses — so a push-only indicator would stay lit
    // forever on an abandoned tab. The scanner's ~2.5s tick is far finer than a minutes-long window, so
    // the hint appears within one tick of a prompt and clears within one tick of going cold.
    // Deliberately its own sweep rather than a rider on _ScanPendingInput: that scan's early-outs are
    // draft-specific (it skips a Codex session and any tab whose control hasn't STARTED), whereas warmth
    // is pure registry + clock and is meaningful for a dormant window-restored tab too — resuming such a
    // conversation inside the window genuinely does reuse the cached prefix.
    winrt::fire_and_forget TerminalPage::_ScanCacheWarmTabs()
    {
        // Agentmaster (terminate-net): an exception escaping a fire_and_forget is std::terminate, so the
        // body is an awaitable IAsyncAction whose exceptions propagate to this co_await (see
        // _SweepClaudeLiveness).
        auto strongThis{ get_strong() };
        try
        {
            co_await _ScanCacheWarmTabsImpl();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ScanCacheWarmTabs");
        }
    }

    winrt::Windows::Foundation::IAsyncAction TerminalPage::_ScanCacheWarmTabsImpl()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (!_sessionRegistry || _claudeTabs.empty())
        {
            co_return;
        }
        // The SAME normalization the Triage-Board card applies (0 -> 5), so the tab strip and the card can
        // never disagree about how long a cache stays warm.
        const uint32_t cacheMin = _appSettings.serverCacheMinutes ? _appSettings.serverCacheMinutes : 5;
        const int64_t now = TtNowMs();
        // Snapshot the ids first — never resolve tabs/controls while iterating _claudeTabs.
        std::vector<std::wstring> ids;
        ids.reserve(_claudeTabs.size());
        for (const auto& [id, weakTab] : _claudeTabs)
        {
            ids.push_back(id);
        }
        for (const auto& id : ids)
        {
            const auto info = _sessionRegistry->Get(id);
            if (!info)
            {
                continue;
            }
            TerminalApp::Tab hostTab{ nullptr };
            if (const auto it = _claudeTabs.find(id); it != _claudeTabs.end())
            {
                hostTab = it->second.get();
            }
            if (!hostTab)
            {
                continue;
            }
            // ServerCacheStillWarm itself gates on live + Claude + a real API-turn signal, so a Codex tab,
            // an archived session and a never-prompted launch all fall out here as "cold" with no extra
            // check — the one predicate owns the whole decision (SessionModels.h).
            const bool warm = ::Agentmaster::ServerCacheStillWarm(*info, cacheMin, now);
            std::optional<winrt::Windows::UI::Color> sparkColor;
            if (warm)
            {
                sparkColor = _CacheWarmSparkColorForTab(hostTab, *info);
            }
            _SetTabCacheWarm(hostTab, warm, sparkColor);
        }
        co_return;
    }

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
            ::Agentmaster::AgentLogCaughtException(L"_ScanPendingInput");
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
            // Agentmaster (PENDING_INPUT.md §9): a DRAFT SWAP is holding this session's box right now.
            // Mid-swap the box is deliberately empty — that is the whole point — so letting this scan
            // observe it would run the clear debounce down and ERASE the very draft the swap is
            // carrying for the user (and drop the "3 dots" for a second). The swap re-records the
            // draft itself when it finishes; until then, leave every pending-input fact alone.
            if (_draftSwapsInFlight.count(id) != 0)
            {
                continue;
            }
            // This session's tab (for the local tab-strip pulse) — a weak_ref in _claudeTabs.
            TerminalApp::Tab hostTab{ nullptr };
            if (const auto it = _claudeTabs.find(id); it != _claudeTabs.end())
            {
                hostTab = it->second.get();
            }
            // Agentmaster (PENDING_INPUT.md §10): a restore RE-FILL is armed/in flight for this
            // session. The freshly resumed box is EMPTY until the pump types the remembered draft
            // back in — letting the clear debounce observe those first empty reads would erase the
            // very memory the re-fill is about to deliver (and a read landing mid-paste could commit
            // a half-filled body as the draft). Drive the dots from the memory (the NotConnected
            // branch's rule below) and leave every stored fact + the streak alone until the pump
            // resolves the entry (verified / refused / abandoned); the next tick then revalidates
            // against the live box as usual.
            if (_pendingDraftRestores.count(id) != 0)
            {
                if (hostTab)
                {
                    const bool restoredPending = !info->pendingInput.empty();
                    std::optional<winrt::Windows::UI::Color> dotsColor;
                    if (restoredPending)
                    {
                        dotsColor = _PendingDotsColorForTab(hostTab, *info);
                    }
                    _SetTabPending(hostTab, restoredPending, dotsColor);
                }
                continue;
            }
            const auto control = _ControlForSession(id);
            // The buffer exists only once the control has STARTED (left NotConnected). A dormant
            // window-restored tab has no claude running -> no LIVE draft readable. Leave the stored
            // draft + streak untouched so a momentarily-unreadable tab doesn't drop a real pending
            // state — but DO drive the tab-strip dots from the stored value: a RESTORED record may
            // carry a persisted draft MEMORY (PENDING_INPUT.md §5), and this branch is the only one
            // that runs before the tab first starts. Once the tab starts, the live read below either
            // confirms it or the clear debounce honestly erases it (claude never restores its own
            // input box across a restart).
            if (!control || control.ConnectionState() == TerminalConnection::ConnectionState::NotConnected)
            {
                // R5: a dormant tab has no box to have a state — record Unknown so a stale
                // NoBox/MenuOpen from before the restart-swap/teardown can't keep holding advances.
                _sessionRegistry->SetPendingBoxState(id, ::Agentmaster::InputBoxState::Unknown);
                if (hostTab)
                {
                    const bool restoredPending = !info->pendingInput.empty();
                    std::optional<winrt::Windows::UI::Color> dotsColor;
                    if (restoredPending)
                    {
                        dotsColor = _PendingDotsColorForTab(hostTab, *info);
                    }
                    _SetTabPending(hostTab, restoredPending, dotsColor);
                }
                continue;
            }
            std::wstring draft;
            auto boxState = ::Agentmaster::InputBoxState::Unknown;
            try
            {
                const auto h = control.ReadPendingInputDraft();
                draft.assign(h.c_str(), h.size());
                // R5: the verdict of the SAME cached scan (a second view over one mutation-gated
                // read — ~free in steady state). Recorded beside the draft so the pure deciders
                // (DecideAdvance's hold, DecideEnterRetry's refusal) see what the screen shows.
                boxState = static_cast<::Agentmaster::InputBoxState>(control.ReadPendingInputBoxState());
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"_ScanPendingInput read (skip tab)");
                continue; // a control torn down mid-tick — skip it
            }
            _sessionRegistry->SetPendingBoxState(id, boxState);

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
            const bool textChanged = info->pendingInput != effectiveDraft; // pre-set snapshot vs committed
            const bool flipped = _sessionRegistry->SetPendingInput(id, effectiveDraft);
            const bool hasPending = !effectiveDraft.empty();
            if (textChanged && hasPending)
            {
                // Paste placeholders ("[Pasted text #N +M lines]" / "[...Truncated ...]") resolve to
                // their real paste-cache file OFF-THREAD (content-anchored + arithmetic-verified,
                // PendingPaste.h) and land quietly on the record; no markers clears a stale annotation
                // synchronously (cheap pure scan).
                if (::Agentmaster::FindPasteMarkers(effectiveDraft).empty())
                {
                    _sessionRegistry->SetPendingPasteRefs(id, L"");
                }
                else
                {
                    _ResolvePendingPasteRefsFor(id, effectiveDraft);
                }
            }
            // Agentmaster (PENDING_INPUT.md §8c): mirror the draft into the DURABLE PER-SESSION store
            // — "if the observer finds out there was a change then empty the cache or upsert the cache
            // in the per session persistence". This is the ONE seam that writes it (the same seam that
            // already knows a change happened), so the fleet record and the per-session file can never
            // disagree about what a session's unsent message is. The registry stays AUTHORITATIVE; the
            // store is the copy that survives the fleet record being dropped and is readable by id
            // alone. The write (and the paste EXPANSION it does first) happens off-thread.
            if (textChanged)
            {
                if (!hasPending)
                {
                    // "Empty the cache." Deliberately NOT throttled: a clear is rare (turn cadence) and
                    // it is the correctness-relevant half — a stale stored draft would keep being
                    // offered for a message the user already sent.
                    _pendingDraftStoreMs.erase(id);
                    _PersistSessionDraft(id, std::wstring{});
                }
                else
                {
                    // "Upsert the cache." Throttled PER SESSION at the same ~10s as the sessions.json
                    // drift save above (the draft moves per keystroke; the durable copy only has to stay
                    // within a throttle window of the live box), so one busy session cannot starve
                    // another's.
                    const int64_t nowMs = TtNowMs();
                    auto& lastStoreMs = _pendingDraftStoreMs[id];
                    if (nowMs - lastStoreMs >= 10000)
                    {
                        lastStoreMs = nowMs;
                        _PersistSessionDraft(id, effectiveDraft);
                    }
                }
            }
            if (textChanged && hasPending && !flipped)
            {
                // A text-only edit is QUIET (no notify → no autosave), so the PERSISTED draft would
                // otherwise freeze at its appear-flip snapshot — the staleness the [pending] log
                // already exhibits must not leak into the on-disk memory (PENDING_INPUT.md §5). A
                // throttled direct save keeps the durable copy within ~10s of the live box; the
                // teardown/close archive persists once more, so a graceful exit always lands the
                // final text (a crash loses at most the last throttle window).
                const int64_t nowMs = TtNowMs();
                if (nowMs - _pendingDraftSaveMs >= 10000)
                {
                    _pendingDraftSaveMs = nowMs;
                    ::Agentmaster::SaveSessions(_sessionRegistry->Snapshot());
                }
            }
            else if (flipped)
            {
                _pendingDraftSaveMs = TtNowMs(); // the flip notify just autosaved — restart the window
            }
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

    // Agentmaster (PENDING_INPUT.md §2b): resolve a draft's paste placeholders to their real
    // paste-cache file. DETACHED background work — the whole cache read (~hundreds of small files)
    // must never ride the UI thread, and nothing after the hop touches UI state: the registry
    // (thread-safe, captured by value) takes the annotation QUIETLY (SetPendingPasteRefs drops it if
    // the draft meanwhile cleared, so a late resolve can't resurrect a sent message's annotation),
    // and the log line is the observability. Terminate-net: the whole body is inside the try.
    winrt::fire_and_forget TerminalPage::_ResolvePendingPasteRefsFor(std::wstring sessionId, std::wstring draft)
    {
        const auto registry = _sessionRegistry; // by-value shared_ptr — safe past page teardown
        try
        {
            co_await winrt::resume_background();
            if (!registry)
            {
                co_return;
            }
            auto refs = ::Agentmaster::ResolvePendingPasteRefs(draft);
            if (refs.empty())
            {
                co_return; // no markers (raced an edit) — nothing to record
            }
            registry->SetPendingPasteRefs(sessionId, refs);
            std::wstring oneLine = refs;
            for (auto& c : oneLine)
            {
                if (c == L'\n')
                {
                    c = L';';
                }
            }
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[pending] " + ::Agentmaster::ShortId(sessionId) + L" paste-refs: " + oneLine + L"\n");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ResolvePendingPasteRefsFor");
        }
    }

    // Agentmaster (PENDING_INPUT.md §8c): write a session's unsent draft into the DURABLE PER-SESSION
    // store (session-store/<sid>.json), or CLEAR it when `draft` is empty. DETACHED background work
    // for two reasons: the paste expansion below reads the whole paste-cache directory, and no state
    // write belongs on the UI thread. Nothing after the hop touches UI state — the registry is
    // thread-safe (captured by value, so this is safe past page teardown) and the store is an atomic
    // per-file write.
    //
    // ORDERING GUARD (the SetPendingPasteRefs idiom): the registry — not this coroutine's argument —
    // is the authority on what the session's draft IS. Two writes for one session can be in flight
    // (a drift, then a clear a tick later), and the background hops give no ordering, so a late upsert
    // could otherwise resurrect a draft the user already sent. Each write therefore re-reads the
    // registry and proceeds ONLY while it still agrees with what we were asked to persist; the loser
    // drops, and the winner's own write is already queued.
    //
    // PASTE EXPANSION (the user's choice: store the FULL content): a draft that reads
    // "[Pasted text #3 +258 lines]" on screen is not the whole prompt, so before storing, the markers
    // are resolved against the paste-cache (ExpandPendingDraftPastes — content-anchored,
    // all-or-refuse). A REFUSED expansion stores the rendered text instead: the store's job is to not
    // lose the memory, and a placeholder-bearing draft is still the honest record of what was typed
    // (§9's "never re-type a placeholder" rule governs FILLING a box, not remembering a draft). A
    // marker-free draft costs one scan and no cache IO.
    winrt::fire_and_forget TerminalPage::_PersistSessionDraft(std::wstring sessionId, std::wstring draft)
    {
        const auto registry = _sessionRegistry; // by-value shared_ptr — safe past page teardown
        try
        {
            co_await winrt::resume_background();
            if (!registry || sessionId.empty())
            {
                co_return;
            }
            const auto current = registry->Get(sessionId);
            if (!current || current->pendingInput != draft)
            {
                co_return; // superseded (or the record is gone) — the newer write owns the store
            }
            if (draft.empty())
            {
                if (!::Agentmaster::SetStoredSessionDraft(sessionId, L"", 0))
                {
                    // A failed CLEAR is the one that matters: a stale draft would keep being offered.
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[persist-fail] " + ::Agentmaster::ShortId(sessionId) + L" stored draft clear failed\n");
                }
                co_return;
            }
            std::wstring text = draft;
            int expanded = 0;
            const auto ex = ::Agentmaster::ExpandPendingDraftPastes(draft);
            if (ex.complete && !ex.text.empty() && ex.expandedCount > 0)
            {
                text = ex.text;
                expanded = ex.expandedCount;
            }
            if (!::Agentmaster::SetStoredSessionDraft(sessionId, text, TtNowMs()))
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[persist-fail] " + ::Agentmaster::ShortId(sessionId) + L" stored draft write failed\n");
                co_return;
            }
            if (expanded > 0)
            {
                // Only the INTERESTING write is logged (a routine upsert rides the scan cadence and
                // would be noise): the stored value materially differs from what is on screen, so say
                // so — this is why the persisted draft is longer than the box's.
                ::Agentmaster::AppendStateLog(L"hooks.log",
                                              L"[pending] " + ::Agentmaster::ShortId(sessionId) + L" stored draft: expanded " +
                                                  std::to_wstring(expanded) + L" paste(s), chars=" + std::to_wstring(text.size()) +
                                                  L" (rendered " + std::to_wstring(draft.size()) + L")\n");
            }
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_PersistSessionDraft");
        }
    }

    // Agentmaster (COMMANDS.md §5 — the over-budget /handover delivery): inject a spawned
    // successor's FULL handover document through the ConPTY stdin as ONE bracketed paste, once
    // its claude has actually STARTED. The commandline tier caps at the CreateProcessW ceiling;
    // the stdin pipe has none — this pump is what makes "never truncate the message" hold for
    // arbitrarily large documents. Ticked by the scanner's liveness probe (~2.5s) alongside
    // _ScanPendingInput; steady state with no pending injections is one empty-map check.
    // SELF-MARSHALS to the UI thread (the _ScanPendingInput fire_and_forget + Impl idiom): the
    // probe fires on the SCANNER thread while _HandleCommandHandover writes the map on the UI
    // thread — the map is UI-thread-only state (and the standby lane below reads the tab's
    // TermControl, which is UI-affine).
    //
    // Per pending entry (successor sessionId -> the queued prompt carrying the document):
    //   * session gone/archived, or the prompt no longer Pending (delivered by the scheduler's
    //     deferred-send under a non-Off autorunner, or the user's manual Send-now) -> drop the
    //     entry (the queue is the single source of truth; this map is only the auto-trigger);
    //   * not yet started (SessionInfo.started — the control initialized and Start() ran; a
    //     pre-Connected WriteInput is SILENTLY dropped by ConPTY, so injecting earlier would
    //     lose the text and strand a phantom Sent) -> wait;
    //   * started -> after a short settle (kHandoverInjectSettleMs — lands the paste closer to
    //     the TUI's raw-mode init, fewer Enter-retries; the submit CR is backed by the
    //     scheduler's Enter-retry watchdog regardless), deliver via the Send-now recipe: mark
    //     Sent -> Inject(BuildPromptSubmission(text)) -> roll back to Pending on failure (Rule
    //     #4 — never a phantom Sent). Echo dedup marks it consumed when the UserPromptSubmit
    //     lands, exactly like any other flight prompt.
    //   * give-up deadline (kHandoverInjectDeadlineMs): stop auto-trying — the prompt stays
    //     Pending in the queue (visible in Auto Testing, Send-now-able), logged. Never lost.
    //
    // The STANDBY lane (COMMANDS.md §5b — /handover-standby; an entry whose standbyText is
    // non-empty): FILL, don't send — Inject(BuildPromptFill(text)), the bracketed paste with NO
    // submit CR, so the briefing sits in the successor's input box one Enter away. No echo ever
    // confirms a fill (nothing is submitted), so delivery is VERIFIED by reading the input box
    // itself (ControlCore::ReadPendingInputDraft — the PENDING_INPUT.md primitive, legal here
    // because the pump marshals to the UI thread):
    //   * before the first fill, a box already holding ANY text means the user is typing — hold
    //     off entirely (never append to a human draft; the deadline caps the wait);
    //   * after a fill, a NON-EMPTY box == verified (logged; the §6b delete-after arms HERE, and
    //     only here — an unverified draft always leaves its briefing file on disk);
    //   * a box still EMPTY after kStandbyVerifyMs means the TUI ate the paste pre-raw-mode (the
    //     Enter-retry gotcha's text-eaten sibling) — re-fill, at most kStandbyMaxAttempts times,
    //     then give up (file kept, logged);
    //   * the user DRIVING the session means hands off immediately, file kept (we cannot tell
    //     "sent our draft" from "our fill was eaten and they typed their own", so never delete).
    //     The gate is the pure StandbySessionTakenOver latch, checked in BOTH phases: a turn in
    //     flight NOW (state off Idle/WaitingForInput), or proof one EVER ran
    //     (turns.lastPromptUnixMs / convLastActivityUnixMs — a fast turn submitted and COMPLETED
    //     between ticks lands the state back at rest, where the emptied box would otherwise read
    //     as an eaten paste and the pump would RE-FILL an already-delivered briefing; both
    //     signals are 0 on a fresh standby successor until a real submit).
    winrt::fire_and_forget TerminalPage::_PumpHandoverInjections()
    {
        // Terminate-net (the _SweepClaudeLiveness idiom): an exception escaping a fire_and_forget
        // IS winrt::terminate, so the body lives in an awaitable IAsyncAction whose exceptions
        // propagate to this co_await.
        auto strongThis{ get_strong() };
        try
        {
            co_await _PumpHandoverInjectionsImpl();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_PumpHandoverInjections");
        }
    }

    winrt::Windows::Foundation::IAsyncAction TerminalPage::_PumpHandoverInjectionsImpl()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (_pendingHandoverInjections.empty() || !_sessionRegistry)
        {
            co_return;
        }
        constexpr int64_t kHandoverInjectSettleMs = 1500; // post-started settle before the paste
        constexpr int64_t kHandoverInjectDeadlineMs = 10 * 60 * 1000; // stop auto-trying after 10 min (dormant tab never focused)
        constexpr int64_t kStandbyVerifyMs = 12 * 1000; // a filled draft must show in the input box within this, else re-fill
        constexpr int32_t kStandbyMaxAttempts = 2; // fills injected before giving up (file kept)
        const int64_t now = static_cast<int64_t>(::GetTickCount64());
        for (auto it = _pendingHandoverInjections.begin(); it != _pendingHandoverInjections.end();)
        {
            const std::wstring& id = it->first;
            auto& entry = it->second;
            const auto s = _sessionRegistry->Get(id);

            // ---- the STANDBY lane (fill, don't send) ----
            if (!entry.standbyText.empty())
            {
                if (!s || !s->live)
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover-standby] " + ::Agentmaster::ShortId(id) + L" successor gone before the fill - briefing NOT typed (md kept on disk)\n");
                    it = _pendingHandoverInjections.erase(it);
                    continue;
                }
                if (now - entry.armedMs >= kHandoverInjectDeadlineMs)
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover-standby] " + ::Agentmaster::ShortId(id) + L" gave up after 10 min " + (entry.injectedAtMs == 0 ? L"(session never started / box never free)" : L"(fill never verified)") + L" - md kept on disk\n");
                    it = _pendingHandoverInjections.erase(it);
                    continue;
                }
                if (!s->started || !_sessionRegistry->HasInjector(id))
                {
                    ++it; // claude not launched yet (a pre-Connected WriteInput silently drops) — wait
                    continue;
                }
                if (entry.startedSeenMs == 0)
                {
                    entry.startedSeenMs = now; // first tick with the session started — begin the settle
                    ++it;
                    continue;
                }
                if (now - entry.startedSeenMs < kHandoverInjectSettleMs)
                {
                    ++it;
                    continue;
                }
                // HANDS-OFF latch, BOTH phases (StandbySessionTakenOver — pure, unit-tested): the
                // user drove this session — a turn in flight NOW, or proof one EVER ran
                // (turns.lastPromptUnixMs / convLastActivityUnixMs; a fast turn submitted AND
                // completed between pump ticks lands the state back at rest, where the state
                // check alone would mistake the emptied box for an eaten paste and RE-FILL the
                // already-delivered briefing). Pre-fill it also covers a user who claimed the
                // fresh successor with their own prompt during the settle (the state-only gap:
                // the box reads empty mid-turn, and filling into a RUNNING session is exactly
                // the intrusion standby exists to avoid). Either way the session is theirs:
                // hands off permanently, and NEVER delete the file (we cannot tell "sent our
                // draft" from "fill eaten + typed their own" — the briefing stays their copy).
                if (::Agentmaster::StandbySessionTakenOver(s->state, s->turns.lastPromptUnixMs, s->convLastActivityUnixMs))
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover-standby] " + ::Agentmaster::ShortId(id) + (entry.injectedAtMs == 0 ? L" session in use before the fill" : L" session took over before verification") + L" (a prompt ran) - hands off, md kept\n");
                    it = _pendingHandoverInjections.erase(it);
                    continue;
                }
                // Read the successor's live input box (the verification channel — UI thread).
                // R5: verdict-aware — "known" now means a PARSEABLE box was seen (Empty or Draft).
                // A NoBox/MenuOpen read (a startup modal, a menu, a mid-repaint frame) is NOT
                // "verified empty": filling into it would feed the modal/menu, and treating it as
                // empty past the verify window would re-fill blind. Wait instead (deadline-capped).
                std::wstring draft;
                bool draftKnown = false;
                if (const auto control = _ControlForSession(id))
                {
                    if (control.ConnectionState() != TerminalConnection::ConnectionState::NotConnected)
                    {
                        try
                        {
                            const auto h = control.ReadPendingInputDraft();
                            draft.assign(h.c_str(), h.size());
                            const auto st = static_cast<::Agentmaster::InputBoxState>(control.ReadPendingInputBoxState());
                            draftKnown = (st == ::Agentmaster::InputBoxState::Empty || st == ::Agentmaster::InputBoxState::Draft);
                        }
                        catch (...)
                        {
                            ::Agentmaster::AgentLogCaughtException(L"_PumpHandoverInjections standby draft read"); // torn down mid-tick — treat as unknown, wait
                        }
                    }
                }
                if (entry.injectedAtMs != 0)
                {
                    // Verify phase — a fill was injected; is it visible in the box? (The latch
                    // above already excluded every "the user drove it" case, so a non-empty box
                    // here is OUR fill.) R4 upgrade: the verify is now a CONTENT compare
                    // (VerifyFillAgainstPrompt — whitespace-tolerant, collapse-aware), not a bare
                    // "non-empty": a PARTIAL paste no longer "verifies" (it waits out the window
                    // and gives up honestly, file kept — never a blind re-fill onto partial text).
                    const auto standbyVerdict = (draftKnown && !draft.empty()) ? ::Agentmaster::VerifyFillAgainstPrompt(draft, entry.standbyText) : ::Agentmaster::FillVerify::Eaten;
                    if (standbyVerdict == ::Agentmaster::FillVerify::Verified || standbyVerdict == ::Agentmaster::FillVerify::VerifiedCollapsed)
                    {
                        ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover-standby] " + ::Agentmaster::ShortId(id) + L" draft VERIFIED in the input box (chars=" + std::to_wstring(draft.size()) + L", " + (standbyVerdict == ::Agentmaster::FillVerify::Verified ? L"exact" : L"collapsed") + L") - one Enter away\n");
                        // The delivery is now secured, so the §6b delete may arm (content tier
                        // only — a pointer fill NAMES the file; setting read LIVE at verify time).
                        // CommandHandoverDeleteEffective: the scratchpad (the default) forces it off
                        // — a standby briefing written there is already outside the repo (§6c).
                        if (!entry.standbyMdPath.empty() && ::Agentmaster::CommandHandoverDeleteEffective(_appSettings))
                        {
                            _pendingHandoverDeletes[id] = PendingHandoverDelete{ entry.standbyMdPath, now };
                        }
                        it = _pendingHandoverInjections.erase(it);
                        continue;
                    }
                    if (now - entry.injectedAtMs < kStandbyVerifyMs)
                    {
                        ++it; // inside the verify window — the pending-input read lags a fill by design
                        continue;
                    }
                    if (entry.fillAttempts >= kStandbyMaxAttempts)
                    {
                        ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover-standby] " + ::Agentmaster::ShortId(id) + L" draft never appeared after " + std::to_wstring(entry.fillAttempts) + L" fill(s) - giving up, md kept on disk\n");
                        it = _pendingHandoverInjections.erase(it);
                        continue;
                    }
                    if (!draftKnown)
                    {
                        ++it; // box unreadable — never re-fill blind (a duplicate paste is worse than a late one)
                        continue;
                    }
                    if (!draft.empty())
                    {
                        // R4: NON-EMPTY but UNVERIFIED past the window (a partial paste, or content
                        // the compare can't claim). Before the content-compare this "verified"; now
                        // it must neither verify NOR fall through to the re-fill (a paste onto a
                        // partial body doubles text). Give up honestly — the briefing file is kept.
                        ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover-standby] " + ::Agentmaster::ShortId(id) + L" box holds text that does not verify as the briefing (chars=" + std::to_wstring(draft.size()) + L") - giving up, md kept on disk\n");
                        it = _pendingHandoverInjections.erase(it);
                        continue;
                    }
                    // Box VERIFIED empty past the window: the paste was eaten (TUI raw-mode race).
                    // Safe to re-fill — nothing of ours is in the box to duplicate.
                }
                else
                {
                    // Pre-fill phase: never type over/append to a box that already holds text —
                    // an early user draft wins, we wait for the box to clear (the deadline caps).
                    if (!draftKnown || !draft.empty())
                    {
                        ++it;
                        continue;
                    }
                }
                // Agentmaster (DELIVERY.md): a delivery/clear owns the box this tick — a fill now
                // would land inside the swap's clear/restore window. Wait a tick; the gate is
                // short-lived and expiry-bounded, and the deadline above still caps everything.
                if (_sessionRegistry->DeliveryGateHeld(id))
                {
                    ++it;
                    continue;
                }
                const bool delivered = _sessionRegistry->Inject(id, ::Agentmaster::BuildPromptFill(entry.standbyText));
                if (delivered)
                {
                    entry.injectedAtMs = now;
                    entry.fillAttempts += 1;
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover-standby] " + ::Agentmaster::ShortId(id) + L" briefing FILLED into the input box (chars=" + std::to_wstring(entry.standbyText.size()) + L", attempt " + std::to_wstring(entry.fillAttempts) + L"/" + std::to_wstring(kStandbyMaxAttempts) + L") - NOT submitted, verifying\n");
                }
                // Injector vanished mid-flight: nothing was typed — retry next tick (deadline caps).
                ++it;
                continue;
            }

            // ---- the classic paste lane (deliver + submit via the Send-now recipe) ----
            const ::Agentmaster::QueuedPrompt* prompt = nullptr;
            if (s)
            {
                for (const auto& p : s->queue)
                {
                    if (p.id == entry.promptId)
                    {
                        prompt = &p;
                        break;
                    }
                }
            }
            if (!s || !s->live || !prompt || prompt->status != ::Agentmaster::PromptStatus::Pending)
            {
                // Gone, archived, or already delivered/handled elsewhere — the map is only the
                // auto-trigger; the queue record (if any) remains the truth.
                it = _pendingHandoverInjections.erase(it);
                continue;
            }
            if (now - entry.armedMs >= kHandoverInjectDeadlineMs)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover] " + ::Agentmaster::ShortId(id) + L" paste-injection gave up after 10 min (session never started) - document stays Pending in the queue (Send-now delivers it)\n");
                it = _pendingHandoverInjections.erase(it);
                continue;
            }
            if (!s->started || !_sessionRegistry->HasInjector(id))
            {
                ++it; // claude not launched yet (a pre-Connected WriteInput silently drops) — wait
                continue;
            }
            if (entry.startedSeenMs == 0)
            {
                entry.startedSeenMs = now; // first tick with the session started — begin the settle
                ++it;
                continue;
            }
            if (now - entry.startedSeenMs < kHandoverInjectSettleMs)
            {
                ++it;
                continue;
            }
            // Deliver via the Send-now recipe (mark Sent -> inject -> roll back on failure).
            std::wstring text;
            _sessionRegistry->Update(id, [&](::Agentmaster::SessionInfo& ss) {
                for (auto& p : ss.queue)
                {
                    if (p.id == entry.promptId && p.status == ::Agentmaster::PromptStatus::Pending)
                    {
                        text = p.text;
                        p.status = ::Agentmaster::PromptStatus::Sent;
                        p.sentAtUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                        p.attempts += 1;
                        p.echoed = false; // await this injection's UserPromptSubmit echo
                        p.enterRetries = 0; // fresh send -> arm the Enter-retry watch
                        p.injectedAtUnixMs = 0; // fresh send -> injection evidence pending (DELIVERY.md §12)
                        break;
                    }
                }
            });
            if (text.empty())
            {
                ++it; // raced a concurrent deliverer — re-evaluated (and likely dropped) next tick
                continue;
            }
            // Agentmaster (PENDING_INPUT.md §9): the shared send seam. A successor is normally a fresh
            // tab with an empty box, so the swap is a no-op here — but a user who started typing into
            // the new tab during the pump's settle must not have their words folded into the briefing.
            const bool delivered = _sessionRegistry->SubmitPrompt({ id, entry.promptId, text, false });
            if (delivered)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover-inject] " + ::Agentmaster::ShortId(id) + L" FULL document pasted (" + std::to_wstring(text.size()) + L" chars) + submit\n");
                it = _pendingHandoverInjections.erase(it);
                continue;
            }
            // Injector vanished mid-flight (tab closing): roll back to Pending (Rule #4) and retry
            // next tick until the deadline; the entry-liveness check above drops a dead session.
            _sessionRegistry->RollbackPromptToPending(id, entry.promptId, false);
            ++it;
        }
    }

    // Agentmaster (COMMANDS.md §6b — "delete the handover file after a successful hand-off"): the
    // DEFERRED half of the opt-in delete. _HandleCommandHandover only ARMS an entry (successor id
    // -> md path); the file is removed here, on the same liveness tick as the paste pump, once the
    // successor has actually STARTED — SessionInfo.started, i.e. its ConPTY/claude really launched.
    //
    // Why deferred rather than deleted at spawn: a successor opened in a BACKGROUND tab starts
    // LAZILY (WT only builds the control on first layout, so claude.exe may not run for minutes),
    // and a launch that never comes up must leave the briefing ON DISK for the user to re-run. So:
    //   * started            -> delete (the delivery is secured: the content tier put the whole
    //                          document on the launch commandline, the paste tier parked it durably
    //                          at the front of the successor's queue);
    //   * gone / archived    -> DROP the entry WITHOUT deleting (the successor died before running
    //                          — the file is the only copy the user can act on);
    //   * past the deadline  -> give up, leave the file (logged). The deadline is the user-set
    //                          commandHandoverDeleteDeadlineMinutes (default 1440 == 24 h), NOT the
    //                          pump's fixed 10 min: the pump is racing a TUI that is either up or
    //                          not, while this waits on a HUMAN visiting a lazily-started background
    //                          tab. 0 == don't wait at all;
    //   * delete failed      -> logged, file left in place (best-effort; never blocks anything).
    // The POINTER tier never arms an entry at all (its successor's first message NAMES the file),
    // and the STANDBY lane arms only after a VERIFIED fill (the pump above). SELF-MARSHALS to the
    // UI thread like the pump — the map is UI-thread-only state written by _HandleCommandHandover.
    winrt::fire_and_forget TerminalPage::_SweepHandoverDeletes()
    {
        // Terminate-net (the _SweepClaudeLiveness idiom).
        auto strongThis{ get_strong() };
        try
        {
            co_await _SweepHandoverDeletesImpl();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_SweepHandoverDeletes");
        }
    }

    winrt::Windows::Foundation::IAsyncAction TerminalPage::_SweepHandoverDeletesImpl()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (_pendingHandoverDeletes.empty() || !_sessionRegistry)
        {
            co_return;
        }
        // Mid-flight OFF is AUTHORITATIVE: if delete-after was on when these entries armed but the
        // user has since turned it off (e.g. changed their mind while a background successor sat
        // un-started — a 24 h window gives plenty of time to), honor the CURRENT intent and drop
        // every armed delete, KEEPING the files. Only the SAFE direction is applied retroactively:
        // the reverse (turning it back ON) deliberately does NOT retro-arm already-spawned
        // successors — that stays "applies to the next handover", the arm-time gate in
        // _HandleCommandHandover / the standby pump. Read per sweep like the deadline below.
        if (!_appSettings.commandHandoverDeleteFileAfterLaunch)
        {
            for (const auto& [id, entry] : _pendingHandoverDeletes)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover] " + ::Agentmaster::ShortId(id) + L" delete-after turned OFF before the successor started - md KEPT: " + entry.mdPath + L"\n");
            }
            _pendingHandoverDeletes.clear();
            co_return;
        }
        // The wait-for-the-successor budget, in the user's minutes (cog -> Commands; default 24 h).
        // Read per sweep so a Save applies to entries already armed — no restart, no re-arm. NB the
        // arm is IN-MEMORY only (this map), so the wait spans a single app RUN: a successor still
        // un-started when Agentmaster exits is NOT re-armed on the next launch, and its briefing is
        // simply KEPT (the safe direction). Durable arming would be needed to sweep across a restart.
        const uint32_t deadlineMinutes = ::Agentmaster::ClampCommandHandoverDeleteDeadlineMinutes(_appSettings.commandHandoverDeleteDeadlineMinutes);
        const int64_t kHandoverDeleteDeadlineMs = static_cast<int64_t>(deadlineMinutes) * 60 * 1000;
        const int64_t now = static_cast<int64_t>(::GetTickCount64());
        for (auto it = _pendingHandoverDeletes.begin(); it != _pendingHandoverDeletes.end();)
        {
            const std::wstring& id = it->first;
            const auto& entry = it->second;
            const auto s = _sessionRegistry->Get(id);
            if (!s || !s->live)
            {
                // The successor vanished/archived before it ever started — KEEP the briefing.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover] " + ::Agentmaster::ShortId(id) + L" successor gone before start - md KEPT: " + entry.mdPath + L"\n");
                it = _pendingHandoverDeletes.erase(it);
                continue;
            }
            if (!s->started)
            {
                if (now - entry.armedMs >= kHandoverDeleteDeadlineMs)
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover] " + ::Agentmaster::ShortId(id) + L" successor never started within " + std::to_wstring(deadlineMinutes) + L" min - md KEPT: " + entry.mdPath + L"\n");
                    it = _pendingHandoverDeletes.erase(it);
                    continue;
                }
                ++it; // still lazily-dormant (a background tab) — wait for its claude to launch
                continue;
            }
            if (::DeleteFileW(entry.mdPath.c_str()))
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover] deleted md after successful hand-off (successor=" + ::Agentmaster::ShortId(id) + L" started): " + entry.mdPath + L"\n");
            }
            else
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[handover] delete-after-hand-off FAILED (le=" + std::to_wstring(::GetLastError()) + L"), file left in place: " + entry.mdPath + L"\n");
            }
            it = _pendingHandoverDeletes.erase(it);
        }
    }

    // Agentmaster (PENDING_INPUT.md §10 — the restore RE-FILL, arm half): prepare a reopened
    // session's remembered unsent draft for the pump. The registry memory keeps the RENDERED form
    // (paste placeholders as Claude showed them); the FILL must not re-type a placeholder literally
    // (a "[Pasted text #N +M lines]" label pasted back is indistinguishable on screen from a real
    // placeholder but submits as junk, silently dropping the content behind it — the §9 draft-swap
    // rule), so a marker-carrying memory is EXPANDED against the paste-cache first —
    // content-anchored + all-or-refuse (ExpandPendingDraftPastes) — and a refusal never arms:
    // the memory then behaves exactly as before this feature (display-only until revalidation).
    // The cache read is real IO (~hundreds of small files), so it runs OFF-THREAD like the §2b
    // annotation resolve; the map write lands back on the dispatcher (UI-thread-only state). The
    // takeover baseline (armedUnixMs) is captured BEFORE any await — it must be the resume-launch
    // instant, not the post-expansion one, so a prompt racing the expansion still reads as "after
    // arming" and hands the session off. Marker-free drafts (the common case) never leave the UI
    // thread: no suspension point executes, the arm completes synchronously inside the launch.
    winrt::fire_and_forget TerminalPage::_ArmDraftRestore(std::wstring sessionId, std::wstring draft)
    {
        // Terminate-net (the _ResolvePendingPasteRefsFor idiom): the whole body is inside the try.
        auto strongThis{ get_strong() };
        try
        {
            const int64_t armedUnixMs = TtNowMs();
            std::wstring fillText;
            int expandedCount = 0;
            if (::Agentmaster::FindPasteMarkers(draft).empty())
            {
                fillText = std::move(draft);
            }
            else
            {
                std::wstring rendered = draft; // keep the rendered form for the refusal log
                co_await winrt::resume_background();
                auto expansion = ::Agentmaster::ExpandPendingDraftPastes(rendered);
                co_await wil::resume_foreground(Dispatcher());
                if (!expansion.complete)
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-restore] " + ::Agentmaster::ShortId(sessionId) + L" re-fill REFUSED: a paste placeholder did not resolve against the paste-cache (chars=" + std::to_wstring(rendered.size()) + L") - never a lossy literal re-type; the memory stays display-only (the cached paste content itself is preserved on disk)\n");
                    co_return;
                }
                fillText = std::move(expansion.text);
                expandedCount = expansion.expandedCount;
                // The expanded content came from FILES (the paste-cache), not the rendered buffer —
                // unlike a buffer-read draft it can carry C0 controls, and an embedded ESC would
                // terminate BuildPromptFill's bracketed paste early with the remainder interpreted
                // as keystrokes. Strip C0 except \n and \t (CR dropped ⇒ CRLF→LF), the same
                // normalization ReadHandoverDocumentPrompt applies for exactly this reason.
                std::wstring clean;
                clean.reserve(fillText.size());
                for (const wchar_t c : fillText)
                {
                    if (c >= 0x20 || c == L'\n' || c == L'\t')
                    {
                        clean.push_back(c);
                    }
                }
                fillText = std::move(clean);
                if (fillText.find_first_not_of(L" \t\n") == std::wstring::npos)
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-restore] " + ::Agentmaster::ShortId(sessionId) + L" re-fill REFUSED: expansion yielded no printable content - memory stays display-only\n");
                    co_return;
                }
            }
            _pendingDraftRestores[sessionId] = PendingDraftRestore{ std::move(fillText), armedUnixMs, 0, 0, 0 };
            const auto& armed = _pendingDraftRestores[sessionId];
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-restore] " + ::Agentmaster::ShortId(sessionId) + L" armed: remembered unsent draft will be typed back once the tab starts (chars=" + std::to_wstring(armed.draftText.size()) + (expandedCount ? (L", " + std::to_wstring(expandedCount) + L" paste placeholder(s) expanded") : std::wstring{}) + L")\n");
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_ArmDraftRestore");
        }
    }

    // Agentmaster (PENDING_INPUT.md §10 — the restore RE-FILL, pump half): TYPE a reopened
    // session's remembered unsent draft back into its fresh claude's input box once the tab
    // actually starts — the /handover-standby lane's recipe verbatim (fill, never send):
    //   * session gone/archived/not-Claude, or the setting turned off, or the memory itself was
    //     cleared elsewhere (the restart-swap's deliberate eager clear) -> drop the entry (an
    //     archived record KEEPS its memory, so a later resume re-arms from it);
    //   * not yet started (a background-restored tab starts lazily — possibly hours later; the
    //     entry is a few bytes, so unlike the standby lane there is NO pre-start deadline: the
    //     wait is on a HUMAN visiting the tab, the _SweepHandoverDeletes lesson) -> wait;
    //   * started -> a short settle (the TUI's raw-mode init), then the HANDS-OFF latch
    //     (RestoredDraftSessionTakenOver — a turn in flight now, or any prompt submitted at/after
    //     the arm instant: user, autorunner, /handover; a resumed session's HISTORY never trips
    //     it) -> hands off permanently, the memory left to honest revalidation;
    //   * pre-fill, box already holding ANY text -> the user typed their own draft into the
    //     resumed session — theirs wins, drop (the scan records the live text as the new memory);
    //   * fill = Inject(BuildPromptFill(text)) — the bracketed paste with NO submit CR — then
    //     VERIFY by reading the box back (no echo ever confirms a fill): non-empty == verified
    //     (the scan's next tick re-reads the live box and re-stamps the memory); still empty past
    //     the verify window == the TUI ate the paste pre-raw-mode -> re-fill, at most
    //     kRestoreFillMaxAttempts, then give up (memory left to revalidation, logged);
    //   * a STARTED session that never resolves (box unreadable forever) is capped by a deadline
    //     anchored at startedSeenMs, so a wedged entry can't hold the scan's revalidation off
    //     forever.
    // While an entry exists, _ScanPendingInput HOLDS the session's clear debounce (the freshly
    // resumed box is empty until this pump fills it) and drives the dots from the memory — so the
    // indicator never flickers across the restart→fill hand-off. SELF-MARSHALS to the UI thread
    // (the map is UI-thread-only state; the verify reads the tab's TermControl, which is UI-affine).
    winrt::fire_and_forget TerminalPage::_PumpDraftRestores()
    {
        // Terminate-net (the _SweepClaudeLiveness idiom).
        auto strongThis{ get_strong() };
        try
        {
            co_await _PumpDraftRestoresImpl();
        }
        catch (...)
        {
            ::Agentmaster::AgentLogCaughtException(L"_PumpDraftRestores");
        }
    }

    winrt::Windows::Foundation::IAsyncAction TerminalPage::_PumpDraftRestoresImpl()
    {
        auto strongThis{ get_strong() };
        co_await wil::resume_foreground(Dispatcher());
        if (_pendingDraftRestores.empty() || !_sessionRegistry)
        {
            co_return;
        }
        constexpr int64_t kRestoreFillSettleMs = 1500; // post-started settle before the paste (the standby lane's value)
        constexpr int64_t kRestoreFillVerifyMs = 12 * 1000; // a filled draft must show in the input box within this, else re-fill
        constexpr int32_t kRestoreFillMaxAttempts = 2; // fills injected before giving up (memory left to revalidation)
        constexpr int64_t kRestoreFillStartedDeadlineMs = 10 * 60 * 1000; // post-STARTED wedge cap (box unreadable forever); pre-start waits unbounded
        const int64_t now = static_cast<int64_t>(::GetTickCount64());
        for (auto it = _pendingDraftRestores.begin(); it != _pendingDraftRestores.end();)
        {
            const std::wstring& id = it->first;
            auto& entry = it->second;
            const auto s = _sessionRegistry->Get(id);
            if (!s || !s->live || s->kind != ::Agentmaster::AgentKind::Claude)
            {
                // Closed/archived before its claude ever came up (or re-keyed away). The archived
                // record KEEPS its pendingInput, so a later resume simply re-arms — nothing lost.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-restore] " + ::Agentmaster::ShortId(id) + L" session gone before the re-fill - memory kept on the record\n");
                it = _pendingDraftRestores.erase(it);
                continue;
            }
            if (!_appSettings.restoreDraftOnResume)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-restore] " + ::Agentmaster::ShortId(id) + L" re-fill disabled in settings while waiting - dropped (memory stays display-only)\n");
                it = _pendingDraftRestores.erase(it);
                continue;
            }
            if (s->pendingInput.empty())
            {
                // The memory was cleared OUTSIDE this pump while armed — the restart-tab swap's
                // deliberate eager clear is the one path (the scan holds while armed). Honor it.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-restore] " + ::Agentmaster::ShortId(id) + L" memory cleared while waiting - re-fill dropped\n");
                it = _pendingDraftRestores.erase(it);
                continue;
            }
            if (!s->started || !_sessionRegistry->HasInjector(id))
            {
                ++it; // claude not launched yet (a pre-Connected WriteInput silently drops) — wait, unbounded (a background tab starts on a HUMAN's first visit)
                continue;
            }
            if (entry.startedSeenMs == 0)
            {
                entry.startedSeenMs = now; // first tick with the session started — begin the settle
                ++it;
                continue;
            }
            if (now - entry.startedSeenMs >= kRestoreFillStartedDeadlineMs)
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-restore] " + ::Agentmaster::ShortId(id) + L" gave up 10 min after start " + (entry.injectedAtMs == 0 ? L"(box never readable)" : L"(fill never verified)") + L" - memory left to revalidation\n");
                it = _pendingDraftRestores.erase(it);
                continue;
            }
            if (now - entry.startedSeenMs < kRestoreFillSettleMs)
            {
                ++it;
                continue;
            }
            // HANDS-OFF latch (RestoredDraftSessionTakenOver — pure, unit-tested): a turn in flight
            // NOW, or any prompt submitted at/after the ARM instant (the user typed+sent their own,
            // the autorunner consumed a queued plan on the reopened session, a /handover fired).
            // Either way the session is owned: hands off permanently — re-typing an old draft into
            // a session someone is driving is exactly the intrusion the standby contract forbids.
            // The memory is left to the scan's honest revalidation (it clears within ~5s unless the
            // live box actually holds text).
            if (::Agentmaster::RestoredDraftSessionTakenOver(s->state, s->turns.lastPromptUnixMs, s->convLastActivityUnixMs, entry.armedUnixMs))
            {
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-restore] " + ::Agentmaster::ShortId(id) + (entry.injectedAtMs == 0 ? L" session in use before the re-fill" : L" session took over before verification") + L" (a prompt ran since the resume) - hands off, memory left to revalidation\n");
                it = _pendingDraftRestores.erase(it);
                continue;
            }
            // Read the session's live input box (the verification channel — UI thread).
            // R5: verdict-aware, like the standby lane — only a PARSEABLE box (Empty/Draft) counts
            // as known; a NoBox/MenuOpen (a startup modal, a menu) waits rather than reading as
            // "verified empty" (a re-fill into a modal would feed IT, not the box).
            std::wstring draft;
            bool draftKnown = false;
            if (const auto control = _ControlForSession(id))
            {
                if (control.ConnectionState() != TerminalConnection::ConnectionState::NotConnected)
                {
                    try
                    {
                        const auto h = control.ReadPendingInputDraft();
                        draft.assign(h.c_str(), h.size());
                        const auto st = static_cast<::Agentmaster::InputBoxState>(control.ReadPendingInputBoxState());
                        draftKnown = (st == ::Agentmaster::InputBoxState::Empty || st == ::Agentmaster::InputBoxState::Draft);
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"_PumpDraftRestores draft read"); // torn down mid-tick — treat as unknown, wait
                    }
                }
            }
            if (entry.injectedAtMs != 0)
            {
                // Verify phase — a fill was injected; is it visible in the box? (The latch above
                // already excluded every "the user drove it" case, so a non-empty box is OUR fill.)
                // R4 upgrade: content-compare (whitespace-tolerant, collapse-aware), not bare
                // non-empty — a partial paste must neither "verify" nor be re-filled onto.
                const auto restoreVerdict = (draftKnown && !draft.empty()) ? ::Agentmaster::VerifyFillAgainstPrompt(draft, entry.draftText) : ::Agentmaster::FillVerify::Eaten;
                if (restoreVerdict == ::Agentmaster::FillVerify::Verified || restoreVerdict == ::Agentmaster::FillVerify::VerifiedCollapsed)
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-restore] " + ::Agentmaster::ShortId(id) + L" draft RESTORED + VERIFIED in the input box (chars=" + std::to_wstring(draft.size()) + L", " + (restoreVerdict == ::Agentmaster::FillVerify::Verified ? L"exact" : L"collapsed") + L") - one Enter away, exactly as before the restart\n");
                    it = _pendingDraftRestores.erase(it);
                    continue; // the scan re-reads the live box next tick and re-stamps the memory
                }
                if (now - entry.injectedAtMs < kRestoreFillVerifyMs)
                {
                    ++it; // inside the verify window — the pending-input read lags a fill by design
                    continue;
                }
                if (entry.fillAttempts >= kRestoreFillMaxAttempts)
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-restore] " + ::Agentmaster::ShortId(id) + L" draft never appeared after " + std::to_wstring(entry.fillAttempts) + L" fill(s) - giving up, memory left to revalidation\n");
                    it = _pendingDraftRestores.erase(it);
                    continue;
                }
                if (!draftKnown)
                {
                    ++it; // box unreadable — never re-fill blind (a duplicate paste is worse than a late one)
                    continue;
                }
                if (!draft.empty())
                {
                    // R4: non-empty but UNVERIFIED past the window — a partial fill or unexplained
                    // content. Never re-fill onto it; leave the memory to the scan's honest
                    // revalidation (which will record whatever the box actually holds).
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-restore] " + ::Agentmaster::ShortId(id) + L" box holds text that does not verify as the remembered draft (chars=" + std::to_wstring(draft.size()) + L") - giving up, memory left to revalidation\n");
                    it = _pendingDraftRestores.erase(it);
                    continue;
                }
                // Box VERIFIED empty past the window: the paste was eaten (TUI raw-mode race).
                // Safe to re-fill — nothing of ours is in the box to duplicate.
            }
            else
            {
                if (!draftKnown)
                {
                    ++it; // just-started control still settling — wait (the started-deadline caps)
                    continue;
                }
                if (!draft.empty())
                {
                    // The user already typed their OWN draft into the resumed session — theirs
                    // wins, permanently (unlike the standby lane we don't wait for the box to
                    // clear: the live text supersedes the memory, and the scan records it as the
                    // new memory on its next tick).
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-restore] " + ::Agentmaster::ShortId(id) + L" box already holds text - the live draft wins, re-fill dropped\n");
                    it = _pendingDraftRestores.erase(it);
                    continue;
                }
            }
            // Agentmaster (DELIVERY.md): a delivery/clear owns the box this tick — a fill now would
            // land inside the swap's clear/restore window (an autorunner consuming the reopened
            // session's queue races this pump by design; the taken-over latch above then retires
            // the entry on ITS next pass). Wait a tick; the gate is short-lived + expiry-bounded.
            if (_sessionRegistry->DeliveryGateHeld(id))
            {
                ++it;
                continue;
            }
            const bool delivered = _sessionRegistry->Inject(id, ::Agentmaster::BuildPromptFill(entry.draftText));
            if (delivered)
            {
                entry.injectedAtMs = now;
                entry.fillAttempts += 1;
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[draft-restore] " + ::Agentmaster::ShortId(id) + L" remembered draft typed back into the input box (chars=" + std::to_wstring(entry.draftText.size()) + L", attempt " + std::to_wstring(entry.fillAttempts) + L"/" + std::to_wstring(kRestoreFillMaxAttempts) + L") - NOT submitted, verifying\n");
            }
            // Injector vanished mid-flight: nothing was typed — retry next tick (the deadline caps).
            ++it;
        }
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
            ::Agentmaster::AgentLogCaughtException(L"_ScanInferredTabColors");
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
            ::Agentmaster::AgentLogCaughtException(L"_ReconcileClaudeTabs");
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
            ::Agentmaster::AgentLogCaughtException(L"_ObserverProbe");
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

        // Agentmaster: UI-thread heartbeat for the engine's [ui-stall] watchdog. This coroutine is
        // ON the UI thread here (the resume_foreground above), dispatched as a dispatcher item off
        // the scanner's ~2s tick — so a beat PROVES the window's dispatcher is draining. When the
        // UI thread wedges (the 2026-08-14 RDP theme-storm class), the beats stop while the engine
        // lanes keep running, and the watchdog names the dead window in hooks.log instead of the
        // freeze being invisible in our own logs.
        ::Agentmaster::NoteUiHeartbeat(_windowId);

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
            ::Agentmaster::AgentLogCaughtException(L"_RefreshObserverData");
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
