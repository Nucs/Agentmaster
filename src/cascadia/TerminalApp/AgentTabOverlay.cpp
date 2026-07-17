// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ======================================================================================
// Agentmaster per-tab link badge / overlay (4 partial files)
// The top-right terminal HUD on every classified tab + its pencil-toggled summary panel
// (TAB_OVERLAY.md). ONE class (AgentTabOverlay) split from the former 3221-line .cpp; all four
// share AgentTabOverlay.Internal.h.
//
// Partial files in this group (★ marks THIS file):
// ★ AgentTabOverlay.cpp          - CORE: ctor/dtor, Initialize/_Detach, ShowActivity (the observe badge), _Refresh (the linked badge), hover/expand, opacities, Autorunner
//   AgentTabOverlay.Internal.h   - shared file-local helpers: StateColor/Glyph/Label, the summary-box renderers, time formatting, launch-CLI + clipboard (anonymous namespace, a per-TU copy)
//   AgentTabOverlay.Actions.cpp  - the hover action row: the folder (Open Path) button, the copy menu, and the shared CopySessionField action
//   AgentTabOverlay.Summary.cpp  - the pencil-toggled summary panel: build/render/load off-thread, the times bar, resize grips, wrap/truncate/previous, JUMP, copy-summary
// ======================================================================================
//
// Agentmaster per-tab link badge / overlay (TAB_OVERLAY.md). CORE: ctor/dtor, Initialize/_Detach, ShowActivity (observe badge), _Refresh (the linked badge), hover/expand, opacities, Autorunner cycle. The action row + the summary panel live in sibling AgentTabOverlay.{Actions,Summary}.cpp TUs.
#include "pch.h"
#include "AgentTabOverlay.h"

#include "AgentCopyActions.h" // the shared CopySessionField (reused by the Triage Board's Copy submenu)
#include "AgentStatusColors.h" // the ONE shared state->color palette (board / overlay / tab dot)
#include "AgentTipHelpers.h" // AgentSetTip — the Dark-pinned, fast-open, stuck-proof hover tooltip recipe (vs raw ToolTipService)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/ClaudeSpawn.h" // ResolveClaudeTranscriptPath / BuildClaude|CodexCommandline (row 3 CLI + transcript)
#include "AgentMaster/ProcessInspect.h" // ReadProcessCommandLine / ReadConversationText / Codex rollout resolve (row 3)
#include "AgentMaster/Persistence.h" // LoadAppSettings (skipPermissions, for the would-use CLI builder)
#include "AgentMaster/ProfileBootstrap.h" // Profiles::IsDevPackage — the Tests Autorunner badge is dev-only
#include "AgentMaster/Engine.h" // SharedEngine (claudeExePath / codexExePath, for the real launch CLI)

#include <winrt/Windows.UI.h> // Color / ColorHelper / Colors
#include <winrt/Windows.UI.Core.h> // CoreWindow / CoreCursor (summary-panel resize-grip cursors)
#include <winrt/Windows.UI.Text.h> // FontWeights
#include <winrt/Windows.UI.Xaml.Documents.h> // Run / Inlines
#include <winrt/Windows.UI.Xaml.Input.h> // PointerRoutedEventArgs
#include <winrt/Windows.UI.Xaml.Media.h> // SolidColorBrush / FontFamily
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h> // FlyoutBase (Button.Flyout)
#include <winrt/Windows.ApplicationModel.DataTransfer.h> // Clipboard / DataPackage (row 3 copy)

#include <shellapi.h> // ShellExecuteExW (row 3 folder button)
#include <mmsystem.h> // PlaySoundW (row 3 copy/open confirmation chime)
#pragma comment(lib, "winmm.lib")

#include <algorithm> // std::clamp / std::min (summary-panel size fractions)
#include <cmath> // NAN (summary-panel "auto" size sentinel on drag release)
#include <chrono> // DispatcherTimer interval (summary times-line ticker)
#include <string>
#include <unordered_set> // summary file-list de-dup (Edited/Created take over Read)
#include <vector>

