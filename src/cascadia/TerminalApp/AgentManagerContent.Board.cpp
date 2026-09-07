// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster Manager tab content -- C1 'Linked Lenses' (7 partial files)
// The pinned leftmost tab's UI (DESIGN section 9): a Triage Board + Explorer Tree + Auto Testing over
// ONE shared SessionRegistry, built imperatively. ONE class (AgentManagerContent) split from the
// former 10864-line .cpp into by-area TUs that share AgentManagerContent.Internal.h.
//
// Partial files in this group (★ marks THIS file):
//   AgentManagerContent.cpp             - CORE: ctor/dtor, the Set* wiring, IPaneContent, the per-window lens, _BuildLayout, _Refresh
//   AgentManagerContent.Internal.h      - the ~48 shared file-local helpers: StateColor/Pill/StateDot/Text/Fill + path/sort utils (anonymous namespace, a per-TU copy)
// ★ AgentManagerContent.Board.cpp       - the Triage Board: cards, columns, splitters, _RebuildBoard
//   AgentManagerContent.Tree.cpp        - the Explorer Tree: managed/external trees, context menus, scope/sort toggles, rename, confirm dialogs
//   AgentManagerContent.Settings.cpp    - keep-awake/reopen/activate buttons + the Settings cog overlay (tabs, save, env editor, UPDATES, claude-missing)
//   AgentManagerContent.AutoTesting.cpp  - the Auto Testing: plan + selection sync, prompt compose/history, Autorunner, the Summary tab, templates
//   AgentManagerContent.Launch.cpp      - the Launch bar: cwd validation, the Claude/Codex toggle, launch/create/fork, the path-picker drop-down
// ======================================================================================
//
// Agentmaster Manager tab: the TRIAGE BOARD -- session/external cards, the state columns, the draggable splitters, and _RebuildBoard. Partial TU of AgentManagerContent.cpp.
#include "pch.h"
#include "AgentManagerContent.h"

#include "AgentTipHelpers.h" // AgentSetTip — hover tooltips with working dismissal (XAML Islands)
#include "AgentCopyActions.h" // CopySessionField — the shared copy-menu action (same path as the per-tab overlay's copy button)
#include "AgentStatusColors.h" // ParseArgbHexColor / FormatArgbHexColor (the cog's color pickers) + ResolveTagDisplayColor (the card's bookmark ribbons)
#include "AgentMaster/ClaudeSpawn.h" // NewSessionId (prompt ids)
#include "AgentMaster/SessionStore.h" // GetSessionTags / LoadAllTagColors — the card title band's bookmark ribbons; CollectGlobalTags / FoldTagName — the header's tag-filter chips
#include "AgentMaster/Persistence.h" // templates: load/save/apply
#include "AgentMaster/ProfileBootstrap.h" // the cog's Profile row (active dir + Change… picker)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/Engine.h" // RecoverableWindows (the "Reopen Windows (N)" recover button)
#include "AgentMaster/ProcessInspect.h" // ReadTranscriptInfo (read-only Auto Testing of an external) + BringClaudeWindowToFront (EXTERNAL menu)
#include "AgentMaster/TranscriptStore.h" // ReadTranscriptQuickFacts — resolve a launch-box session id's cwd
#include "AgentMaster/Updater.h" // the in-app updater: the cog's "Check for updates" + the "vX available!" label

// Agentmaster: the build-stamped git commit + branch (the Settings page header). Generated into
// $(GeneratedFilesDir) by TerminalAppLib.vcxproj's AgentmasterGenerateBuildInfo target, which is
// on the include path. The __has_include guard + fallback defines keep this file compilable if
// the generator hasn't run yet (e.g. opened in an IDE before any build); a real build always
// regenerates the header first (BeforeTargets ClCompile).
#if __has_include("AgentmasterBuildInfo.g.h")
#include "AgentmasterBuildInfo.g.h"
#endif
#ifndef AGENTMASTER_COMMIT_HASH
#define AGENTMASTER_COMMIT_HASH L"unknown"
#endif
#ifndef AGENTMASTER_COMMIT_BRANCH
#define AGENTMASTER_COMMIT_BRANCH L"unknown"
#endif

#include <algorithm>
#include <chrono>
#include <cmath> // std::pow — relative-luminance black/white contrast pick for the card title band
#include <filesystem> // create_directories — the "Create & Launch" affordance for a not-yet-existing dir
#include <system_error> // std::error_code — non-throwing create_directories
#include <thread> // background transcript read for an external's read-only plan
#include <shobjidl.h> // IFileOpenDialog — Browse for claude.exe (native-exe-only policy)

using namespace winrt::Windows::Foundation;
// Using-DECLARATIONS (not a directive) for the color helpers: a `using namespace
// winrt::Windows::UI;` would also pull the nested `Text` namespace into scope and collide
// with our Text() TextBlock helper below.
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;
using winrt::Windows::UI::Colors;
using winrt::Windows::UI::Core::CoreCursor;
using winrt::Windows::UI::Core::CoreCursorType;
using winrt::Windows::UI::Core::CoreWindow;
using namespace winrt::Windows::UI::Text; // FontWeights
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input; // KeyRoutedEventArgs
using namespace winrt::Windows::UI::Xaml::Media; // brushes
using namespace winrt::Windows::System; // DispatcherQueue, VirtualKey
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace Agentmaster;
// The shared tooltip recipe (AgentTipHelpers.h) — a using-DECLARATION so the file-scope
// helpers below (e.g. TimingText) can call it unqualified too.
using winrt::TerminalApp::implementation::AgentSetTip;
using winrt::TerminalApp::implementation::AgentSetTitledTip;
#include "AgentManagerContent.Internal.h" // the shared file-local helpers (StateColor/Pill/Text/...)

