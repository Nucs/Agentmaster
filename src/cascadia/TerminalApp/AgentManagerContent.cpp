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
// ★ AgentManagerContent.cpp             - CORE: ctor/dtor, the Set* wiring, IPaneContent, the per-window lens, _BuildLayout, _Refresh
//   AgentManagerContent.Internal.h      - the ~48 shared file-local helpers: StateColor/Pill/StateDot/Text/Fill + path/sort utils (anonymous namespace, a per-TU copy)
//   AgentManagerContent.Board.cpp       - the Triage Board: cards, columns, splitters, _RebuildBoard
//   AgentManagerContent.Tree.cpp        - the Explorer Tree: managed/external trees, context menus, scope/sort toggles, rename, confirm dialogs
//   AgentManagerContent.Settings.cpp    - keep-awake/reopen/activate buttons + the Settings cog overlay (tabs, save, env editor, UPDATES, claude-missing)
//   AgentManagerContent.AutoTesting.cpp  - the Auto Testing: plan + selection sync, prompt compose/history, Autorunner, the Summary tab, templates
//   AgentManagerContent.Launch.cpp      - the Launch bar: cwd validation, the Claude/Codex toggle, launch/create/fork, the path-picker drop-down
// ======================================================================================
//
// Agentmaster Manager tab content (C1 "Linked Lenses"). CORE: ctor/dtor, the Set* wiring, IPaneContent, the per-window lens, _BuildLayout, _Refresh. The board/tree/auto-testing/settings/launch live in sibling AgentManagerContent.*.cpp TUs (the TerminalPage.Agent*.cpp pattern).
#include "pch.h"
#include "AgentManagerContent.h"

#include "AgentTipHelpers.h" // AgentSetTip — hover tooltips with working dismissal (XAML Islands)
#include "AgentCopyActions.h" // CopySessionField — the shared copy-menu action (same path as the per-tab overlay's copy button)
#include "AgentStatusColors.h" // ParseArgbHexColor / FormatArgbHexColor — the cog's "status flashing color" picker <-> AppSettings::flashRingColor
#include "AgentMaster/ClaudeSpawn.h" // NewSessionId (prompt ids)
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
#include "AgentManagerContent.Internal.h" // the shared file-local helpers (StateColor/Pill/Text/...)

namespace winrt::TerminalApp::implementation
{
    AgentManagerContent::AgentManagerContent()
    {
        _root = Grid{};
        // Agentmaster: the Manager pane is ALWAYS dark, regardless of the Windows / Windows Terminal
        // theme (a hard product requirement). RequestedTheme(Dark) forces every built-in control in
        // this subtree (buttons, text boxes, scrollbars, combo lists, …) to render dark. Surfaces that
        // render OUTSIDE this subtree's visual root — the Popup-hosted path picker and the modal
        // overlay cards — re-assert Dark on themselves, and the confirm ContentDialogs follow
        // _root.ActualTheme() (now Dark). WT's theme passes only push brush *resources* into panes
        // (rootPane->UpdateResources) and never set RequestedTheme on pane content, so this sticks
        // across light/dark theme switches.
        _root.RequestedTheme(ElementTheme::Dark);
        _dispatcher = DispatcherQueue::GetForCurrentThread();
        _templates = ::Agentmaster::LoadTemplates(); // persisted plan templates (M8)
        _recentDirs = ::Agentmaster::LoadRecentDirs(); // MRU for the Launch path-picker
        _layout = ::Agentmaster::LoadLayout(); // persisted splitter geometry (pane sizes)

        // Agentmaster: paint the Manager pane an OPAQUE background. This is LOAD-BEARING for input,
        // not cosmetics. IPaneContent::BackgroundBrush() returns _root.Background(), and the WT pane
        // root is left transparent on purpose (Pane.cpp — so vintage/acrylic opacity shows through),
        // so the CONTENT must paint its own fill. A Grid with a NULL Background is not hit-test-visible,
        // so a click on the gaps between widgets (the tip of a text run, a thin border, any empty spot)
        // falls THROUGH the transparent pane -> the XAML island -> NonClientIslandWindow::_OnNcHitTest,
        // which returns HTCAPTION for any client point the island doesn't consume: the window then DRAGS
        // on left-drag and pops the system/caption menu on right-click, exactly as if you'd grabbed the
        // title bar (the user-reported "the tabs bar moves / its context menu opens" bug).
        //
        // The fill is the DARK TabViewBackground (#2e2e2e) UNCONDITIONALLY. We force this pane to dark
        // (RequestedTheme above), so the gaps must be dark too. This used to look the brush up via
        // Application.Resources().Lookup(L"UnfocusedBorderBrush"), but that resolves the resource
        // against the *application* theme — which returns the LIGHT value (#e8e8e8) whenever Windows /
        // Windows Terminal is in light mode, bleeding a light-gray rectangle through the widget gaps.
        // App.xaml defines the dark UnfocusedBorderBrush == TabViewBackground == #2e2e2e, so hardcoding
        // it here is exactly the theme-correct dark value with no light-mode bleed. (Must stay non-null /
        // opaque for the hit-testing reason above.)
        _root.Background(Fill(0xFF, 0x2E, 0x2E, 0x2E)); // opaque #2e2e2e == TabViewBackground (dark)

        // Agentmaster: explicit Button STATE brushes for the WHOLE Manager subtree. SAME root cause as the
        // opaque _root fill above — a NULL background brush is NOT hit-test-visible. Under the forced-Dark
        // island theme (RequestedTheme above), the default {ThemeResource ButtonBackground} / *PointerOver /
        // *Pressed brushes do NOT resolve here (they come back null), so a plain Button's BODY isn't
        // hit-testable: hover + clicks only land on its text CONTENT, never the button's padding — the
        // "buttons don't click or hover; it hits the label/text instead" report. The board CARDS dodge this
        // only because each sets an explicit Fill() Background (and the keep-awake/scope buttons because
        // PaintHoldButton seeds these very keys per-button). Seed them ONCE at _root scope so every default
        // Button under the Manager (the toolbar cog / Pause / Sessions / Keep-Awake, the launch + header
        // toggles, the Auto-Testing compose buttons, the settings-overlay buttons, …) is hit-testable across
        // its whole body and shows a real hover/press. A near-invisible rest fill (alpha 0x01 — the
        // "~invisible yet hit-testable" value used elsewhere here) keeps the flat look; hover/press lift.
        // A button with its OWN Background/Resources (cards, PaintHoldButton) overrides these locally. This
        // is the global twin of PaintHoldButton (which proved per-button Resources resolve in this subtree).
        // Inserted BEFORE _BuildLayout so the buttons it creates resolve these on first style-apply.
        {
            auto br = _root.Resources();
            br.Insert(winrt::box_value(L"ButtonBackground"), Fill(0x01, 0xFF, 0xFF, 0xFF)); // flat rest, hit-testable
            br.Insert(winrt::box_value(L"ButtonBackgroundPointerOver"), Fill(0x22, 0xFF, 0xFF, 0xFF)); // subtle hover lift
            br.Insert(winrt::box_value(L"ButtonBackgroundPressed"), Fill(0x33, 0xFF, 0xFF, 0xFF)); // a touch more on press
            br.Insert(winrt::box_value(L"ButtonForeground"), Fill(0xFF, 0xE6, 0xE6, 0xE6)); // light text on the dark UI
            br.Insert(winrt::box_value(L"ButtonForegroundPointerOver"), Fill(0xFF, 0xFF, 0xFF, 0xFF));
            br.Insert(winrt::box_value(L"ButtonForegroundPressed"), Fill(0xFF, 0xFF, 0xFF, 0xFF));
        }

        _BuildLayout();

        // Agentmaster: an INVISIBLE, caret-less keyboard-focus SINK, parked-on by Focus() when this pane
        // is activated. The Manager pane's key handlers (_KeyDownHandler / _ManagerPaneNavPreviewKeyDown —
        // alt+left/right, ctrl+tab, shift+home) are routed events on the pane ROOT and only fire when
        // keyboard focus is INSIDE the pane subtree; Focus() used to leave focus untouched (to avoid a
        // blinking caret in the cwd box), which left focus on the tab HEADER after a tab switch, so those
        // chords did nothing until you clicked an element in the pane. A 1x1, opacity-0, no-focus-visual
        // Button gives the pane keyboard focus with NO visible caret and NO path-picker popup (the popup
        // keys off the cwd box's OWN focus, not this). Appended AFTER _BuildLayout so it is not cleared.
        _focusSink = winrt::Windows::UI::Xaml::Controls::Button{};
        _focusSink.Width(1.0);
        _focusSink.Height(1.0);
        _focusSink.MinWidth(0.0);
        _focusSink.MinHeight(0.0);
        _focusSink.Opacity(0.0);
        _focusSink.IsTabStop(true);
        _focusSink.UseSystemFocusVisuals(false);
        _focusSink.HorizontalAlignment(winrt::Windows::UI::Xaml::HorizontalAlignment::Left);
        _focusSink.VerticalAlignment(winrt::Windows::UI::Xaml::VerticalAlignment::Top);
        _root.Children().Append(_focusSink);

        // Agentmaster (Waiting-for-you "unread" model): a low-frequency board refresh so TIME-derived
        // adornments stay current without a hook event — the card's ⚡ "still cached" hint (a few-minute
        // window) and the "-2h30m" timing text. _Refresh() recomputes them from the live snapshot; it is
        // idempotent and already runs on every registry event, so this only covers quiet periods. Weak
        // self so a closed window never leaks a ticking timer.
        _cardRefreshTimer = DispatcherTimer{};
        _cardRefreshTimer.Interval(std::chrono::seconds(30));
        _cardRefreshTimer.Tick([weak = get_weak()](const IInspectable& sender, const IInspectable&) {
            if (auto self = weak.get())
            {
                self->_Refresh();
            }
            else if (const auto t = sender.try_as<DispatcherTimer>())
            {
                t.Stop();
            }
        });
        _cardRefreshTimer.Start();
    }

    // Agentmaster (M9): the registry is a process singleton shared by every window. Detach this
    // lens's observer on teardown so the shared registry stops invoking a dead window's refresh
    // (and stops pinning its DispatcherQueue alive). The observer captures get_weak(), so a stray
    // late call is already a no-op; this also keeps the observer list bounded across many window
    // open/close cycles. _registry (held by SharedEngine) outlives us, so the call stays valid.
    AgentManagerContent::~AgentManagerContent()
    {
        if (_registry && _observerToken)
        {
            _registry->RemoveObserver(_observerToken);
        }
        if (_cardRefreshTimer)
        {
            _cardRefreshTimer.Stop(); // UI thread; stop the periodic ⚡/timing refresh
        }
        if (_progressTimer)
        {
            _progressTimer.Stop(); // UI thread; stop the Waiting-for-you countdown-bar drainer
        }
    }