using namespace winrt::Windows::Foundation;
// Narrow using-DECLARATIONS for the color helpers: a `using namespace winrt::Windows::UI;` would
// also pull the nested `Text` namespace into scope and clash with the Text() helpers (see the
// AgentManagerContent gotcha / CLAUDE.md).
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;
using winrt::Windows::UI::Core::CoreCursorType; // summary-panel resize-grip cursors
using namespace winrt::Windows::UI::Text; // FontWeights
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Documents; // Run / Inlines
using namespace winrt::Windows::UI::Xaml::Input; // PointerRoutedEventArgs
using namespace winrt::Windows::UI::Xaml::Media; // brushes
using namespace winrt::Windows::System; // DispatcherQueue
using namespace Agentmaster;
#include "AgentTabOverlay.Internal.h" // the shared file-local helpers (StateColor/renderers/CLI/...)

namespace winrt::TerminalApp::implementation
{
    AgentTabOverlay::AgentTabOverlay()
    {
        _dispatcher = DispatcherQueue::GetForCurrentThread();

        // Row 1: a horizontal strip of DISCRETE parts (model · status · actions · Autorunner[button] ·
        // queue · link) so every part can carry its OWN tooltip and the Autorunner part can be a clickable
        // button. _Refresh / ShowActivity fill _row1.Children(); right-aligned so the badge hugs the right edge.
        _row1 = StackPanel{};
        _row1.Orientation(Orientation::Horizontal);
        _row1.HorizontalAlignment(HorizontalAlignment::Right);

        // Row 2 label: "<root workdir folder>/<branch>" — secondary (smaller + dimmer), width-capped +
        // ellipsized so a long branch path can't balloon the HUD. Collapsed until _Refresh() fills it;
        // stays collapsed on the registry-less observe badge (no session). (The action buttons used to sit
        // to its left in row 2; they now live in row 1, right after the status block.)
        _subline = TextBlock{};
        _subline.FontSize(11);
        _subline.Foreground(Fill(0xFF, 0xB0, 0xB0, 0xB0));
        _subline.IsTextSelectionEnabled(false);
        _subline.TextWrapping(TextWrapping::NoWrap);
        _subline.TextTrimming(TextTrimming::CharacterEllipsis);
        _subline.VerticalAlignment(VerticalAlignment::Center);
        _subline.MaxWidth(380);
        _subline.Visibility(Visibility::Collapsed);

        // Row 2: just the "<root workdir folder>/<branch>" label, right-aligned so it hugs the right edge.
        // (The action buttons used to live here at index 0; they now sit in ROW 1, immediately after the
        // status block — see _BuildActionsRow / _Refresh.)
        _row2 = StackPanel{};
        _row2.Orientation(Orientation::Horizontal);
        _row2.HorizontalAlignment(HorizontalAlignment::Right);
        _row2.Margin(ThicknessHelper::FromLengths(0, 1, 0, 0)); // a 1px gap below row 1
        _row2.Children().Append(_subline);

        // Row 3: a preview of the NEXT queued prompt waiting to be sent (hourglass + the prompt's first
        // line, ≤300 chars). Filled by _Refresh for a LINKED session with a Pending prompt; collapsed
        // otherwise (and on observe badges, which never run _Refresh). WRAPS (so up to 300 chars can show
        // without a giant single line) but is width-capped so it can't span the terminal; right-aligned so
        // the badge stays right-anchored. The default Foreground is the text colour — the hourglass Run
        // overrides itself to amber in _Refresh.
        _promptLine = TextBlock{};
        _promptLine.FontSize(11);
        _promptLine.Foreground(Fill(0xFF, 0xC8, 0xC8, 0xC8)); // light gray (secondary; the hourglass run is amber)
        _promptLine.IsTextSelectionEnabled(false);
        _promptLine.TextWrapping(TextWrapping::Wrap);
        _promptLine.HorizontalAlignment(HorizontalAlignment::Right);
        _promptLine.MaxWidth(420);
        _promptLine.Margin(ThicknessHelper::FromLengths(0, 1, 0, 0)); // a 1px gap below row 2
        _promptLine.Visibility(Visibility::Collapsed);

        _stack = StackPanel{};
        _stack.Orientation(Orientation::Vertical);
        _stack.Children().Append(_row1);
        _stack.Children().Append(_row2);
        _stack.Children().Append(_promptLine); // row 3: the next-queued-prompt preview

        _root = Border{};
        // Dark, but FULLY OPAQUE: the badge's see-through-ness is owned SOLELY by _root.Opacity (the
        // GLOBAL "Overlay opacity" setting — tabOverlayRestOpacity/Hover, applied via SetOverlayOpacities).
        // A translucent background brush here (was 0xCC) would multiply with that Opacity, so the setting
        // could never reach a truly opaque badge — at "100%" the badge still showed the terminal through
        // its 80%-alpha fill. Opaque background => perceived opacity == the setting (100% = solid; the
        // dark #202020 still gives text contrast, and the default rest 0.50 keeps it translucent by default).
        _root.Background(Fill(0xFF, 0x20, 0x20, 0x20));
        _root.BorderBrush(Fill(0x40, 0xFF, 0xFF, 0xFF));
        _root.BorderThickness(ThicknessHelper::FromUniformLength(1));
        _root.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        _root.Padding(ThicknessHelper::FromLengths(7, 2, 7, 2));
        _root.Opacity(_restOpacity); // dim at rest; brightens to _hoverOpacity on hover (the chosen interaction)
        _root.Child(_stack);

        // Hover handlers are wired in _WireHover() (from Initialize / first ShowActivity), NOT here:
        // they capture get_weak() so they can also reveal row 3 + honor the pinned (menu-open) state,
        // and get_weak() is only valid once the object is fully constructed + ref-counted.
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
        _WireHover(); // pointer-over brighten (opacity)
        _BuildActionsRow(); // row-2 actions: folder + copy menu + pencil (linked sessions only), left of the dir/branch label
        _BuildSummaryPanel(); // the 2nd slot (summary panel), collapsed until the pencil toggles it on
        // No single line-wide tooltip here: _Refresh builds row 1 as DISCRETE parts, each with its OWN
        // concise tooltip (status / model·effort / Autorunner / queue / link); the row-2 label + action
        // buttons carry theirs too.
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
        _WireHover(); // observe badges still brighten on hover (no actions — that's linked-only)
        if (!_row1 || !_root)
        {
            return;
        }
        if (kind == _lastActivitySig)
        {
            return; // unchanged -> no XAML churn (this runs every probe tick)
        }
        _lastActivitySig = kind;
        _root.Visibility(Visibility::Visible);
        // An observe badge is ONE logical statement ("○ <kind> · unlinked"), so it stays a single
        // TextBlock with one tooltip (unlike the linked badge's per-part row-1 strip built in _Refresh).
        _row1.Children().Clear();
        TextBlock tb{};
        tb.FontSize(12);
        tb.IsTextSelectionEnabled(false);
        tb.TextWrapping(TextWrapping::NoWrap);
        tb.VerticalAlignment(VerticalAlignment::Center);
        Run glyph{};
        glyph.Text(winrt::hstring{ L"\x25CB" }); // ○ gray — observed, but not a linked session
        glyph.Foreground(Fill(0xFF, 0x9E, 0x9E, 0x9E)); // gray
        glyph.FontWeight(FontWeights::SemiBold());
        tb.Inlines().Append(glyph);
        Run text{};
        text.Text(winrt::hstring{ std::wstring{ L" " } + kind + L"  " + kDot + L"  unlinked" });
        tb.Inlines().Append(text);
        AgentSetTip(tb, winrt::hstring{
            L"Observed by Agentmaster, not a linked session. A claude links on its first prompt;\n"
            L"shells (pwsh/cmd) and external codex stay observe-only here." });
        _row1.Children().Append(tb);
    }