namespace winrt::TerminalApp::implementation
{
    void AgentManagerContent::_UpdateCardProgress()
    {
        if (_cardProgress.empty())
        {
            return;
        }
        const int64_t now = NowMs();
        for (const auto& p : _cardProgress)
        {
            if (!p.bar || p.timeoutMs <= 0)
            {
                continue;
            }
            const auto st = p.bar.RenderTransform().try_as<ScaleTransform>();
            if (!st)
            {
                continue;
            }
            double frac = 1.0 - static_cast<double>(now - p.lastActivityUnixMs) / static_cast<double>(p.timeoutMs);
            frac = frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac);
            st.ScaleX(frac);
        }
    }

    // Agentmaster (Waiting-for-you countdown bar): start the ~1s drainer iff any bar is tracked, else
    // stop it (no Waiting-for-you cards -> no per-second work). Built lazily; weak self so a closed
    // window never leaks a ticking timer. Called at the end of each board rebuild (from _Refresh).
    void AgentManagerContent::_SyncProgressTimer()
    {
        if (_cardProgress.empty())
        {
            if (_progressTimer)
            {
                _progressTimer.Stop();
            }
            return;
        }
        if (!_progressTimer)
        {
            _progressTimer = DispatcherTimer{};
            _progressTimer.Interval(std::chrono::seconds(1));
            _progressTimer.Tick([weak = get_weak()](const IInspectable& sender, const IInspectable&) {
                if (auto self = weak.get())
                {
                    self->_UpdateCardProgress();
                }
                else if (const auto t = sender.try_as<DispatcherTimer>())
                {
                    t.Stop(); // page destroyed — stop ticking (UI thread, safe)
                }
            });
        }
        if (!_progressTimer.IsEnabled())
        {
            _progressTimer.Start();
        }
        _UpdateCardProgress(); // paint the correct fractions NOW (don't wait up to 1s for the first tick)
    }

    Button AgentManagerContent::_MakeCard(const SessionInfo& s)
    {
        const bool selected = (s.id == _selectedId);
        const auto accent = StateColor(s.state);

        // Agentmaster: the card BODY (everything below the colored title band) — codex pill,
        // working dir, model·effort, timing, autorunner badge. The title itself lives in the band.
        auto stack = StackPanel{};
        stack.Spacing(2);

        // Agentmaster: a colored TITLE BAND across the top of the card, painted the session's
        // TAB color — the SAME color its terminal tab wears under the active tab-color MODE
        // (ResolveSessionColorHex: the working dir's permanent color by default [Rule #12 /
        // dir-colors.json], the session's OWN color under Individual, the INFERRED dir's under
        // InferredWorkingDirectory) — so a card reads its group at a glance and clusters with its
        // siblings across the state columns. Its TOP corners follow the card's rounding while its
        // BOTTOM is a straight edge (square corners) where it meets the neutral body: it covers
        // ONLY the title. The title text flips black/white for contrast (PreferDarkTextOn) so it
        // stays legible on a LIGHT or DARK band. Falls back to the neutral card fill (white text)
        // if no color resolves. Persisted color first (matches the tab exactly), else the
        // deterministic auto color — the same precedence the Sessions-page chip uses.
        std::optional<Color> bandColor;
        {
            bandColor = HexToColor(::Agentmaster::ResolveSessionColorHex(_appSettings.tabColorMode, s));
        }
        const std::wstring_view fullTitle = s.title.empty() ? std::wstring_view{ L"(untitled)" } : std::wstring_view{ s.title };
        auto titleText = Text(OneLine(fullTitle), 14, true, 1.0);
        if (bandColor)
        {
            const uint8_t ink = PreferDarkTextOn(*bandColor) ? 0x10 : 0xFF; // near-black on light, white on dark
            titleText.Foreground(Fill(0xFF, ink, ink, ink));
        }
        Border band;
        band.Background(bandColor ? SolidColorBrush{ *bandColor } : Fill(selected ? 0x40 : 0x20, 0x80, 0x80, 0x80));
        band.CornerRadius(CornerRadius{ 4, 4, 0, 0 }); // rounded top (matches the card), straight bottom edge
        band.Padding(Thickness{ 8, 4, 8, 4 });
        // Agentmaster (eager-init / "Activate Tab"): a DORMANT card (live but its claude hasn't started —
        // a restored tab the user never opened) leads its title band with the half-hollow state dot, so the
        // board flags which cards still need waking (the card has no always-on state dot otherwise — its
        // column conveys state). A started card is unchanged (no dot). Right-click / "Activate All Tabs (N)"
        // wakes them; the dot fills once the control starts.
        if (IsSessionDormant(s))
        {
            auto bandRow = StackPanel{};
            bandRow.Orientation(Orientation::Horizontal);
            bandRow.Spacing(6);
            bandRow.VerticalAlignment(VerticalAlignment::Center);
            bandRow.Children().Append(StateDotDormant(StateColor(s.state)));
            bandRow.Children().Append(titleText);
            band.Child(bandRow);
        }
        else
        {
            band.Child(titleText);
        }
        // Agentmaster: hovering the title band (the card's "top label") shows the FULL title \x2014 the
        // band trims with an ellipsis on a narrow card and OneLine() collapses a multi-line title for
        // the dense card, so the complete name is otherwise unreadable here. When Claude Code has written
        // an idle RECAP for this session (the >5-min "what we did / what's next" away_summary, mirrored
        // onto SessionInfo.recap by the scanner), append it below the title \x2014 so a hover tells the
        // sessions apart at a glance (the whole point of the recap), not just by name. Shown in FULL \x2014
        // never length-capped (the tooltip wraps / grows as needed).
        // The FULL name is the tip's TITLE (the panel wraps it, so nothing is lost); the recap follows
        // as the body. With no recap the body still has to say something — the tip renders only when it
        // has text — so it explains why hovering the band was worth it in the first place.
        std::wstring bandTip{ L"The session's full name \x2014 the card trims it to fit." };
        if (!s.recap.empty())
        {
            bandTip += L"\n\nRecap \x2014 Claude Code's own \x201C";
            bandTip += L"what we did / what's next\x201D note, written after this session sat idle:\n";
            bandTip += s.recap;
        }
        AgentSetTitledTip(band, winrt::hstring{ fullTitle }, winrt::hstring{ bandTip }, kCardTipDelay);

        // Agentmaster (bookmark tags): the session's bookmark ribbons hang out of the TITLE BAND's
        // bottom edge — the tab badges' "bookmark out of the book" look (~30% riding ON the colored
        // band, ~70% overhanging below it), x-aligned with the title's first character (the band's
        // 8px left inset). Unlike the tab (whose strip ScrollViewer CLIPS at the tab's bottom edge,
        // forcing the popup-root host), the card clips nothing — a NEGATIVE top margin in the
        // band→body stack straddles the boundary (the row is appended AFTER the band, so it draws
        // OVER it), and the row's remaining in-flow height pushes the body down so ribbons never
        // cover body text. Same 6.5x9.3 ribbon + resolved colors as the tab and the Sessions Tags
        // column (ResolveTagDisplayColor: user-picked > name-hash, from _boardTagColors — both maps
        // re-read each _RebuildBoard), and the SAME rich hover panel (every carrier + status dot,
        // click a live row to jump) via the page's SetTagHoverHandlers wiring.
        StackPanel tagRow{ nullptr };
        if (const auto tit = _boardTags.find(s.id); tit != _boardTags.end() && !tit->second.empty())
        {
            tagRow = StackPanel{};
            tagRow.Orientation(Orientation::Horizontal);
            tagRow.Spacing(2);
            tagRow.HorizontalAlignment(HorizontalAlignment::Left);
            tagRow.Margin(Thickness{ 8, -3, 8, 0 }); // pull ~30% of the 9.3px ribbon up ONTO the band; the rest flows below its edge
            constexpr size_t kCardMaxTagRibbons = 8; // bound a heavily-tagged card; a dim "+N" marks overflow
            size_t shownRibbons = 0;
            for (const auto& tg : tit->second)
            {
                if (shownRibbons >= kCardMaxTagRibbons)
                {
                    auto more = Text(winrt::hstring{ L"+" + std::to_wstring(tit->second.size() - kCardMaxTagRibbons) }, 9, false, 0.6);
                    more.VerticalAlignment(VerticalAlignment::Center);
                    more.IsHitTestVisible(false);
                    tagRow.Children().Append(more);
                    break;
                }
                winrt::Windows::UI::Xaml::Shapes::Polygon ribbon; // the tab badges' 6.5x9.3 bookmark shape
                ribbon.Points().Append(Point{ 0.0f, 0.0f });
                ribbon.Points().Append(Point{ 6.5f, 0.0f });
                ribbon.Points().Append(Point{ 6.5f, 9.3f });
                ribbon.Points().Append(Point{ 3.25f, 6.5f });
                ribbon.Points().Append(Point{ 0.0f, 9.3f });
                ribbon.Fill(SolidColorBrush{ ResolveTagDisplayColor(tg, _boardTagColors) });
                ribbon.Stroke(SolidColorBrush{ Colors::Black() });
                ribbon.StrokeThickness(0.75);
                const winrt::hstring tagName{ tg };
                ribbon.PointerEntered([this, tagName](const IInspectable& sdr, const PointerRoutedEventArgs&) {
                    if (_tagHoverBeginHandler)
                    {
                        if (const auto el = sdr.try_as<UIElement>())
                        {
                            _tagHoverBeginHandler(tagName, el);
                        }
                    }
                });
                ribbon.PointerExited([this](const IInspectable&, const PointerRoutedEventArgs&) {
                    if (_tagHoverEndHandler)
                    {
                        _tagHoverEndHandler();
                    }
                });
                tagRow.Children().Append(ribbon);
                ++shownRibbons;
            }
        }

        // Agentmaster (PENDING_INPUT.md): an UNSENT-DRAFT pulse at the top of the card body — when the
        // Fleet Observer detects the user has typed but not yet submitted a message in this session's
        // input box (the debounced SessionInfo::pendingInput), a goldenrod "3 dots" animation rides here,
        // mirroring the tab-strip pulse. The card rebuilds on the pending flip notify (SetPendingInput
        // notifies on the empty<->non-empty transition), so the dots appear/clear with the draft; a hover
        // previews the draft's first line. (Claude-only — Codex sessions aren't draft-scanned in v1.)
        if (!s.pendingInput.empty())
        {
            // Contrast-pick the dots' color for the card BODY background — the always-dark Manager fill
            // (#2E2E2E) — so they read here (this yields the LIGHT pending color; the DARK one is what
            // shows on a LIGHT tab strip). User-configurable via the Settings cog (pendingDots*).
            const auto dotsColor = PendingDotsColorFor(ColorHelper::FromArgb(0xFF, 0x2E, 0x2E, 0x2E),
                                                       _appSettings.pendingDotsLightColor,
                                                       _appSettings.pendingDotsDarkColor);
            auto dots = BuildPendingDots(5.0, dotsColor);
            std::wstring tip = L"A message is typed into this session's input box but hasn't been sent yet \x2014 the same dots ride its tab. It stays waiting until you (or Send now) submit it.";
            auto firstLine = s.pendingInput.substr(0, s.pendingInput.find(L'\n'));
            if (firstLine.size() > 120)
            {
                firstLine = firstLine.substr(0, 120) + L"\x2026";
            }
            tip += L"\n\n\x201C" + firstLine + L"\x201D";
            // STALENESS (PENDING_INPUT.md §5): the observation stamp refreshes every live scan tick,
            // so an age past a few ticks means this is a carried MEMORY — a restored/dormant/archived
            // session's persisted draft, not a live read. Label it honestly instead of presenting it
            // as current truth: it revalidates (confirm or clear) once the tab's claude runs.
            constexpr int64_t kPendingStaleAfterMs = 15000;
            if (s.pendingInputUnixMs > 0)
            {
                const int64_t age = NowMs() - s.pendingInputUnixMs;
                if (age > kPendingStaleAfterMs)
                {
                    tip += L"\n\nlast seen " + FormatSpan(age, true) + L" ago \x2014 remembered from before this session's tab (re)started; it clears automatically once the live input box reads empty.";
                }
            }
            // The paste-cache resolution (PENDING_INPUT.md §2b): name the verified file(s) behind any
            // "[Pasted text \x2026]" placeholder the draft carries, so the content is findable even
            // though the box only shows the marker.
            if (!s.pendingPasteRefs.empty())
            {
                tip += L"\n\n" + s.pendingPasteRefs + L"\n(paste-cache \x2014 <claude home>\\paste-cache)";
            }
            AgentSetTitledTip(dots, L"Unsent draft", winrt::hstring{ tip }, kCardTipDelay);
            stack.Children().Append(dots);
        }

        // Agentmaster (Codex-launch): a teal "codex" agent pill so a MANAGED Codex card reads distinct
        // from Claude (the implicit default — no pill, visuals unchanged).
        if (s.kind == AgentKind::Codex)
        {
            auto cp = Pill(L"codex", Color{ 0xFF, 0x4E, 0xC9, 0xB0 });
            cp.Opacity(0.9);
            cp.HorizontalAlignment(HorizontalAlignment::Left);
            AgentSetTitledTip(cp, L"Codex agent", L"This managed session runs the OpenAI Codex CLI instead of Claude. Agentmaster launches, resumes and tracks it like any session, but cannot drive its prompts \x2014 there is no queue or Tests Autorunner for Codex, and it reports only Running / Waiting / Idle.", kCardTipDelay);
            stack.Children().Append(cp);
        }
        {
            // The working dir reads as plain gray text under the title; name it AND explain the
            // per-directory color (a non-obvious concept) in one tip. Shows the EFFECTIVE work dir
            // (_WorkDirOf) — the same key the title-band color and the Explorer-Tree group use; when
            // the Inferred mode detected the session working OUTSIDE its launch cwd, the tip carries
            // the launch cwd so neither dir is ever hidden.
            const std::wstring effDir = _WorkDirOf(s);
            auto dirText = Text(winrt::hstring{ effDir }, 11, false, 0.6);
            const bool diverged = !PathEq(effDir, s.workingDir);
            AgentSetTitledTip(dirText,
                              diverged ? L"Inferred working directory" : L"Working directory",
                              diverged ? winrt::hstring{ L"Where this session actually works, judged by the files it touches \x2014 it was launched in " + s.workingDir + L". Every session sharing this folder wears the same color, on its card and on its tab." } :
                                         winrt::hstring{ L"Where this session runs. Every session sharing this folder wears the same color, on its card and on its tab." },
                              kCardTipDelay);
            stack.Children().Append(dirText);
        }

        // Agentmaster (API-error triage): when this card is in the Error state, show WHY the turn died
        // \x2014 the preserved error message + its HTTP status code (SessionInfo.errorMessage/errorStatus,
        // set by the scanner's recon-error). So the Error column is actionable at a glance ("\x26A0 429:
        // API Error: Server is temporarily limiting requests \x2026 Rate limited") instead of a bare
        // crimson dot. The code is prefixed so it stays visible if the line wraps/clips; the FULL,
        // untruncated message is in the hover tooltip. Crimson, matching the column + the state dot.
        if (s.state == SessionState::Error && !s.errorMessage.empty())
        {
            std::wstring shown = s.errorMessage;
            for (auto& c : shown)
            {
                if (c == L'\r' || c == L'\n' || c == L'\t')
                {
                    c = L' '; // a tidy card line; the tooltip keeps the original
                }
            }
            if (shown.size() > 200)
            {
                shown.resize(200);
                shown += L"\x2026"; // bound the card height; full text in the tip
            }
            std::wstring prefix = L"\x26A0 "; // ⚠
            if (s.errorStatus > 0)
            {
                prefix += std::to_wstring(s.errorStatus) + L": ";
            }
            auto errText = Text(winrt::hstring{ prefix + shown }, 11, false, 1.0);
            errText.Foreground(SolidColorBrush{ StateColor(SessionState::Error) });
            errText.TextWrapping(TextWrapping::Wrap); // show the reason fully (up to the 200-char bound)
            std::wstring tip = s.errorMessage;
            if (s.errorStatus > 0)
            {
                tip = L"HTTP " + std::to_wstring(s.errorStatus) + L"\n\n" + tip;
            }
            tip += L"\n\nThe card line is trimmed \x2014 this is the full text. The session leaves Error on its next turn; right-click \x2192 Move to Idle / Done to dismiss it now.";
            AgentSetTitledTip(errText, L"Why the turn failed", winrt::hstring{ tip }, kCardTipDelay);
            stack.Children().Append(errText);
        }

        // Per-session timing (created-ago / active-for / last-activity-ago) from the transcript.
        {
            const int64_t last = s.convLastActivityUnixMs ? s.convLastActivityUnixMs : s.lastActivityUnixMs;
            if (auto t = TimingText(s.convCreatedUnixMs, last))
            {
                stack.Children().Append(t);
                _cardTimingBinds.push_back({ s.id, t }); // the 30s tick rewrites this text in place (perf — no rebuild)
            }
        }

        // Agentmaster (current-model adornment) + context occupancy, on ONE dim row — both derive
        // from the same newest assistant line the SessionScanner tails. The MODEL is the transcript
        // truth (SessionDisplayModel: currentModel — what the last reply actually ran on, so a
        // mid-session /model switch shows here on its next reply — falling back to the launch-request
        // `model`, which is also where a managed Codex's rollout model lives), shortened for the card
        // (ShortModelName: "fable-5" / "opus-4.6", never "claude-opus-4-6-20260105"; a Codex
        // "gpt-5.1-codex" passes through verbatim). The ctx TOKEN COUNT keeps its raw form (PR
        // feedback, Eli: a % needs a context-window denominator that can't be reliably known from the
        // model id — Opus 4.8 doesn't advertise its 1M variant — so the raw count is unambiguous and
        // matches what Claude Code reports). Row shown once either fact exists.
        {
            auto modelCtxRow = StackPanel{};
            modelCtxRow.Orientation(Orientation::Horizontal);
            modelCtxRow.Spacing(8);
            if (const std::wstring shortModel = ::Agentmaster::ShortModelName(::Agentmaster::SessionDisplayModel(s), _ModelFamilies()); !shortModel.empty()) // parsed once per settings string, not per card (perf)
            {
                auto modelText = Text(winrt::hstring{ shortModel }, 10, false, 0.45);
                std::wstring mtip = L"What this session's last reply actually ran on, read from its transcript \x2014 so a mid-session /model switch shows up here on the next reply, not before.";
                if (!s.currentModel.empty())
                {
                    mtip += L"\n\n" + s.currentModel; // the full id behind the short form
                }
                else if (!s.model.empty())
                {
                    mtip += L"\n\nNothing has replied yet, so this is only what was asked for at launch: " + s.model;
                }
                AgentSetTitledTip(modelText, L"Model", winrt::hstring{ mtip }, kCardTipDelay);
                modelCtxRow.Children().Append(modelText);
            }
            if (s.contextTokens > 0)
            {
                auto ctxText = Text(winrt::hstring{ L"ctx " } + winrt::hstring{ FormatTokenCount(s.contextTokens) }, 10, false, 0.45);
                const auto tip = GroupDigits(s.contextTokens) +
                                 std::wstring{ L" tokens carried by this session's newest turn (input + cache + output) \x2014 the same figure Claude Code reports. Shown as a raw count, not a percentage: the context window depends on the model variant in use, which can't be read reliably from its id." };
                AgentSetTitledTip(ctxText, L"Context", winrt::hstring{ tip }, kCardTipDelay);
                modelCtxRow.Children().Append(ctxText);
            }
            if (modelCtxRow.Children().Size() > 0)
            {
                stack.Children().Append(modelCtxRow);
            }
        }

        // autorunner badge ⚙ sent/total + the "still server-cached" ⚡ indicator, on ONE row (⚡ to the
        // right of ⚙ N/M). The ⚙ badge shows only when there's a queue; the ⚡ shows whenever the
        // session is still inside Claude's server-side prompt-cache window (serverCacheMinutes).
        {
            auto metaRow = StackPanel{};
            metaRow.Orientation(Orientation::Horizontal);
            metaRow.Spacing(8);

            // DEV OR --debug: the ⚙ sent/total badge is the Auto Testing prompt queue, gated to the
            // AgentmasterDev package OR a `--debug` / AGENTMASTER_DEBUG release (IsDevOrDebugPackage; an
            // ordinary release never queues prompts — the autorunner is off). The ⚡ server-cache
            // indicator below is unrelated and stays in every build.
            if (::Agentmaster::Profiles::IsDevOrDebugPackage() && !s.queue.empty())
            {
                int sent = 0;
                for (const auto& p : s.queue)
                {
                    if (p.status == PromptStatus::Sent)
                    {
                        ++sent;
                    }
                }
                const auto badge = winrt::hstring{ L"\x2699 " } + winrt::to_hstring(sent) + L"/" + winrt::to_hstring(static_cast<int>(s.queue.size()));
                auto bt = Text(badge, 11, false, 0.8);
                if (s.autorunner.mode != AutorunnerMode::Off)
                {
                    bt.Foreground(SolidColorBrush{ Colors::DodgerBlue() });
                }
                // Agentmaster (the INTERRUPT HOLD — DELIVERY.md §14): a gray badge on a session whose
                // autorunner parked ITSELF (the user pressed Esc) says so — and names the mode that
                // comes back on their next message — instead of reading like a plain "waits for you".
                std::wstring queueTip = L"Prompts already sent / total queued for this session. Blue means its Tests Autorunner is on, so the rest will go out on their own as turns complete; gray means they wait for you.";
                if (::Agentmaster::InterruptHoldActive(s.autorunner))
                {
                    queueTip += std::wstring{ L" Right now the autorunner is parked by your interrupt (Esc) and resumes to " } +
                                ::Agentmaster::AutorunnerModeLabel(s.autorunner.interruptHeldMode) +
                                L" on your next message; setting the mode yourself cancels that.";
                }
                AgentSetTitledTip(bt, L"Auto Testing queue", winrt::hstring{ queueTip }, kCardTipDelay);
                metaRow.Children().Append(bt);
            }

            // Agentmaster (Waiting-for-you "unread" model): a ⚡ "still server-cached" hint. Claude's
            // server-side prompt cache stays warm for ~serverCacheMinutes after the last REAL API
            // turn, so a follow-up within the window reuses the cached prefix (cheaper & faster).
            // Purely cosmetic; shown only while the card is inside that window AND the session is AT
            // REST (Waiting-for-you / Needs-approval / Error / Idle / Done — never Running: mid-turn
            // there is nothing to decide, and the hint would be permanently lit because a running turn
            // keeps refreshing the timestamps the window is measured from). The board's periodic
            // refresh (a 30s timer + every registry event) clears it once the window lapses.
            // The tab strip's SPARK CROWN reads the SAME predicate, so the two can never disagree.
            // Keyed on the TWO API-turn signals via the pure ServerCacheStillWarm (SessionModels.h)
            // — the transcript's line-derived conv activity + the hook-side IsApiTurnEvidence stamp —
            // deliberately NOT the lastActivityUnixMs decay anchor, which a launch/adopt/resume
            // SessionStart and the "Move to Waiting-for-you" triage promote stamp "now" with ZERO
            // API traffic (the reported ⚡ false positives: a just-adopted / just-launched /
            // manually-promoted card read "still cached" though no request was ever made).
            // Claude-only — the cache (and this tooltip's copy) are Claude's; a managed Codex
            // never shows it.
            {
                const uint32_t cacheMin = _appSettings.serverCacheMinutes ? _appSettings.serverCacheMinutes : 5;
                const bool warmNow = ::Agentmaster::ServerCacheStillWarm(s, cacheMin, NowMs());
                _cardWarmShown[s.id] = warmNow; // the 30s tick compares against this to decide whether a rebuild is due (perf)
                if (warmNow)
                {
                    auto cacheGlyph = Text(L"\x26A1", 11, false, 0.95); // ⚡ warm cache
                    cacheGlyph.Foreground(Fill(0xFF, 0xFF, 0xC1, 0x07)); // amber
                    AgentSetTitledTip(cacheGlyph, L"Still server-cached", winrt::hstring{ L"Claude's prompt cache stays warm for about " } + winrt::to_hstring(static_cast<int>(cacheMin)) + L" minutes after a real API turn, so a follow-up sent now reuses this session's cached context \x2014 cheaper and faster than starting cold. The glyph clears itself once the window lapses (set the span in Settings \x2192 Behavior).", kCardTipDelay);
                    metaRow.Children().Append(cacheGlyph);
                }
            }

            if (metaRow.Children().Size() > 0)
            {
                stack.Children().Append(metaRow);
            }
        }

        // Agentmaster: a hover-revealed "\x22EF" more-button in the card's top-right corner — a
        // discoverable twin of the right-click menu (some users never right-click). Wrap the content
        // in a Grid so the dots float over the top-right; they carry the SAME session menu as a
        // Button.Flyout (a click opens it). Hidden at rest, faded in on the card's hover (wired on
        // PointerEntered/Exited below; OpacityTransition animates the Opacity change — no Storyboard
        // to manage). IsHitTestVisible(false) at rest keeps the transparent corner from eating a card
        // click. Built per-card; cheap.
        auto dotsBtn = Button{};
        {
            FontIcon moreGlyph;
            moreGlyph.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
            moreGlyph.Glyph(L"\xE712"); // "More" — three dots
            moreGlyph.FontSize(14);
            dotsBtn.Content(moreGlyph);
        }
        dotsBtn.Padding(Thickness{ 4, 0, 4, 0 });
        dotsBtn.MinWidth(26);
        dotsBtn.Height(20);
        dotsBtn.HorizontalAlignment(HorizontalAlignment::Right);
        dotsBtn.VerticalAlignment(VerticalAlignment::Top);
        dotsBtn.Margin(Thickness{ 0, 4, 6, 0 }); // inset from the top-right corner (the card no longer pads its content); floats over the title band, clear of the rounded corner
        dotsBtn.Background(Fill(0x66, 0x30, 0x30, 0x30)); // faint chip so the glyph reads over the title behind it
        dotsBtn.Foreground(Fill(0xF0, 0xFF, 0xFF, 0xFF));
        dotsBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        dotsBtn.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
        dotsBtn.Opacity(0.0); // hidden at rest; the card's hover fades it in
        dotsBtn.IsHitTestVisible(false); // an invisible corner must not swallow a card click
        dotsBtn.IsTabStop(false); // a hover affordance — keep the invisible button out of the keyboard tab order (the menu is reachable via right-click / the context-menu key)
        {
            ScalarTransition st;
            st.Duration(winrt::Windows::Foundation::TimeSpan{ std::chrono::milliseconds{ 140 } });
            dotsBtn.OpacityTransition(st); // genuine fade on any Opacity change
        }
        AgentSetTitledTip(dotsBtn, L"More", L"This session's actions \x2014 jump to its tab, rename, tag, fork, restart, close, copy its details, or start a new session in its folder. Exactly what right-clicking the card gives you.", kCardTipDelay);
        dotsBtn.Flyout(_MakeLazySessionMenu(s.id, _WorkDirOf(s), dotsBtn)); // a click opens the session menu (built at OPEN time — perf; the button anchors its Tags panel); Open-New-Here targets the effective work dir
        const auto dotsWeak = winrt::make_weak(dotsBtn);

        // Agentmaster: the body carries the inset the card used to own (card Padding is now 0 so
        // the colored band can bleed to the card's rounded top corners + side edges); the outer
        // StackPanel pins the band over the body with spacing 0 so the band's straight bottom sits
        // flush against the body.
        auto bodyBorder = Border{};
        bodyBorder.Padding(Thickness{ 8, 6, 8, 6 });
        bodyBorder.Child(stack);

        auto outer = StackPanel{};
        outer.Spacing(0);
        outer.Children().Append(band);
        if (tagRow)
        {
            outer.Children().Append(tagRow); // after the band in z-order — the ribbons' top ~30% draws OVER the colored band
        }
        outer.Children().Append(bodyBorder);

        // Agentmaster: the hover/selection outline is drawn as an OVERLAY ring, NOT on the card
        // Button's own border. A Button's BorderThickness is part of its layout box, so toggling it
        // on hover grows the card (every card below shifts) AND insets the title band off its rounded
        // corners (the content shifts in) — the visible "card jumps when I mouse over it" bug. This
        // ring is a transparent Border layered in the same Grid cell (drawn ON TOP, IsHitTestVisible
        // false), so changing its thickness redraws the outline inward over the card edges WITHOUT
        // resizing the card or moving anything. 0 at rest, 1 on hover, 2 when selected. Its corners
        // match the card so the outline rounds with the edge. (As a bonus this also stops a SELECTED
        // card from being 2px larger than its unselected siblings — both are now the same size.)
        auto ring = Border{};
        ring.CornerRadius(CornerRadius{ 4, 4, 4, 4 }); // matches the card rounding
        ring.BorderBrush(SolidColorBrush{ accent });
        ring.BorderThickness(selected ? Thickness{ 2, 2, 2, 2 } : Thickness{ 0, 0, 0, 0 });
        ring.IsHitTestVisible(false); // a decorative overlay must not eat card clicks/hover
        ring.HorizontalAlignment(HorizontalAlignment::Stretch);
        ring.VerticalAlignment(VerticalAlignment::Stretch);
        const auto ringWeak = winrt::make_weak(ring);

        auto grid = Grid{};
        grid.Children().Append(outer);
        grid.Children().Append(ring); // over the content, under the dots
        grid.Children().Append(dotsBtn);

        // Agentmaster (Waiting-for-you countdown bar): a 1px goldenrod bar pinned INSIDE the card's
        // bottom edge that drains from full width (100% of the waiting window) to 0 as the
        // Waiting-for-you timeout approaches. At empty the wait has expired — a READ card then decays to
        // Idle / Done (an unread one keeps waiting until read). Shown only for a WaitingForInput card with
        // a finite timeout that isn't manually held unread (a Mark-Unread / "Never" card never time-decays,
        // so it has no countdown). Overlaid in the grid OVER the hover/selected ring so it stays visible,
        // and IsHitTestVisible(false) so the 1px strip never eats a card click. ScaleX (origin LEFT) =
        // fraction remaining; _progressTimer drains it live in place, and each _RebuildBoard re-seeds it.
        if (s.state == SessionState::WaitingForInput && !s.manualUnread && _appSettings.waitingForYouTimeoutMinutes > 0 && s.lastActivityUnixMs > 0)
        {
            const int64_t timeoutMs = static_cast<int64_t>(_appSettings.waitingForYouTimeoutMinutes) * 60000;
            double frac = 1.0 - static_cast<double>(NowMs() - s.lastActivityUnixMs) / static_cast<double>(timeoutMs);
            frac = frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac);

            auto barScale = ScaleTransform{};
            barScale.ScaleX(frac);

            auto bar = Border{};
            bar.Height(1);
            bar.VerticalAlignment(VerticalAlignment::Bottom);
            bar.HorizontalAlignment(HorizontalAlignment::Stretch);
            bar.Background(SolidColorBrush{ StateColor(SessionState::WaitingForInput) }); // goldenrod, matching the state
            bar.RenderTransformOrigin(Point{ 0.0f, 0.0f }); // drain from the RIGHT (the left edge stays pinned)
            bar.RenderTransform(barScale);
            bar.IsHitTestVisible(false); // a decorative 1px overlay must never swallow a card click
            grid.Children().Append(bar);

            _cardProgress.push_back(CardProgress{ bar, s.lastActivityUnixMs, timeoutMs });
        }

        auto card = Button{};
        card.Content(grid);
        card.HorizontalAlignment(HorizontalAlignment::Stretch);
        card.HorizontalContentAlignment(HorizontalAlignment::Stretch); // let the Grid fill so the band reaches the card edges + the dots reach the true top-right corner
        card.Padding(Thickness{ 0, 0, 0, 0 }); // band + body own their insets now, so the title band can reach the rounded top corners
        card.Margin(Thickness{ 0, 0, 0, 6 });
        card.CornerRadius(CornerRadius{ 4, 4, 4, 4 }); // explicit, so the title band's top corners (4,4,0,0) line up with the card rounding
        card.Background(Fill(selected ? 0x40 : 0x20, 0x80, 0x80, 0x80));
        // Agentmaster: the state-colored outline lives on the `ring` OVERLAY above, NOT on this
        // Button's own border — toggling a Button BorderThickness grows the card and nudges the
        // title band, which is the shift we're avoiding. The card's own border stays a constant 0;
        // only the overlay ring's thickness toggles (0 rest / 1 hover / 2 selected).
        card.BorderThickness(Thickness{ 0, 0, 0, 0 });
        if (!selected)
        {
            // Grow the overlay ring on hover (a selected card keeps its fixed 2). ringWeak is a
            // weak_ref so the card's handler never strong-captures a child that chains back to the
            // card (the no-self-capture rule — a strong ring ref would cycle
            // card -> handler -> ring -> grid -> card and leak the whole tree).
            card.PointerEntered([ringWeak](const IInspectable&, const PointerRoutedEventArgs&) {
                if (const auto r = ringWeak.get())
                {
                    r.BorderThickness(Thickness{ 1, 1, 1, 1 });
                }
            });
            card.PointerExited([ringWeak](const IInspectable& sender, const PointerRoutedEventArgs& e) {
                if (PointerStillWithin(sender, e))
                {
                    return; // a child label's exit bubbled up — the pointer never left the card; don't flicker the ring
                }
                if (const auto r = ringWeak.get())
                {
                    r.BorderThickness(Thickness{ 0, 0, 0, 0 });
                }
            });
        }
        const auto id = s.id;
        // Agentmaster (Linked Lenses): report hover so the page pills THIS session's terminal tab
        // while the Manager tab is active (a live preview that follows the mouse). Capture id by
        // value + `this` (never the Button into its own handler — a self-capture leaks the element);
        // fires for selected cards too, so hovering the selected card keeps its tab pilled.
        // Also fade the "\x22EF" more-button in (and arm its hit-testing) while the card is hovered;
        // fade it out on exit. dotsWeak is a weak_ref so the handler never strong-captures the button
        // it lives under (the codebase's no-self-capture rule). Hovering the dots (a child) keeps the
        // card "entered" — a child within the card's bounds doesn't raise the card's PointerExited —
        // so the dots stay up while you aim for them.
        card.PointerEntered([this, id, dotsWeak](const IInspectable&, const PointerRoutedEventArgs&) {
            _ReportHover(id, true);
            if (const auto d = dotsWeak.get())
            {
                d.IsHitTestVisible(true);
                d.Opacity(0.85);
            }
        });
        card.PointerExited([this, id, dotsWeak](const IInspectable& sender, const PointerRoutedEventArgs& e) {
            if (PointerStillWithin(sender, e))
            {
                return; // a child label's exit bubbled up — keep the tab pill + dots up (Linked-Lenses anti-flicker)
            }
            _ReportHover(id, false);
            if (const auto d = dotsWeak.get())
            {
                d.Opacity(0.0);
                d.IsHitTestVisible(false);
            }
        });
        // Single click = select; double click (within the OS threshold) = Activate (jump to
        // the session's live terminal tab — the page fans out to the hosting WINDOW when the
        // tab lives in another one), mirroring the Explorer Tree rows. A Button swallows
        // DoubleTapped, so we time the successive clicks ourselves.
        card.Click([this, id](const IInspectable&, const RoutedEventArgs&) {
            // Shift+Click = Activate Tab IN PLACE (start a dormant session's claude without switching to
            // it) — the click twin of the "Activate Tab (Shift+Click)" menu item. _activateDormantHandler
            // is a no-op when the session isn't dormant / isn't hosted here, so this is safe on any card.
            if (ShiftHeld())
            {
                if (_activateDormantHandler)
                {
                    _activateDormantHandler(winrt::hstring{ id });
                }
                return; // don't select / jump / sync scope — Shift+Click is the in-place activate gesture
            }
            const auto nowTick = ::GetTickCount64();
            const bool dbl = (id == _lastCardClickId) && (nowTick - _lastCardClickTick) <= ::GetDoubleClickTime();
            _lastCardClickId = id;
            _lastCardClickTick = nowTick;
            if (dbl && _activateHandler)
            {
                _activateHandler(winrt::hstring{ id });
            }
            else
            {
                // Agentmaster (Linked Lenses): a managed board-card single-click syncs the Explorer
                // Tree scope to where THIS session lives — LOCAL when this window hosts it, else
                // GLOBAL (hosted by another window). The managed twin of an External card click
                // switching the tree to EXTERNAL (_SelectExternal), so all three regions agree on the
                // clicked card's lens. Select FIRST (aims the Launch box, sets the selection), THEN
                // sync the scope LAST with its own refresh: _SetTreeScope no-ops (no refresh) when the
                // scope is already correct, and refreshes when it changes — so a re-click of the
                // already-selected card (where _SelectSession early-outs without refreshing) still
                // repaints if the user toggled the scope away in between. Only when the locality is
                // knowable: with no provider (mid-init / standalone tests) leave the scope as-is,
                // exactly like the board's own LOCAL filter (see _RebuildBoard).
                _SelectSession(id);
                if (_localScopeProvider)
                {
                    const auto localIds = _localScopeProvider();
                    const bool isLocal = localIds.find(id) != localIds.end();
                    _SetTreeScope(isLocal ? TreeScope::Local : TreeScope::Global, /*refresh*/ true);
                }
                // Agentmaster (double-click fix): _SelectSession (+ the scope sync above) just TORE
                // DOWN and rebuilt this board — every card Button is cleared + recreated from scratch
                // (_RebuildBoard: _boardHost.Children().Clear() then _MakeCard per session). The
                // replacement card for THIS id is in the tree but NOT yet arranged (a fresh element
                // has 0x0 bounds until the next async layout pass), so a second mouse-down arriving
                // microseconds later (the user double-clicking to Activate) hit-tests to nothing and
                // its Click never fires — a double-click on an UNSELECTED card silently did nothing
                // ("if it's not selected it doesn't work"), while an already-selected card worked (its
                // _SelectSession early-outs, no rebuild). Force a synchronous layout so the replacement
                // card has real bounds NOW and the follow-up click lands on it. Only this not-selected
                // path rebuilds, so it is the only one that needs it; cheap (a card click is rare).
                if (_boardHost)
                {
                    _boardHost.UpdateLayout();
                }
            }
        });
        // Right-click (or context key / long-press): the SAME menu as the Explorer-Tree session
        // row — Rename… / Archive… / Open New Session Here — one card/row, one action set
        // (Linked Lenses). The menu acts on the captured id/cwd, never "the selected session".
        card.ContextFlyout(_MakeLazySessionMenu(id, _WorkDirOf(s), card)); // built at OPEN time (perf); the card anchors its Tags panel; Open-New-Here targets the effective work dir
        // Agentmaster: tag + register the card so _Refresh can RESTORE keyboard focus onto it after
        // a rebuild (a title/state change recreates every card). "b:" marks the board lens, so the
        // focused element's id + lens are read off its Tag alone — no visual-tree ancestry walk.
        card.Tag(winrt::box_value(winrt::hstring{ L"b:" + s.id }));
        _boardCardsById[s.id] = card;
        return card;
    }

    // ---- Resizable splitters ------------------------------------------------

    Border AgentManagerContent::_MakeSplitter(bool vertical)
    {
        // A thin grab-bar in its own auto-sized grid track. The near-transparent fill keeps
        // the whole bar hit-testable; a centered grip line + hover highlight signal it is
        // draggable, and the OS cursor flips to the resize arrow while the pointer is over it.
        const auto idleGrip = Fill(0x40, 0x80, 0x80, 0x80);
        const auto hotGrip = Fill(0x90, 0xC0, 0xC0, 0xC0);

        auto grip = Border{};
        grip.Background(idleGrip);
        grip.CornerRadius(CornerRadius{ 1, 1, 1, 1 });

        auto bar = Border{};
        if (vertical)
        {
            bar.Width(10);
            bar.VerticalAlignment(VerticalAlignment::Stretch);
            grip.Width(2);
            grip.HorizontalAlignment(HorizontalAlignment::Center);
            grip.VerticalAlignment(VerticalAlignment::Stretch);
            grip.Margin(Thickness{ 0, 10, 0, 10 });
        }
        else
        {
            bar.Height(10);
            bar.HorizontalAlignment(HorizontalAlignment::Stretch);
            grip.Height(2);
            grip.VerticalAlignment(VerticalAlignment::Center);
            grip.HorizontalAlignment(HorizontalAlignment::Stretch);
            grip.Margin(Thickness{ 10, 0, 10, 0 });
        }
        bar.Background(Fill(0x01, 0x80, 0x80, 0x80)); // ~invisible, yet hit-testable
        bar.Child(grip);
        // Agentmaster: the grab bar is draggable but easy to miss (it is near-invisible at rest);
        // name what it resizes so the affordance is discoverable beyond the hover cursor change.
        AgentSetTitledTip(bar, L"Resize", vertical ?
                                              winrt::hstring{ L"Drag to give the Explorer Tree or the pane on its right more room \x2014 they share this divider. The split is remembered per window." } :
                                              winrt::hstring{ L"Drag to give the Triage Board or the panes below it more room \x2014 they share this divider. The split is remembered per window." });

        const auto cursorType = vertical ? CoreCursorType::SizeWestEast : CoreCursorType::SizeNorthSouth;

        bar.PointerEntered([this, grip, cursorType, hotGrip](const IInspectable&, const PointerRoutedEventArgs&) {
            ApplyCursor(cursorType);
            grip.Background(hotGrip);
        });
        bar.PointerExited([this, grip, idleGrip](const IInspectable& sender, const PointerRoutedEventArgs& e) {
            // The grip is a hit-testable child filling the bar's center band, so moving off it onto the
            // bar's own margin area bubbles the grip's PointerExited here — a false leave that would dim
            // the grip (drop the "draggable" highlight) while the pointer is still on the divider. Swallow
            // those; only a real leave (pointer outside the bar) resets. (PointerStillWithin, like the cards.)
            if (PointerStillWithin(sender, e))
            {
                return;
            }
            if (_dragKind == DragKind::None) // mid-drag the pointer may leave the thin bar — keep it hot
            {
                ApplyCursor(CoreCursorType::Arrow);
                grip.Background(idleGrip);
            }
        });
        bar.PointerPressed([this, vertical, grip, hotGrip](const IInspectable& s, const PointerRoutedEventArgs& e) {
            grip.Background(hotGrip);
            _OnSplitterPressed(s, e, vertical);
        });
        bar.PointerMoved([this, vertical, cursorType](const IInspectable&, const PointerRoutedEventArgs& e) {
            if (_dragKind == DragKind::None)
            {
                ApplyCursor(cursorType); // re-assert the resize cursor while hovering (covers post-release)
                return;
            }
            _OnSplitterMoved(e, vertical);
        });
        bar.PointerReleased([this, grip, idleGrip](const IInspectable& s, const PointerRoutedEventArgs& e) {
            _OnSplitterReleased(s, e);
            grip.Background(idleGrip);
        });
        bar.PointerCaptureLost([this, grip, idleGrip](const IInspectable& s, const PointerRoutedEventArgs& e) {
            _OnSplitterReleased(s, e);
            grip.Background(idleGrip);
        });
        return bar;
    }

    void AgentManagerContent::_OnSplitterPressed(const IInspectable& sender, const PointerRoutedEventArgs& e, bool vertical)
    {
        if (!_root)
        {
            return;
        }
        _dragKind = vertical ? DragKind::Cols : DragKind::Rows;
        // Pin the two tracks' sizes + the pointer's root-relative coord at press; the move
        // handler derives everything from these fixed values, so the boundary tracks the
        // cursor 1:1 with no feedback from the live re-layout. Track ActualWidth/Height is the
        // exact star-space allotment, so star weights set to pixels land pixel-perfect.
        const auto pos = e.GetCurrentPoint(_root).Position();
        if (vertical)
        {
            _dragOrigin = pos.X;
            _dragSizeA = _treeCol ? _treeCol.ActualWidth() : 0.0;
            _dragSizeB = _planCol ? _planCol.ActualWidth() : 0.0;
        }
        else
        {
            _dragOrigin = pos.Y;
            _dragSizeA = _boardRow ? _boardRow.ActualHeight() : 0.0;
            _dragSizeB = _bottomRow ? _bottomRow.ActualHeight() : 0.0;
        }
        if (const auto el = sender.try_as<UIElement>())
        {
            el.CapturePointer(e.Pointer());
        }
        ApplyCursor(vertical ? CoreCursorType::SizeWestEast : CoreCursorType::SizeNorthSouth);
        e.Handled(true);
    }

    void AgentManagerContent::_OnSplitterMoved(const PointerRoutedEventArgs& e, bool vertical)
    {
        if (_dragKind == DragKind::None || !_root)
        {
            return;
        }
        const auto pos = e.GetCurrentPoint(_root).Position();
        const double cur = vertical ? static_cast<double>(pos.X) : static_cast<double>(pos.Y);
        const double total = _dragSizeA + _dragSizeB;
        constexpr double minPx = 80.0; // never let a pane shrink below this
        if (total < (minPx * 2.0) + 1.0)
        {
            return; // not enough room to split sensibly — leave the panes alone
        }
        const double newA = std::clamp(_dragSizeA + (cur - _dragOrigin), minPx, total - minPx);
        const double newB = total - newA;
        const auto star = [](double v) { return GridLengthHelper::FromValueAndType(v, GridUnitType::Star); };
        if (vertical)
        {
            if (_treeCol)
            {
                _treeCol.Width(star(newA));
            }
            if (_planCol)
            {
                _planCol.Width(star(newB));
            }
        }
        else
        {
            if (_boardRow)
            {
                _boardRow.Height(star(newA));
            }
            if (_bottomRow)
            {
                _bottomRow.Height(star(newB));
            }
        }
        e.Handled(true);
    }

    void AgentManagerContent::_OnSplitterReleased(const IInspectable& sender, const PointerRoutedEventArgs& e)
    {
        if (_dragKind == DragKind::None)
        {
            return; // a capture-lost echo of our own release, or a stray event — nothing to do
        }
        const bool vertical = (_dragKind == DragKind::Cols);
        _dragKind = DragKind::None; // clear BEFORE releasing capture so the re-entrant CaptureLost no-ops

        if (const auto el = sender.try_as<UIElement>())
        {
            el.ReleasePointerCaptures();
        }

        // Persist the new split as a fraction read straight from the star weights we just
        // applied (synchronous + exact, unlike ActualWidth which trails by a layout pass).
        // a/t is correct whether the weights are the seed FRACTIONS (sum == 1.0, e.g. a stray
        // click with no drag) or post-drag PIXELS (sum in the hundreds); guard only t == 0.
        const auto fraction = [](double a, double b) {
            const double t = a + b;
            return t > 0.0 ? std::clamp(a / t, 0.1, 0.9) : 0.4;
        };
        if (vertical)
        {
            if (_treeCol && _planCol)
            {
                _layout.treeFraction = fraction(_treeCol.Width().Value, _planCol.Width().Value);
            }
        }
        else
        {
            if (_boardRow && _bottomRow)
            {
                _layout.boardFraction = fraction(_boardRow.Height().Value, _bottomRow.Height().Value);
            }
        }
        ::Agentmaster::SaveLayout(_layout);
        _NotifyLensChanged(); // M10: splitter sizes ride in the per-window record too

        ApplyCursor(CoreCursorType::Arrow);
        e.Handled(true);
    }

    // Agentmaster (cog "Overlay opacity" slider): position both dots from their current values + refresh the
    // "Rest N% · Hover M%" readout. A FIXED track width makes this layout-independent, so it is correct
    // before first layout too (seed on _ShowSettings) and on every drag move. Values are already clamped
    // [0,1] + rest<=hover by the drag handler / seed.
    void AgentManagerContent::_LayoutOverlayOpacitySlider()
    {
        const double usable = kOverlayTrackW - kOverlayThumb;
        if (_overlayRestThumb)
        {
            Canvas::SetLeft(_overlayRestThumb, _overlayRestVal * usable);
        }
        if (_overlayHoverThumb)
        {
            Canvas::SetLeft(_overlayHoverThumb, _overlayHoverVal * usable);
        }
        if (_overlayOpacityLabel)
        {
            const int rp = static_cast<int>(_overlayRestVal * 100.0 + 0.5);
            const int hp = static_cast<int>(_overlayHoverVal * 100.0 + 0.5);
            _overlayOpacityLabel.Text(winrt::hstring{ L"Rest " + std::to_wstring(rp) + L"%   \x00B7   Hover " + std::to_wstring(hp) + L"%" });
        }
    }

    void AgentManagerContent::_RebuildBoard(const std::vector<SessionInfo>& sessions)
    {
        // Agentmaster: preserve each column's vertical scroll offset across this rebuild. _RebuildBoard
        // recreates the per-column ScrollViewers from scratch (a fresh ScrollViewer sits at offset 0),
        // so without this a mere _Refresh — a select, a state/title change, an observer enrichment —
        // would snap the board to the TOP, losing the card the user just clicked near the bottom of a
        // tall column. The remembered offsets live in the DURABLE member _boardColumnOffsets (keyed by
        // column title), not a per-rebuild local: a refresh that lands before a PRIOR rebuild's
        // restore-on-Loaded has fired would otherwise read that rebuild's fresh, not-yet-restored
        // ScrollViewer sitting at 0 and PERMANENTLY lose the saved scroll — the "a click jumps the scroll
        // to top" race when a background scanner/observer refresh coincides with the user's click. Capture
        // the live offsets now (the old scrollers are still valid), but ONLY trust a LOADED ScrollViewer:
        // a not-yet-loaded SV (IsLoaded == false) reports a meaningless 0 — keep the remembered offset
        // rather than clobber it; a genuinely-scrolled-to-top LOADED column records its real 0. Each new
        // column re-applies its remembered offset on Loaded (see _MakeBoardColumn).
        for (const auto& [key, sv] : _boardColumnScrollers)
        {
            if (!sv)
            {
                continue;
            }
            const auto off = sv.VerticalOffset();
            if (off > 0.0)
            {
                _boardColumnOffsets[key] = off; // a real, restored scroll position
            }
            else if (sv.IsLoaded())
            {
                _boardColumnOffsets[key] = 0.0; // genuinely at the top (the SV has loaded, so 0 is real)
            }
            // else: a fresh / not-yet-restored SV reading 0 — preserve the remembered offset (its
            // restore-on-Loaded is still pending), so a coincident refresh can't lose the user's scroll.
        }
        _boardColumnScrollers.clear(); // refilled by _MakeBoardColumn below

        _boardHost.Children().Clear();
        _boardCardsById.clear(); // refilled by _MakeCard below (focus-restore map; see _Refresh)
        _cardProgress.clear(); // Agentmaster: refilled by _MakeCard for each Waiting-for-you countdown bar (drained by _progressTimer)
        if (_boardScope)
        {
            // The label exists only WHILE a directory is scoped (paired with "Show all"); unscoped
            // it collapses — the old "[all directories]" placeholder was display-only noise.
            _boardScope.Text(_scopeDir.empty() ? winrt::hstring{} : (winrt::hstring{ L"[scope: " } + winrt::hstring{ _scopeDir } + L"]"));
            _boardScope.Visibility(_scopeDir.empty() ? Visibility::Collapsed : Visibility::Visible);
        }
        if (_showAllBtn)
        {
            // "Show all" disappears when we ARE showing all (no scope) and reappears once a dir is scoped.
            _showAllBtn.Visibility(_scopeDir.empty() ? Visibility::Collapsed : Visibility::Visible);
        }
        if (_clearSelBtn)
        {
            // "Clear" is shown only while something is selected (managed OR external), like "Show all".
            const bool hasSel = !_selectedId.empty() || !_selectedExternalSessionId.empty();
            _clearSelBtn.Visibility(hasSel ? Visibility::Visible : Visibility::Collapsed);
        }

        // Agentmaster: the board's LOCAL/GLOBAL scope (the toggle next to the title — ONE state
        // with the Explorer Tree's; External there reads GLOBAL here). LOCAL keeps only THIS
        // window's sessions (the page's _claudeTabs via _localScopeProvider), computed once for
        // all five columns; with no provider (mid-init / standalone tests) everything counts as
        // local — no filter — exactly like the tree. The External (N) census column below is NOT
        // scoped by this: externals are not managed sessions of any window.
        const bool boardLocal = (_treeScope == TreeScope::Local) && static_cast<bool>(_localScopeProvider);
        std::unordered_set<std::wstring> boardLocalIds;
        if (boardLocal)
        {
            boardLocalIds = _localScopeProvider();
        }

        // Agentmaster (bookmark tags): re-read the LIVE sessions' tags + the user-picked tag colors
        // once per rebuild so each card can hang its bookmark ribbons out of the title band
        // (_MakeCard reads _boardTags/_boardTagColors). Per-session GetSessionTags (a tiny
        // session-store file each, absent for most sessions) beats LoadAllSessionTags here — the
        // store holds every session EVER titled/starred/tagged while the board shows only the live
        // handful. The colors file loads only when something is actually tagged.
        //
        // The map is narrowed to the sessions this board actually SHOWS (live + the LOCAL/GLOBAL scope
        // + the directory scope — the same three guards each state column applies below, minus the tag
        // filter itself). That is what makes the header's tag chips honest: every chip has at least one
        // card behind it and its count is the number of cards you will see, instead of advertising a
        // tag whose only carrier lives in another window or another folder.
        _boardTags.clear();
        // sid -> last activity. NOT the chips' ordering (that is alphabetical — see
        // _RebuildBoardTagChips): CollectGlobalTags also uses it to pick which carrier's CASING a tag
        // displays under, so feeding it real activity keeps a chip's spelling identical to the same
        // tag's chip on the Sessions page.
        std::unordered_map<std::wstring, int64_t> boardTagActivity;
        for (const auto& s : sessions)
        {
            if (!s.live)
            {
                continue;
            }
            if (boardLocal && boardLocalIds.find(s.id) == boardLocalIds.end())
            {
                continue; // LOCAL scope: hosted by another window
            }
            if (!_scopeDir.empty() && !PathEq(_WorkDirOf(s), _scopeDir))
            {
                continue; // dir scope: another folder's session
            }
            if (auto tags = ::Agentmaster::GetSessionTags(s.id); !tags.empty())
            {
                _boardTags.emplace(s.id, std::move(tags));
                // The transcript-derived recency, falling back to the hook-side stamp for a session
                // whose transcript hasn't been read yet (a just-launched one) — the same precedence
                // the card timing uses.
                boardTagActivity.emplace(s.id, s.convLastActivityUnixMs != 0 ? s.convLastActivityUnixMs : s.lastActivityUnixMs);
            }
        }
        // Colors are needed for the chips too, so load them whenever a chip could render — that
        // includes a PICKED tag whose carriers have all gone (its chip stays, so it can be clicked off).
        _boardTagColors = (_boardTags.empty() && _boardTagFilter.empty()) ? std::map<std::wstring, std::wstring>{} : ::Agentmaster::LoadAllTagColors();
        _RebuildBoardTagChips(boardTagActivity);

        struct Col
        {
            winrt::hstring title;
            SessionState state;
        };
        const Col cols[] = {
            { L"Running", SessionState::Running },
            { L"Waiting-for-you", SessionState::WaitingForInput },
            { L"Needs-approval", SessionState::NeedsApproval },
            { L"Error", SessionState::Error },
            { L"Idle / Done", SessionState::Idle },
        };

        for (const auto& col : cols)
        {
            auto colStack = StackPanel{};
            colStack.Spacing(0);

            // collect matching sessions (respecting the LOCAL/GLOBAL scope + the directory scope)
            std::vector<const SessionInfo*> matches;
            for (const auto& s : sessions)
            {
                if (!s.live)
                {
                    continue; // closed sessions live in the Sessions browser, not the board (FAVORITES.md)
                }
                if (boardLocal && boardLocalIds.find(s.id) == boardLocalIds.end())
                {
                    continue; // LOCAL scope: hosted by another window
                }
                if (!_scopeDir.empty() && !PathEq(_WorkDirOf(s), _scopeDir))
                {
                    continue; // dir scope keys on the EFFECTIVE work dir — the tree group the user clicked
                }
                if (!_BoardTagFilterAccepts(s.id))
                {
                    continue; // bookmark-tag chips: this session carries none of the picked tags (OR)
                }
                const bool isIdleDone = (s.state == SessionState::Idle || s.state == SessionState::Done);
                const bool match = (col.state == SessionState::Idle) ? isIdleDone : (s.state == col.state);
                if (match)
                {
                    matches.push_back(&s);
                }
            }

            // Agentmaster: the Error column is SPECIAL — it collapses out of the board entirely when
            // it holds no cards and reappears in place (between Needs-approval and Idle / Done) the
            // moment a session errors. Skipping the Append below means the horizontal StackPanel
            // (_boardHost) reserves NO width — nor its 8px inter-column spacing — for it, so an empty
            // Error column costs zero board space; and because the columns are appended in fixed order
            // on every rebuild, it always returns to the SAME slot when it reappears. The other four
            // states are always shown — they are the steady-state columns of the triage model.
            if (col.state == SessionState::Error && matches.empty())
            {
                continue;
            }

            // Agentmaster: order the cards WITHIN this state column by the (global, persisted) board
            // sort — default MostActive (most-recently-active first). stable_sort so equal keys keep
            // their prior on-screen order across refreshes. The board reuses the SAME SortKey/SortKeyLess
            // comparator as the Explorer Tree, just driven by _appSettings.boardSort (its own setting) and
            // applied to the flat per-column list (no directory grouping — cards are grouped by STATE here).
            std::stable_sort(matches.begin(), matches.end(), [&](const SessionInfo* a, const SessionInfo* b) {
                return SortKeyLess(_appSettings.boardSort, MakeSortKey(*a), MakeSortKey(*b));
            });

            auto hdr = StackPanel{};
            hdr.Orientation(Orientation::Horizontal);
            hdr.Spacing(6);
            hdr.Margin(Thickness{ 0, 0, 0, 6 });
            auto dot = Text(L"\x25CF", 12, false, 1.0);
            dot.Foreground(SolidColorBrush{ StateColor(col.state) });
            hdr.Children().Append(dot);
            hdr.Children().Append(Text(col.title, 12, true, 0.9));
            hdr.Children().Append(Text(winrt::to_hstring(static_cast<int>(matches.size())), 12, false, 0.6));
            // Agentmaster: explain what each Triage state means — the board's five columns ARE the
            // state model, so naming them on hover is the core learning-curve aid. The state's name is
            // the tip's TITLE (the header is a StackPanel, so nothing derives one), which leaves the
            // body free to say what actually puts a session here and what gets it out again.
            const wchar_t* colTipTitle =
                col.state == SessionState::Running         ? L"Running" :
                col.state == SessionState::WaitingForInput ? L"Waiting-for-you" :
                col.state == SessionState::NeedsApproval   ? L"Needs-approval" :
                col.state == SessionState::Error           ? L"Error" :
                                                             L"Idle / Done";
            const wchar_t* colTip =
                col.state == SessionState::Running         ? L"The agent is working on a turn right now. Work that outlives the turn \x2014 a background shell, a subagent, a teammate \x2014 keeps a session here too, rather than letting it read as finished while it is still going." :
                col.state == SessionState::WaitingForInput ? L"The turn is finished and the agent is waiting for your next prompt. With Tests Autorunner on, the next queued prompt goes out on its own. A card you haven't looked at yet stays here \x2014 it only drops to Idle / Done once you have read it and the unread timeout has passed." :
                col.state == SessionState::NeedsApproval   ? L"The agent stopped part-way through a turn on a tool-permission prompt or a question, and cannot continue until you answer. Nothing is auto-sent into it \x2014 the queue is held so a queued prompt can never answer the question for you." :
                col.state == SessionState::Error           ? L"The agent's last turn died \x2014 a rate or usage limit, a prompt too long for the context, an auth failure, a dropped connection. The card carries the reason. A session leaves Error on its next turn; right-click \x2192 Move to Idle / Done dismisses it now." :
                                                             L"No turn in progress \x2014 freshly launched, just resumed, or done. A queued prompt still starts from here: a session that has never run emits no turn to wait for.";
            AgentSetTitledTip(hdr, colTipTitle, colTip);
            // colStack holds the cards only; _MakeBoardColumn pins the header above a vertically
            // scrolling card list so a tall column scrolls within the board height instead of
            // clipping past the bottom edge (the board ScrollViewer's vertical scroll is disabled).
            for (const auto* s : matches)
            {
                colStack.Children().Append(_MakeCard(*s));
            }

            // Preserve this column's remembered scroll offset (keyed by its title; durable across rebuilds).
            const std::wstring colKey{ col.title };
            const auto savedIt = _boardColumnOffsets.find(colKey);
            const double restore = (savedIt != _boardColumnOffsets.end()) ? savedIt->second : 0.0;
            _boardHost.Children().Append(_MakeBoardColumn(hdr, colStack, true, colKey, restore));
        }

        // Agentmaster (O6): a trailing observe-only "External (N)" group for real-WindowsTerminal
        // claudes the observer detected (NOT our tabs — no registry session, no Auto Testing). Shown
        // unscoped (it is a global census, not part of the managed directory tree).
        if (!_externalClaudes.empty())
        {
            const auto extIt = _boardColumnOffsets.find(L"External");
            _boardHost.Children().Append(_MakeExternalColumn(extIt != _boardColumnOffsets.end() ? extIt->second : 0.0));
        }
    }

    // ===== the board header's TAG FILTER chips (bookmark tags) ===============================
    //
    // The Triage Board twin of the Sessions browser's chip row (_RebuildSessionsTagChips), in the
    // board header right after "Clear": one ToggleButton per tag — each carrying the tag's own
    // bookmark ribbon, so a chip reads as exactly the ribbon the cards and tabs wear — plus a
    // trailing ✕ that drops every pick at once. An unpicked chip wears the stock control chrome of
    // the buttons beside it (see the Background note below); a picked one, the accent fill.
    //
    // ⚠ ONE deliberate difference from the Sessions page: the picks combine with **OR**, not AND.
    // The Sessions browser is a search tool, where narrowing to "carries all of these" is the useful
    // question; the board is a triage surface you point at a few concerns at once ("show me anything
    // tagged release or hotfix"), and ANDing there only ever shrinks toward the single card carrying
    // every tag. _BoardTagFilterAccepts is the one predicate the column loop asks.
    //
    // The universe comes from _boardTags — already narrowed by _RebuildBoard to the sessions the
    // board SHOWS — plus, via CollectGlobalTags' knownTags leg, whatever is currently PICKED. That
    // second half is not cosmetic: it is what guarantees a picked tag always has a chip to click off
    // even when nothing carries it any more (untagged elsewhere, its carrier closed, or — the case
    // that matters most — a reopened window whose lens restored a filter before the fleet finished
    // loading). Deliberately no pruning pass anywhere: a pick is dropped only by the user.
    void AgentManagerContent::_RebuildBoardTagChips(const std::unordered_map<std::wstring, int64_t>& activityBySession)
    {
        if (!_boardTagChipsPanel || !_boardTagChipsScroll)
        {
            return; // header not built yet (mid-init)
        }
        _boardTagChipsPanel.Children().Clear();

        auto universe = ::Agentmaster::CollectGlobalTags(_boardTags, activityBySession, _boardTagFilter);
        // ⚠ Re-sorted ALPHABETICALLY, deliberately dropping CollectGlobalTags' most-active-first order
        // (which the Sessions page and the tab menu's Tag panel both keep). Those two are built on
        // demand — you open the page, the order holds while you use it. This strip is rebuilt by
        // _RebuildBoard, which runs on EVERY registry notification (a state transition, a title change,
        // a draft flip), and it re-reads each tag's max carrier activity as it goes — so an
        // activity-ordered strip would visibly reshuffle under the pointer while the fleet works, and a
        // rebuild landing between aiming and clicking would swap the chip out from under the click. A
        // control strip must hold still: position is how you find a tag the second time. Folded so
        // "Bug"/"bug" sort as one name, which is how they compare everywhere else.
        std::sort(universe.begin(), universe.end(), [](const auto& a, const auto& b) {
            return ::Agentmaster::FoldTagName(a.name) < ::Agentmaster::FoldTagName(b.name);
        });
        if (universe.empty() && _boardShowUntagged)
        {
            // Nothing tagged and nothing picked — collapse the whole row so an untagged fleet's
            // header looks exactly as it did before this feature. A lone "Untagged" chip on a fleet
            // with no tags anywhere would filter nothing and only add noise. ⚠ The _boardShowUntagged
            // half of the condition is what stops that from being a trap: if the chip is OFF, the row
            // MUST render (its one chip) even with no tags left, or the untagged cards would stay
            // hidden with nothing on screen to switch them back on.
            _boardTagChipsScroll.Visibility(Visibility::Collapsed);
            return;
        }
        _boardTagChipsScroll.Visibility(Visibility::Visible);

        // The leading "Untagged" chip — the (N+1)th bucket, before the alphabetical tags and pinned
        // there (it is not a tag, so it takes no part in their ordering). Same chip build as the tags,
        // with a HOLLOW ribbon: an outlined bookmark with no fill is the natural glyph for "carries
        // none", and it keeps the row's ribbon-then-name rhythm while telling the chip apart from a
        // real tag that happens to be named "untagged".
        _boardTagChipsPanel.Children().Append(_MakeBoardTagChip(
            L"Untagged",
            std::nullopt, // no color => the hollow outline
            _boardShowUntagged,
            L"Untagged",
            winrt::hstring{ (_boardShowUntagged ? std::wstring{ L"Click to HIDE the cards that carry no tag at all. " } :
                                                  std::wstring{ L"Click to show the cards that carry no tag at all. " }) +
                            L"It is on by default, which is why an untouched board shows everything.\n\nIt stands on its own rather than joining the tags above: an untagged card is decided by this chip alone, a tagged one by the tags alone. So turning it off is how you narrow to tagged cards only." },
            [this]() { _ToggleBoardUntagged(); }));

        std::unordered_set<std::wstring> picked;
        for (const auto& t : _boardTagFilter)
        {
            picked.insert(::Agentmaster::FoldTagName(t));
        }

        for (const auto& info : universe)
        {
            const bool on = picked.count(::Agentmaster::FoldTagName(info.name)) > 0;
            const std::wstring tagName = info.name;
            _boardTagChipsPanel.Children().Append(_MakeBoardTagChip(
                tagName,
                ResolveTagDisplayColor(tagName, _boardTagColors),
                on,
                L"Tag \x201C" + tagName + L"\x201D",
                winrt::hstring{ (on ? std::wstring{ L"Click to stop narrowing the board to this tag. " } :
                                      std::wstring{ L"Click to show only the cards carrying it. " }) +
                                (info.sessionCount == 0 ? std::wstring{ L"No card on the board carries it right now." } :
                                 info.sessionCount == 1 ? std::wstring{ L"1 card carries it." } :
                                                          std::to_wstring(info.sessionCount) + L" cards carry it.") +
                                L"\n\nPicking several tags widens the board rather than narrowing it \x2014 a card shows if it carries ANY of them. The picked set is remembered for this window." },
                [this, tagName]() { _ToggleBoardTagFilter(tagName); }));
        }

        // A trailing ✕ that restores the DEFAULT view — every pick dropped AND "Untagged" back on.
        // Shown whenever the board is filtered at all (a pick, or untagged hidden), so it never adds
        // noise to a resting header and is always there when there is something to undo.
        if (!_boardTagFilter.empty() || !_boardShowUntagged)
        {
            auto clearChip = Button{};
            clearChip.MinWidth(0);
            clearChip.MinHeight(0);
            clearChip.Padding(Thickness{ 8, 2, 8, 3 });
            clearChip.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
            clearChip.FontSize(12);
            clearChip.Content(winrt::box_value(winrt::hstring{ L"\x2715" }));
            AgentSetTitledTip(clearChip, L"Clear tag filter", L"Unpick every tag and put \x201CUntagged\x201D back on \x2014 the board goes back to showing all its cards. (This clears only the tag chips; it doesn't change the card selection or the directory scope.)");
            clearChip.Click([this](const IInspectable&, const RoutedEventArgs&) {
                if (_dispatcher)
                {
                    _dispatcher.TryEnqueue([weak = get_weak()]() {
                        if (const auto self = weak.get())
                        {
                            self->_ClearBoardTagFilter();
                        }
                    });
                }
            });
            _boardTagChipsPanel.Children().Append(clearChip);
        }
    }

    // Build ONE chip for that row. The tag chips and the leading "Untagged" chip both come through
    // here, which is what makes "styled the same as the tags" structurally true rather than a pair of
    // literals someone has to keep in sync: the only thing that varies is the ribbon's fill.
    //   ribbonColor set     => a tag: the ribbon in that tag's own resolved color.
    //   ribbonColor nullopt => "Untagged": the same bookmark OUTLINED and empty — the natural glyph
    //                          for "carries none", and what tells it apart from a real tag that
    //                          happens to be named "untagged".
    winrt::Windows::UI::Xaml::Controls::Button AgentManagerContent::_MakeBoardTagChip(const std::wstring& label,
                                                                                      const std::optional<winrt::Windows::UI::Color>& ribbonColor,
                                                                                      bool isChecked,
                                                                                      const std::wstring& tipTitle,
                                                                                      const winrt::hstring& tipBody,
                                                                                      std::function<void()> onToggle)
    {
        // A plain Button, NOT a ToggleButton — deliberately. A ToggleButton's Checked visual state
        // paints the accent as a BACKGROUND FILL, and a VSM setter outranks any local value, so that
        // fill can't be suppressed without re-templating the control. The selection here is a 1px
        // accent BORDER and no background change at all (the ask), so we drive the "selected" look
        // ourselves from `isChecked` rather than from a toggle's checked state — free, because the
        // whole chips row is rebuilt on every click, so `isChecked` is always current at build time.
        Button chip;
        chip.MinWidth(0);
        chip.MinHeight(0);
        chip.Padding(Thickness{ 10, 2, 10, 3 });
        chip.CornerRadius(CornerRadius{ 4, 4, 4, 4 }); // a gently-rounded rectangle, not a pill — matches the header's square-cornered buttons
        chip.FontSize(12);
        chip.BorderThickness(Thickness{ 1, 1, 1, 1 }); // constant in BOTH states, so selecting never reflows the row
        // ⚠ Background is NEVER set here, nor is any *Background* theme resource overridden — so the
        // fill is 100% stock Button chrome in every state (rest AND selected), byte-for-byte the
        // "Clear"/"Show all"/scope Buttons beside it. Selection changes ONLY the border:
        //   * OFF  => no BorderBrush set => the stock subtle button border (matches the neighbours).
        //   * ON   => a 1px accent border. The rest-state brush is a local value; the two interaction
        //             states get resource overrides too, because the Button template's PointerOver /
        //             Pressed states set BorderBrush from ButtonBorderBrush{PointerOver,Pressed} and a
        //             VSM setter outranks a local value — so without these, hovering a selected chip
        //             would swap the accent back to the stock border. Overriding those two keys on the
        //             chip's OWN Resources keeps the accent through hover/press. (Belt-and-suspenders:
        //             if a future template doesn't animate BorderBrush, the overrides are simply
        //             unused; if a key name ever drifts, the worst case is the accent flickering to
        //             stock on hover — never a wrong fill.)
        if (isChecked)
        {
            const auto accent = Fill(0xFF, 0x4F, 0xA3, 0xE3); // full-opacity of the chips' old border blue — reads as a highlight on the dark board
            chip.BorderBrush(accent);
            chip.Resources().Insert(winrt::box_value(winrt::hstring{ L"ButtonBorderBrushPointerOver" }), accent);
            chip.Resources().Insert(winrt::box_value(winrt::hstring{ L"ButtonBorderBrushPressed" }), accent);
        }
        {
            StackPanel chipContent;
            chipContent.Orientation(Orientation::Horizontal);
            chipContent.Spacing(6);
            chipContent.VerticalAlignment(VerticalAlignment::Center);
            winrt::Windows::UI::Xaml::Shapes::Polygon chipRibbon; // the 6.5x9.3 bookmark shape the cards + tab badges draw
            chipRibbon.Points().Append(Point{ 0.0f, 0.0f });
            chipRibbon.Points().Append(Point{ 6.5f, 0.0f });
            chipRibbon.Points().Append(Point{ 6.5f, 9.3f });
            chipRibbon.Points().Append(Point{ 3.25f, 6.5f });
            chipRibbon.Points().Append(Point{ 0.0f, 9.3f });
            if (ribbonColor)
            {
                chipRibbon.Fill(SolidColorBrush{ *ribbonColor });
                chipRibbon.Stroke(SolidColorBrush{ Colors::Black() });
            }
            else
            {
                // Hollow: no fill, and a light-gray stroke rather than the tags' black — an unfilled
                // outline in black would all but vanish against this chrome, and the whole point of
                // the glyph is to read as an EMPTY bookmark.
                chipRibbon.Fill(SolidColorBrush{ Colors::Transparent() });
                chipRibbon.Stroke(Fill(0xCC, 0xB0, 0xB0, 0xB0));
            }
            chipRibbon.StrokeThickness(0.75);
            chipRibbon.VerticalAlignment(VerticalAlignment::Center);
            // Nudge the ribbon DOWN 3px — its geometric center reads optically high beside the
            // text's ink. A render transform shifts only the visual, so no layout math to break.
            TranslateTransform chipRibbonNudge;
            chipRibbonNudge.Y(3.0);
            chipRibbon.RenderTransform(chipRibbonNudge);
            chipContent.Children().Append(chipRibbon);
            auto chipLabel = TextBlock{}; // no explicit Foreground — inherits the Button's, so it adapts to hover/press like the neighbours
            chipLabel.Text(winrt::hstring{ label });
            chipLabel.VerticalAlignment(VerticalAlignment::Center);
            chipContent.Children().Append(chipLabel);
            chip.Content(chipContent);
        }
        AgentSetTitledTip(chip, winrt::hstring{ tipTitle }, tipBody);
        chip.Click([this, onToggle = std::move(onToggle)](const IInspectable&, const RoutedEventArgs&) {
            // DEFER: the toggle rebuilds this very chips row, destroying the Button whose Click
            // handler we are standing in (the Sessions page's chip discipline). The action is captured
            // by value, so a chip already torn down by the time it runs still applies the flip the
            // user asked for; it captures `this`, hence the weak re-check before invoking it.
            if (_dispatcher)
            {
                _dispatcher.TryEnqueue([weak = get_weak(), onToggle]() {
                    if (const auto self = weak.get())
                    {
                        onToggle();
                    }
                });
            }
        });
        return chip;
    }

    // Flip ONE tag in the board's filter (case-insensitive identity, like everywhere tags are
    // compared), push the lens so the window's record picks it up, and re-render. Order in the
    // vector is click order — it is what the lens round-trips, and nothing reads it as a priority.
    // Deliberately UNLOGGED: a pure view filter, like the scope/sort toggles beside it.
    void AgentManagerContent::_ToggleBoardTagFilter(const std::wstring& tag)
    {
        if (tag.empty())
        {
            return;
        }
        const auto folded = ::Agentmaster::FoldTagName(tag);
        const auto it = std::find_if(_boardTagFilter.begin(), _boardTagFilter.end(), [&](const std::wstring& t) {
            return ::Agentmaster::FoldTagName(t) == folded;
        });
        if (it != _boardTagFilter.end())
        {
            _boardTagFilter.erase(it);
        }
        else
        {
            _boardTagFilter.push_back(tag);
        }
        _NotifyLensChanged(); // the picked set is part of the per-window lens (ManagerState::boardTagFilter)
        _Refresh(); // re-filter the columns + re-style the chips (both ride _RebuildBoard)
    }

    // The leading "Untagged" chip: show/hide the cards carrying no tag at all. A bucket of its own,
    // not a member of the tag OR (see _BoardTagFilterAccepts) — so it reads the same whether or not
    // any tag is picked. Same lens push + rebuild as a tag pick; unlogged for the same reason.
    void AgentManagerContent::_ToggleBoardUntagged()
    {
        _boardShowUntagged = !_boardShowUntagged;
        _NotifyLensChanged(); // rides the per-window record (ManagerState::boardShowUntagged)
        _Refresh();
    }

    // The trailing ✕: back to the DEFAULT view in one click — no tag picked AND untagged shown. It
    // restores both halves, not just the picks, because "clear the filter" has to mean the board you
    // get before you touch anything; leaving Untagged off would silently keep cards hidden.
    void AgentManagerContent::_ClearBoardTagFilter()
    {
        if (_boardTagFilter.empty() && _boardShowUntagged)
        {
            return; // already the default view — nothing to undo
        }
        _boardTagFilter.clear();
        _boardShowUntagged = true;
        _NotifyLensChanged();
        _Refresh();
    }

    // Does this session pass the board's tag filter? Two independent questions, by design — which is
    // what lets "Untagged" default ON without hiding anything:
    //
    //   * an UNTAGGED session (absent from _boardTags, which only holds carriers) is judged by the
    //     "Untagged" chip ALONE. It is not in the tag OR at all — folding it in would mean that with
    //     Untagged on and nothing else picked, only untagged cards showed, i.e. the DEFAULT board
    //     would hide every tagged session.
    //   * a TAGGED session is judged by the picks ALONE: **OR** — carrying ANY picked tag is enough
    //     (see _RebuildBoardTagChips for why the board differs from the Sessions browser here) — and
    //     no picks at all means tags don't restrict, so it shows.
    //
    // Default state (Untagged on, nothing picked) therefore passes everything, exactly as before the
    // chips existed; "only tagged cards" is Untagged off; "only release cards" is release picked AND
    // Untagged off.
    bool AgentManagerContent::_BoardTagFilterAccepts(const std::wstring& sessionId) const
    {
        const auto it = _boardTags.find(sessionId);
        if (it == _boardTags.end() || it->second.empty())
        {
            return _boardShowUntagged; // carries no tag — the "Untagged" chip decides, on its own
        }
        if (_boardTagFilter.empty())
        {
            return true; // tagged, but nothing picked — tags aren't restricting anything
        }
        for (const auto& want : _boardTagFilter)
        {
            const auto folded = ::Agentmaster::FoldTagName(want);
            for (const auto& has : it->second)
            {
                if (::Agentmaster::FoldTagName(has) == folded)
                {
                    return true; // one match is enough
                }
            }
        }
        return false;
    }

    // Agentmaster: assemble one Triage Board column. When `fill` is true the column fills the board
    // height with a pinned `header` (Grid row 0) over a vertically-scrolling `cards` list (row 1),
    // so a tall column (e.g. a large External census) scrolls within the board instead of clipping
    // past the bottom edge — the board's own ScrollViewer (BuildUI) has vertical scroll disabled.
    // When `fill` is false the box hugs its content (a collapsed column: header only, no scroll).
    // Shared by the per-state columns and the External group so they stay visually in lockstep.
    Border AgentManagerContent::_MakeBoardColumn(const UIElement& header, const UIElement& cards, bool fill, const std::wstring& columnKey, double restoreOffset)
    {
        auto col_border = Border{};
        col_border.Width(220);
        col_border.Padding(Thickness{ 8, 8, 8, 8 });
        col_border.CornerRadius(CornerRadius{ 6, 6, 6, 6 });
        col_border.Background(Fill(0x14, 0x80, 0x80, 0x80));

        if (fill)
        {
            auto grid = Grid{};
            auto rdHeader = RowDefinition{};
            rdHeader.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            grid.RowDefinitions().Append(rdHeader);
            auto rdCards = RowDefinition{};
            rdCards.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            grid.RowDefinitions().Append(rdCards);

            Grid::SetRow(header.as<FrameworkElement>(), 0); // Grid::SetRow takes a FrameworkElement; header is typed UIElement
            grid.Children().Append(header);

            auto cardsSv = ScrollViewer{};
            cardsSv.VerticalScrollMode(ScrollMode::Enabled);
            cardsSv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            cardsSv.HorizontalScrollMode(ScrollMode::Disabled);
            cardsSv.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            cardsSv.Content(cards);
            Grid::SetRow(cardsSv, 1);
            grid.Children().Append(cardsSv);

            // Agentmaster: track this column's ScrollViewer so the NEXT _RebuildBoard can capture its
            // offset, and re-apply the offset this rebuild inherited. A fresh ScrollViewer sits at 0
            // until restored; do it on Loaded with animation disabled (an instant restore, no visible
            // jump). Capture only the offset — the sender IS the ScrollViewer, never self-capture the
            // element (that leaks it via the delegate).
            if (!columnKey.empty())
            {
                _boardColumnScrollers[columnKey] = cardsSv;
                if (restoreOffset > 0.0)
                {
                    cardsSv.Loaded([restoreOffset](const IInspectable& sender, const RoutedEventArgs&) {
                        if (const auto sv = sender.try_as<ScrollViewer>())
                        {
                            // Agentmaster (scroll-jump fix): when Loaded fires, a fresh ScrollViewer's
                            // CONTENT extent often isn't realized yet — ScrollableHeight still reads 0 —
                            // so a bare ChangeView(restoreOffset) CLAMPS to 0 and the column snaps to the
                            // TOP. That is the "sometimes a click jumps the scroll to top" report:
                            // _RebuildBoard recreates these ScrollViewers on EVERY refresh (a select, a
                            // state/title change, an observer enrichment), and the restore raced the
                            // content's first measure. Force the content to measure+arrange first so
                            // ScrollableHeight reflects the real height, THEN restore — the same realize-
                            // then-scroll recipe BringSelectedIntoView uses (UpdateLayout before the
                            // scroll). UpdateLayout is synchronous + idempotent, so it is a no-op when the
                            // extent is already valid.
                            sv.UpdateLayout();
                            sv.ChangeView(nullptr, restoreOffset, nullptr, true);
                        }
                    });
                }
            }

            // Stretch so the board's horizontal StackPanel gives the column the full viewport height
            // (the board SV's vertical scroll is off, so the cross-axis is bounded) -> the inner
            // ScrollViewer has a real height to scroll within.
            col_border.VerticalAlignment(VerticalAlignment::Stretch);
            col_border.Child(grid);
        }
        else
        {
            auto stack = StackPanel{};
            stack.Spacing(0);
            stack.Children().Append(header);
            stack.Children().Append(cards);
            col_border.VerticalAlignment(VerticalAlignment::Top);
            col_border.Child(stack);
        }
        return col_border;
    }

    // Agentmaster (O6): the "External (N)" board column. Observe-only — each card is a real
    // Windows Terminal claude the observer correlated out-of-band but will never bind (Rule #9/#13).
    Border AgentManagerContent::_MakeExternalColumn(double restoreOffset)
    {
        auto colStack = StackPanel{};
        colStack.Spacing(0);

        auto hdr = StackPanel{};
        hdr.Orientation(Orientation::Horizontal);
        hdr.Spacing(6);
        auto dot = Text(L"\x25CF", 12, false, 1.0);
        dot.Foreground(Fill(0xFF, 0x9E, 0x9E, 0x9E)); // gray — external / observe-only
        hdr.Children().Append(dot);
        hdr.Children().Append(Text(L"External", 12, true, 0.9));
        hdr.Children().Append(Text(winrt::to_hstring(static_cast<int>(_externalClaudes.size())), 12, false, 0.6));
        hdr.Children().Append(Text(_externalCollapsed ? winrt::hstring{ L"\x25B8" } : winrt::hstring{ L"\x25BE" }, 11, false, 0.6)); // ▸ / ▾

        // The header doubles as the collapse toggle (a Button styled to read like the other column
        // headers — transparent, borderless, left-aligned).
        auto hdrBtn = Button{};
        hdrBtn.Content(hdr);
        hdrBtn.Background(Fill(0x00, 0, 0, 0));
        hdrBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        hdrBtn.Padding(Thickness{ 0, 0, 0, 0 });
        hdrBtn.HorizontalAlignment(HorizontalAlignment::Stretch);
        hdrBtn.HorizontalContentAlignment(HorizontalAlignment::Left);
        hdrBtn.Margin(Thickness{ 0, 0, 0, 6 });
        AgentSetTitledTip(hdrBtn, L"External", L"Claude and Codex sessions running outside Agentmaster \x2014 in a plain console, in Windows Terminal, or in another install. They are found and read, never driven, and no window scope applies to them. Click a card to read its conversation, or right-click to Adopt it. Click this header to collapse or expand the column.");
        hdrBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
            _externalCollapsed = !_externalCollapsed;
            _Refresh();
        });
        // Collapsed: just the header in a hugging box (no card list to scroll). Expanded: the header
        // pinned above a vertically-scrolling card list (fills the board height) so a large External
        // census scrolls within the board instead of clipping past the bottom edge. colStack holds
        // the cards only (the header is pinned by _MakeBoardColumn, not stacked above them).
        if (_externalCollapsed)
        {
            return _MakeBoardColumn(hdrBtn, colStack, false);
        }

        // Agentmaster: order the External census cards by the same (global) board sort as the managed
        // columns, so the whole board reads in one consistent order (the rows arrive pid-sorted from the
        // observer; re-sort a pointer copy here, leaving _externalClaudes untouched). Externals carry no
        // run-state (MakeSortKey sets active=false), so MostActive ranks them by recency only.
        std::vector<const ::Agentmaster::ExternalClaudeRow*> exts;
        exts.reserve(_externalClaudes.size());
        for (const auto& ex : _externalClaudes)
        {
            exts.push_back(&ex);
        }
        std::stable_sort(exts.begin(), exts.end(), [&](const ::Agentmaster::ExternalClaudeRow* a, const ::Agentmaster::ExternalClaudeRow* b) {
            return SortKeyLess(_appSettings.boardSort, MakeSortKey(*a), MakeSortKey(*b));
        });
        for (const auto* ex : exts)
        {
            colStack.Children().Append(_MakeExternalCard(*ex));
        }
        return _MakeBoardColumn(hdrBtn, colStack, true, L"External", restoreOffset);
    }

    winrt::Windows::UI::Xaml::Controls::Button AgentManagerContent::_MakeExternalCard(const ::Agentmaster::ExternalClaudeRow& ex)
    {
        auto stack = StackPanel{};
        stack.Spacing(2);

        // Title: the conversation's first prompt (from the transcript), else the cwd leaf, else
        // "claude" (recent transcripts carry no summary — verified — so the first prompt is the title).
        std::wstring title = ex.title;
        if (title.empty())
        {
            std::wstring leaf = ex.cwd;
            const auto slash = leaf.find_last_of(L"\\/");
            if (slash != std::wstring::npos && slash + 1 < leaf.size())
            {
                leaf = leaf.substr(slash + 1);
            }
            title = leaf.empty() ? std::wstring{ L"claude" } : leaf;
        }
        if (title.size() > 64)
        {
            title = title.substr(0, 61) + L"\x2026";
        }
        // Phase C2: a Codex row leads its title with a state dot (rollout-derived turn state — blue
        // running / gold waiting / gray idle). A Claude external carries no PULL state -> plain title.
        if (ex.kind == AgentKind::Codex)
        {
            auto titleRow = StackPanel{};
            titleRow.Orientation(Orientation::Horizontal);
            titleRow.Spacing(6);
            titleRow.VerticalAlignment(VerticalAlignment::Center);
            auto sd = Text(L"\x25CF", 11, false, 1.0);
            sd.Foreground(SolidColorBrush{ CodexStateColor(ex.codexState) });
            AgentSetTitledTip(sd, L"Codex turn state", winrt::hstring{ L"Currently " } + CodexStateLabel(ex.codexState) + winrt::hstring{ L", read from this session's rollout transcript. Codex reports only Running, Waiting and Idle \x2014 its rollout records no approval or error event, so it has no needs-you or error state to show." }, kCardTipDelay);
            titleRow.Children().Append(sd);
            titleRow.Children().Append(Text(winrt::hstring{ title }, 13, true, 0.9));
            stack.Children().Append(titleRow);
        }
        else
        {
            stack.Children().Append(Text(winrt::hstring{ title }, 13, true, 0.9));
        }
        // Agentmaster (Phase C1): a Codex row carries a teal "codex" agent pill so a mixed External
        // group reads at a glance (Claude is the implicit default — no pill, visuals unchanged).
        if (ex.kind == AgentKind::Codex)
        {
            auto p = Pill(L"codex", Color{ 0xFF, 0x4E, 0xC9, 0xB0 });
            p.Opacity(0.9);
            p.HorizontalAlignment(HorizontalAlignment::Left);
            AgentSetTitledTip(p, L"Codex agent", L"This external session runs the OpenAI Codex CLI. It is observed, not managed \x2014 right-click to fork a copy of its rollout, or resume it, into a tab here.", kCardTipDelay);
            stack.Children().Append(p);
        }
        if (!ex.cwd.empty())
        {
            auto cwdText = Text(winrt::hstring{ ex.cwd }, 11, false, 0.55);
            AgentSetTitledTip(cwdText, L"Working directory", L"Where this external session runs \x2014 read from the process itself, so it follows a cd. Right-click the card to start a managed session of your own here.", kCardTipDelay);
            stack.Children().Append(cwdText);
        }

        // host (the foreign terminal) · git branch
        {
            // The Fleet Observer resolves a clear host label by the hosting terminal's identity
            // (package family / image path): "Windows Terminal" (real WT) vs "Agentmaster" /
            // "Agentmaster Dev" (another of our instances) vs a shell leaf — see ResolveExternalHostLabel.
            std::wstring hostLabel = ex.hostLabel;
            if (hostLabel.empty())
            {
                // Fallback for an older/missing reading: the parent shell leaf, else generic.
                if (!ex.hostImage.empty())
                {
                    hostLabel = ex.hostImage;
                    const auto dot = hostLabel.rfind(L".exe");
                    if (dot != std::wstring::npos)
                    {
                        hostLabel = hostLabel.substr(0, dot);
                    }
                }
                else
                {
                    hostLabel = (ex.host == RunningApp::WindowsTerminal) ? L"Windows Terminal" : L"external";
                }
            }
            std::wstring hb = L"via " + hostLabel;
            if (!ex.gitBranch.empty())
            {
                hb += L"  \x00B7  [" + ex.gitBranch + L"]";
            }
            auto hbText = Text(winrt::hstring{ hb }, 10, false, 0.5);
            AgentSetTitledTip(hbText, L"Host \x00B7 branch", L"The terminal application this session runs in \x2014 Windows Terminal, a plain console, or another Agentmaster install \x2014 and, in [brackets], the git branch its folder is on right now. Right-click the card to bring that window to the front.", kCardTipDelay);
            stack.Children().Append(hbText);
        }

        // model · effort · bg · pid
        {
            std::wstring me;
            const auto addPart = [&](const std::wstring& part) {
                if (part.empty())
                {
                    return;
                }
                if (!me.empty())
                {
                    me += L"  \x00B7  ";
                }
                me += part;
            };
            // The CURRENT model (transcript truth, tail-read by the observer) wins over the launch
            // cmdline `--model` — usually empty on a bare external claude — shortened for the card
            // (a Codex row's rollout model rides ex.model and passes through ShortModelName verbatim;
            // the family words come from the cog's Model families box).
            addPart(::Agentmaster::ShortModelName(ex.currentModel.empty() ? ex.model : ex.currentModel, ::Agentmaster::ParseModelFamilies(_appSettings.modelFamilies)));
            addPart(ex.effort);
            addPart(ex.sandbox); // Codex only (empty for Claude) — model · effort · sandbox · approval
            addPart(ex.approvalMode); // Codex only
            if (ex.background)
            {
                addPart(L"bg");
            }
            me += (me.empty() ? L"pid " : L"  \x00B7  pid ") + std::to_wstring(ex.pid);
            auto meText = Text(winrt::hstring{ me }, 10, false, 0.5);
            AgentSetTitledTip(meText, L"How this session was started", L"Its model \xB7 reasoning effort \xB7 (for Codex, its sandbox and approval policy) \xB7 bg if it runs in the background \xB7 pid, its process id. Read from the running process, not from a config file.", kCardTipDelay);
            stack.Children().Append(meText);
        }

        // timing (created-ago / active-for / last-activity-ago)
        {
            const int64_t created = ex.createdUnixMs ? ex.createdUnixMs : ex.startUnixMs;
            if (auto t = TimingText(created, ex.lastActivityUnixMs))
            {
                stack.Children().Append(t);
            }
        }

        // The whole card is clickable — left-click SELECTS this external, EXACTLY like clicking its
        // row in the Explorer Tree (_SelectExternal): the Auto Testing shows its conversation read-only
        // and the tree syncs to EXTERNAL with this one highlighted (Linked Lenses). Right-click opens
        // the SAME menu the tree row uses — Adopt / Open New Session Here / Bring Window To Front. (No
        // inline "observe"/"Adopt" affordance: the card itself is the observe action; the rest lives
        // on the right-click menu.)
        const bool selected = !ex.sessionId.empty() && ex.sessionId == _selectedExternalSessionId;

        // Agentmaster: the same hover-revealed "\x22EF" more-button as the managed cards (_MakeCard) —
        // here it opens the EXTERNAL menu (Adopt / Open New Session Here / Bring Window To Front). Wrap
        // the content in a Grid so the dots float top-right; faded in on the card's hover (wired below).
        auto dotsBtn = Button{};
        {
            FontIcon moreGlyph;
            moreGlyph.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
            moreGlyph.Glyph(L"\xE712"); // "More" — three dots
            moreGlyph.FontSize(14);
            dotsBtn.Content(moreGlyph);
        }
        dotsBtn.Padding(Thickness{ 4, 0, 4, 0 });
        dotsBtn.MinWidth(26);
        dotsBtn.Height(20);
        dotsBtn.HorizontalAlignment(HorizontalAlignment::Right);
        dotsBtn.VerticalAlignment(VerticalAlignment::Top);
        dotsBtn.Background(Fill(0x66, 0x30, 0x30, 0x30));
        dotsBtn.Foreground(Fill(0xF0, 0xFF, 0xFF, 0xFF));
        dotsBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
        dotsBtn.CornerRadius(CornerRadius{ 4, 4, 4, 4 });
        dotsBtn.Opacity(0.0); // hidden at rest; the card's hover fades it in
        dotsBtn.IsHitTestVisible(false); // an invisible corner must not swallow a card click
        dotsBtn.IsTabStop(false); // a hover affordance — keep the invisible button out of the keyboard tab order (the menu is reachable via right-click / the context-menu key)
        {
            ScalarTransition st;
            st.Duration(winrt::Windows::Foundation::TimeSpan{ std::chrono::milliseconds{ 140 } });
            dotsBtn.OpacityTransition(st);
        }
        AgentSetTitledTip(dotsBtn, L"More", L"What you can do with a session you don't own \x2014 adopt its conversation into a tab here, start a new session in its folder, or bring its window to the front. Exactly what right-clicking the card gives you.", kCardTipDelay);
        dotsBtn.Flyout(_MakeLazyExternalMenu(ex)); // a click opens the external menu (built at OPEN time — perf)
        const auto dotsWeak = winrt::make_weak(dotsBtn);

        auto grid = Grid{};
        grid.Children().Append(stack);
        grid.Children().Append(dotsBtn);

        auto card = Button{};
        card.Content(grid);
        card.HorizontalAlignment(HorizontalAlignment::Stretch);
        card.HorizontalContentAlignment(HorizontalAlignment::Stretch); // let the Grid fill so the dots reach the true top-right corner
        card.Padding(Thickness{ 8, 6, 8, 6 });
        card.Margin(Thickness{ 0, 0, 0, 6 });
        card.Background(Fill(selected ? 0x40 : 0x18, 0x80, 0x80, 0x80));
        card.BorderBrush(Fill(selected ? 0xFF : 0x60, 0x9E, 0x9E, 0x9E)); // gray — external / observe-only
        card.BorderThickness(selected ? Thickness{ 2, 2, 2, 2 } : Thickness{ 1, 1, 1, 1 });
        card.ContextFlyout(_MakeLazyExternalMenu(ex)); // built at OPEN time (perf)
        const auto exId = ex.sessionId;
        const auto exCwd = ex.cwd;
        const auto exTitle = title;
        const auto exKind = ex.kind; // Phase C1: Claude vs Codex selects the read-only-plan reader
        const auto exRollout = ex.rolloutPath; // Codex rollout path (empty for Claude)
        card.Click([this, exId, exCwd, exTitle, exKind, exRollout](const IInspectable&, const RoutedEventArgs&) {
            _SelectExternal(exId, exCwd, exTitle, exKind, exRollout);
        });
        // Agentmaster: when the Fleet Observer has tailed an idle RECAP (away_summary) for this external
        // — read out-of-band from the SAME transcript-tail region a managed session's recap comes from
        // (ExternalClaudeRow.recap; see ProcessObserver) — append it below the base hint so a hover tells
        // the external sessions apart by what they were last doing, exactly like the managed card band
        // tooltip does with SessionInfo.recap. Shown in FULL (the tooltip wraps); no recap == base hint only.
        std::wstring cardTip{ L"An agent running outside Agentmaster \x2014 found and read, never driven. Click to read its conversation; right-click to adopt it into a tab here, start a session in its folder, or bring its window forward." };
        if (!ex.recap.empty())
        {
            cardTip += L"\n\nRecap \x2014 its own \x201C";
            cardTip += L"what we did / what's next\x201D note, written after it sat idle:\n";
            cardTip += ex.recap;
        }
        AgentSetTitledTip(card, winrt::hstring{ exTitle }, winrt::hstring{ cardTip }, kCardTipDelay);
        // Fade the "\x22EF" more-button in (and arm its hit-testing) while the card is hovered; fade it
        // out on exit. dotsWeak is a weak_ref so the handler never strong-captures the button it lives
        // under. (No _ReportHover here — an external has no managed tab for the page to pill.)
        card.PointerEntered([dotsWeak](const IInspectable&, const PointerRoutedEventArgs&) {
            if (const auto d = dotsWeak.get())
            {
                d.IsHitTestVisible(true);
                d.Opacity(0.85);
            }
        });
        card.PointerExited([dotsWeak](const IInspectable& sender, const PointerRoutedEventArgs& e) {
            if (PointerStillWithin(sender, e))
            {
                return; // a child label's exit bubbled up — the pointer is still on the card; keep the dots up
            }
            if (const auto d = dotsWeak.get())
            {
                d.Opacity(0.0);
                d.IsHitTestVisible(false);
            }
        });
        return card;
    }

    void AgentManagerContent::SetExternalClaudes(std::vector<::Agentmaster::ExternalClaudeRow> rows)
    {
        // Diff vs the current list (the observer pushes every probe tick) so an unchanged set is a
        // no-op — no board rebuild churn. Rows arrive pid-sorted from the observer, a stable order.
        bool same = (rows.size() == _externalClaudes.size());
        for (size_t i = 0; same && i < rows.size(); ++i)
        {
            const auto& a = rows[i];
            const auto& b = _externalClaudes[i];
            // Include the enrichment fields (id/title/host/branch) so a row that gains its title or
            // host a tick after first sight triggers one refresh. Timestamps are deliberately NOT
            // compared — mtime ticks constantly; the "ago" is recomputed live on any rebuild.
            if (a.pid != b.pid || a.cwd != b.cwd || a.model != b.model || a.currentModel != b.currentModel || a.effort != b.effort || a.background != b.background ||
                a.sessionId != b.sessionId || a.title != b.title || a.host != b.host || a.hostLabel != b.hostLabel || a.gitBranch != b.gitBranch || a.hostPid != b.hostPid ||
                a.kind != b.kind || a.sandbox != b.sandbox || a.approvalMode != b.approvalMode || // Phase C1: a codex row gaining its model/sandbox a tick after first sight triggers one refresh
                a.codexState != b.codexState || // Phase C2: a Codex turn flip (running<->waiting) repaints the row's state dot
                a.recap != b.recap) // Agentmaster: a fresh idle recap (away_summary) the observer tailed repaints the card/tree tooltip + read-only plan
            {
                same = false;
            }
        }
        if (same)
        {
            return;
        }
        _externalClaudes = std::move(rows);
        _Refresh();
    }

}