    void AgentManagerContent::SetRegistry(std::shared_ptr<::Agentmaster::SessionRegistry> registry)
    {
        // Detach any previous observer (defensive — SetRegistry is normally called exactly once).
        if (_registry && _observerToken)
        {
            _registry->RemoveObserver(_observerToken);
            _observerToken = 0;
        }
        _registry = std::move(registry);
        if (_registry)
        {
            auto weak = get_weak();
            auto disp = _dispatcher;
            _observerToken = _registry->AddObserver([weak, disp](const SessionInfo&, HookEvent) {
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
        _Refresh();
    }

    void AgentManagerContent::SetSpawnHandler(std::function<void(winrt::hstring, winrt::hstring)> handler)
    {
        _spawnHandler = std::move(handler);
    }
    void AgentManagerContent::SetActivateHandler(std::function<void(winrt::hstring)> handler)
    {
        _activateHandler = std::move(handler);
    }
    void AgentManagerContent::SetActivateDormantHandler(std::function<void(winrt::hstring)> handler)
    {
        _activateDormantHandler = std::move(handler);
    }
    void AgentManagerContent::SetActivateAllHandler(std::function<void(bool)> handler)
    {
        _activateAllHandler = std::move(handler);
    }
    void AgentManagerContent::SetHoverSessionHandler(std::function<void(winrt::hstring, bool)> handler)
    {
        _hoverSessionHandler = std::move(handler);
    }
    void AgentManagerContent::SetArchiveHandler(std::function<void(winrt::hstring)> handler)
    {
        _archiveHandler = std::move(handler);
    }
    void AgentManagerContent::SetRestoreHandler(std::function<void(winrt::hstring)> handler)
    {
        _restoreHandler = std::move(handler);
    }
    void AgentManagerContent::SetResumeSessionHandler(std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> handler)
    {
        _resumeSessionHandler = std::move(handler);
    }
    void AgentManagerContent::SetForkSessionHandler(std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> handler)
    {
        _forkSessionHandler = std::move(handler);
    }
    void AgentManagerContent::SetRestartSessionHandler(std::function<void(winrt::hstring)> handler)
    {
        _restartSessionHandler = std::move(handler);
    }
    void AgentManagerContent::SetForkManagedSessionHandler(std::function<void(winrt::hstring)> handler)
    {
        _forkManagedSessionHandler = std::move(handler);
    }
    void AgentManagerContent::SetRenameHandler(std::function<void(winrt::hstring, winrt::hstring)> handler)
    {
        _renameHandler = std::move(handler);
    }
    void AgentManagerContent::SetTagsHandler(std::function<void(winrt::hstring, winrt::Windows::UI::Xaml::FrameworkElement)> handler)
    {
        _tagsHandler = std::move(handler);
    }
    void AgentManagerContent::SetAdoptExternalHandler(std::function<void(uint32_t, winrt::hstring, bool)> handler)
    {
        _adoptExternalHandler = std::move(handler);
    }
    void AgentManagerContent::SetCodexLaunchHandler(std::function<void(uint32_t, winrt::hstring, bool, bool)> handler)
    {
        _codexLaunchHandler = std::move(handler);
    }
    void AgentManagerContent::SetLocalScopeProvider(std::function<std::unordered_set<std::wstring>()> provider)
    {
        _localScopeProvider = std::move(provider);
        // Agentmaster: the provider DEFINES the Explorer Tree's LOCAL scope (the set of sessions THIS
        // window hosts) — installing it changes what the tree should show, so re-render now. Wiring
        // installs the provider AFTER SetRegistry's initial _Refresh() (TerminalPage::_WireAgentManagerContent),
        // so without this the first paint — and every paint until the next registry event — runs with
        // haveLocal==false and skips the filter: a freshly opened window lists EVERY window's sessions
        // under the "LOCAL" label until unrelated hook activity happens to trigger a rebuild. Re-render
        // here so the scope takes effect immediately, independent of wiring order. _Refresh() no-ops
        // until the layout exists (it always does — the ctor runs _BuildLayout before any Set*), and is
        // cheap + idempotent.
        _Refresh();
    }
    void AgentManagerContent::SetWindowForegroundProvider(std::function<bool()> provider)
    {
        _windowForegroundProvider = std::move(provider);
    }
    void AgentManagerContent::SetPauseHandler(std::function<void(bool)> handler)
    {
        _pauseHandler = std::move(handler);
    }
    void AgentManagerContent::SetReopenWindowsHandler(std::function<void()> handler)
    {
        _reopenWindowsHandler = std::move(handler);
    }
    void AgentManagerContent::SetOpenSessionsHandler(std::function<void()> handler)
    {
        _openSessionsHandler = std::move(handler);
    }
    void AgentManagerContent::SetResetHiddenSessionsHandler(std::function<void()> handler)
    {
        _resetHiddenSessionsHandler = std::move(handler);
    }
    void AgentManagerContent::SetRefreshHandler(std::function<void()> handler)
    {
        _refreshHandler = std::move(handler);
    }
    // Agentmaster: force a UI redraw from the current data sources (registry snapshot + the last
    // pushed external census). Called by the page after an out-of-band reload (the observer survey
    // lands asynchronously) so the freshly enriched / re-surveyed data shows. Marshal to the UI
    // thread is the caller's responsibility (the page resumes on the dispatcher before calling).
    void AgentManagerContent::RefreshNow()
    {
        _Refresh();
    }
    // Agentmaster (Linked Lenses — per-tab -> Manager sync): drive the lens selection from the page when
    // the user switches to a managed session's terminal tab. Routes through _SelectSession (the same path
    // a board-card single-click takes), so the board card + tree row highlight and the Auto Testing show
    // that session. _SelectSession early-outs when the id is already selected, so a re-select is cheap.
    void AgentManagerContent::SelectSession(winrt::hstring id)
    {
        const std::wstring sid{ id };
        if (sid.empty())
        {
            return; // not a managed session (e.g. a pwsh/cmd/external tab) -> leave the current selection
        }
        _SelectSession(sid);
    }
    // Agentmaster (Linked Lenses — selection VISIBILITY sync): scroll the currently-selected board card
    // (and its Explorer-tree row) into view within their scrolling regions. The page calls this when the
    // user switches TO the Manager tab: while the Manager was hidden the selection followed the user's
    // tab switches (SelectSession from _SyncManagerSelectionToTab), or a state change moved the card into
    // a column where it sits below the fold — so on return the HIGHLIGHTED card can be scrolled
    // off-screen. Revealing it ON tab-entry — deliberately NOT on every _Refresh, which would fight the
    // user's own scrolling while they sit on the Manager tab and undo _RebuildBoard's offset
    // preservation — synchronizes the selection's visibility with its highlight. A no-op when nothing
    // managed is selected (an external selection has no tracked card) or the selected session has no
    // visible card/row (archived / out of the current scope / its directory group collapsed).
    void AgentManagerContent::BringSelectedIntoView()
    {
        if (_selectedId.empty())
        {
            return;
        }
        auto weak = get_weak();
        auto disp = _dispatcher;
        if (!disp)
        {
            return;
        }
        // Defer to a clean tick: the Manager content was just made the visible tab, so its board may not
        // have completed a layout pass yet (MUX TabView hosts only the selected tab's content). Realize
        // it (UpdateLayout) so each card has a real extent, then StartBringIntoView walks up to the
        // card's column ScrollViewer and scrolls it into view (a no-op if already fully visible) — the
        // same UpdateLayout-then-scroll recipe the Auto-Testing auto-scroll-to-bottom uses. Run at LOW
        // priority so this lands AFTER the framework's own restore work this attach triggers (each fresh
        // column ScrollViewer re-applies its saved offset on Loaded; see _MakeBoardColumn) — our reveal
        // must be the last word on the selected card's column, else the offset restore would re-hide it.
        // _selectedId is re-read inside (it may change before this runs).
        disp.TryEnqueue(winrt::Windows::System::DispatcherQueuePriority::Low, [weak]() {
            auto self = weak.get();
            if (!self || self->_selectedId.empty())
            {
                return;
            }
            if (self->_boardHost)
            {
                self->_boardHost.UpdateLayout();
            }
            if (const auto it = self->_boardCardsById.find(self->_selectedId); it != self->_boardCardsById.end() && it->second)
            {
                it->second.StartBringIntoView();
            }
            if (const auto it = self->_treeRowsById.find(self->_selectedId); it != self->_treeRowsById.end() && it->second)
            {
                it->second.StartBringIntoView();
            }
        });
    }
    void AgentManagerContent::SetConfirmHandler(std::function<void(winrt::hstring, bool)> handler)
    {
        _confirmHandler = std::move(handler);
    }
    void AgentManagerContent::SetSettings(const ::Agentmaster::AppSettings& settings)
    {
        _appSettings = settings;
        // Auto Testing is a DEV-ONLY feature: in a RELEASE install the bottom-right pane is the
        // read-only Summary view only (no [Summary | Auto Testing] toggle). Force the Summary tab
        // selected so every reader (the pane visibility, _RefreshSummaryTab, _LoadSummaryForSession)
        // shows it regardless of what settings.json carries. In memory only — never re-persisted.
        if (!::Agentmaster::Profiles::IsDevPackage())
        {
            _appSettings.autoTestingShowsSummary = true;
        }
        // Seed the Launch cwd box with the configured default (wiring runs after _BuildLayout,
        // which had defaulted the box to %USERPROFILE%). Only override when a default is set.
        if (_cwdBox && !settings.defaultLaunchDir.empty())
        {
            _cwdBox.Text(winrt::hstring{ settings.defaultLaunchDir });
            _ValidateLaunchBox(); // re-validate explicitly: don't lean on TextChanged for this programmatic
                                  // set. It works today only because SetSettings runs while the Manager tab
                                  // is active (attached), but a future detached caller (cross-window settings
                                  // push) would otherwise leave the underline/button stale (see _SelectSession).
        }
        // Reflect the (global, persisted) Explorer Tree + Triage Board sorts on their toggles. Safe
        // before the UI is built (each updater no-ops while its button is null); the views adopt the
        // order on the next data-driven rebuild. Lets a window pick up the loaded/changed sorts, not
        // just the ctor defaults — including a board sort changed in another window (adopted on launch).
        _UpdateTreeSortButton();
        _UpdateBoardSortButton();
        _UpdatePlanPaneTab(); // reflect the (global, persisted) Auto-Testing pane tab; no-op while its controls are null
    }
    void AgentManagerContent::SetSettingsHandler(std::function<void(::Agentmaster::AppSettings)> handler)
    {
        _settingsSink = std::move(handler);
    }

    // Agentmaster (cross-window settings broadcast): a GLOBAL setting changed in ANOTHER window — adopt
    // the merged settings and re-apply what this window renders live. Runs on THIS window's UI thread
    // (the engine sink marshals here). We do NOT push these back through _settingsSink (that would loop
    // / re-persist what the source already saved) and we deliberately skip SetSettings's cwd-box reseed
    // (it would stomp in-progress typing). Adopt _appSettings wholesale (the broadcast value is the
    // freshest-disk-merged truth, so future spawns + the cog's next open use it), repaint both sort
    // toggles, then _Refresh so the board + tree re-sort with the new (global) order immediately.
    void AgentManagerContent::ApplyExternalSettings(const ::Agentmaster::AppSettings& settings)
    {
        _appSettings = settings;
        if (!::Agentmaster::Profiles::IsDevPackage())
        {
            _appSettings.autoTestingShowsSummary = true; // release: Summary-only pane (Auto Testing is dev-only)
        }
        _UpdateTreeSortButton();
        _UpdateBoardSortButton();
        _UpdatePlanPaneTab(); // adopt another window's Auto-Testing pane tab choice (GLOBAL setting)
        _Refresh();
    }

    // ---- Per-window Manager lens (M10; PERSISTENCE.md §13) ------------------

    ::Agentmaster::ManagerState AgentManagerContent::GetManagerState() const
    {
        ::Agentmaster::ManagerState st;
        st.selectedId = _selectedId;
        st.scopeDir = _scopeDir;
        st.selectedPromptId = _selectedPromptId;
        st.collapsedDirs.assign(_collapsedDirs.begin(), _collapsedDirs.end());
        st.layout = _layout;
        st.treeScope = static_cast<int>(_treeScope); // the shared tree/board scope (persisted)
        return st;
    }

    void AgentManagerContent::SetManagerState(const ::Agentmaster::ManagerState& state)
    {
        _ResetPromptHistory(); // a lens (re)seed changes the selected session — start history fresh
        _selectedId = state.selectedId;
        _scopeDir = state.scopeDir;
        _selectedPromptId = state.selectedPromptId;
        _collapsedDirs.clear();
        _collapsedDirs.insert(state.collapsedDirs.begin(), state.collapsedDirs.end());
        // The shared tree/board scope. Direct assignment, NOT _SetTreeScope: this is a seed, not a
        // user transition — no enter/leave-External selection cleanup (the persisted lens is already
        // self-consistent: entering External cleared the managed selection before it was saved), and
        // no lens push (we are APPLYING the lens). EXTERNAL (2) is a transient observe-only view, never
        // a window's OPENING scope: a window left in EXTERNAL at close reopens in LOCAL (the default
        // working lens) rather than staring at other hosts' claudes. So only LOCAL (0) / GLOBAL (1)
        // restore as saved; EXTERNAL — and any out-of-range value — fall back to LOCAL.
        _treeScope = (state.treeScope == 1) ? TreeScope::Global : TreeScope::Local;
        _UpdateTreeScopeButton();
        _UpdateBoardScopeButton();
        // A restored per-window layout overrides the global default loaded in the ctor; push the
        // fractions into the live tracks so the splitters land where the window left them.
        _layout = state.layout;
        _ApplyLayoutToTracks();
        _Refresh();
    }

    void AgentManagerContent::SetLensChangedHandler(std::function<void(::Agentmaster::ManagerState)> handler)
    {
        _lensChangedHandler = std::move(handler);
    }

    void AgentManagerContent::_NotifyLensChanged()
    {
        if (_lensChangedHandler)
        {
            _lensChangedHandler(GetManagerState());
        }
    }

    void AgentManagerContent::_ApplyLayoutToTracks()
    {
        if (_boardRow)
        {
            _boardRow.Height(GridLengthHelper::FromValueAndType(_layout.boardFraction, GridUnitType::Star));
        }
        if (_bottomRow)
        {
            _bottomRow.Height(GridLengthHelper::FromValueAndType(1.0 - _layout.boardFraction, GridUnitType::Star));
        }
        if (_treeCol)
        {
            _treeCol.Width(GridLengthHelper::FromValueAndType(_layout.treeFraction, GridUnitType::Star));
        }
        if (_planCol)
        {
            _planCol.Width(GridLengthHelper::FromValueAndType(1.0 - _layout.treeFraction, GridUnitType::Star));
        }
    }

    // ---- IPaneContent -------------------------------------------------------

    FrameworkElement AgentManagerContent::GetRoot()
    {
        return _root;
    }
    void AgentManagerContent::UpdateSettings(const CascadiaSettings& /*settings*/)
    {
    }
    Size AgentManagerContent::MinimumSize()
    {
        return { 1, 1 };
    }
    void AgentManagerContent::Focus(FocusState /*reason*/)
    {
        // Agentmaster: park keyboard focus on the INVISIBLE, caret-less sink (built in the ctor) — NOT the
        // cwd box. The host calls IPaneContent::Focus whenever the Manager pane is activated (app open, tab
        // open, tab switch). We must put focus SOMEWHERE inside the pane subtree, or the pane-root routed
        // key handlers (_KeyDownHandler / _ManagerPaneNavPreviewKeyDown — alt+left/right, ctrl+tab,
        // shift+home) never fire (focus stays on the tab header), the bug where those chords did nothing
        // until you clicked an element. Focusing the cwd box was the ORIGINAL behavior but it put a blinking
        // caret in it on every open (and popped the path-picker) — which the user disliked — so we focus a
        // 1x1, opacity-0, no-focus-visual Button instead: keyboard works immediately, NOTHING looks focused,
        // and the path-picker (which keys off the cwd box's own focus) stays shut. Programmatic focus shows
        // no focus visual; if the sink isn't focusable yet (pre-layout on first realize) retry once next tick.
        if (!_focusSink)
        {
            return;
        }
        if (!_focusSink.Focus(winrt::Windows::UI::Xaml::FocusState::Programmatic) && _dispatcher)
        {
            auto sink = _focusSink;
            _dispatcher.TryEnqueue([sink]() {
                if (sink)
                {
                    sink.Focus(winrt::Windows::UI::Xaml::FocusState::Programmatic);
                }
            });
        }
    }
    void AgentManagerContent::Close()
    {
    }
    INewContentArgs AgentManagerContent::GetNewTerminalArgs(const BuildStartupKind /*kind*/) const
    {
        return BaseContentArgs(L"agentManager");
    }
    winrt::hstring AgentManagerContent::Title()
    {
        // The Manager tab's title doubles as the at-a-glance which-install-am-I marker: the
        // DEV package (AgentmasterDev — the loose-layout deploy) reads "Agent Manager Dev",
        // the release install plain "Agent Manager". Runtime identity, not a build flag, so
        // one binary serves both installs; cached — the package family never changes mid-run.
        static const winrt::hstring title = ::Agentmaster::Profiles::IsDevPackage() ?
                                                winrt::hstring{ L"Agent Manager Dev" } :
                                                winrt::hstring{ L"Agent Manager" };
        return title;
    }
    winrt::hstring AgentManagerContent::Icon() const
    {
        static constexpr std::wstring_view glyph{ L"\xE71D" }; // AllApps
        return winrt::hstring{ glyph };
    }
    Brush AgentManagerContent::BackgroundBrush()
    {
        return _root.Background();
    }

    // ---- Layout (built once) ------------------------------------------------

    void AgentManagerContent::_BuildLayout()
    {
        auto starRow = [](double v) {
            RowDefinition rd;
            rd.Height(GridLengthHelper::FromValueAndType(v, GridUnitType::Star));
            return rd;
        };
        auto autoRow = []() {
            RowDefinition rd;
            rd.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            return rd;
        };
        auto starCol = [](double v) {
            ColumnDefinition cd;
            cd.Width(GridLengthHelper::FromValueAndType(v, GridUnitType::Star));
            return cd;
        };
        auto autoCol = []() {
            ColumnDefinition cd;
            cd.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
            return cd;
        };

        const auto panelBorder = Fill(0x40, 0x80, 0x80, 0x80);

        auto section = [&](const FrameworkElement& child) {
            Border b;
            b.BorderBrush(SolidColorBrush{ panelBorder });
            b.BorderThickness(Thickness{ 1, 1, 1, 1 });
            b.CornerRadius(CornerRadius{ 6, 6, 6, 6 });
            b.Margin(Thickness{ 6, 6, 6, 6 });
            b.Padding(Thickness{ 8, 6, 8, 8 });
            b.Child(child);
            return b;
        };

        // Rows: toolbar (auto) · board (★) · splitter (auto) · bottom (★). The two ★ rows are
        // seeded from the persisted fraction and are what the horizontal splitter resizes.
        _boardRow = starRow(_layout.boardFraction);
        _bottomRow = starRow(1.0 - _layout.boardFraction);
        _root.RowDefinitions().Append(autoRow()); // 0: toolbar
        _root.RowDefinitions().Append(_boardRow); // 1: board
        _root.RowDefinitions().Append(autoRow()); // 2: splitter
        _root.RowDefinitions().Append(_bottomRow); // 3: bottom

        // ---- Toolbar ----
        {
            // Agentmaster: the toolbar is a VERTICAL stack — a TOP row (the "Agentmaster" title +
            // the launch controls) over a compact ACTIONS row (Settings, Pause Autorunner, Sessions,
            // Keep Awake) tucked just below the title in the top-left. The actions buttons are
            // deliberately thinner (smaller font + slim padding), matching the header-toggle idiom.
            _toolbarCol = StackPanel{};
            _toolbarCol.Orientation(Orientation::Vertical);
            _toolbarCol.Spacing(6);
            _toolbarCol.Margin(Thickness{ 12, 8, 12, 0 });
            auto& toolbarCol = _toolbarCol;

            _launchBar = StackPanel{}; // top row: the title + launch controls
            _launchBar.Orientation(Orientation::Horizontal);
            _launchBar.Spacing(8);
            _launchBar.VerticalAlignment(VerticalAlignment::Center);
            auto& bar = _launchBar;

            // The compact actions row, left-aligned directly under the title. Its buttons are
            // appended below as each is built; the row itself is added to toolbarCol at the end.
            auto actionsRow = StackPanel{};
            actionsRow.Orientation(Orientation::Horizontal);
            actionsRow.Spacing(6);
            actionsRow.HorizontalAlignment(HorizontalAlignment::Left);
            actionsRow.VerticalAlignment(VerticalAlignment::Center);

            // Agentmaster (responsive launch bar): "Agentmaster" + "\x2014" stay; "launch a" / "session in"
            // (below) collapse first when the pane narrows (_ReflowLaunchBar), leaving "Agentmaster \x2014
            // [\x25CF Claude] [box]". Built as members so reflow can toggle their Visibility.
            _agentmasterText = Text(L"Agentmaster", 18, true, 1.0);
            bar.Children().Append(_agentmasterText);
            _dashText = Text(L"\x2014", 13, false, 0.6); // em-dash, always shown
            bar.Children().Append(_dashText);
            _launchAText = Text(L"launch a", 13, false, 0.6); // collapsible
            bar.Children().Append(_launchAText);

            // Agentmaster (Codex-launch): the agent toggle — Claude (default) <-> Codex. Click cycles it
            // (the scope/sort/autorunner toggle idiom). It retargets the SAME cwd box + Launch button, so a
            // managed Codex launches exactly the way a Claude does ("do what we do for Claude"). A Codex
            // launch is directory-only — Codex has no typed-id resume/fork here (Codex resume is reached via
            // the Archive page / window-restore / EXTERNAL Adopt), so _ValidateLaunchBox suppresses those.
            _launchAgentBtn = Button{};
            _launchAgentBtn.FontSize(11);
            _launchAgentBtn.Padding(Thickness{ 8, 1, 8, 1 });
            AgentSetTip(_launchAgentBtn, L"Agent to launch \x2014 click to toggle between Claude and Codex (the Launch button and box retarget to match).");
            _launchAgentBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                _launchCodex = !_launchCodex;
                _UpdateLaunchAgentButton();
                _ValidateLaunchBox(); // repaint the Launch button text + resume/fork affordances for the new agent
            });
            bar.Children().Append(_launchAgentBtn);
            _UpdateLaunchAgentButton();

            _sessionInText = Text(L"session in", 13, false, 0.6); // collapsible (with "launch a")
            bar.Children().Append(_sessionInText);

            _cwdBox = TextBox{};
            // Agentmaster: 504 (the old fixed width) is the COMFORTABLE width; the box grows with its typed
            // content — a NoWrap TextBox in the horizontal launch bar measures to its text — between the
            // live MinWidth/MaxWidth that _ReflowLaunchBar sets per stage (504 floor when there's room; a
            // 240/160 floor once the pane narrows enough to shrink / wrap), so a long path never pushes the
            // Launch button off-screen. Left alignment keeps it content-sized rather than stretched-to-fill.
            _cwdBox.MinWidth(504);
            _cwdBox.MaxWidth(504); // seed; _ReflowLaunchBar (re)computes Min/Max per stage on first layout
            _cwdBox.HorizontalAlignment(HorizontalAlignment::Left);
            _cwdBox.PlaceholderText(L"working directory (the M axis)");
            AgentSetTip(_cwdBox, L"Where to launch: a working directory for a new session, or a Claude session id to resume or fork. Start typing to pick from recent and matching folders."); // Agentmaster: the box accepts EITHER a working dir (new session) OR a session id (Resume / Fork)
            {
                wchar_t up[MAX_PATH];
                const DWORD n = ::GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
                if (n > 0 && n < MAX_PATH)
                {
                    _cwdBox.Text(winrt::hstring{ up, n });
                }
            }
            // Path-picker drop-down. Open it only on *user* focus (Pointer/Keyboard) so the
            // dropdown doesn't pop every time the tab is programmatically activated.
            _cwdBox.GotFocus([this](const IInspectable&, const RoutedEventArgs&) {
                if (!_cwdBox)
                {
                    return;
                }
                // (Re)focusing the box clears any prior Esc/Enter/blur dismissal, so the next
                // keystroke brings the list back; when the focus itself came from the user
                // (pointer/keyboard) open it right away.
                _pathPickerUserDismissed = false;
                const auto fs = _cwdBox.FocusState();
                if (fs == FocusState::Pointer || fs == FocusState::Keyboard)
                {
                    _OpenPathPicker();
                }
            });
            // Clicking back into an ALREADY-focused box fires no GotFocus, so a tap also re-shows
            // a previously dismissed picker (open only if needed; a tap is not a drag, so it
            // won't fight text selection).
            _cwdBox.Tapped([this](const IInspectable&, const winrt::Windows::UI::Xaml::Input::TappedRoutedEventArgs&) {
                if (!_cwdBox)
                {
                    return;
                }
                _pathPickerUserDismissed = false;
                if (!_pathPopup || !_pathPopup.IsOpen())
                {
                    _OpenPathPicker();
                }
            });
            _cwdBox.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) {
                if (!_cwdBox)
                {
                    return;
                }
                // Agentmaster: paint the validation underline + enable/disable launch on every edit
                // (a working dir OR a session id). Runs before the picker logic's focus early-out so
                // it always reflects the current text.
                _ValidateLaunchBox();
                // Typing should ALWAYS surface the list. The prior version only refreshed an
                // already-open popup, so whenever the box held focus while the popup was closed
                // (focus arrived programmatically, a stray LostFocus closed it, etc.) typing
                // showed nothing — exactly the reported "list doesn't show" symptom. Now we
                // reopen when closed and refresh when open. The lone exception is an explicit
                // Esc/Enter dismissal, which sticks until the box is refocused or tapped.
                if (_cwdBox.FocusState() == FocusState::Unfocused || _pathPickerUserDismissed)
                {
                    return;
                }
                if (_pathPopup && _pathPopup.IsOpen())
                {
                    _RebuildPathPicker();
                }
                else
                {
                    _OpenPathPicker();
                }
            });
            // Close only when focus truly left (not when a row button briefly takes it):
            // defer the check a tick, and bail if the box has refocused itself (after a pick).
            {
                auto weak = get_weak();
                auto disp = _dispatcher;
                _cwdBox.LostFocus([weak, disp](const IInspectable&, const RoutedEventArgs&) {
                    if (!disp)
                    {
                        return;
                    }
                    disp.TryEnqueue([weak]() {
                        auto self = weak.get();
                        if (!self || !self->_cwdBox)
                        {
                            return;
                        }
                        if (self->_cwdBox.FocusState() != FocusState::Unfocused)
                        {
                            return; // regained focus (e.g. after clicking a row) — leave it alone
                        }
                        // Focus truly left the box: close the picker (if open) and normalize
                        // what's there. Mark it dismissed so the normalize's TextChanged won't
                        // reopen the (now-closed) picker; refocusing/tapping the box clears it.
                        self->_pathPickerUserDismissed = true;
                        self->_ClosePathPicker();
                        self->_NormalizeCwdBox();
                    });
                });
            }
            _cwdBox.KeyDown([this](const IInspectable&, const KeyRoutedEventArgs& e) {
                if (e.Key() == VirtualKey::Escape)
                {
                    if (_pathPopup && _pathPopup.IsOpen())
                    {
                        _pathPickerUserDismissed = true; // stays dismissed until refocus/tap
                        _ClosePathPicker();
                        e.Handled(true);
                    }
                }
                else if (e.Key() == VirtualKey::Enter)
                {
                    _pathPickerUserDismissed = true; // committing dismisses; keep normalize from reopening it
                    _ClosePathPicker();
                    _NormalizeCwdBox(); // commit: normalize what the user typed
                }
            });
            // Agentmaster: the box + a validation underline beneath it, in a vertical column so the
            // underline tracks the box width. The path-picker Popup anchors via _cwdBox.TransformToVisual
            // (robust to this wrapping), so the dropdown placement is unaffected.
            auto cwdCol = StackPanel{};
            cwdCol.Orientation(Orientation::Vertical);
            cwdCol.Spacing(2);
            cwdCol.VerticalAlignment(VerticalAlignment::Center);
            cwdCol.Children().Append(_cwdBox);
            _cwdUnderline = Border{};
            _cwdUnderline.Height(2);
            // Stretch (no fixed width) so the underline always spans the box's CURRENT width: the box now
            // grows with content and cwdCol's width tracks it, so a stretched underline stays matched.
            _cwdUnderline.HorizontalAlignment(HorizontalAlignment::Stretch);
            _cwdUnderline.CornerRadius(CornerRadius{ 1, 1, 1, 1 });
            _cwdUnderline.Background(Fill(0x00, 0x00, 0x00, 0x00)); // transparent = neutral; kept present so painting it never reflows the bar
            cwdCol.Children().Append(_cwdUnderline);
            bar.Children().Append(cwdCol);

            // Agentmaster (responsive launch bar): the cwd box's width + the whole top row reflow as the
            // pane narrows — _ReflowLaunchBar stages it (full label -> collapse "launch a"/"session in" ->
            // shrink the box to its floor -> wrap the launch buttons to their own line). Driven off _root's
            // SizeChanged; the box grows with its content between the live Min/Max the reflow sets. Setting
            // a child's width / visibility / parent never resizes _root (the pane owns _root's size), so
            // there's no layout loop.
            if (_root)
            {
                _root.SizeChanged([this](const IInspectable&, const SizeChangedEventArgs& e) {
                    _lastRootWidth = e.NewSize().Width;
                    _ReflowLaunchBar();
                });
            }

            // Agentmaster (responsive launch bar): the launch buttons (Launch / Fork / Reopen / Activate)
            // live in their OWN panel so _ReflowLaunchBar can move the whole group to a 2nd line (below the
            // title row) when the bar can no longer fit them inline beside a floored cwd box.
            _launchBtns = StackPanel{};
            _launchBtns.Orientation(Orientation::Horizontal);
            _launchBtns.Spacing(8);
            _launchBtns.VerticalAlignment(VerticalAlignment::Center);

            _launchBtn = Button{};
            _launchBtn.Content(winrt::box_value(L"Launch Claude"));
            AgentSetTip(_launchBtn, L"Start the selected agent in the working directory above \x2014 or resume the conversation when a session id is entered.");
            _launchBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _OnLaunch(); });
            _launchBtns.Children().Append(_launchBtn);

            // Fork — hidden unless the box holds a FOUND session id; forks that conversation into a
            // NEW one (the original transcript is untouched), mirroring the Sessions page's "Fork here".
            _forkBtn = Button{};
            _forkBtn.Content(winrt::box_value(L"Fork"));
            _forkBtn.Visibility(Visibility::Collapsed);
            AgentSetTip(_forkBtn, L"Fork the entered session into a NEW, independent conversation \x2014 the original transcript is left untouched.");
            _forkBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _OnForkFromBox(); });
            _launchBtns.Children().Append(_forkBtn);

            _ValidateLaunchBox(); // initial state for the seeded cwd (USERPROFILE -> neutral, enabled)

            // Agentmaster (M10 Increment 3; PERSISTENCE.md §13.5): the "Reopen Windows (N)" recover
            // button — the "if I answered No" path. It reopens saved windows that are NOT currently
            // open (the runtime analog of the WindowEmperor's startup reopen loop). Hidden when there
            // is nothing to recover (N==0); _UpdateReopenButton (driven from _Refresh) maintains both.
            _reopenBtn = Button{};
            _reopenBtn.Content(winrt::box_value(L"Reopen Windows"));
            _reopenBtn.Visibility(Visibility::Collapsed);
            AgentSetTip(_reopenBtn, L"Reopen saved windows that aren't currently open \x2014 restores each window's tabs, layout, and sessions.");
            _reopenBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _OnReopenWindows(); });
            _launchBtns.Children().Append(_reopenBtn);

            // Agentmaster (eager-init): "Activate All Tabs (N)" — wake every DORMANT managed tab in this
            // window (a window-restored / re-homed tab spawns its claude lazily, only when first shown; this
            // starts them all IN PLACE without switching tabs). Hidden when N==0 (the hide-when-idle idiom,
            // like Reopen Windows); _UpdateActivateAllButton (driven from _Refresh) maintains label + show.
            // If OTHER windows also have dormant tabs, the click prompts to choose the scope (_OnActivateAllTabs).
            _activateAllBtn = Button{};
            _activateAllBtn.Content(winrt::box_value(L"Activate All Tabs"));
            _activateAllBtn.Visibility(Visibility::Collapsed);
            AgentSetTip(_activateAllBtn, L"Start every Claude session in this window that hasn't initialized yet (restored tabs you haven't opened) \x2014 in place, without switching tabs. Their half-hollow dots fill as they start.");
            _activateAllBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _OnActivateAllTabs(); });
            _launchBtns.Children().Append(_activateAllBtn);

            // The launch-buttons group sits inline at the end of the title row by default; _ReflowLaunchBar
            // moves it to its own line (between the title row and the actions row) when the bar is too narrow.
            bar.Children().Append(_launchBtns);

            // Settings cog (opens the in-content settings overlay; built at the end of layout).
            // Lives in the compact actions row below the title — thinner, smaller font.
            _settingsBtn = Button{};
            _settingsBtn.FontSize(11);
            _settingsBtn.Padding(Thickness{ 8, 1, 8, 1 });
            {
                FontIcon cog;
                cog.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
                cog.Glyph(L"\xE713"); // Settings (cog)
                cog.FontSize(13);
                _settingsBtn.Content(cog);
            }
            AgentSetTip(_settingsBtn, L"Settings \x2014 model & launch options, Tests Autorunner defaults, the Claude binary, the active profile, and app behavior.");
            _settingsBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _ShowSettings(); });
            actionsRow.Children().Append(_settingsBtn);

            // Global Autorunner backstop: Pause-all / Resume-all. Built here but appended LAST, so it
            // sits at the RIGHT end of the actions row (after cog, Sessions, Keep Awake).
            _pauseBtn = Button{};
            _pauseBtn.FontSize(11);
            _pauseBtn.Padding(Thickness{ 8, 1, 8, 1 });
            _pauseBtn.Content(winrt::box_value(L"Pause Tests Autorunning"));
            AgentSetTip(_pauseBtn, L"Global Tests Autorunning backstop \x2014 pauses or resumes auto-sending across ALL sessions at once.");
            _pauseBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                _globalPaused = !_globalPaused;
                ::Agentmaster::LogNav(_globalPaused ? L"pause-all on (global Autorunner backstop)" : L"pause-all off (global Autorunner resumed)");
                if (_pauseHandler)
                {
                    _pauseHandler(_globalPaused);
                }
                if (_pauseBtn)
                {
                    _pauseBtn.Content(winrt::box_value(_globalPaused ? L"Resume Tests Autorunning" : L"Pause Tests Autorunning"));
                }
            });
            // (appended LAST — see below, after Keep Awake)

            // Agentmaster (Sessions page; SESSIONS.md / FAVORITES.md): the global on-disk Claude-sessions
            // browser — EVERY session on the machine in a selectable window, searchable, with the ★
            // Favorite column + filter. This is the SOLE history view (the separate "Archived" button +
            // page were removed: closing a session keeps it here, resumable, marked by Favorite).
            _sessionsBtn = Button{};
            _sessionsBtn.FontSize(11);
            _sessionsBtn.Padding(Thickness{ 8, 1, 8, 1 });
            _sessionsBtn.Content(winrt::box_value(L"Sessions"));
            AgentSetTip(_sessionsBtn, L"Browse and search every Claude Code session on this machine \x2014 not just managed ones (last month by default). Star the ones you want to keep.");
            _sessionsBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { if (_openSessionsHandler) { _openSessionsHandler(); } });
            actionsRow.Children().Append(_sessionsBtn);

            // Agentmaster: tri-mode "Keep Awake" button — prevents the PC (and display) from sleeping
            // while a long unattended run is in flight, mirroring the user's stay-awake.ps1. It calls
            // SetThreadExecutionState from this (persistent) UI thread, so ES_CONTINUOUS holds the flag
            // until released — no timer/loop needed (the per-thread state persists for the thread's life).
            // Click cycles Off -> Always -> While-Running; While-Running holds only while a session is
            // actively working (re-evaluated each _Refresh) so the machine can still sleep when all idle.
            _keepAwakeBtn = Button{};
            _keepAwakeBtn.FontSize(11);
            _keepAwakeBtn.Padding(Thickness{ 8, 1, 8, 1 });
            AgentSetTip(_keepAwakeBtn, L"Keep this PC (and display) awake. Click to cycle: Off \x2192 Always \x2192 While Running (holds only while a session is actively working, so the machine can still sleep once every agent is idle). Released when set Off or the window closes.");
            _keepAwakeBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _CycleKeepAwake(); });
            actionsRow.Children().Append(_keepAwakeBtn);
            _UpdateKeepAwakeButton();

            // Pause/Resume Tests Autorunning — the rightmost button in the actions row (built far
            // above). Auto Testing / Tests Autorunner is a DEV-ONLY feature: in a RELEASE install the
            // autorunner never runs (the scheduler isn't started — see Engine.cpp), so this global
            // pause/resume backstop is hidden. Built unconditionally above but only ATTACHED under the
            // AgentmasterDev package, so the actions row never carries a no-op control in release.
            if (::Agentmaster::Profiles::IsDevPackage())
            {
                actionsRow.Children().Append(_pauseBtn);
            }

            // Stack the compact actions row directly below the top (title + launch) row.
            toolbarCol.Children().Append(bar);
            toolbarCol.Children().Append(actionsRow);
            Grid::SetRow(toolbarCol, 0);
            _root.Children().Append(toolbarCol);
            _ReflowLaunchBar(); // initial pass (no-op until laid out — SizeChanged drives the first real reflow)
        }

        // ---- Triage Board (row 1) ----
        {
            auto outer = Grid{};
            outer.RowDefinitions().Append(autoRow());
            outer.RowDefinitions().Append(starRow(1));

            auto header = StackPanel{};
            header.Orientation(Orientation::Horizontal);
            header.Spacing(8);
            header.Children().Append(Text(L"TRIAGE BOARD", 12, true, 0.8));
            // Agentmaster: the board's LOCAL/GLOBAL scope toggle — ONE state with the Explorer
            // Tree's 3-way toggle (same style, same per-window lens persistence): LOCAL shows only
            // this window's sessions, GLOBAL every window's. While the tree sits in EXTERNAL the
            // board reads GLOBAL (it has no External mode); a click then flips the shared scope to
            // LOCAL. _SetTreeScope is the one mutator behind both buttons.
            _boardScopeBtn = Button{};
            _boardScopeBtn.FontSize(11);
            _boardScopeBtn.Padding(Thickness{ 8, 1, 8, 1 });
            EmphasizeScopeButton(_boardScopeBtn); // Agentmaster: the primary header toggle — louder than sort/refresh/Clear
            AgentSetTip(_boardScopeBtn, L"Which sessions the board shows \x2014 LOCAL (this window) or GLOBAL (all windows). Shares one setting with the Explorer Tree's scope; remembered per window.");
            _boardScopeBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                _SetTreeScope(_treeScope == TreeScope::Local ? TreeScope::Global : TreeScope::Local);
            });
            header.Children().Append(_boardScopeBtn);
            _UpdateBoardScopeButton();
            // Agentmaster: the board's SORT toggle, right after the scope toggle (mirroring the Explorer
            // Tree's scope-then-sort layout). Cycles MOST ACTIVE -> NEWEST -> OLDEST -> A-Z (the tree's set
            // minus BY PID — host/shell grouping is meaningless once cards split across state columns).
            // Default MOST ACTIVE. A SEPARATE global setting from the tree's sort (AppSettings::boardSort),
            // so each remembers its own; persisted + shared by every window (the changing window re-sorts
            // live; others adopt on next launch — the treeSort idiom). _CycleBoardSort advances + persists
            // through the settings sink; _UpdateBoardSortButton paints the label.
            _boardSortBtn = Button{};
            _boardSortBtn.FontSize(11);
            _boardSortBtn.Padding(Thickness{ 8, 1, 8, 1 });
            AgentSetTip(_boardSortBtn, L"Sort the cards within each column \x2014 MOST ACTIVE (most recent activity first \x2014 the default) \xB7 NEWEST \xB7 OLDEST \xB7 A\x2013Z. Global across windows; saved.");
            _boardSortBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _CycleBoardSort(); });
            header.Children().Append(_boardSortBtn);
            _UpdateBoardSortButton();
            // Agentmaster: a refresh button AFTER the sort toggle — the twin of the Explorer Tree's
            // _treeRefreshBtn. Re-scans + redraws the ENTIRE tab: _Refresh() rebuilds the board, tree
            // AND auto testing (recomputing the live "ago" timing), and _refreshHandler forces the
            // Fleet Observer to re-survey NOW (re-enrich the registry + recompute the external census)
            // instead of waiting for the next tick. Same handler as the tree button by design.
            _boardRefreshBtn = Button{};
            _boardRefreshBtn.FontSize(11);
            _boardRefreshBtn.Padding(Thickness{ 8, 1, 8, 1 });
            _boardRefreshBtn.Content(winrt::box_value(L"\x21BB")); // ↻ refresh glyph
            AgentSetTip(_boardRefreshBtn, L"Refresh now \x2014 re-scan and redraw the whole tab (also re-detects external sessions).");
            _boardRefreshBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                _Refresh(); // immediate redraw from current data (board + tree + auto testing; recomputes the "ago" timing)
                if (_refreshHandler)
                {
                    _refreshHandler(); // page: wake the observer + re-probe -> fresh data lands shortly
                }
            });
            header.Children().Append(_boardRefreshBtn);
            // Agentmaster: a "Clear" button right next to LOCAL/GLOBAL — deselect the current card/row
            // (the Auto Testing then shows nothing-selected). Hidden while nothing is selected (kept in
            // sync by _RebuildBoard, like "Show all"); shown once a session/external is selected.
            _clearSelBtn = Button{};
            _clearSelBtn.Content(winrt::box_value(L"Clear"));
            _clearSelBtn.FontSize(11);
            _clearSelBtn.Padding(Thickness{ 8, 1, 8, 1 });
            _clearSelBtn.Visibility(Visibility::Collapsed); // nothing selected at build; _RebuildBoard syncs
            AgentSetTip(_clearSelBtn, L"Deselect the current card / row \x2014 nothing stays selected and the Auto Testing empties.");
            _clearSelBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _ClearSelection(); });
            header.Children().Append(_clearSelBtn);
            // The directory-scope label appears ONLY while a directory is scoped ("[scope: <dir>]"
            // next to the "Show all" clear button). The old unscoped "[all directories]"
            // placeholder is gone — it was display-only, restating the default.
            _boardScope = Text(L"", 12, false, 0.6);
            _boardScope.Visibility(Visibility::Collapsed);
            header.Children().Append(_boardScope);
            _showAllBtn = Button{};
            _showAllBtn.Content(winrt::box_value(L"Show all"));
            _showAllBtn.Padding(Thickness{ 6, 0, 6, 0 });
            AgentSetTip(_showAllBtn, L"Show sessions from every directory again \x2014 clears the directory filter.");
            // Hidden while we ARE showing all (the default scope is "" == all directories); it
            // reappears once a directory is scoped. _RebuildBoard keeps this in sync on every refresh.
            _showAllBtn.Visibility(_scopeDir.empty() ? Visibility::Collapsed : Visibility::Visible);
            _showAllBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _SetScope(L""); });
            header.Children().Append(_showAllBtn);
            Grid::SetRow(header, 0);
            outer.Children().Append(header);

            _boardHost = StackPanel{};
            _boardHost.Orientation(Orientation::Horizontal);
            _boardHost.Spacing(8);
            _boardHost.Margin(Thickness{ 0, 8, 0, 0 });

            auto sv = ScrollViewer{};
            sv.HorizontalScrollMode(ScrollMode::Enabled);
            sv.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
            sv.VerticalScrollBarVisibility(ScrollBarVisibility::Disabled);
            sv.Content(_boardHost);
            Grid::SetRow(sv, 1);
            outer.Children().Append(sv);

            auto b = section(outer);
            Grid::SetRow(b, 1);
            _root.Children().Append(b);
        }

        // ---- Bottom: Explorer Tree | Auto Testing (row 3) ----
        {
            auto bottom = Grid{};
            // Columns: tree (★) · splitter (auto) · plan (★). The two ★ cols are seeded from
            // the persisted fraction and are what the vertical splitter resizes.
            _treeCol = starCol(_layout.treeFraction);
            _planCol = starCol(1.0 - _layout.treeFraction);
            bottom.ColumnDefinitions().Append(_treeCol); // 0: tree
            bottom.ColumnDefinitions().Append(autoCol()); // 1: splitter
            bottom.ColumnDefinitions().Append(_planCol); // 2: plan

            // Explorer Tree
            {
                auto outer = Grid{};
                outer.RowDefinitions().Append(autoRow());
                outer.RowDefinitions().Append(starRow(1));
                // Header row: the title + a LOCAL/GLOBAL scope toggle (Agentmaster). LOCAL shows
                // only this window's sessions (the page's _claudeTabs, via _localScopeProvider);
                // GLOBAL shows every window's sessions (the whole process-wide registry). The
                // button's label is the current mode; clicking flips it and rebuilds the tree.
                auto hdrow = StackPanel{};
                hdrow.Orientation(Orientation::Horizontal);
                hdrow.Spacing(8);
                hdrow.VerticalAlignment(VerticalAlignment::Center);
                hdrow.Children().Append(Text(L"EXPLORER TREE", 12, true, 0.8));
                _treeScopeBtn = Button{};
                _treeScopeBtn.FontSize(11);
                _treeScopeBtn.Padding(Thickness{ 8, 1, 8, 1 });
                EmphasizeScopeButton(_treeScopeBtn); // Agentmaster: the primary header toggle — louder than sort/refresh
                AgentSetTip(_treeScopeBtn, L"Which sessions the tree shows \x2014 LOCAL (this window), GLOBAL (all windows), or EXTERNAL (claudes running outside Agentmaster, observe-only). Right-click an EXTERNAL row to Adopt it, start a session, or bring its window forward.");
                _treeScopeBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _ToggleTreeScope(); });
                hdrow.Children().Append(_treeScopeBtn);
                _UpdateTreeScopeButton();

                // Agentmaster: a sort toggle AFTER the scope toggle — NEWEST / OLDEST / MOST ACTIVE
                // (currently-running first) / A-Z. Orders the directory groups AND the rows within
                // each, in every scope (LOCAL/GLOBAL/EXTERNAL). GLOBAL setting: a click persists it
                // (AppSettings::treeSort) so the choice survives restart and applies to new windows.
                _treeSortBtn = Button{};
                _treeSortBtn.FontSize(11);
                _treeSortBtn.Padding(Thickness{ 8, 1, 8, 1 });
                AgentSetTip(_treeSortBtn, L"Sort order for directories and the sessions in them \x2014 NEWEST \xB7 OLDEST \xB7 MOST ACTIVE (running first) \xB7 A\x2013Z \xB7 BY PID (group by host window). Applies to every scope and is saved across windows.");
                _treeSortBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _CycleTreeSort(); });
                hdrow.Children().Append(_treeSortBtn);
                _UpdateTreeSortButton();

                // Agentmaster: a refresh button AFTER the sort toggle — reload the tree's data for the
                // CURRENTLY DISPLAYED scope (LOCAL/GLOBAL: re-pull the registry; EXTERNAL: re-pull the
                // observer's external census). Redraws immediately (recomputes the live "ago" timing)
                // and fires _refreshHandler so the page forces the Fleet Observer to re-survey NOW
                // (re-enrich + recompute the external census) instead of waiting for the next tick.
                _treeRefreshBtn = Button{};
                _treeRefreshBtn.FontSize(11);
                _treeRefreshBtn.Padding(Thickness{ 8, 1, 8, 1 });
                _treeRefreshBtn.Content(winrt::box_value(L"\x21BB")); // ↻ refresh glyph
                AgentSetTip(_treeRefreshBtn, L"Refresh now \x2014 re-scan and redraw the current view (also re-detects external sessions).");
                _treeRefreshBtn.Click([this](const IInspectable&, const RoutedEventArgs&) {
                    _Refresh(); // immediate redraw from current data (recomputes the "ago" timing)
                    if (_refreshHandler)
                    {
                        _refreshHandler(); // page: wake the observer + re-probe -> fresh data lands shortly
                    }
                });
                hdrow.Children().Append(_treeRefreshBtn);
                Grid::SetRow(hdrow, 0);
                outer.Children().Append(hdrow);

                _treeHost = StackPanel{};
                _treeHost.Margin(Thickness{ 0, 8, 0, 0 });
                auto sv = ScrollViewer{};
                sv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
                sv.Content(_treeHost);
                Grid::SetRow(sv, 1);
                outer.Children().Append(sv);

                auto b = section(outer);
                Grid::SetColumn(b, 0);
                bottom.Children().Append(b);
            }

            // Auto Testing
            {
                auto outer = Grid{};
                outer.RowDefinitions().Append(autoRow()); // header
                outer.RowDefinitions().Append(starRow(1)); // prompt list
                outer.RowDefinitions().Append(autoRow()); // action bar

                _planHeaderHost = StackPanel{};
                _planHeaderHost.Spacing(2);
                Grid::SetRow(_planHeaderHost, 0);
                outer.Children().Append(_planHeaderHost);

                _planListHost = StackPanel{};
                _planListHost.Margin(Thickness{ 0, 8, 0, 8 });
                auto sv = ScrollViewer{};
                sv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
                sv.Content(_planListHost);
                Grid::SetRow(sv, 1);
                outer.Children().Append(sv);
                _planScroll = sv; // Agentmaster: kept so the plan can default-scroll to the bottom (see _PinPlanToBottomOnSubjectChange)

                // Action bar (persistent).
                auto actions = StackPanel{};
                actions.Spacing(6);

                // Autorunner mode now lives as a toggle in the FLIGHT PLAN header (Agentmaster) —
                // _autorunnerBtn / _CycleAutorunner / _UpdateAutorunnerButton, mirroring the EXPLORER
                // TREE LOCAL/GLOBAL toggle but acting on the selected session. (No combo here.)

                // Compose row (Agentmaster): the action icons stick to the TOP-LEFT and the prompt
                // textarea fills the rest, growing downward as it becomes multiline —
                //   [eye = Focus]  [! = Send now (asks first)]  [envelope = Add to queue]  |  [textarea]
                // Per-message actions (Move up / Move down / Delete / Archive) live on the message
                // right-click menu now (see _MakePromptMenu, attached in _RebuildPlan), not a button row.
                auto fluentGlyph = [](const winrt::hstring& g) {
                    FontIcon fi;
                    fi.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
                    fi.Glyph(g);
                    fi.FontSize(16);
                    return fi;
                };
                // The "!" isn't an icon-font glyph, so render it as a FontIcon in the UI text font
                // (bold). A FontIcon centers its glyph the same way the Segoe Fluent ones above do,
                // so the exclamation lines up with the eye/envelope — a plain TextBlock rode high
                // because its line box reserves descent space below the glyph.
                auto textIconGlyph = [](const winrt::hstring& g) {
                    FontIcon fi;
                    fi.FontFamily(FontFamily{ L"Segoe UI" });
                    fi.Glyph(g);
                    fi.FontSize(16);
                    fi.FontWeight(FontWeights::Bold());
                    return fi;
                };
                auto mkIconBtn = [&](const winrt::hstring& tip, const IInspectable& glyph, std::function<void()> fn) {
                    auto btn = Button{};
                    btn.Content(glyph);
                    btn.Padding(Thickness{ 9, 6, 9, 6 });
                    btn.VerticalAlignment(VerticalAlignment::Top);
                    AgentSetTip(btn, tip);
                    btn.Click([fn](const IInspectable&, const RoutedEventArgs&) { fn(); });
                    return btn;
                };

                auto composeRow = Grid{};
                composeRow.ColumnDefinitions().Append(autoCol()); // icon column (sticks top-left)
                composeRow.ColumnDefinitions().Append(starCol(1)); // textarea fills the rest
                composeRow.ColumnDefinitions().Append(autoCol()); // templates (paper) icon, top-right

                auto iconCol = StackPanel{};
                iconCol.Orientation(Orientation::Horizontal);
                iconCol.Spacing(4);
                iconCol.VerticalAlignment(VerticalAlignment::Top); // stay at the top as the box grows
                iconCol.Margin(Thickness{ 0, 0, 6, 0 });
                // Eye = Focus the session (jump to its live tab).
                iconCol.Children().Append(mkIconBtn(L"Jump to this session's live terminal tab", fluentGlyph(L"\xE7B3"), [this]() {
                    if (_activateHandler && !_selectedId.empty())
                    {
                        _activateHandler(winrt::hstring{ _selectedId });
                    }
                }));
                // Exclamation point = Send now (a literal bold "!"; confirmed before it fires).
                iconCol.Children().Append(mkIconBtn(L"Send the composed prompt now \x2014 confirms first, and skips the queue", textIconGlyph(L"!"), [this]() { _OnSendNow(); }));
                // Envelope = Add the composed prompt to the queue.
                iconCol.Children().Append(mkIconBtn(L"Add the composed prompt to this session's queue", fluentGlyph(L"\xE715"), [this]() { _OnAddPrompt(); }));
                Grid::SetColumn(iconCol, 0);
                composeRow.Children().Append(iconCol);

                _addPromptBox = TextBox{};
                _addPromptBox.PlaceholderText(L"queue a prompt for the selected session\x2026");
                _addPromptBox.AcceptsReturn(true);
                _addPromptBox.TextWrapping(TextWrapping::Wrap);
                _addPromptBox.MinHeight(34);
                _addPromptBox.MaxHeight(160);
                _addPromptBox.VerticalAlignment(VerticalAlignment::Top);
                _addPromptBox.VerticalContentAlignment(VerticalAlignment::Top);
                // TextBox has no direct VerticalScrollBarVisibility in this projection — it's the
                // attached ScrollViewer property (see Gotchas: this XAML projection differs from WPF).
                ScrollViewer::SetVerticalScrollBarVisibility(_addPromptBox, ScrollBarVisibility::Auto);
                // Agentmaster (prompt history): recall the selected session's previously SENT prompts
                // (the shell/REPL idiom) — see _BuildPromptHistory / _ApplyPromptHistoryText. On the live
                // DRAFT (the "bottom prompt") Up enters history once the caret reaches the FIRST VISUAL ROW
                // (so it walks within a wrapped/multi-line draft and only recalls at the very top), while
                // Down is plain caret motion — nothing is newer than the draft, so only it is "moved by
                // Down". Once BROWSING history Up/Down walk older/newer FREELY (no caret gate — a recalled
                // prompt is navigated, not caret-edited; Esc cancels back to the draft), and Down off the
                // newest entry restores the draft. PreviewKeyDown (tunneling) runs BEFORE the TextBox's own
                // arrow handling — the only place we can read the caret's pre-move position AND suppress the
                // default caret motion via Handled (the rename box uses PreviewKeyDown for Enter likewise).
                _addPromptBox.PreviewKeyDown([this](const IInspectable&, const KeyRoutedEventArgs& e) {
                    const auto key = e.Key();
                    // Modifier snapshot (mirrors the rename box's CoreWindow::GetKeyState check).
                    bool shift = false, ctrl = false, alt = false;
                    if (const auto w = CoreWindow::GetForCurrentThread())
                    {
                        const auto down = winrt::Windows::UI::Core::CoreVirtualKeyStates::Down;
                        shift = WI_IsFlagSet(w.GetKeyState(VirtualKey::Shift), down);
                        ctrl = WI_IsFlagSet(w.GetKeyState(VirtualKey::Control), down);
                        alt = WI_IsFlagSet(w.GetKeyState(VirtualKey::Menu), down);
                    }
                    // Esc while browsing history cancels back to the draft you started from.
                    if (key == VirtualKey::Escape)
                    {
                        if (_promptHistoryIndex >= 0)
                        {
                            _promptHistoryIndex = -1;
                            _ApplyPromptHistoryText(_promptHistoryDraft);
                            e.Handled(true);
                        }
                        return;
                    }
                    if (key != VirtualKey::Up && key != VirtualKey::Down)
                    {
                        return; // only the bare Up/Down arrows drive history
                    }
                    if (shift || ctrl || alt)
                    {
                        return; // a MODIFIED arrow belongs to the TextBox (Shift selects, Ctrl/Alt move)
                    }
                    if (key == VirtualKey::Up)
                    {
                        if (_promptHistoryIndex < 0)
                        {
                            // On the draft: move the caret up within a wrapped/multi-line draft until the
                            // first VISUAL ROW; only THERE enter history (the "cursor at the top" trigger).
                            if (!_PromptCaretOnFirstRow())
                            {
                                return;
                            }
                            _promptHistory = _BuildPromptHistory();
                            if (_promptHistory.empty())
                            {
                                return; // nothing to recall — leave Up alone
                            }
                            _promptHistoryDraft = _addPromptBox ? std::wstring{ _addPromptBox.Text() } : std::wstring{};
                            _promptHistoryIndex = 0;
                            _ApplyPromptHistoryText(_promptHistory[_promptHistoryIndex]);
                        }
                        else if (_promptHistoryIndex + 1 < static_cast<int>(_promptHistory.size()))
                        {
                            // Browsing history: Up walks OLDER freely (no caret gate).
                            ++_promptHistoryIndex;
                            _ApplyPromptHistoryText(_promptHistory[_promptHistoryIndex]);
                        }
                        // else: already at the oldest — fall through to swallow so the caret doesn't jump.
                        e.Handled(true);
                    }
                    else // VirtualKey::Down (others returned above)
                    {
                        if (_promptHistoryIndex < 0)
                        {
                            return; // the bottom prompt (draft) — nothing newer; let Down move the caret
                        }
                        // Browsing history: Down walks NEWER freely (no caret gate). Off the newest entry
                        // it restores the draft you had before entering history (back to the bottom prompt).
                        if (_promptHistoryIndex == 0)
                        {
                            _promptHistoryIndex = -1;
                            _ApplyPromptHistoryText(_promptHistoryDraft);
                        }
                        else
                        {
                            --_promptHistoryIndex; // newer
                            _ApplyPromptHistoryText(_promptHistory[_promptHistoryIndex]);
                        }
                        e.Handled(true);
                    }
                });
                // Agentmaster (prompt history): a real user edit leaves history navigation — the recalled
                // text becomes the new draft, so the next Up at the top line walks history fresh. Our own
                // recall writes set _promptHistoryNavigating, which suppresses this reset.
                _addPromptBox.TextChanged([this](const IInspectable&, const TextChangedEventArgs&) {
                    if (!_promptHistoryNavigating)
                    {
                        _ResetPromptHistory();
                    }
                });
                // Discoverability: surface the keyboard affordances (they have no on-screen control).
                AgentSetTip(_addPromptBox, L"Compose a prompt for the selected session.\n\x2191 / \x2193  recall previously sent prompts");
                Grid::SetColumn(_addPromptBox, 1);
                composeRow.Children().Append(_addPromptBox);

                // Paper icon at the textarea's TOP-RIGHT: toggles the (collapsed-by-default)
                // Templates row open/closed (Agentmaster). Kept inline (not a Flyout) so its
                // TextBox keeps receiving keypresses — a text box in a popup/ContentDialog gets
                // none in XAML Islands (see Gotchas).
                auto paperBtn = mkIconBtn(L"Test Templates \x2014 save the current queue as a plan, or apply a saved one", fluentGlyph(L"\xE8A5"), [this]() {
                    if (_templatesRow)
                    {
                        _templatesRow.Visibility(_templatesRow.Visibility() == Visibility::Visible ? Visibility::Collapsed : Visibility::Visible);
                    }
                });
                paperBtn.Margin(Thickness{ 6, 0, 0, 0 });
                Grid::SetColumn(paperBtn, 2);
                composeRow.Children().Append(paperBtn);

                actions.Children().Append(composeRow);

                // mkBtn — the plain text buttons used by the Templates row below.
                auto mkBtn = [&](const winrt::hstring& label, const winrt::hstring& tip, std::function<void()> fn) {
                    auto btn = Button{};
                    btn.Content(winrt::box_value(label));
                    AgentSetTip(btn, tip);
                    btn.Click([fn](const IInspectable&, const RoutedEventArgs&) { fn(); });
                    return btn;
                };

                // Templates row (M8): save the current plan, apply a saved plan to this session or
                // broadcast it to every session in the directory. Collapsed by default — the paper
                // icon at the textarea's top-right toggles it open (Agentmaster).
                _templatesRow = StackPanel{};
                _templatesRow.Orientation(Orientation::Horizontal);
                _templatesRow.Spacing(6);
                _templatesRow.Visibility(Visibility::Collapsed);
                _templateNameBox = TextBox{};
                _templateNameBox.Width(150);
                _templateNameBox.PlaceholderText(L"test template name");
                AgentSetTip(_templateNameBox, L"Name to save the current queue under as a reusable test template");
                _templatesRow.Children().Append(_templateNameBox);
                _templatesRow.Children().Append(mkBtn(L"Save as test template", L"Save the selected session's current queue as a reusable plan, under the name on the left", [this]() { _OnSaveTemplate(); }));
                _templateCombo = ComboBox{};
                _templateCombo.MinWidth(140);
                AgentSetTip(_templateCombo, L"Pick a saved test template to apply");
                _templatesRow.Children().Append(_templateCombo);
                _templatesRow.Children().Append(mkBtn(L"Apply", L"Append the selected template's prompts to this session's queue", [this]() { _OnApplyTemplate(false); }));
                _templatesRow.Children().Append(mkBtn(L"Apply to dir", L"Append the selected template's prompts to EVERY session in this directory", [this]() { _OnApplyTemplate(true); }));
                actions.Children().Append(_templatesRow);
                _RefreshTemplateCombo();

                Grid::SetRow(actions, 2);
                outer.Children().Append(actions);

                // Top line (Agentmaster): a two-state [Summary | Auto Testing] segmented toggle that
                // REPLACES the old "FLIGHT PLAN" label — a COMPACT pill split in two, only one half
                // "checked" at a time. Both halves share ONE width (symmetric), sized to fit the LONGER
                // label ("Auto Testing", measured in its bold/selected form so it never clips), and the
                // pill is LEFT-aligned rather than stretched across the pane. The selected half is accent-
                // filled (holds through hover/press via PaintHoldButton — the scope-toggle accent) + bold;
                // the other reads as the inactive segment. Summary is the default and the choice is GLOBAL
                // (AppSettings::autoTestingShowsSummary), so it persists + syncs across every window (see
                // _SelectPlanPaneTab / _UpdatePlanPaneTab). The Summary tab is empty for now; the Flight
                // Plan tab holds the existing pane (Autorunner + queue + compose box).
                auto tabBar = Grid{};
                tabBar.HorizontalAlignment(HorizontalAlignment::Left); // compact — size to the two segments, don't stretch the pane width
                tabBar.ColumnDefinitions().Append(autoCol()); // Summary segment (fixed symmetric width)
                tabBar.ColumnDefinitions().Append(autoCol()); // Auto Testing segment
                // Symmetric segment width = the wider label's measured width (measure the SemiBold form —
                // the selected state — so a bold label never clips) + horizontal padding + a little slack.
                // Both segments take this one width, so "Summary" is simply padded out to match "Auto Testing".
                const double tabFont = 11.0;
                const double tabHPad = 10.0;
                const auto measureLabel = [tabFont](const winrt::hstring& s) -> double {
                    TextBlock t;
                    t.Text(s);
                    t.FontSize(tabFont);
                    t.FontWeight(FontWeights::SemiBold());
                    t.Measure(winrt::Windows::Foundation::Size{ 10000.0f, 10000.0f });
                    return static_cast<double>(t.DesiredSize().Width);
                };
                const double wSummary = measureLabel(L"Summary");
                const double wFlight = measureLabel(L"Auto Testing");
                const double tabLabelW = wFlight > wSummary ? wFlight : wSummary;
                const double tabSegW = (tabLabelW > 1.0 ? tabLabelW : 80.0) + tabHPad * 2 + 8.0; // + padding + slack (fallback if Measure runs pre-tree)
                auto mkTabBtn = [&](const winrt::hstring& label, const winrt::hstring& tip, const CornerRadius& cr, bool summary) {
                    auto btn = Button{};
                    btn.Content(winrt::box_value(label));
                    btn.FontSize(tabFont);
                    btn.Padding(Thickness{ tabHPad, 2, tabHPad, 2 });
                    btn.Width(tabSegW); // both segments equal -> symmetric, sized to fit the longer label
                    btn.HorizontalContentAlignment(HorizontalAlignment::Center);
                    btn.CornerRadius(cr); // outer edges rounded, the middle seam square -> reads as one segmented pill
                    AgentSetTip(btn, tip);
                    btn.Click([this, summary](const IInspectable&, const RoutedEventArgs&) { _SelectPlanPaneTab(summary); });
                    return btn;
                };
                _summaryTabBtn = mkTabBtn(L"Summary", L"Summary \x2014 a per-session overview (coming soon).", CornerRadius{ 6, 0, 0, 6 }, true);
                _autoTestTabBtn = mkTabBtn(L"Auto Testing", L"Auto Testing \x2014 the selected session's prompt queue, Tests Autorunner, and compose box.", CornerRadius{ 0, 6, 6, 0 }, false);
                Grid::SetColumn(_summaryTabBtn, 0);
                tabBar.Children().Append(_summaryTabBtn);
                Grid::SetColumn(_autoTestTabBtn, 1);
                tabBar.Children().Append(_autoTestTabBtn);

                // Auto Testing TAB body: a thin strip carrying the Autorunner toggle (relocated from the
                // old header — mirrors the EXPLORER TREE toggle but acts on the SELECTED session; a colored
                // state dot cycles Off -> Semi-auto -> Full, dim/disabled with no live session) over the
                // existing prompt list / compose box (`outer`).
                _autorunnerBtn = Button{};
                _autorunnerBtn.FontSize(11);
                _autorunnerBtn.Padding(Thickness{ 8, 1, 8, 1 });
                AgentSetTip(_autorunnerBtn, L"Tests Autorunner for the selected session \x2014 click to cycle: Off (manual) \xB7 Semi-auto (you confirm each send) \xB7 Full (auto-send the queue when a turn completes).");
                _autorunnerBtn.Click([this](const IInspectable&, const RoutedEventArgs&) { _CycleAutorunner(); });
                _UpdateAutorunnerButton(AutorunnerMode::Off, false);
                auto apStrip = StackPanel{};
                apStrip.Orientation(Orientation::Horizontal);
                apStrip.HorizontalAlignment(HorizontalAlignment::Right);
                apStrip.Margin(Thickness{ 0, 0, 0, 6 });
                apStrip.Children().Append(_autorunnerBtn);

                _autoTestBody = Grid{};
                _autoTestBody.RowDefinitions().Append(autoRow()); // 0: Autorunner strip
                _autoTestBody.RowDefinitions().Append(starRow(1)); // 1: the existing body (`outer`)
                Grid::SetRow(apStrip, 0);
                _autoTestBody.Children().Append(apStrip);
                Grid::SetRow(outer, 1);
                _autoTestBody.Children().Append(outer);

                // Summary TAB body (Agentmaster): the SAME session-summary box the Sessions page + the
                // per-tab overlay render (RenderSessionSummaryBox), shown for the selected managed Claude
                // session — analyzed off-thread + cached (see _RefreshSummaryTab / _LoadSummaryForSession),
                // user MESSAGES reversed to newest-first, and the WHOLE box inside ONE inner ScrollViewer
                // so the narrow Auto-Testing pane scrolls a long summary instead of clipping it.
                _summaryHost = Grid{};
                _summaryBoxHost = StackPanel{};
                _summaryBoxHost.Spacing(0); // the rendered box manages its own spacing (mono TextBlocks + rules)
                _summaryBoxHost.Margin(Thickness{ 0, 6, 6, 0 }); // a little top gap below the toggle + right gap clear of the scrollbar
                {
                    auto ssv = ScrollViewer{};
                    ssv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto); // the inner scrollbar
                    ssv.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
                    ssv.Content(_summaryBoxHost);
                    _summaryScroll = ssv;
                    _summaryHost.Children().Append(ssv);
                }

                // Both tab bodies share one grid cell; _UpdatePlanPaneTab toggles which is Visible.
                auto contentArea = Grid{};
                contentArea.Children().Append(_summaryHost);
                contentArea.Children().Append(_autoTestBody);

                auto wrap = Grid{};
                wrap.RowDefinitions().Append(autoRow()); // 0: the [Summary | Auto Testing] toggle (dev only)
                wrap.RowDefinitions().Append(starRow(1)); // 1: the selected tab's body
                // Auto Testing is a DEV-ONLY feature: the [Summary | Auto Testing] toggle is shown ONLY
                // under the AgentmasterDev package. In a RELEASE install this pane is the read-only
                // Summary view only — SetSettings/ApplyExternalSettings force autoTestingShowsSummary=true,
                // so _UpdatePlanPaneTab keeps the Summary body visible and collapses _autoTestBody (which
                // is still built so all members stay non-null and the methods are no-ops).
                if (::Agentmaster::Profiles::IsDevPackage())
                {
                    Grid::SetRow(tabBar, 0);
                    wrap.Children().Append(tabBar);
                }
                Grid::SetRow(contentArea, 1);
                wrap.Children().Append(contentArea);

                _UpdatePlanPaneTab(); // initial paint + visibility from _appSettings (default: Summary)

                auto b = section(wrap);
                Grid::SetColumn(b, 2);
                bottom.Children().Append(b);
            }

            // Vertical splitter between Tree and Auto Testing (drag = resize ↔).
            {
                auto vbar = _MakeSplitter(true);
                Grid::SetColumn(vbar, 1);
                bottom.Children().Append(vbar);
            }

            Grid::SetRow(bottom, 3);
            _root.Children().Append(bottom);
        }

        // ---- Horizontal splitter between Triage Board and the bottom (drag = resize ↕) ----
        {
            auto hbar = _MakeSplitter(false);
            Grid::SetRow(hbar, 2);
            _root.Children().Append(hbar);
        }

        // ---- Launch path-picker drop-down (a Popup anchored under the cwd box) ----
        // Parented into _root (top-left aligned) so its offset is _root-relative; it renders
        // in the overlay above the board. A fixed dark theme keeps the list readable over
        // whatever the app theme is.
        {
            _pathListHost = StackPanel{};
            _pathListHost.Spacing(0);

            auto sv = ScrollViewer{};
            sv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            sv.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            sv.MaxHeight(380);
            sv.AllowFocusOnInteraction(false); // scrollbar drags mustn't steal focus from the box either
            sv.IsTabStop(false); // ...and the scroll viewer must never become the focused element itself
            sv.Content(_pathListHost);

            _pathPanelBorder = Border{};
            _pathPanelBorder.Background(Fill(0xFF, 0x20, 0x20, 0x20));
            _pathPanelBorder.BorderBrush(Fill(0x90, 0x80, 0x80, 0x80));
            _pathPanelBorder.BorderThickness(Thickness{ 1, 1, 1, 1 });
            _pathPanelBorder.CornerRadius(CornerRadius{ 6, 6, 6, 6 });
            _pathPanelBorder.Padding(Thickness{ 4, 4, 4, 6 });
            _pathPanelBorder.Width(380);
            _pathPanelBorder.RequestedTheme(ElementTheme::Dark);
            _pathPanelBorder.Child(sv);

            _pathPopup = winrt::Windows::UI::Xaml::Controls::Primitives::Popup{};
            _pathPopup.HorizontalAlignment(HorizontalAlignment::Left);
            _pathPopup.VerticalAlignment(VerticalAlignment::Top);
            _pathPopup.Child(_pathPanelBorder);
            Grid::SetRow(_pathPopup, 0);
            _root.Children().Append(_pathPopup);
        }

        // Agentmaster (FAVORITES.md): both the in-content archive overlay AND the full-window Archive
        // page are GONE — the Sessions browser is the sole history view (closed sessions stay there,
        // resumable, marked by Favorite). Nothing archive-related is built here anymore.
        _BuildSettingsOverlay(); // modal settings layer, appended last so it renders on top
        _BuildClaudeMissingOverlay(); // native-exe-only gate modal (shown when no claude.exe is detected)
    }

    // ---- Refresh / rebuild --------------------------------------------------

    void AgentManagerContent::_Refresh()
    {
        if (!_root || !_boardHost || !_treeHost || !_planListHost)
        {
            return;
        }

        // Agentmaster: preserve keyboard focus across the rebuild below. _Refresh fires on ANY
        // registry notification — including a mere title change (a rename, or claude floating its
        // OSC title) — and _RebuildBoard/_RebuildTree CLEAR + recreate every card/row Button, which
        // would otherwise drop keyboard focus off the card the user just clicked (the selection
        // highlight survives via _selectedId; the focused ELEMENT does not). Capture the focused
        // card/row's lens+id from its "b:<id>"/"t:<id>" Tag, then re-focus the rebuilt element after.
        //
        // Agentmaster (focus-steal fix): do this ONLY when this Manager's window is the OS FOREGROUND
        // window. In XAML Islands, Control.Focus() escalates to Win32 activation of the island's host
        // window — so re-focusing a card here while this window is in the BACKGROUND (the Triage Board
        // window B sitting behind a Claude tab you're working in, in window A) would yank the OS
        // foreground to B on every ~2s observer/registry tick (the reported "an interval steals focus
        // to the triage board window"). When backgrounded the user isn't keyboard-navigating this
        // board, so there is nothing to preserve — leave refocusTag empty and the restore block below
        // no-ops. No provider (standalone/tests) ⇒ keep the original always-restore behavior.
        const bool windowIsForeground = !_windowForegroundProvider || _windowForegroundProvider();
        std::wstring refocusTag;
        auto refocusState = FocusState::Unfocused;
        if (const auto xr = windowIsForeground ? _root.XamlRoot() : nullptr)
        {
            if (const auto fe = winrt::Windows::UI::Xaml::Input::FocusManager::GetFocusedElement(xr).try_as<FrameworkElement>())
            {
                const std::wstring tag{ winrt::unbox_value_or<winrt::hstring>(fe.Tag(), winrt::hstring{}) };
                if (tag.rfind(L"b:", 0) == 0 || tag.rfind(L"t:", 0) == 0)
                {
                    refocusTag = tag;
                    if (const auto ctrl = fe.try_as<Control>())
                    {
                        refocusState = ctrl.FocusState();
                    }
                }
            }
        }

        std::vector<SessionInfo> sessions;
        if (_registry)
        {
            sessions = _registry->Snapshot();
        }
        _RebuildBoard(sessions);
        _SyncProgressTimer(); // Agentmaster: run the 1s countdown-bar drainer iff any Waiting-for-you bar is now tracked
        _RebuildTree(sessions);
        _RebuildPlan(sessions);
        _RefreshSummaryTab(); // Agentmaster: refresh the Summary tab (cheap; no-op unless that tab is active)
        _UpdateReopenButton();
        _UpdateActivateAllButton(); // Agentmaster (eager-init): recount this window's dormant tabs -> label + show/hide
        _RefreshKeepAwakeHold(&sessions); // Agentmaster: WhileRunning mode tracks the fleet live (no-op for Off/Always)

        // Re-focus the same card/row if a tagged one held focus and still exists post-rebuild (it may
        // have moved columns on a state change, or be gone if archived — then we leave focus be). The
        // maps are layout-independent (filled during the rebuild), so this is reliable pre-layout.
        if (!refocusTag.empty() && refocusState != FocusState::Unfocused)
        {
            const auto& map = (refocusTag.front() == L'b') ? _boardCardsById : _treeRowsById;
            const auto it = map.find(refocusTag.substr(2));
            if (it != map.end() && it->second)
            {
                // A freshly-rebuilt card/row isn't focusable until it Loads (the same reason the
                // in-place rename box focuses in its Loaded). Try now; if it isn't ready yet, re-try
                // on Loaded. The handler captures only the state (the sender IS the element), so there
                // is no element<->handler cycle to leak the card.
                if (!it->second.Focus(refocusState))
                {
                    it->second.Loaded([refocusState](const IInspectable& s, const RoutedEventArgs&) {
                        if (const auto c = s.try_as<Control>())
                        {
                            c.Focus(refocusState);
                        }
                    });
                }
            }
        }
    }

    // Agentmaster (Waiting-for-you countdown bar): drain every tracked bar to its current fraction.
    // Cheap — sets ScaleX on a handful of ScaleTransforms (a render-transform write, no layout/rebuild).
    // Ticked ~1s by _progressTimer. A bar at 0 has expired (a read card decays to Idle on the scanner's
    // next tick, which rebuilds the board and drops the track); an unread one sits empty until read.
}