    void AgentTabOverlay::_Refresh()
    {
        if (_pending)
        {
            return; // a pending badge is static — it has no registry session to refresh from
        }
        if (!_row1 || !_registry)
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
        // Link state is surfaced ONLY when NOT linked (observe / unlinked). When linked (bound) the badge
        // shows NOTHING for link state — a managed tab's badge being present already implies the link, so
        // only the abnormal states are worth calling out.
        const std::wstring link = s.external ? std::wstring{ L"observe" } : std::wstring{ L"unlinked" };

        // Row 1 is built as DISCRETE, individually-tooltipped parts: model · status · Autorunner[button]
        // · queue · link. A small helper appends a text part (optional tooltip); separators reproduce the
        // one-line look ("  ·  ") as their own tooltip-less elements so the strip reads as one line.
        _row1.Children().Clear();
        const auto fg = Fill(0xFF, 0xEC, 0xEC, 0xEC);
        const auto appendText = [&](const std::wstring& t, const wchar_t* tip, const SolidColorBrush& brush, double padRight = 0.0) {
            TextBlock tb{};
            tb.FontSize(12);
            tb.IsTextSelectionEnabled(false);
            tb.TextWrapping(TextWrapping::NoWrap);
            tb.VerticalAlignment(VerticalAlignment::Center);
            tb.Foreground(brush);
            tb.Text(winrt::hstring{ t });
            if (padRight > 0.0)
            {
                tb.Padding(ThicknessHelper::FromLengths(0, 0, padRight, 0));
            }
            if (tip)
            {
                AgentSetTip(tb, winrt::hstring{ tip });
            }
            _row1.Children().Append(tb);
        };
        const std::wstring sepText = std::wstring{ L"  " } + kDot + L"  ";
        const auto appendSep = [&]() { appendText(sepText, nullptr, fg); };

        // Agentmaster (current-model adornment): the CURRENT model, LEFT of the status indicator —
        // the transcript truth (SessionDisplayModel: what the session's last reply actually ran on,
        // scanner-fed SessionInfo.currentModel, so a mid-session /model switch shows on its next
        // reply; falls back to the launch-request `model`, also where a managed Codex's rollout model
        // lives), shortened for the HUD (ShortModelName: "fable-5" / "opus-4.6"; a Codex id passes
        // through verbatim). Dimmer than the status so it reads as metadata; hidden until known (a
        // never-prompted bare launch has no model to tell).
        if (const std::wstring shortModel = ShortModelName(SessionDisplayModel(s), ParseModelFamilies(_modelFamiliesCsv)); !shortModel.empty())
        {
            appendText(shortModel,
                       L"Model \x2014 what this session's last reply actually ran on (read from the\n"
                       L"transcript; a /model switch shows here on its next reply).",
                       Fill(0xFF, 0xC8, 0xC8, 0xC8),
                       2.0); // 2px pad-right — the only separation from the status indicator (no · dot)
        }

        // Status: the colored state glyph + its label, one tooltip for the pair.
        {
            TextBlock tb{};
            tb.FontSize(12);
            tb.IsTextSelectionEnabled(false);
            tb.TextWrapping(TextWrapping::NoWrap);
            tb.VerticalAlignment(VerticalAlignment::Center);
            Run g{};
            g.Text(winrt::hstring{ StateGlyph(s.state) });
            g.Foreground(SolidColorBrush{ StateColor(s.state) });
            g.FontWeight(FontWeights::SemiBold());
            tb.Inlines().Append(g);
            Run lbl{};
            lbl.Text(winrt::hstring{ std::wstring{ L" " } + StateLabel(s.state) });
            lbl.Foreground(fg);
            tb.Inlines().Append(lbl);
            AgentSetTip(tb, winrt::hstring{
                L"Live session status, colour-matched to the Triage Board\n"
                L"(running / waiting / needs-approval / error / done / idle)." });
            _row1.Children().Append(tb);
        }

        // Action buttons (folder / copy menu / pencil) sit immediately AFTER the status block, so the strip
        // reads status (leftmost) -> actions -> autorunner · queue · link. Built once by _BuildActionsRow
        // (a LINKED session only); re-appended here every pass because _row1 is cleared+rebuilt above.
        if (_actions)
        {
            _row1.Children().Append(_actions);
        }

        // Autorunner mode — DEV ONLY: Auto Testing / Tests Autorunner is gated to the AgentmasterDev
        // package (the autorunner never runs in a release build — see Engine.cpp), so a release tab's
        // badge carries no autorunner control. A CLICKABLE button that cycles Off -> Semi -> Full -> Off,
        // colored by mode (gray Off / amber Semi / green Full) to match the Triage Board's language.
        static const bool kDevAutoTesting = ::Agentmaster::Profiles::IsDevPackage();
        if (kDevAutoTesting)
        {
        appendSep();
        {
            const auto mode = s.autorunner.mode;
            const auto modeBrush = (mode == AutorunnerMode::Full)     ? Fill(0xFF, 0x3C, 0xB3, 0x71) :  // MediumSeaGreen
                                   (mode == AutorunnerMode::SemiAuto) ? Fill(0xFF, 0xDA, 0xA5, 0x20) :  // Goldenrod
                                                                       Fill(0xFF, 0xB0, 0xB0, 0xB0);   // gray (Off)
            Button b{};
            b.Background(Fill(0x00, 0, 0, 0)); // transparent — still hit-testable; the template gives a hover highlight ("appears clickable")
            b.BorderThickness(ThicknessHelper::FromUniformLength(0));
            b.Padding(ThicknessHelper::FromLengths(2, 0, 2, 0));
            b.MinWidth(0);
            b.MinHeight(0);
            b.IsTabStop(false); // never pull keyboard focus off the ConPTY
            b.VerticalAlignment(VerticalAlignment::Center);
            b.VerticalContentAlignment(VerticalAlignment::Center);
            TextBlock t{};
            t.FontSize(12);
            t.VerticalAlignment(VerticalAlignment::Center);
            t.Foreground(modeBrush);
            t.Text(winrt::hstring{ ModeLabel(mode) });
            // CLICK-THROUGH the label so a click on the text (not just the button background) still cycles
            // the mode: the on-top TextBlock would otherwise catch the glyph-area pointer without driving
            // the ButtonBase click. The button keeps hover + tooltip (AgentSetTip is on `b`).
            t.IsHitTestVisible(false);
            b.Content(t);
            AgentSetTip(b, winrt::hstring{
                L"Tests Autorunner \x2014 click to cycle Off \x2192 Semi \x2192 Full.\n"
                L"Off: you drive. Semi: it proposes the next queued prompt, you confirm.\n"
                L"Full: it auto-sends the queue on each turn-complete." });
            const auto weak = get_weak();
            b.Click([weak](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    self->_CycleAutorunner();
                }
            });
            // "appear clickable on hover": a hand cursor over the button (the Button template adds the
            // background highlight). Restore the arrow on exit. CoreWindow drives the cursor (no
            // per-element cursor in this XAML projection — same approach as the summary-panel grips).
            b.PointerEntered([](const IInspectable&, const PointerRoutedEventArgs&) { ApplyCursor(CoreCursorType::Hand); });
            b.PointerExited([](const IInspectable&, const PointerRoutedEventArgs&) { ApplyCursor(CoreCursorType::Arrow); });
            _row1.Children().Append(b);
        }
        } // if (kDevAutoTesting) — the Tests Autorunner button is dev-only

