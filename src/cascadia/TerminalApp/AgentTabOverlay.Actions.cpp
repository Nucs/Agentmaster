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
#include "AgentAutomation.h" // the badge buttons' accessible names and ids (E2E_TESTING.md §6.3)
#include "AgentTipHelpers.h" // AgentSetTip — the Dark-pinned, fast-open, stuck-proof hover tooltip recipe (vs raw ToolTipService)
#include "AgentMaster/PendingInput.h" // PickCurrentPromptText — the pure live-vs-remembered draft rule ("Copy Current Prompt")
#include "AgentMaster/PendingPaste.h" // FindPasteMarkers — the cheap pure "does this prompt carry a paste placeholder?" pre-check (PENDING_INPUT.md §8e)
#include "AgentMaster/Scheduler.h" // kAnswersQuestionOk — the MAIL button's queued draft carries the question-guard opt-out (DELIVERY.md §8)
#include "AgentMaster/SessionRegistry.h"
#include "AgentMaster/ClaudeSpawn.h" // ResolveClaudeTranscriptPath / BuildClaude|CodexCommandline (row 3 CLI + transcript)
#include "AgentMaster/ProcessInspect.h" // ReadProcessCommandLine / ReadConversationText / Codex rollout resolve (row 3)
#include "AgentMaster/Persistence.h" // LoadAppSettings (skipPermissions, for the would-use CLI builder)
#include "AgentMaster/TranscriptTurns.h" // RenderTranscriptTurnsFile — the Copy ▸ Transcript levels (codes 9-13)
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
#include <atomic> // the process-wide prompt-copy generation (CopyPromptTextExpandingPastes — a newer copy supersedes a slow expansion)
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
        // Agentmaster (accessibility, E2E_TESTING.md §6.3): a FontIcon (or a bare arrow glyph) gives the button no
        // accessible name, so each one is NAMED (`name`) and gets the id am.badge.<key>.<element>, the session's key
        // first so a test addresses one tab's badge among many.
        //   glyph / tip - the icon and its tooltip.
        //   name        - what a screen reader says for it ("Copy session details").
        //   element     - the id's last part (up, down, folder, mail, copy, pencil).
        // Returns the button. Throws what building a XAML element throws.
        const auto mkIconBtn = [this](const wchar_t* glyph, const wchar_t* tip, std::wstring_view name, std::wstring_view element) {
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
            AgentSetAccessibleName(b, name);
            AgentSetAutomationId(b, AgentAutomationIdFor(L"am.badge.", _sessionId) + L"." + std::wstring{ element });
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
            upBtn = mkIconBtn(L"\x2191", L"Go to the previous sent prompt that's off-screen (like Alt+Up)", L"Previous prompt", L"up"); // ↑
            downBtn = mkIconBtn(L"\x2193", L"Go to the next sent prompt that's off-screen (like Alt+Down)", L"Next prompt", L"down"); // ↓
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

        Button folderBtn = mkIconBtn(L"\xE8B7", L"Open the working folder in Explorer", L"Open the working folder", L"folder"); // Folder
        folderBtn.Click([weak](const IInspectable&, const RoutedEventArgs&) {
            if (auto self = weak.get())
            {
                self->_OpenFolder();
            }
        });

        // Agentmaster (PENDING_INPUT.md §8): MAIL — queue this session's UNSENT input-box draft straight
        // into its prompt queue. The badge's twin of the Manager compose row's "Add to queue"
        // envelope, down to the SAME \xE715 glyph and the same registry append (_OnAddPrompt), so the
        // prompt you typed in the terminal but never sent can be handed to the Prompt Queue Runner
        // without being retyped in the Manager.
        //
        // Shown in EVERY build since 2026-10-09, like row 1's runner button + its ⏳N count (the runner
        // was dev-or-debug until then: an ordinary release never started the Scheduler's auto-advance,
        // so a queue button there would have appended prompts that nothing would ever send). Claude
        // only — Codex renders no ❯ rule-wrapped input box, so it never has a draft to queue (and a
        // managed codex has neither injector nor runner). Placed between the folder and copy buttons.
        Button queueBtn{ nullptr };
        if (!isCodexSession)
        {
            queueBtn = mkIconBtn(L"\xE715", // Mail — the same glyph the Manager's "Add to queue" envelope uses
                                 L"Queue this session's unsent prompt into its Prompt Queue and CLEAR it from the input box (a move).\n"
                                 L"Shift+Click to queue but KEEP it in the box (a copy).\n"
                                 L"Enabled only while the box holds an unsent draft (the pulsing 3 dots).",
                                 L"Queue the unsent prompt", L"mail");
            // Enabled ONLY while a draft actually exists, so a prominent toolbar button never fires a
            // silent no-op on an empty box (the "dead button" trap the pencil-icon comment records).
            // Seeded disabled; _Refresh -> _RefreshQueueButtonEnabled flips it in lockstep with the "3
            // dots" (both key on SessionInfo::pendingInput, whose empty<->non-empty FLIP is exactly what
            // fires the notify that drives _Refresh — so the enabled state is reliably maintained).
            queueBtn.IsEnabled(false);
            // Default (plain) click MOVES the draft: queue it, then clear it from the input box. Shift+Click
            // KEEPS it (the historical copy). Shift is read at click time via the overlay's shared
            // IsShiftDown() (the same helper the summary-panel Shift-resize uses).
            queueBtn.Click([weak](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    self->_QueueCurrentPrompt(/*clearBox*/ !IsShiftDown());
                }
            });
            _queueBtn = queueBtn; // remembered so _RefreshQueueButtonEnabled can toggle it each refresh
        }

        Button copyBtn = mkIconBtn(L"\xE8C8", L"Copy session details\x2026 (id, path, branch, current prompt, launch CLI, summary, transcript)", L"Copy session details", L"copy"); // Copy
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
        // Transcript ▸ Default / Max / Full / Compact / Minimal — the conversation as Markdown at a detail level
        // (CopySessionField codes 9-13, TranscriptTurns.h). It replaced the flat "Transcript" (5) and "Transcript
        // Followup" (8) items, and is built from the ONE shared table (AgentCopyActions.h kTranscriptCopyMenu) so
        // this menu offers exactly the levels — and the descriptions — the Manager and WT tab menus do.
        MenuFlyoutSubItem transcriptSub{};
        transcriptSub.Text(kTranscriptCopyMenuLabel);
        AgentSetTip(transcriptSub, winrt::hstring{ kTranscriptCopyMenuTip });
        // Agentmaster (transcript overlay): "Show" FIRST, then a separator, then the copy levels — it opens
        // this session's transcript in the window-level reading view (AgentTranscriptOverlay.h) through the
        // page-wired opener (inert when unwired).
        {
            MenuFlyoutItem showItem{};
            showItem.Text(kTranscriptShowMenuLabel);
            AgentSetTip(showItem, winrt::hstring{ kShowTranscriptMenuTip });
            showItem.Click([weak](const IInspectable&, const RoutedEventArgs&) {
                // Contained: a XAML event handler — an escape would be a process fail-fast for a menu click.
                try
                {
                    if (const auto self = weak.get(); self && self->_onShowTranscript)
                    {
                        self->_onShowTranscript();
                    }
                }
                catch (...)
                {
                    ::Agentmaster::AgentLogCaughtException(L"AgentTabOverlay copy menu Show Transcript");
                }
            });
            transcriptSub.Items().Append(showItem);
            transcriptSub.Items().Append(MenuFlyoutSeparator{});
        }
        for (const auto& level : kTranscriptCopyMenu)
        {
            MenuFlyoutItem item{};
            item.Text(level.label);
            AgentSetTip(item, winrt::hstring{ level.tip });
            const int which = level.which;
            item.Click([weak, which](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    self->_CopyField(which);
                }
            });
            transcriptSub.Items().Append(item);
        }
        flyout.Items().Append(transcriptSub);
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
        Button pencilBtn = mkIconBtn(L"\xE70F", L"Show/hide the session summary panel (all tabs)", L"Summary panel", L"pencil"); // Edit (pencil)
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
        if (queueBtn)
        {
            _actions.Children().Append(queueBtn); // mail: queue the unsent draft (left of copy, right of the folder)
        }
        _actions.Children().Append(copyBtn);
        _actions.Children().Append(pencilBtn);
        // ALWAYS shown (no longer hover-only). The buttons live in ROW 1 now, immediately to the RIGHT of
        // the status block (so the strip reads status -> actions -> autorunner · queue · link). The actual
        // append is done by _Refresh (it clears + rebuilds row 1 each pass), right after the status part.
    }

    void AgentTabOverlay::_OpenFolder()
    {
        // Delegate to the ONE shared open-path action (AgentCopyActions.h), which the WT tab menu's
        // "Open In Explorer" item also calls — so the overlay's folder button and the tab menu resolve
        // the SAME EffectiveWorkingDir(inferred→cwd) folder and can never disagree.
        if (_registry)
        {
            OpenSessionFolder(*_registry, _sessionId, _tabColorMode);
        }
    }

    // Agentmaster (AgentCopyActions.h): the shared "open a session's working folder in explorer.exe"
    // action — the overlay's folder button (Open Path, via _OpenFolder above) AND the WT tab menu's
    // "Open In Explorer" item both route through here, so they can never open a DIFFERENT folder.
    // Resolves EffectiveWorkingDir(tabColorMode, s) (the INFERRED dir while the session infers, else the
    // launch cwd — matching Copy Path / the overlay subline), falling back to the live PEB cwd.
    void OpenSessionFolder(SessionRegistry& registry, const std::wstring& sessionId, int tabColorMode)
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
        const std::wstring effDir = ::Agentmaster::EffectiveWorkingDir(static_cast<::Agentmaster::TabColorMode>(tabColorMode), *info);
        const std::wstring dir = !effDir.empty() ? effDir : info->liveCwd;
        if (dir.empty())
        {
            return;
        }
        // Nav audit: the user asked to open the session's working dir in explorer.exe (the overlay's
        // folder button OR the tab menu's "Open In Explorer" — same [nav] tag, same action).
        ::Agentmaster::LogNav(L"open-path " + ::Agentmaster::ShortId(sessionId) + L" dir=" + dir);
        OpenPathInExplorerAsync(dir);
        PlayActionSound(); // same click feedback as the copy menu (Open Path)
    }

    namespace
    {
        // Agentmaster (PENDING_INPUT.md §8e): the process-wide copy GENERATION. Every call into the shared
        // copy actions (CopySessionField, CopyPromptTextExpandingPastes) takes the next number, and an
        // off-thread paste expansion writes the clipboard ONLY if its number is still the newest when it
        // lands back on the UI thread. Without it, copying a paste-bearing prompt (the expansion reads the
        // whole paste cache — ~100 ms over a real one) and then anything else would leave the OLDER text on
        // the clipboard, because the slow write lands last. Process-wide rather than per-window because the
        // clipboard is; atomic because the count is bumped by whichever window's UI thread copies while the
        // check runs on the expanding copy's own (the Emperor runs every window on one thread today —
        // nothing here depends on that).
        std::atomic<uint64_t> g_promptCopyGeneration{ 0 };

        // How an off-thread prompt copy resolved. Decided on the worker thread, rendered into the trace on
        // the UI thread — so the worker's catch handler never has to build (allocate) a message itself.
        enum class PromptCopyOutcome
        {
            Expanded, // every placeholder proven + substituted: the pasted content is what gets copied
            Refused, // the all-or-refuse resolver declined (no/ambiguous cache match, typed text around a collapsed label)
            Failed, // the expansion threw (logged with its throw stack) — copied as rendered
        };

        // The UI-thread tail of an expanding prompt copy: honor the generation, write the clipboard (this
        // TU's CopyTextToClipboard — the retry-looped Win32 writer + the confirmation chime on a real
        // success), then trace how the copy resolved.
        //   text          — what to copy: the expansion when `outcome` is Expanded, else the as-rendered original.
        //   sessionId     — the owning session, for the trace only.
        //   what          — the trace noun ("auto-testing prompt" / "current prompt").
        //   outcome       — how the worker resolved (see PromptCopyOutcome).
        //   expandedCount — placeholders substituted (meaningful for Expanded only).
        //   renderedChars — the as-rendered text's length, for the chars=A->B trace.
        //   generation    — this copy's number; if a newer copy started meanwhile, NOTHING is written.
        // Throws std::bad_alloc only (building the trace line); the caller's lambda catches + logs it.
        void CommitPromptCopy(const std::wstring& text,
                              const std::wstring& sessionId,
                              const std::wstring& what,
                              PromptCopyOutcome outcome,
                              int expandedCount,
                              size_t renderedChars,
                              uint64_t generation)
        {
            const std::wstring head = L"[pending] " + ::Agentmaster::ShortId(sessionId) + L" copy " + what + L": ";
            if (generation != g_promptCopyGeneration.load())
            {
                // The user copied something else while the paste cache was being read: the NEWER copy owns
                // the clipboard, so this stale result is dropped instead of being written over it.
                ::Agentmaster::AppendStateLog(L"hooks.log", head + L"superseded by a newer copy before the paste expansion finished (not written)\n");
                return;
            }
            CopyTextToClipboard(text);
            std::wstring line = head;
            switch (outcome)
            {
            case PromptCopyOutcome::Expanded:
                line += L"expanded " + std::to_wstring(expandedCount) + L" paste placeholder(s) from the paste journal/paste-cache chars=" +
                        std::to_wstring(renderedChars) + L"->" + std::to_wstring(text.size());
                break;
            case PromptCopyOutcome::Refused:
                // Honest, not an error: the resolver would rather hand over the label than risk the WRONG
                // text (a paste the journal never saw + no unique cache file, an [Image #N], a duplicate id).
                line += L"paste placeholder(s) NOT expanded (no proven paste-journal/paste-cache match) - copied as rendered chars=" + std::to_wstring(text.size());
                break;
            case PromptCopyOutcome::Failed:
            default:
                line += L"paste expansion threw (see the [exc] line) - copied as rendered chars=" + std::to_wstring(text.size());
                break;
            }
            ::Agentmaster::AppendStateLog(L"hooks.log", line + L"\n");
        }

        // The off-thread half of CopyPromptTextExpandingPastes (AgentCopyActions.h): expand the text's paste
        // placeholders against Claude's paste cache on a pool thread, then hop back to `disp` to write the
        // clipboard via CommitPromptCopy. Parameters are BY VALUE on purpose — the coroutine outlives the
        // caller's frame (the menu Click handler has long returned when the expansion finishes).
        //   disp       — the calling window's UI-thread DispatcherQueue (the clipboard is written there).
        //   rendered   — the prompt exactly as stored/rendered; also the fallback copy on any non-Expanded outcome.
        //   sessionId  — for the trace.
        //   what       — the trace noun.
        //   generation — the number CopyPromptTextExpandingPastes took for this copy.
        //   journal    — the session's PASTE JOURNAL snapshot (PENDING_INPUT.md §2c; empty ⇒ the paste-cache tier alone).
        // Returns nothing (fire_and_forget). Never lets an exception escape: one escaping a fire_and_forget
        // IS winrt::terminate() — the whole process, every window — so the expansion and the marshal each
        // sit in their own try and log through AgentLogCaughtException (Rule #18).
        winrt::fire_and_forget CopyPromptExpandingPastesAsync(DispatcherQueue disp,
                                                              std::wstring rendered,
                                                              std::wstring sessionId,
                                                              std::wstring what,
                                                              uint64_t generation,
                                                              std::vector<::Agentmaster::PasteJournalEntry> journal)
        {
            std::wstring expanded;
            PromptCopyOutcome outcome = PromptCopyOutcome::Refused;
            int expandedCount = 0;
            try
            {
                co_await winrt::resume_background(); // the paste-cache read (up to 64 MiB) must never run on the UI thread
                // Fold '\r' / "\r\n" to '\n' FIRST: the resolver's line model is '\n'-split, while a prompt
                // queued from the Manager's compose box carries '\r' newlines (the UWP TextBox quirk) — under
                // which a placeholder standing alone on its own line would read as mid-line and be refused.
                // §2c: the journal answers first (an unsent paste exists nowhere else); the cache is read only
                // when it can't. LIVE policy — an unbacked MID-LINE label is a quote and stays as written.
                const auto plan = ::Agentmaster::ResolvePendingDraftCarry(::Agentmaster::FoldCrToLf(rendered), journal, /*unresolvedMidLineIsLiteral*/ true);
                const int blocks = plan.fromJournal + plan.fromCache;
                // blocks > 0 as well as complete: "complete with nothing substituted" means every placeholder
                // was a kept quote — then the original (unfolded) text is the faithful copy, not the folded one.
                if (plan.complete && blocks > 0)
                {
                    if (auto full = ::Agentmaster::DraftCarryExpandedText(plan); !full.empty())
                    {
                        expanded = std::move(full);
                        expandedCount = blocks;
                        outcome = PromptCopyOutcome::Expanded;
                    }
                }
            }
            catch (...)
            {
                // Rule #18: never lose a swallowed exception. Recovery = the as-rendered copy below.
                ::Agentmaster::AgentLogCaughtException(L"CopyPromptTextExpandingPastes expand");
                outcome = PromptCopyOutcome::Failed;
            }
            try
            {
                const size_t renderedChars = rendered.size();
                // The expansion when every placeholder was proven, else the as-rendered original — so every
                // outcome short of a closed window still copies SOMETHING. Moved, not copied: a prompt can
                // be tens of KB and neither local is read again.
                std::wstring payload = (outcome == PromptCopyOutcome::Expanded) ? std::move(expanded) : std::move(rendered);
                const bool queued = disp && disp.TryEnqueue([toCopy = std::move(payload), sessionId, what, outcome, expandedCount, renderedChars, generation]() {
                    // A throw escaping a DispatcherQueue handler is reported against the UI thread; contain
                    // it here so a failed trace line can never cost more than this one copy.
                    try
                    {
                        CommitPromptCopy(toCopy, sessionId, what, outcome, expandedCount, renderedChars, generation);
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"CopyPromptTextExpandingPastes commit");
                    }
                });
                if (!queued)
                {
                    // TryEnqueue refuses once the window's dispatcher is shutting down: the window closed while
                    // the paste cache was being read, so there is no UI thread left to write the clipboard from.
                    ::Agentmaster::AppendStateLog(L"hooks.log", L"[pending] " + ::Agentmaster::ShortId(sessionId) + L" copy " + what +
                                                                    L": the window closed before the paste expansion finished (nothing copied)\n");
                }
            }
            catch (...)
            {
                ::Agentmaster::AgentLogCaughtException(L"CopyPromptTextExpandingPastes marshal");
            }
        }

        // The off-thread half of the Copy ▸ Transcript levels (CopySessionField codes 9-13, AgentCopyActions.h):
        // render the session's conversation at one detail level on a pool thread, then hop back to `disp` to write
        // the clipboard. A Claude session renders through TranscriptTurns (RenderTranscriptTurnsFile — the turns
        // view the am-agentmaster-cli skill prints, with the sessions it was forked / plan-restarted from and the
        // subagents it spawned, times in local time). A Codex rollout has no turns view, so its levels fall back
        // to the plain-text readers: Minimal and Compact copy the per-turn brief (ReadConversationFollowupText —
        // the user's messages with each final reply, which is what Minimal means), Default / Full / Max the
        // whole conversation text (ReadConversationText). Parameters are BY VALUE: the coroutine outlives the
        // menu Click that started it.
        //   disp       — the calling window's UI-thread DispatcherQueue (the clipboard is written there).
        //   codex      — the session runs Codex (read its rollout, not a Claude transcript).
        //   sessionId  — the managed session id: a Claude session's transcript id; for Codex only the trace's tag.
        //   codexId    — the Codex rollout uuid (empty until its first prompt; unused for Claude).
        //   forkParentId — Claude only (empty otherwise): the session's fork source. A fork with no messages of
        //                its own yet has no transcript, so its source's is rendered under the note the transcript
        //                overlay shows above it (ResolveClaudeDisplayTranscript / ForkSourceTranscriptNote) — the
        //                overlay's "Show" and this copy stay the same text.
        //   level      — the TurnsLevel value 0-4 (the code minus kCopyFieldTranscriptLevelBase).
        //   generation — the copy generation CopySessionField took: a newer copy (of anything) started while
        //                this one rendered supersedes it, so a slow render never lands on top of a later copy.
        // Returns nothing (fire_and_forget). Never lets an exception escape — one escaping a fire_and_forget IS
        // winrt::terminate(), the whole process — and every outcome leaves a [transcript] line in hooks.log
        // (the copy, how big and how slow; nothing to copy; superseded; the clipboard refused the write; the
        // window closed first).
        winrt::fire_and_forget CopyTranscriptTurnsAsync(DispatcherQueue disp,
                                                        bool codex,
                                                        std::wstring sessionId,
                                                        std::wstring codexId,
                                                        std::wstring forkParentId,
                                                        int level,
                                                        uint64_t generation)
        {
            try
            {
                const auto turnsLevel = static_cast<::Agentmaster::TurnsLevel>(level);
                const std::wstring head = L"[transcript] " + ::Agentmaster::ShortId(sessionId) + L" copy " + ::Agentmaster::TurnsLevelName(turnsLevel) + L": ";
                co_await winrt::resume_background(); // reads the transcript, its chain and its agents' transcripts: never on the UI thread
                const auto started = std::chrono::steady_clock::now();
                std::wstring path;
                std::wstring forkSourceId; // set when `path` is the fork SOURCE's transcript (this fork has none yet)
                if (codex)
                {
                    if (!codexId.empty())
                    {
                        path = ::Agentmaster::ResolveCodexRolloutPathIn(::Agentmaster::CodexDefaultHome(), codexId);
                    }
                }
                else
                {
                    auto display = ::Agentmaster::ResolveClaudeDisplayTranscript(sessionId, forkParentId);
                    path = std::move(display.path);
                    forkSourceId = std::move(display.forkSourceId);
                }
                if (path.empty())
                {
                    ::Agentmaster::AppendStateLog(L"hooks.log", head + L"nothing to copy (no transcript yet - the session was never prompted)\n");
                    co_return;
                }
                std::wstring text;
                if (codex)
                {
                    const bool brief = turnsLevel == ::Agentmaster::TurnsLevel::Minimal || turnsLevel == ::Agentmaster::TurnsLevel::Compact;
                    text = brief ? ::Agentmaster::ReadConversationFollowupText(path, true, 0 /* whole file */) :
                                   ::Agentmaster::ReadConversationText(path, true, 0 /* whole file */);
                }
                else
                {
                    text = ::Agentmaster::RenderTranscriptTurnsFile(path, ::Agentmaster::TurnsOptionsForLevel(turnsLevel), /*utc*/ false).value_or(std::wstring{});
                }
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                if (text.empty())
                {
                    // An unreadable transcript, a render failure (already in hooks.log through Rule #18), or a
                    // Codex rollout with no visible message: say so instead of a silent dead click.
                    ::Agentmaster::AppendStateLog(L"hooks.log", head + L"nothing copied (the transcript could not be read or rendered) path=" + path + L"\n");
                    co_return;
                }
                if (!forkSourceId.empty())
                {
                    // The same note the transcript overlay shows above a fork source's turns: the pasted text
                    // says whose conversation it is, and "Show" and this copy stay one text.
                    text.insert(0, ::Agentmaster::ForkSourceTranscriptNote(forkSourceId));
                }
                const size_t chars = text.size();
                // The trace names the fork source when the copied turns are its (this fork has no messages yet).
                const std::wstring sourceNote = forkSourceId.empty() ? std::wstring{} :
                                                                      L" from the fork source " + ::Agentmaster::ShortId(forkSourceId) + L"'s transcript";
                const bool queued = disp && disp.TryEnqueue([text = std::move(text), head, generation, chars, elapsedMs, sourceNote]() {
                    // A throw escaping a DispatcherQueue handler is reported against the UI thread; contain it so a
                    // failed trace line can never cost more than this one copy.
                    try
                    {
                        if (generation != g_promptCopyGeneration.load())
                        {
                            // The user copied something else while this rendered: the NEWER copy owns the clipboard.
                            ::Agentmaster::AppendStateLog(L"hooks.log", head + L"superseded by a newer copy before the render finished (not written)\n");
                            return;
                        }
                        // The robust writer itself rather than CopyTextToClipboard (which is it + the chime), so this
                        // trace can say whether the copy LANDED: a clipboard another program holds past the writer's
                        // ~10 s retry budget fails the write (the writer logs that as a [wil] line), and a "chars=N"
                        // line then would claim a copy that never happened.
                        if (RobustCopyTextToClipboard(text))
                        {
                            PlayActionSound(); // the same "copy is done" chime every other copy plays
                            ::Agentmaster::AppendStateLog(L"hooks.log", head + L"chars=" + std::to_wstring(chars) + L" rendered in " + std::to_wstring(elapsedMs) + L"ms" + sourceNote + L"\n");
                        }
                        else
                        {
                            ::Agentmaster::AppendStateLog(L"hooks.log", head + L"rendered chars=" + std::to_wstring(chars) + L" in " + std::to_wstring(elapsedMs) +
                                                                            L"ms, but the clipboard could not be opened (nothing copied)\n");
                        }
                    }
                    catch (...)
                    {
                        ::Agentmaster::AgentLogCaughtException(L"CopyTranscriptTurnsAsync commit");
                    }
                });
                if (!queued)
                {
                    // No dispatcher, or it refused: the window closed while the transcript was rendering.
                    ::Agentmaster::AppendStateLog(L"hooks.log", head + L"the window closed before the render finished (nothing copied)\n");
                }
            }
            catch (...)
            {
                // Rule #18: never lose a swallowed exception. Recovery = no copy (the click simply didn't chime).
                ::Agentmaster::AgentLogCaughtException(L"CopyTranscriptTurnsAsync");
            }
        }
    }

    // Agentmaster (PENDING_INPUT.md §8e): the shared "copy a PROMPT" action (declared + fully documented in
    // AgentCopyActions.h) — the Auto-Testing row menu's Copy and every Copy Current Prompt (CopySessionField
    // code 7) route through here, so a paste placeholder is copied as the pasted text itself everywhere.
    // Placeholder-free text keeps the old synchronous copy; a placeholder sends the copy through
    // CopyPromptExpandingPastesAsync (off-thread expansion, UI-thread write). The function-try is the
    // "never throws" contract: a copy is best-effort UI feedback and must never escape into a Click handler.
    void CopyPromptTextExpandingPastes(const DispatcherQueue& dispatcher, const std::wstring& text, const std::wstring& sessionId, const std::wstring& what, bool expandPastes, const ::Agentmaster::SessionRegistry* registry)
    try
    {
        if (text.empty())
        {
            return; // nothing to copy — no clipboard write, no chime (the callers' honest no-op)
        }
        // This copy is now the user's newest copy intent: any older prompt copy still expanding is superseded.
        const uint64_t generation = ++g_promptCopyGeneration;
        // The cheap, pure pre-check — the common case has no placeholder at all and costs one scan, no cache
        // IO and no thread hop: copied synchronously, byte-for-byte as before. FindDraftPlaceholders inspects
        // every '[' whatever the line endings (and knows claude's bare "[Pasted text #N]" form too), so the
        // UNFOLDED text is fine for detection; only the resolve needs the fold (CopyPromptExpandingPastesAsync).
        const bool hasPlaceholder = expandPastes && !::Agentmaster::FindDraftPlaceholders(text).empty();
        if (!hasPlaceholder)
        {
            CopyTextToClipboard(text);
            return;
        }
        if (!dispatcher)
        {
            // No UI thread to hop back to (never the case for the shipped callers, which all pass their
            // window's queue): expanding here would read the paste cache ON the calling UI thread — the very
            // stall the off-thread path exists to avoid — so copy as rendered, and say so.
            CopyTextToClipboard(text);
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[pending] " + ::Agentmaster::ShortId(sessionId) + L" copy " + what +
                                                            L": no dispatcher - paste placeholder(s) copied as rendered chars=" + std::to_wstring(text.size()) + L"\n");
            return;
        }
        // §2c: snapshot the session's paste journal HERE (the registry is thread-safe, but the coroutine must
        // not hold a pointer that could outlive its owner) — it travels by value into the off-thread resolve.
        CopyPromptExpandingPastesAsync(dispatcher, text, sessionId, what, generation, registry ? registry->PasteJournal(sessionId) : std::vector<::Agentmaster::PasteJournalEntry>{});
    }
    catch (...)
    {
        // Rule #18: never lose a swallowed exception. Recovery = no copy (the click simply didn't chime).
        ::Agentmaster::AgentLogCaughtException(L"CopyPromptTextExpandingPastes");
    }

    // Agentmaster: the shared copy-field action (declared in AgentCopyActions.h) — the SINGLE
    // implementation behind both this overlay's copy menu (_CopyField, below) and the Triage Board /
    // Explorer-tree session menu's Copy submenu (AgentManagerContent::_MakeSessionMenu). It lives here
    // because the launch-CLI / transcript / summary helpers it leans on are this file's anon-namespace
    // helpers; both menus call it so they can never drift apart. `which` is the copy-menu code (see the
    // header). Synchronous clipboard writes (cases 0-4, and 7 when the draft has no paste placeholder) run
    // on the calling UI thread; the transcript / summary cases (5/6/8, and the Copy ▸ Transcript levels 9-13 —
    // CopyTranscriptTurnsAsync) read off-thread and hop back via
    // `dispatcher`, as does case 7 when a placeholder must first be expanded from the paste cache
    // (CopyPromptTextExpandingPastes — PENDING_INPUT.md §8e). `liveDraft` is the optional live input-box
    // reader used by case 7 alone (see the header) — absent/failing degrades to the observer's remembered
    // draft, never to nothing.
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
        // Indexed by code: 0-8 the classic fields, 9-13 the Copy ▸ Transcript levels (kCopyFieldTranscriptLevelBase
        // + the TurnsLevel value), so `grep '\[nav\] copy transcript-'` shows which level a user picked.
        static const wchar_t* const kCopyFieldNames[] = { L"session-id", L"path", L"branch", L"claude-cli", L"codex-cli", L"transcript", L"summary", L"current-prompt", L"transcript-followup",
                                                          L"transcript-minimal", L"transcript-compact", L"transcript-default", L"transcript-full", L"transcript-max" };
        constexpr int kCopyFieldNameCount = static_cast<int>(sizeof(kCopyFieldNames) / sizeof(kCopyFieldNames[0]));
        ::Agentmaster::LogNav(std::wstring{ L"copy " } + ((which >= 0 && which < kCopyFieldNameCount) ? kCopyFieldNames[which] : L"?") + L" " + ::Agentmaster::ShortId(sessionId));
        // This pick is the user's NEWEST copy intent: a prompt copy whose paste expansion is still reading
        // the cache (CopyPromptTextExpandingPastes) must not land on top of it, so take a new generation —
        // the stale expansion then drops its write when it comes back (PENDING_INPUT.md §8e).
        ++g_promptCopyGeneration;
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
            // The fork link (Claude only): a fork with no messages yet copies the conversation it was forked from.
            CopyConversationAsync(dispatcher, codex, s.id, s.codexSessionId, codex ? std::wstring{} : s.forkParentId);
            break;
        case 8: // Transcript Followup — the per-turn ❯ user + ● final-reply brief (legend header), off-thread
            CopyConversationAsync(dispatcher, codex, s.id, s.codexSessionId, codex ? std::wstring{} : s.forkParentId, /*followup*/ true);
            break;
        case kCopyFieldTranscriptLevelBase + 0: // Transcript ▸ Minimal
        case kCopyFieldTranscriptLevelBase + 1: // Transcript ▸ Compact
        case kCopyFieldTranscriptLevelBase + 2: // Transcript ▸ Default
        case kCopyFieldTranscriptLevelBase + 3: // Transcript ▸ Full
        case kCopyFieldTranscriptLevelBase + 4: // Transcript ▸ Max
            // The conversation as Markdown at that detail level (TranscriptTurns.h), rendered off-thread; this
            // copy's generation (taken just above) lets a later copy supersede a slow render. The fork link
            // (Claude only): a fork with no messages yet copies the conversation it was forked from, under the
            // overlay's note.
            CopyTranscriptTurnsAsync(dispatcher, codex, s.id, s.codexSessionId, codex ? std::wstring{} : s.forkParentId, which - kCopyFieldTranscriptLevelBase, g_promptCopyGeneration.load());
            break;
        case 6: // Summary — the FULL textual session-end.js box (everything; the panel shows a trimmed view)
        {
            const std::wstring dir = !s.workingDir.empty() ? s.workingDir : s.liveCwd;
            const std::wstring resume = BuildLaunchCli(s, codex); // the same REAL launch CLI as cases 3/4
            // The process-wide registry (by shared_ptr: the copy runs off-thread) carries the /clear and
            // /handover links + titles the copied whole conversation follows (SessionInfo::continuedFromId).
            // The fork link (Claude only): a fork with no messages yet summarizes the conversation it was
            // forked from — what its summary panel shows.
            CopySummaryAsync(dispatcher, codex, s.id, s.codexSessionId, codex ? std::wstring{} : s.forkParentId, dir, resume,
                             std::wstring{ StateGlyph(s.state) }, std::wstring{ StateLabel(s.state) }, wrapNewlines, truncate,
                             ::Agentmaster::SharedEngine().registry);
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
            // Whole (multi-line drafts included) through the shared PROMPT copy (PENDING_INPUT.md §8e): a draft
            // holding Claude's "[Pasted text #N +M lines]" / "[...Truncated text #N +M lines...]" label is
            // copied as the pasted text itself — resolved off-thread from this session's PASTE JOURNAL (§2c —
            // an unsent paste exists nowhere else) else the paste cache, AS RENDERED on any refusal — while a
            // placeholder-free draft (the common case) is still copied synchronously and verbatim. Chimes on
            // success either way.
            CopyPromptTextExpandingPastes(dispatcher, pick.text, sessionId, L"current prompt", /*expandPastes*/ true, &registry);
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

    // Agentmaster (PENDING_INPUT.md §8): the row-1 MAIL button — take this session's UNSENT input-box
    // draft and QUEUE it into that session's own Auto-Testing queue, so a prompt typed in the terminal
    // but never sent reaches the Tests Autorunner without being retyped in the Manager.
    //
    // It appends through the SAME registry seam as the Manager compose row's envelope
    // (AgentManagerContent::_OnAddPrompt): one Update -> a default QueuedPrompt (Pending /
    // OnTurnComplete) pushed onto s.queue. The scheduler therefore cannot tell the two apart, and the
    // entry shows in Auto Testing / row 1's ⏳N / row 3's next-prompt preview like any other.
    //
    // The DRAFT is resolved by the SAME two-source rule as "Copy Current Prompt" (CopySessionField case
    // 7) — a wrapped LIVE read of the box off this tab's buffer wins, else the observer's recorded
    // SessionInfo::pendingInput — through the one pure PickCurrentPromptText, so the button, the copy
    // menu and the compose-box pull can never disagree about what "the current prompt" is. Here the live
    // read is the one that normally answers: this overlay sits INSIDE the session's own pane, so its
    // window always hosts the tab (the Manager's copy of this decision is the one that routinely falls
    // back, because the board spans windows).
    //
    // It is a COPY, not a move (Rule #13 — reading a buffer never writes to it): the draft stays in the
    // terminal's input box exactly as typed and its "3 dots" keep pulsing until the user sends or clears
    // it there. Sending the queued copy later cannot eat that draft either — the DRAFT SWAP (§9) takes it
    // out of the way around every submit and puts it back.
    //
    // NO one-shot latch, deliberately: §8a's pull into the compose box is a SILENT insert on focus (hence
    // its per-(session, draft) latch against double-queueing), while every append here is an explicit
    // click — so a second click queues a second copy, exactly like clicking the Manager's envelope twice.
    //
    // clearBox (a PLAIN click) then REMOVES the draft from Claude's input box after queueing — the draft
    // is MOVED to the queue (PENDING_INPUT.md §8d), via the page's verified clearer. Shift+Click passes
    // false and KEEPS it in the box (the historical copy, Rule #13). The clear runs only AFTER a
    // successful queue, so a failed/empty queue never touches the box.
    void AgentTabOverlay::_QueueCurrentPrompt(bool clearBox)
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
        if (info->kind == AgentKind::Codex)
        {
            return; // backstop: Codex has no U+276F input box to read (the button isn't built for it either)
        }
        // (1) The LIVE read, WRAPPED — the same contract as CopySessionField case 7: a control torn down
        // mid-click, a not-yet-initialized buffer or a dormant restored tab all read as "" and the
        // remembered draft answers instead. A failure must never cost the user the queue.
        std::wstring live;
        if (_onReadLiveDraft)
        {
            try
            {
                live = _onReadLiveDraft();
            }
            catch (...)
            {
                // Rule #18: never lose a swallowed exception. Recovery is unchanged (fall back to the
                // remembered draft) — this only records what threw + where.
                ::Agentmaster::AgentLogCaughtException(L"AgentTabOverlay::_QueueCurrentPrompt live draft read");
            }
        }
        // (2) The pick: a non-empty live read wins, else the observer's recorded draft (possibly the
        // persisted memory of a session that is not running). ONE pure rule (PendingInput.h).
        const auto pick = ::Agentmaster::PickCurrentPromptText(live, info->pendingInput);
        if (pick.text.empty())
        {
            // Nothing typed anywhere: queue nothing, chime nothing — and log it, so a "why did nothing
            // happen?" is answerable from hooks.log instead of being a silent dead click (the same honest
            // no-op the copy item makes).
            ::Agentmaster::AppendStateLog(L"hooks.log", L"[pending] " + ::Agentmaster::ShortId(_sessionId) + L" queue current prompt: nothing (box empty, no remembered draft)\n");
            return;
        }
        // The short display label, mirroring _OnAddPrompt (first 56 chars, newlines flattened) so a prompt
        // queued from here is indistinguishable in the Auto-Testing list from one queued in the Manager.
        std::wstring label = pick.text.substr(0, 56);
        std::replace(label.begin(), label.end(), L'\n', L' ');
        std::replace(label.begin(), label.end(), L'\r', L' ');
        _registry->Update(_sessionId, [&](SessionInfo& s) {
            QueuedPrompt p;
            p.id = NewSessionId();
            p.label = label;
            p.text = pick.text; // verbatim + whole (multi-line drafts included) — this is what gets SENT
            // Agentmaster (DELIVERY.md §8 — the question-guard trap, closed for this surface): this
            // queues the session's OWN unsent input-box draft — the message the user typed TO this
            // session and was one Enter away from sending. When the agent's last turn ended on a
            // clarifying question, that draft IS (by construction) the user's next message to it, so
            // it carries the answers-a-question opt-out and never parks behind lastMessageWasQuestion
            // the way a PLANNED test prompt (the guard's real subject) must. The reported trap
            // otherwise: mail-move the answer -> the box is cleared -> the queue holds it
            // indefinitely -> "3 queued messages but none are auto-sending".
            p.guardPattern = std::wstring{ ::Agentmaster::kAnswersQuestionOk };
            s.queue.push_back(std::move(p));
        });
        // Nav audit: the SAME "queue" verb the Manager's envelope logs, tagged with this surface (the
        // overlay's Autorunner cycle already carries "(overlay)" the same way), with WHERE the text came
        // from (the user never typed it into a compose box) and whether the box is being cleared or kept.
        ::Agentmaster::LogNav(L"queue " + ::Agentmaster::ShortId(_sessionId) + L" \"" + label + L"\" (overlay draft, " + (clearBox ? L"clear box" : L"keep box") + L")");
        // ...and the [pending] mechanism line naming WHICH source answered, beside the copy item's.
        ::Agentmaster::AppendStateLog(L"hooks.log",
                                      L"[pending] " + ::Agentmaster::ShortId(_sessionId) + L" queue current prompt: " +
                                          (pick.fromLive ? L"live" : L"remembered") + L" chars=" + std::to_wstring(pick.text.size()) + L"\n");
        PlayActionSound(); // same click feedback as the copy menu / Open Path
        // A PLAIN click MOVES the draft: now that it is safely queued, remove it from the input box (the
        // page runs the verified clear — a no-op if the box isn't readable here / is already empty). A
        // Shift+Click (clearBox == false) KEEPS it, the historical copy. Done AFTER the queue append, so a
        // clear can never run without the prompt being safely captured first.
        if (clearBox && _onClearDraft)
        {
            _onClearDraft();
        }
        _Refresh(); // immediate repaint of row 1's ⏳N + row 3's next-prompt preview (the registry observer also refreshes, async)
    }

    // Agentmaster (PENDING_INPUT.md §8): enable the row-1 MAIL button ONLY while an unsent draft exists,
    // so a prominent, always-present toolbar button can never fire a silent no-op on an empty box (the
    // "dead button" trap the pencil-icon comment records — a click that changes nothing visible reads as
    // broken). Called from _Refresh with the live snapshot. It keys on the SAME signal as the "3 dots"
    // indicator (SessionInfo::pendingInput), so the button is clickable EXACTLY when the dots are showing
    // — one legible affordance. That signal is reliable here: SetPendingInput fires its notify (which
    // drives _Refresh) on precisely the empty<->non-empty FLIP this predicate turns on, so the button can
    // never latch stale in either the "appeared" or "cleared" direction. (A text-only draft edit does NOT
    // notify, but it also can't change this boolean, so nothing is missed.) The click still does the
    // authoritative live-else-remembered read via PickCurrentPromptText; this only governs the affordance.
    // No-op when the button wasn't built (a release / Codex badge — _queueBtn stays null).
    void AgentTabOverlay::_RefreshQueueButtonEnabled(const SessionInfo& s)
    {
        if (!_queueBtn)
        {
            return;
        }
        _queueBtn.IsEnabled(!::Agentmaster::pending_detail::AllWhitespace(s.pendingInput));
    }

}
