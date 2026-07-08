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
//   AgentTabOverlay.cpp          - CORE: ctor/dtor, Initialize/_Detach, ShowActivity (the observe badge), _Refresh (the linked badge), hover/expand, opacities, Autorunner
//   AgentTabOverlay.Internal.h   - shared file-local helpers: StateColor/Glyph/Label, the summary-box renderers, time formatting, launch-CLI + clipboard (anonymous namespace, a per-TU copy)
// ★ AgentTabOverlay.Actions.cpp  - the hover action row: the folder (Open Path) button, the copy menu, and the shared CopySessionField action
//   AgentTabOverlay.Summary.cpp  - the pencil-toggled summary panel: build/render/load off-thread, the times bar, resize grips, wrap/truncate/previous, JUMP, copy-summary
// ======================================================================================
//
// Agentmaster per-tab overlay: the hover ACTION ROW -- the folder (Open Path) button, the copy menu, and the shared CopySessionField action (declared in AgentCopyActions.h; reused by the Manager Copy submenu). Partial TU of AgentTabOverlay.cpp.
#include "pch.h"
#include "AgentTabOverlay.h"

#include "AgentCopyActions.h" // the shared CopySessionField (reused by the Triage Board's Copy submenu)
#include "AgentStatusColors.h" // the ONE shared state->color palette (board / overlay / tab dot)
#include "AgentTipHelpers.h" // AgentSetTip — the Dark-pinned, fast-open, stuck-proof hover tooltip recipe (vs raw ToolTipService)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/ClaudeSpawn.h" // ResolveClaudeTranscriptPath / BuildClaude|CodexCommandline (row 3 CLI + transcript)
#include "AgentMaster/ProcessInspect.h" // ReadProcessCommandLine / ReadConversationText / Codex rollout resolve (row 3)
#include "AgentMaster/Persistence.h" // LoadAppSettings (skipPermissions, for the would-use CLI builder)
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
    void AgentTabOverlay::_BuildActionsRow()
    {
        if (_actions || !_row1)
        {
            return; // built once, and only for a LINKED session (never an observe badge)
        }
        auto weak = get_weak();

        // A minimal transparent icon button: no chrome at rest, the default template still gives a
        // hover highlight. IsTabStop(false) so it never pulls keyboard focus off the ConPTY.
        const auto mkIconBtn = [](const wchar_t* glyph, const wchar_t* tip) {
            Button b{};
            b.Background(Fill(0x00, 0, 0, 0)); // transparent (alpha 0) — still hit-testable, unlike null
            b.BorderThickness(ThicknessHelper::FromUniformLength(0));
            b.Padding(ThicknessHelper::FromLengths(4, 0, 4, 0));
            b.MinWidth(0);
            b.IsTabStop(false);
            FontIcon fi{};
            fi.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
            fi.Glyph(glyph);
            fi.FontSize(12);
            // CLICK-THROUGH the glyph: a hit-testable FontIcon sitting on the button's transparent
            // background intercepts the pointer on the glyph's own pixels but does NOT reliably drive the
            // ButtonBase press->release, so a click landing ON the icon (rather than the surrounding
            // background) was lost. IsHitTestVisible(false) makes the glyph transparent to input, so EVERY
            // click over the button — glyph or background — lands on the button itself (the one click
            // target). The button still gets hover/tooltip (AgentSetTip is on `b`). (Codebase pattern: a
            // decorative/content child is click-through so the parent is the sole hit target.)
            fi.IsHitTestVisible(false);
            b.Content(fi);
            AgentSetTip(b, winrt::hstring{ tip });
            // ── DIAGNOSTIC (dead-click trace — remove once the icon-click root cause is confirmed
            // fixed). The reported bug: clicking an action ICON did nothing. Root cause found: the
            // DEV-ONLY tooltip-id feature (AgentDevTooltipNames.h) resolved the button's TEMPLATE
            // ContentPresenter (the stock template root is x:Name="ContentPresenter") and permanently
            // SetToolTip'd that template CHILD — framework tooltip machinery below ButtonBase on the
            // press route, eating the press before the button could see it. That resolver is fixed;
            // this trace stays one deploy to PROVE the fix (and to name any second eater if one
            // exists): for every press/release it logs whether the event REACHED the button and
            // whether a descendant had already Handled it (handledEventsToo=true sees those too),
            // plus capture-loss mid-press (a steal/reparent cancels a click) and the Click itself.
            // Rare, human-scale events — one short hooks.log line each ([overlay-hit]).
            const uint32_t glyphCp = static_cast<uint32_t>(glyph[0]); // E8B7 folder / E8C8 copy / E70F pencil / 2191 up / 2193 down
            const auto hitLine = [glyphCp](const wchar_t* what, const winrt::Windows::Foundation::IInspectable& src, bool handled) {
                wchar_t cp[8]{};
                swprintf_s(cp, L"%04X", glyphCp);
                std::wstring cls{ L"-" };
                if (src)
                {
                    cls = std::wstring{ winrt::get_class_name(src) };
                    if (const auto dot = cls.find_last_of(L'.'); dot != std::wstring::npos)
                    {
                        cls = cls.substr(dot + 1); // "Windows.UI.Xaml.Controls.Button" -> "Button"
                    }
                }
                ::Agentmaster::AppendStateLog(L"hooks.log", std::wstring{ L"[overlay-hit] btn=" } + cp + L" " + what + L" src=" + cls + (handled ? L" handled=1\n" : L" handled=0\n"));
            };
            b.AddHandler(UIElement::PointerPressedEvent(),
                         winrt::box_value(PointerEventHandler{ [hitLine](const IInspectable&, const PointerRoutedEventArgs& e) {
                             hitLine(L"press", e.OriginalSource(), e.Handled());
                         } }),
                         true /* handledEventsToo — see the press even when a descendant already ate it */);
            b.AddHandler(UIElement::PointerReleasedEvent(),
                         winrt::box_value(PointerEventHandler{ [hitLine](const IInspectable&, const PointerRoutedEventArgs& e) {
                             hitLine(L"release", e.OriginalSource(), e.Handled());
                         } }),
                         true);
            b.PointerCaptureLost([hitLine](const IInspectable&, const PointerRoutedEventArgs& e) {
                hitLine(L"capture-lost", e.OriginalSource(), e.Handled());
            });
            b.Click([hitLine](const IInspectable&, const RoutedEventArgs&) {
                hitLine(L"CLICK", nullptr, false); // the ButtonBase click actually fired
            });
            // ── end DIAGNOSTIC ──
            return b;
        };

        // Agentmaster: this session's agent kind (Claude default), read ONCE at build time — a session's
        // kind is fixed for the overlay's lifetime. It drives two kind-specific menu choices below: the
        // Claude-only prompt-nav (↑/↓) buttons and the launch-CLI copy item (Claude vs Codex, never both).
        const bool isCodexSession = [&]() {
            if (_registry && !_sessionId.empty())
            {
                if (const auto info = _registry->Get(_sessionId))
                {
                    return info->kind == AgentKind::Codex;
                }
            }
            return false; // unknown -> Claude (the default kind)
        }();

        // Agentmaster (SUMMARY_JUMP.md §7): ↑ / ↓ — step the view to the previous / next SENT prompt that
        // is currently off-screen, and highlight it in the summary panel, exactly like alt+up / alt+down
        // (the page runs the SAME _ScrollAdjacentPrompt). Placed LEFT of the folder button. Claude only —
        // Codex has no in-buffer prompt resolve in v1, so the buttons are omitted for a Codex session.
        const bool promptNavEligible = !isCodexSession;
        Button upBtn{ nullptr };
        Button downBtn{ nullptr };
        if (promptNavEligible)
        {
            upBtn = mkIconBtn(L"\x2191", L"Go to the previous sent prompt that's off-screen (like Alt+Up)"); // ↑
            downBtn = mkIconBtn(L"\x2193", L"Go to the next sent prompt that's off-screen (like Alt+Down)"); // ↓
            // Standard Unicode arrows render from a text symbol font, not the icon font (which would tofu
            // them) — same reason the ▸ summary-jump glyph uses "Segoe UI Symbol".
            if (auto ic = upBtn.Content().try_as<FontIcon>())
            {
                ic.FontFamily(FontFamily{ L"Segoe UI Symbol" });
            }
            if (auto ic = downBtn.Content().try_as<FontIcon>())
            {
                ic.FontFamily(FontFamily{ L"Segoe UI Symbol" });
            }
            upBtn.Click([weak](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    if (self->_onAdjacentPrompt)
                    {
                        self->_onAdjacentPrompt(true); // up
                    }
                }
            });
            downBtn.Click([weak](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    if (self->_onAdjacentPrompt)
                    {
                        self->_onAdjacentPrompt(false); // down
                    }
                }
            });
        }

        Button folderBtn = mkIconBtn(L"\xE8B7", L"Open the working folder in Explorer"); // Folder
        folderBtn.Click([weak](const IInspectable&, const RoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_OpenFolder();
            }
        });

        Button copyBtn = mkIconBtn(L"\xE8C8", L"Copy session details\x2026 (id, path, branch, launch CLI, summary, transcript)"); // Copy
        MenuFlyout flyout{};
        // Each menu item carries a tooltip that says exactly WHAT gets copied (the labels are terse;
        // the tip spells out the value), mirroring _CopyField's per-case behavior.
        const auto addItem = [&flyout, weak](const wchar_t* text, const wchar_t* tip, int which) {
            MenuFlyoutItem item{};
            item.Text(text);
            AgentSetTip(item, winrt::hstring{ tip });
            item.Click([weak, which](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    self->_CopyField(which);
                }
            });
            flyout.Items().Append(item);
        };
        addItem(L"Session Id", L"Copy the resumable conversation id (Codex: its rollout uuid)", 0);
        addItem(L"Copy Path", L"Copy the session's working-directory path", 1);
        addItem(L"Copy Branch Name", L"Copy the session's current git branch name", 2);
        // Offer ONLY the launch-CLI that matches this session's agent — a Claude session gets "Claude Launch
        // CLI", a Codex session "Codex Launch CLI" (never the other, which would synthesize a command for an
        // agent this session isn't running).
        if (isCodexSession)
        {
            addItem(L"Codex Launch CLI", L"Copy the full codex launch command line", 4);
        }
        else
        {
            addItem(L"Claude Launch CLI", L"Copy the full claude.exe launch command line (with --settings hooks and flags)", 3);
        }
        addItem(L"Summary", L"Copy the FULL session summary \x2014 the complete box (id, resume CLI, dir, folder, branch, duration, tasks, messages, files), including everything the displayed panel trims", 6);
        addItem(L"Transcript", L"Copy the whole conversation as text (your prompts + the agent's replies)", 5);
        // The pointer must LEAVE the badge to reach the menu, so pin the expanded state while it's open.
        flyout.Opened([weak](const IInspectable&, const IInspectable&) {
            if (auto self = weak.get())
            {
                self->_pinned = true;
                self->_SetExpanded(true);
            }
        });
        flyout.Closed([weak](const IInspectable&, const IInspectable&) {
            if (auto self = weak.get())
            {
                self->_pinned = false;
                self->_SetExpanded(self->_hovering);
            }
        });
        copyBtn.Flyout(flyout);

        // Pencil: toggle the SUMMARY PANEL (the 2nd overlay slot, below this badge). The visibility is
        // a GLOBAL setting (AppSettings::showSummaryPanel) — shared across windows + persisted — so the
        // pencil hands off to the page (_ToggleSummary -> _onToggleSummary), which flips it everywhere.
        Button pencilBtn = mkIconBtn(L"\xE70F", L"Show/hide the session summary panel (all tabs)"); // Edit (pencil)
        // Render the pencil from "Segoe MDL2 Assets" rather than "Segoe Fluent Icons": at the SAME codepoint
        // (E70F) the MDL2 glyph draws the pencil WITH a line across its bottom (the line it's drawing), while
        // the Fluent variant is a plain pencil with no line. The with-line look reads as "edit/notes" far
        // better here, so override just this one button's font (folder/copy stay Fluent). MDL2 first, Fluent
        // as the graceful fallback if MDL2 isn't present.
        if (auto pencilIcon = pencilBtn.Content().try_as<FontIcon>())
        {
            pencilIcon.FontFamily(FontFamily{ L"Segoe MDL2 Assets, Segoe Fluent Icons" });
        }
        pencilBtn.Click([weak](const IInspectable&, const RoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_ToggleSummary();
            }
        });

        _actions = StackPanel{};
        _actions.Orientation(Orientation::Horizontal);
        _actions.VerticalAlignment(VerticalAlignment::Center); // line up with the row-1 status/autorunner parts
        _actions.Spacing(2);
        _actions.Margin(ThicknessHelper::FromLengths(4, 0, 0, 0)); // a small gap after the status block to its left
        if (upBtn)
        {
            _actions.Children().Append(upBtn); // ↑ prev off-screen prompt (left of the folder)
        }
        if (downBtn)
        {
            _actions.Children().Append(downBtn); // ↓ next off-screen prompt
        }
        _actions.Children().Append(folderBtn);
        _actions.Children().Append(copyBtn);
        _actions.Children().Append(pencilBtn);
        // ALWAYS shown (no longer hover-only). The buttons live in ROW 1 now, immediately to the RIGHT of
        // the status block (so the strip reads status -> actions -> autorunner · queue · link). The actual
        // append is done by _Refresh (it clears + rebuilds row 1 each pass), right after the status part.
    }

    void AgentTabOverlay::_OpenFolder()
    {
        if (!_registry || _sessionId.empty())
        {
            return;
        }
        const auto info = _registry->Get(_sessionId);
        if (!info)
        {
            return;
        }
        // The session's EFFECTIVE work dir (EffectiveWorkingDir — the INFERRED dir under the Inferred
        // tab-color mode, else the persisted M-axis workingDir; the same resolution as row 2); fall
        // back to the live PEB cwd. Opens where the session actually WORKS — the folder row 2 names.
        const std::wstring effDir = ::Agentmaster::EffectiveWorkingDir(static_cast<::Agentmaster::TabColorMode>(_tabColorMode), *info);
        std::wstring dir = !effDir.empty() ? effDir : info->liveCwd;
        if (!dir.empty())
        {
            // Nav audit: the user clicked the overlay's folder button (Open Path) — opens the session's
            // working dir in explorer.exe.
            ::Agentmaster::LogNav(L"open-path " + ::Agentmaster::ShortId(_sessionId) + L" dir=" + dir);
            OpenPathInExplorerAsync(dir);
            PlayActionSound(); // same click feedback as the copy menu (Open Path)
        }
    }

    // Agentmaster: the shared copy-field action (declared in AgentCopyActions.h) — the SINGLE
    // implementation behind both this overlay's copy menu (_CopyField, below) and the Triage Board /
    // Explorer-tree session menu's Copy submenu (AgentManagerContent::_MakeSessionMenu). It lives here
    // because the launch-CLI / transcript / summary helpers it leans on are this file's anon-namespace
    // helpers; both menus call it so they can never drift apart. `which` is the copy-menu code (see the
    // header). Synchronous clipboard writes (cases 0-4) run on the calling UI thread; the transcript /
    // summary cases (5/6) read off-thread and hop back via `dispatcher`.
    void CopySessionField(SessionRegistry& registry, const std::wstring& sessionId, int which, const DispatcherQueue& dispatcher, bool wrapNewlines, bool truncate, int tabColorMode)
    {
        if (sessionId.empty())
        {
            return;
        }
        const auto info = registry.Get(sessionId);
        if (!info)
        {
            return;
        }
        const auto& s = *info;
        const bool codex = (s.kind == AgentKind::Codex);
        // Nav audit: the user copied a session field to the clipboard. This ONE shared action backs BOTH
        // copy menus (the per-tab overlay's + the Triage Board / Explorer-tree Copy submenu), so logging
        // here covers "what was picked" for every copy site at once.
        static const wchar_t* const kCopyFieldNames[] = { L"session-id", L"path", L"branch", L"claude-cli", L"codex-cli", L"transcript", L"summary" };
        ::Agentmaster::LogNav(std::wstring{ L"copy " } + ((which >= 0 && which < 7) ? kCopyFieldNames[which] : L"?") + L" " + ::Agentmaster::ShortId(sessionId));
        switch (which)
        {
        case 0: // Session Id — the resumable conversation id (Codex: its rollout uuid)
        {
            const std::wstring convId = codex ? (s.codexSessionId.empty() ? s.id : s.codexSessionId) : s.id;
            if (!convId.empty())
            {
                CopyTextToClipboard(convId);
            }
            break;
        }
        case 1: // Copy Path — the session's EFFECTIVE working directory (the inferred dir under the Inferred tab-color mode, else the launch cwd — matches the overlay subline / Open Path)
        {
            const std::wstring effDir = EffectiveWorkingDir(static_cast<TabColorMode>(tabColorMode), s);
            const std::wstring dir = !effDir.empty() ? effDir : s.liveCwd;
            if (!dir.empty())
            {
                CopyTextToClipboard(dir);
            }
            break;
        }
        case 2: // Copy Branch Name — the session's git branch (live-observed; empty => no-op)
            if (!s.branch.empty())
            {
                CopyTextToClipboard(s.branch);
            }
            break;
        case 3: // Claude Launch CLI — the REAL full command (live commandline / would-use builder)
            CopyTextToClipboard(BuildLaunchCli(s, /*wantCodex*/ false));
            break;
        case 4: // Codex Launch CLI — the REAL full command
            CopyTextToClipboard(BuildLaunchCli(s, /*wantCodex*/ true));
            break;
        case 5: // Transcript — the whole conversation (user + assistant text only), off-thread
            CopyConversationAsync(dispatcher, codex, s.id, s.codexSessionId);
            break;
        case 6: // Summary — the FULL textual session-end.js box (everything; the panel shows a trimmed view)
        {
            const std::wstring dir = !s.workingDir.empty() ? s.workingDir : s.liveCwd;
            const std::wstring resume = BuildLaunchCli(s, codex); // the same REAL launch CLI as cases 3/4
            CopySummaryAsync(dispatcher, codex, s.id, s.codexSessionId, dir, resume,
                             std::wstring{ StateGlyph(s.state) }, std::wstring{ StateLabel(s.state) }, wrapNewlines, truncate);
            break;
        }
        default:
            break;
        }
    }

    void AgentTabOverlay::_CopyField(int which)
    {
        if (!_registry)
        {
            return;
        }
        // Delegate to the shared action (reused by the Triage Board's Copy submenu); the overlay's
        // mirrored GLOBAL flags drive the Summary case so its render matches the displayed panel,
        // and the mirrored tab-color mode drives the Path case's effective-work-dir resolution.
        CopySessionField(*_registry, _sessionId, which, _dispatcher, _summaryWrapNewlines, _summaryTruncate, _tabColorMode);
    }

}