        // queued (Pending) count — ⏳N. Dev-only too (a release build never queues, so pending is always 0).
        if (kDevAutoTesting && pending > 0)
        {
            appendSep();
            appendText(std::wstring{ kHourglass } + std::to_wstring(pending),
                       L"Prompts queued and waiting to be sent (Pending).", fg);
        }

        // link state — surfaced ONLY when NOT linked (observe / unlinked); a linked session shows nothing.
        if (!bound)
        {
            appendSep();
            appendText(link,
                       L"Link state \x2014 Agentmaster cannot drive this session.\n"
                       L"observe: read-only (hosted elsewhere). unlinked: not bound.",
                       fg);
        }

        // Row 2: "<root workdir folder>/<branch>" — the leaf of the session's working dir joined with
        // its git branch (e.g. C:/folder/myworkdir + "feature/issue123" -> "myworkdir/feature/issue123").
        // Shows the session's EFFECTIVE work dir (EffectiveWorkingDir — the INFERRED dir when the
        // session infers (SessionInfersWorkingDir: the Inferred tab-color mode, or a home-dir launch
        // in ANY mode) and the scan detected it working OUTSIDE its launch cwd,
        // else the persisted M-axis workingDir); fall back to the live PEB cwd. Hidden when neither a
        // folder nor a branch is known.
        if (_subline)
        {
            const bool inferredShown = !s.inferredWorkingDir.empty() && ::Agentmaster::SessionInfersWorkingDir(static_cast<::Agentmaster::TabColorMode>(_tabColorMode), s);
            const std::wstring effDir = ::Agentmaster::EffectiveWorkingDir(static_cast<::Agentmaster::TabColorMode>(_tabColorMode), s);
            std::wstring dir = !effDir.empty() ? effDir : s.liveCwd;
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
                // Row 2 is width-capped + ellipsized and shows only the leaf folder; the tooltip names
                // what it is and reveals the FULL working path (+ branch) behind it (the Archive-page
                // reveal-behind-truncation pattern). When the shown dir is the INFERRED one, the tip
                // also carries the launch cwd so neither directory is ever hidden.
                std::wstring detail = dir;
                if (!s.branch.empty())
                {
                    detail = detail.empty() ? s.branch : (detail + L"  " + kDot + L"  " + s.branch);
                }
                std::wstring tip = std::wstring{ inferredShown ? L"Inferred working directory \x00B7 git branch\n" : L"Working directory \x00B7 git branch\n" } + detail;
                if (inferredShown && !s.workingDir.empty())
                {
                    tip += L"\nlaunched in " + s.workingDir;
                }
                AgentSetTip(_subline, winrt::hstring{ tip });
                _subline.Visibility(Visibility::Visible);
            }
        }

        // Row 3: a preview of the NEXT queued prompt waiting to be sent — the first Pending prompt (the
        // same one DecideAdvance would fire next), shown as "<hourglass> <first line, ≤300 chars>". This
        // is the per-tab echo of row 1's "⏳N" count: the count says HOW MANY are queued, this says WHAT
        // is next. Hidden when nothing is Pending.
        if (_promptLine)
        {
            const QueuedPrompt* nextPrompt = nullptr;
            for (const auto& p : s.queue)
            {
                if (p.status == PromptStatus::Pending)
                {
                    nextPrompt = &p;
                    break;
                }
            }
            // Prefer the prompt body (what actually gets sent); fall back to its short label.
            const std::wstring body = nextPrompt ? (!nextPrompt->text.empty() ? nextPrompt->text : nextPrompt->label) : std::wstring{};
            const std::wstring preview = nextPrompt ? FirstLinePreview(body, 300) : std::wstring{};
            _promptLine.Inlines().Clear();
            if (preview.empty())
            {
                _promptLine.Visibility(Visibility::Collapsed); // nothing queued (or an empty prompt)
            }
            else
            {
                Run hg{};
                hg.Text(winrt::hstring{ std::wstring{ kHourglass } + L" " });
                hg.Foreground(Fill(0xFF, 0xDA, 0xA5, 0x20)); // goldenrod — "queued / pending", matching the row-1 ⏳N
                _promptLine.Inlines().Append(hg);
                Run txt{};
                txt.Text(winrt::hstring{ preview }); // inherits the TextBlock's light-gray foreground
                _promptLine.Inlines().Append(txt);
                // The preview is the FIRST line trimmed to 300 chars; the tooltip names the row and reveals
                // the FULL prompt behind it (capped so a huge prompt can't make an unwieldy tooltip).
                std::wstring full = body;
                if (full.size() > 4000)
                {
                    full = full.substr(0, 4000) + L"\x2026"; // …
                }
                const std::wstring tip = std::wstring{ L"Next queued prompt \x2014 sent on the next turn-complete (or via Send now).\n\n" } + full;
                AgentSetTip(_promptLine, winrt::hstring{ tip });
                _promptLine.Visibility(Visibility::Visible);
            }
        }

        // Summary panel (2nd slot): show/hide per the persisted toggle + (re)load when the transcript grew.
        _UpdateSummary(s);
    }

    void AgentTabOverlay::_WireHover()
    {
        if (_hoverWired || !_root)
        {
            return;
        }
        _hoverWired = true;
        auto weak = get_weak();
        // Expanded == pointer over the badge OR the copy menu pinned open. Weak captures only, so the
        // handlers (owned by _root) never keep the overlay (which owns _root) alive -> no ref cycle.
        _root.PointerEntered([weak](const IInspectable&, const PointerRoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_hovering = true;
                self->_SetExpanded(true);
            }
        });
        _root.PointerExited([weak](const IInspectable& sender, const PointerRoutedEventArgs& e) {
            if (PointerWithin(sender, e))
            {
                return; // a child's bubbled exit while the pointer is still on the badge — not a real leave
            }
            if (auto self = weak.get())
            {
                self->_hovering = false;
                self->_SetExpanded(self->_pinned); // stay open while the copy menu is up
            }
        });
        // ── DIAGNOSTIC (dead-click trace; the badge-root leg — see _BuildActionsRow's per-button
        // traces for the story + removal note). handledEventsToo=true: logs every press that lands
        // ANYWHERE in the badge subtree with the element it originated on + whether some descendant
        // had already Handled it. If a glyph press shows here but NOT on its button, the eater sits
        // between them; if it doesn't show at all, the hit landed outside our tree (a popup?).
        _root.AddHandler(UIElement::PointerPressedEvent(),
                         winrt::box_value(PointerEventHandler{ [](const IInspectable&, const PointerRoutedEventArgs& e) {
                             std::wstring cls{ L"-" };
                             if (const auto src = e.OriginalSource())
                             {
                                 cls = std::wstring{ winrt::get_class_name(src) };
                                 if (const auto dot = cls.find_last_of(L'.'); dot != std::wstring::npos)
                                 {
                                     cls = cls.substr(dot + 1);
                                 }
                             }
                             ::Agentmaster::AppendStateLog(L"hooks.log", std::wstring{ L"[overlay-hit] root press src=" } + cls + (e.Handled() ? L" handled=1\n" : L" handled=0\n"));
                         } }),
                         true /* handledEventsToo */);
        // ── end DIAGNOSTIC ──
    }

    void AgentTabOverlay::_SetExpanded(bool on)
    {
        // The action buttons are ALWAYS shown now (they live in row 1, right after the status block), so
        // this only dims/brightens the WHOLE badge: dim at rest, full on hover OR while the copy menu is
        // pinned open.
        if (_root)
        {
            _root.Opacity(on ? _hoverOpacity : _restOpacity);
        }
    }

    // Agentmaster (TAB_OVERLAY.md): adopt the GLOBAL rest/hover opacities (cog slider; seeded on attach,
    // broadcast on Save). Enforce rest <= hover defensively (the slider already does), then re-apply live:
    // the badge to its CURRENT expanded state (_hovering / copy-menu _pinned), and the summary panel to
    // rest (its own pointer-over handlers re-brighten it on the next move — and in practice nothing is
    // hovered when a cog Save lands, since the modal covers the window).
    void AgentTabOverlay::SetOverlayOpacities(double rest, double hover)
    {
        if (!(rest > 0.0 && rest <= 1.0))
        {
            rest = 0.50;
        }
        if (!(hover > 0.0 && hover <= 1.0))
        {
            hover = 1.0;
        }
        if (rest > hover)
        {
            rest = hover;
        }
        _restOpacity = rest;
        _hoverOpacity = hover;
        if (_root)
        {
            _SetExpanded(_hovering || _pinned);
        }
        if (_summaryRoot)
        {
            _summaryRoot.Opacity(_restOpacity);
        }
    }

    // Agentmaster (inferred working dir): adopt the GLOBAL tab-color MODE (seeded on attach, broadcast
    // by _ReapplyManagedTabColors on a cog Save / cross-window apply). It keys the EFFECTIVE work dir
    // the badge shows/acts on — row 2's "<workdir folder>/<branch>" subline, Open Path, and Copy Path
    // resolve through EffectiveWorkingDir, so under InferredWorkingDirectory they follow the dir the
    // session actually works in (the same dir its tab color / board card / tree group key on). A
    // change re-renders a LINKED badge (an observe badge shows no dir — nothing to redo).
    void AgentTabOverlay::SetTabColorMode(int mode)
    {
        if (_tabColorMode == mode)
        {
            return;
        }
        _tabColorMode = mode;
        if (!_pending && !_sessionId.empty() && _registry)
        {
            _Refresh(); // repaint row 2 under the new mode
        }
    }

    // Agentmaster (current-model adornment): adopt the GLOBAL model-family list (seeded on attach,
    // broadcast alongside SetTabColorMode on a cog Save) — the words row 1's model shortener
    // recognizes when the model is a BARE --model alias. Kept as the raw csv (ShortModelName parses
    // via ParseModelFamilies at each refresh — a refresh is notify-driven and the list is tiny);
    // change-gated so the steady-state re-seed is a no-op.
    void AgentTabOverlay::SetModelFamilies(const std::wstring& familiesCsv)
    {
        if (_modelFamiliesCsv == familiesCsv)
        {
            return;
        }
        _modelFamiliesCsv = familiesCsv;
        if (!_pending && !_sessionId.empty() && _registry)
        {
            _Refresh(); // repaint row 1's model part under the new list
        }
    }

    // The row-1 Autorunner button: cycle THIS session's mode Off -> Semi-auto -> Full -> Off, mirroring
    // AgentManagerContent::_CycleAutorunner / _OnAutorunnerChanged. The overlay already holds the SHARED
    // registry, so we mutate it directly: SessionRegistry::Update fires observers, which (a) marshals our
    // own _Refresh to repaint the button and (b) wakes the scheduler's OnObserved — cycling to Semi/Full
    // while the session sits Idle/WaitingForInput naturally kicks a queued plan (Correctness Rule #1). No
    // new send path is invented (Rule #2); we only set the mode + reset the per-run backstops on arming.
    void AgentTabOverlay::_CycleAutorunner()
    {
        if (!_registry || _sessionId.empty())
        {
            return;
        }
        const auto info = _registry->Get(_sessionId);
        if (!info || !info->live)
        {
            return; // nothing live to drive (same guard as the Manager's header toggle)
        }
        const auto cur = info->autorunner.mode;
        const AutorunnerMode next = (cur == AutorunnerMode::Off)      ? AutorunnerMode::SemiAuto :
                                   (cur == AutorunnerMode::SemiAuto) ? AutorunnerMode::Full :
                                                                      AutorunnerMode::Off;
        _registry->Update(_sessionId, [&](SessionInfo& s) {
            s.autorunner.mode = next;
            if (next != AutorunnerMode::Off)
            {
                // Arming resets the per-run backstop counter + clears any stale confirm (== _OnAutorunnerChanged).
                s.autorunner.autoSendsThisRun = 0;
                s.pendingConfirmPromptId.clear();
            }
        });
        _Refresh(); // immediate repaint (the async registry observer also refreshes)
    }

}
