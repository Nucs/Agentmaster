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

#include "AgentCatchLog.h" // AgentLogCaughtException — full-detail swallowed-exception forensics (the wrapped live-draft read, case 7)
#include "AgentCopyActions.h" // the shared CopySessionField (reused by the Triage Board's Copy submenu)
#include "AgentStatusColors.h" // the ONE shared state->color palette (board / overlay / tab dot)
#include "AgentTipHelpers.h" // AgentSetTip — the Dark-pinned, fast-open, stuck-proof hover tooltip recipe (vs raw ToolTipService)
#include "AgentMaster/PendingInput.h" // PickCurrentPromptText — the pure live-vs-remembered draft rule ("Copy Current Prompt")
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

        Button copyBtn = mkIconBtn(L"\xE8C8", L"Copy session details\x2026 (id, path, branch, current prompt, launch CLI, summary, transcript)"); // Copy
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
        // Copy Current Prompt (PENDING_INPUT.md) — the UNSENT draft in the input box, read LIVE from
        // the terminal buffer with the observer's recorded draft as the fallback. Claude only: Codex's
        // TUI has no ❯ rule-wrapped input box, so there is no draft to read for it.
        if (!isCodexSession)
        {
            addItem(L"Copy Current Prompt", L"Copy what is typed into this session's input box but NOT yet sent \x2014 read live from the terminal, falling back to the last observed draft (nothing is copied when the box is empty)", 7);
        }
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
            // Keep the glyph so SetSummaryEnabled can recolor it: the pencil is a TOGGLE (the GLOBAL
            // showSummaryPanel) and was the ONE toggle with no state visual — on a tab whose panel has
            // nothing to render, a click changed NOTHING visible anywhere, which reads as a dead button
            // (the "button not hit" report's residue: the log showed every CLICK firing + the setting
            // flipping, invisibly). Now the glyph itself answers every click: dim OFF / lighter ON.
            _summaryPencilIcon = pencilIcon;
        }
        _UpdateSummaryPencilVisual(); // seed the pencil state color (default: dim/off; the page's SetSummaryEnabled seed re-paints)
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
    // header). Synchronous clipboard writes (cases 0-4 + 7) run on the calling UI thread; the transcript /
    // summary cases (5/6) read off-thread and hop back via `dispatcher`. `liveDraft` is the optional
    // live input-box reader used by case 7 alone (see the header) — absent/failing degrades to the
    // observer's remembered draft, never to nothing.
    void CopySessionField(SessionRegistry& registry, const std::wstring& sessionId, int which, const DispatcherQueue& dispatcher, bool wrapNewlines, bool truncate, int tabColorMode, const std::function<std::wstring()>& liveDraft)
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
        static const wchar_t* const kCopyFieldNames[] = { L"session-id", L"path", L"branch", L"claude-cli", L"codex-cli", L"transcript", L"summary", L"current-prompt" };
        ::Agentmaster::LogNav(std::wstring{ L"copy " } + ((which >= 0 && which < 8) ? kCopyFieldNames[which] : L"?") + L" " + ::Agentmaster::ShortId(sessionId));
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
            // PwshRunnable: prefix the `&` call operator so the copied (quoted-exe) command pastes-and-
            // runs in PowerShell (the Windows-native default shell) instead of ParserError-ing.
            CopyTextToClipboard(PwshRunnable(BuildLaunchCli(s, /*wantCodex*/ false)));
            break;
        case 4: // Codex Launch CLI — the REAL full command
            CopyTextToClipboard(PwshRunnable(BuildLaunchCli(s, /*wantCodex*/ true)));
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
        case 7: // Current Prompt — the UNSENT draft sitting in this session's input box (PENDING_INPUT.md)
        {
            if (codex)
            {
                // Codex's TUI has no U+276F rule-wrapped input box, so nothing monitors (or could read)
                // a draft for it — the menus omit this item for a Codex session; this is the backstop.
                break;
            }
            // (1) The LIVE read, WRAPPED: the hosting window hands us a provider that reads the input
            // box straight out of the terminal buffer this instant. It is absent by design whenever the
            // tab isn't reachable from the calling window (another window's UI thread, a dormant
            // restored tab, a closed session), and its own failures (a control torn down mid-click, a
            // buffer not yet initialized) must never cost the user the copy — hence the catch, which
            // simply leaves `live` empty and falls through to (2).
            std::wstring live;
            if (liveDraft)
            {
                try
                {
                    live = liveDraft();
                }
                catch (...)
                {
                    // Rule #18: never lose a swallowed exception. Recovery is unchanged (fall back to
                    // the remembered draft) — this only records what threw + where.
                    ::Agentmaster::AgentLogCaughtException(L"CopySessionField live draft read");
                }
            }
            // (2) The pick: a non-empty live read wins, else the observer's recorded draft (which may be
            // the persisted memory of a session that is no longer running). ONE pure rule, shared by
            // every copy menu (PendingInput.h).
            const auto pick = ::Agentmaster::PickCurrentPromptText(live, s.pendingInput);
            // NOT a third tier on the durable per-session store (PENDING_INPUT.md §8c), deliberately:
            // this function has already returned for an unknown session id, so reaching here means the
            // registry KNOWS this session — and a known session with an empty `pendingInput` is an
            // authoritative "there is no draft", not a "don't know". Falling back to the stored copy
            // there could only ever hand over a STALE draft (the one the user just sent, in the window
            // before the async store clear lands). The store's read tier belongs to a caller the
            // registry CANNOT answer for (a closed / never-managed session on the Sessions page, the
            // agentmaster CLI) — see §8c.
            if (pick.text.empty())
            {
                // Nothing typed anywhere: no clipboard write, no chime — and a log line, so a "why did
                // nothing happen?" is answerable from hooks.log instead of being a silent dead click.
                ::Agentmaster::AppendStateLog(L"hooks.log", L"[pending] " + ::Agentmaster::ShortId(sessionId) + L" copy current prompt: nothing (box empty, no remembered draft)\n");
                break;
            }
            CopyTextToClipboard(pick.text); // verbatim + whole (multi-line drafts included); chimes on success
            // The SOURCE belongs in the mechanism layer next to the other [pending] traces: `live` == read
            // from the box this instant, `remembered` == the scan lane's value (possibly a restored memory,
            // which the age of pendingInputUnixMs is what makes honest).
            ::Agentmaster::AppendStateLog(L"hooks.log",
                                          L"[pending] " + ::Agentmaster::ShortId(sessionId) + L" copy current prompt: " +
                                              (pick.fromLive ? L"live" : L"remembered") + L" chars=" + std::to_wstring(pick.text.size()) + L"\n");
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
        // The live-draft provider ("Copy Current Prompt", case 7) is the page's — this overlay sits
        // INSIDE the session's own pane, so its window always hosts the tab and the live read is the
        // one that applies; a missing handler (never wired / page gone) degrades to the remembered draft.
        CopySessionField(*_registry, _sessionId, which, _dispatcher, _summaryWrapNewlines, _summaryTruncate, _tabColorMode, _onReadLiveDraft);
    }

}
