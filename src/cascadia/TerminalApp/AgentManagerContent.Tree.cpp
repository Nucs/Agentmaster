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
//   AgentManagerContent.Board.cpp       - the Triage Board: cards, columns, splitters, _RebuildBoard
// ★ AgentManagerContent.Tree.cpp        - the Explorer Tree: managed/external trees, context menus, scope/sort toggles, rename, confirm dialogs
//   AgentManagerContent.Settings.cpp    - keep-awake/reopen/activate buttons + the Settings cog overlay (tabs, save, env editor, UPDATES, claude-missing)
//   AgentManagerContent.AutoTesting.cpp  - the Auto Testing: plan + selection sync, prompt compose/history, Autorunner, the Summary tab, templates
//   AgentManagerContent.Launch.cpp      - the Launch bar: cwd validation, the Claude/Codex toggle, launch/create/fork, the path-picker drop-down
// ======================================================================================
//
// Agentmaster Manager tab: the EXPLORER TREE -- managed + external trees, the session/external/prompt context menus, the scope + sort toggles, in-place rename, and the buttons-only confirm dialogs. Partial TU of AgentManagerContent.cpp.
#include "pch.h"
#include "AgentManagerContent.h"

#include "AgentTipHelpers.h" // AgentSetTip — hover tooltips with working dismissal (XAML Islands)
#include "AgentCatchLog.h" // AgentLogCaughtException — the lazy menus' Opening handlers (Rule #18: never a bare catch)
#include "AgentCopyActions.h" // CopySessionField — the shared copy-menu action (same path as the per-tab overlay's copy button)
#include "AgentModelMenu.h" // AgentFillModelPickItems / AgentModelEditHint — the shared "Open New Session Here ▸ <model>" picker (launch-model picker)
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
using winrt::TerminalApp::implementation::AgentSetTitledTip;
#include "AgentManagerContent.Internal.h" // the shared file-local helpers (StateColor/Pill/Text/...)

namespace winrt::TerminalApp::implementation
{
    void AgentManagerContent::_RebuildTree(const std::vector<SessionInfo>& sessions)
    {
        // While an in-place rename editor is live, leave the tree untouched so the frequent
        // hook-driven refreshes can't steal its focus or discard typed text. Commit/cancel
        // clears _renamingId and rebuilds. (_OnRenameSession nulls _renameBox to bootstrap
        // the one rebuild that creates the editor.)
        if (!_renamingId.empty() && _renameBox)
        {
            return;
        }
        _treeHost.Children().Clear();
        _rowTimingBinds.clear(); // re-seeded by the rows below (the 30s tick rewrites their timing text in place — perf)
        _treeRowsById.clear(); // refilled below (focus-restore map; see _Refresh). Cleared ONLY on
                               // the full-rebuild path — the rename early-return above keeps the tree
                               // (and so its existing map) intact.

        // Agentmaster: EXTERNAL scope renders the Fleet Observer's observe-only external claudes
        // (the _externalClaudes table, a different source than the registry snapshot) grouped by cwd,
        // with an Adopt / Open New Session Here / Bring Window To Front right-click menu. Delegate
        // and return.
        if (_treeScope == TreeScope::External)
        {
            _RebuildExternalTree();
            return;
        }

        // Agentmaster: Explorer Tree scope + ordering. localIds == the sessions hosted in THIS
        // window (the page's _claudeTabs, surfaced by _localScopeProvider). LOCAL (default) keeps
        // ONLY those; GLOBAL keeps every window's session (the whole process-wide registry) but
        // ORDERS this window's first and tags the rest "outside" (see the dir partition + the row
        // bucketing below). With no provider (mid-init) everything counts as local — no filter, no
        // reorder, no tag. isLocal() answers "is this session hosted in this window?".
        const bool haveLocal = static_cast<bool>(_localScopeProvider);
        std::unordered_set<std::wstring> localIds;
        if (haveLocal)
        {
            localIds = _localScopeProvider();
        }
        const auto isLocal = [&](const SessionInfo& s) { return !haveLocal || localIds.find(s.id) != localIds.end(); };

        std::vector<SessionInfo> scopedStore;
        const std::vector<SessionInfo>* scopedPtr = &sessions;
        if (_treeScope == TreeScope::Local && haveLocal)
        {
            scopedStore.reserve(sessions.size());
            for (const auto& s : sessions)
            {
                if (isLocal(s))
                {
                    scopedStore.push_back(s);
                }
            }
            scopedPtr = &scopedStore;
        }
        const std::vector<SessionInfo>& scoped = *scopedPtr;

        // Ordered, de-duplicated working directories. Paths that differ only by case (on
        // Windows) collapse into one root; the first-seen spelling becomes its display name.
        // Grouped by the EFFECTIVE work dir (_WorkDirOf — the inferred dir under the Inferred tab-
        // color mode, else the launch cwd), so a session detected working OUTSIDE its cwd sits
        // under the directory it actually works in — the same key its tab color wears.
        std::vector<std::wstring> dirs;
        for (const auto& s : scoped)
        {
            if (!s.live)
            {
                continue; // closed sessions are shown in the Sessions browser, not the tree (FAVORITES.md)
            }
            if (std::find_if(dirs.begin(), dirs.end(), [&](const std::wstring& d) { return PathEq(d, _WorkDirOf(s)); }) == dirs.end())
            {
                dirs.push_back(_WorkDirOf(s));
            }
        }

        if (dirs.empty())
        {
            // In LOCAL scope an empty tree can simply mean other windows hold the sessions; say so
            // (and hint at GLOBAL) rather than implying the whole fleet is empty.
            const wchar_t* empty = (_treeScope == TreeScope::Local && haveLocal)
                                       ? L"No sessions in this window \x2014 Launch above, or switch to GLOBAL for all windows."
                                       : L"No sessions yet \x2014 use Launch Claude above.";
            _treeHost.Children().Append(Text(empty, 12, false, 0.6));
            return;
        }

        // Agentmaster: order the directory groups by the active (global) sort. A dir's rank is an
        // aggregate over its live sessions — NEWEST: its newest session; OLDEST: its oldest; MOST
        // ACTIVE: its most-recent activity (a running session pins it to the top); A-Z: the dir name.
        // The local-first stable_partition below runs AFTER this and preserves the order within each
        // group, so the sort governs ordering while GLOBAL still floats this window's dirs first.
        const auto sortMode = _appSettings.treeSort;
        {
            struct DirAgg
            {
                int64_t maxCreated{ 0 };
                int64_t minCreated{ INT64_MAX };
                int64_t bestLast{ 0 };
                uint32_t minPid{ UINT32_MAX }; // BY PID: a dir's rank is its lowest host/claude pid
            };
            std::vector<std::pair<std::wstring, DirAgg>> ranked;
            ranked.reserve(dirs.size());
            for (const auto& dir : dirs)
            {
                DirAgg agg;
                for (const auto& s : scoped)
                {
                    if (s.live && PathEq(_WorkDirOf(s), dir))
                    {
                        const auto k = MakeSortKey(s);
                        agg.maxCreated = (std::max)(agg.maxCreated, k.created);
                        agg.minCreated = (std::min)(agg.minCreated, k.created);
                        agg.bestLast = (std::max)(agg.bestLast, k.active ? INT64_MAX : k.last);
                        agg.minPid = (std::min)(agg.minPid, k.pid);
                    }
                }
                ranked.push_back({ dir, agg });
            }
            std::stable_sort(ranked.begin(), ranked.end(), [&](const std::pair<std::wstring, DirAgg>& A, const std::pair<std::wstring, DirAgg>& B) {
                const auto& a = A.second;
                const auto& b = B.second;
                switch (sortMode)
                {
                case ::Agentmaster::ExplorerSort::Newest:
                    if (a.maxCreated != b.maxCreated)
                        return a.maxCreated > b.maxCreated;
                    break;
                case ::Agentmaster::ExplorerSort::Oldest:
                    if (a.minCreated != b.minCreated)
                        return a.minCreated < b.minCreated;
                    break;
                case ::Agentmaster::ExplorerSort::MostActive:
                    if (a.bestLast != b.bestLast)
                        return a.bestLast > b.bestLast;
                    break;
                case ::Agentmaster::ExplorerSort::ByPid:
                    if (a.minPid != b.minPid)
                        return a.minPid < b.minPid;
                    if (a.bestLast != b.bestLast)
                        return a.bestLast > b.bestLast; // then most active
                    break;
                case ::Agentmaster::ExplorerSort::Alpha:
                    break;
                }
                return CiLess(A.first, B.first); // A-Z primary, and the deterministic tiebreak for all modes
            });
            dirs.clear();
            for (auto& p : ranked)
            {
                dirs.push_back(std::move(p.first));
            }
        }

        // Agentmaster: surface directories holding at least one of THIS window's sessions before
        // directories that are entirely from other windows (stable within each group). In LOCAL
        // scope every dir is local, so this is a no-op; it only reorders the GLOBAL view.
        std::stable_partition(dirs.begin(), dirs.end(), [&](const std::wstring& d) {
            for (const auto& s : scoped)
            {
                if (s.live && PathEq(_WorkDirOf(s), d) && isLocal(s))
                {
                    return true;
                }
            }
            return false;
        });

        // Tracks the directory header rendered just above the current one, so collapsing the
        // selected directory can move the scope to its predecessor ("" — all directories — when
        // the selected one is the topmost header).
        std::wstring prevDir;
        for (const auto& dir : dirs)
        {
            const bool collapsed = _collapsedDirs.find(dir) != _collapsedDirs.end();

            // dir header. A click resolves the select-vs-collapse collision by state:
            //   collapsed             -> uncollapse + select
            //   expanded + unselected -> select (stay expanded)
            //   expanded + selected   -> collapse + select the previous directory
            int count = 0;
            for (const auto& s : scoped)
            {
                if (s.live && PathEq(_WorkDirOf(s), dir))
                {
                    ++count;
                }
            }

            auto dh = StackPanel{};
            dh.Orientation(Orientation::Horizontal);
            dh.Spacing(6);
            dh.Children().Append(Text(collapsed ? L"\x25B8" : L"\x25BE", 12, false, 0.8)); // ▸ / ▾
            dh.Children().Append(Text(winrt::hstring{ dir }, 13, true, 0.95));
            dh.Children().Append(Text(winrt::to_hstring(count), 12, false, 0.5));

            auto dirBtn = Button{};
            dirBtn.Content(dh);
            dirBtn.HorizontalAlignment(HorizontalAlignment::Stretch);
            dirBtn.HorizontalContentAlignment(HorizontalAlignment::Left);
            dirBtn.Background(Fill(PathEq(dir, _scopeDir) ? 0x30 : 0x00, 0x80, 0x80, 0x80));
            dirBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
            dirBtn.Padding(Thickness{ 4, 2, 4, 2 });
            AgentSetTitledTip(dirBtn, L"Working directory", L"A folder your sessions run in, and how many are open in it. Click to narrow the board and this tree to just its sessions; click the folder that is already scoped to collapse it and fall back to the one above it (the topmost falls back to all directories).");
            const auto capturedDir = dir;
            const auto capturedPrevDir = prevDir; // predecessor at build time, for "collapse + select previous"
            dirBtn.Click([this, capturedDir, capturedPrevDir](const IInspectable&, const RoutedEventArgs&) {
                const bool isCollapsed = _collapsedDirs.find(capturedDir) != _collapsedDirs.end();
                const bool isSelected = PathEq(capturedDir, _scopeDir);
                if (isCollapsed)
                {
                    // click + collapsed -> uncollapse + select
                    _collapsedDirs.erase(capturedDir);
                    _SetScope(capturedDir);
                }
                else if (!isSelected)
                {
                    // click + expanded + unselected -> select (leave it expanded)
                    _SetScope(capturedDir);
                }
                else
                {
                    // click + expanded + selected -> collapse + select the previous directory
                    // (capturedPrevDir is "" for the topmost header, which scopes to all dirs)
                    _collapsedDirs.insert(capturedDir);
                    _SetScope(capturedPrevDir);
                }
            });
            _treeHost.Children().Append(dirBtn);

            // This header is the predecessor of the next one. Set before the collapse-skip
            // below so a collapsed directory still counts as a predecessor.
            prevDir = dir;

            if (collapsed)
            {
                continue;
            }

            // Order this dir's rows THIS window's sessions first, then "outside" ones (other
            // windows), each group ordered by the active (global) sort (Agentmaster). The pointers
            // stay valid: `scoped` is not mutated past this point. In LOCAL scope the outside group
            // is empty, so the sort governs the whole list.
            std::vector<const SessionInfo*> rowOrder;
            {
                std::vector<const SessionInfo*> localRows, outsideRows;
                for (const auto& s : scoped)
                {
                    if (s.live && PathEq(_WorkDirOf(s), dir))
                    {
                        (isLocal(s) ? localRows : outsideRows).push_back(&s);
                    }
                }
                const auto rowLess = [&](const SessionInfo* a, const SessionInfo* b) {
                    return SortKeyLess(sortMode, MakeSortKey(*a), MakeSortKey(*b));
                };
                std::stable_sort(localRows.begin(), localRows.end(), rowLess);
                std::stable_sort(outsideRows.begin(), outsideRows.end(), rowLess);
                rowOrder.reserve(localRows.size() + outsideRows.size());
                rowOrder.insert(rowOrder.end(), localRows.begin(), localRows.end());
                rowOrder.insert(rowOrder.end(), outsideRows.begin(), outsideRows.end());
            }

            for (const auto* sp : rowOrder)
            {
                const auto& s = *sp;
                const auto id = s.id;

                // In-place rename editor for this row (see _renameBox note in the header).
                // Editing inline sidesteps the XAML-Islands trap where a text box inside a
                // ContentDialog receives no keypresses.
                if (!_renamingId.empty() && _renamingId == id)
                {
                    auto box = TextBox{};
                    box.Text(s.title);
                    box.Margin(Thickness{ 16, 0, 0, 4 });
                    // Multi-line titles: a NON-commit Return INSERTS a newline (AcceptsReturn) rather
                    // than committing — matching the WT tab-rename box (TabHeaderControl). Focus-loss
                    // (click away / select another row) ALWAYS commits; Enter / Shift+Enter optionally
                    // commit per the GLOBAL TabRenameCommitMode (Rule #11 keeps this path == the tab
                    // renamer). In every case the TextBox or our PreviewKeyDown marks Return handled, so
                    // it won't bubble to the row's Enter=Activate. Escape still cancels.
                    box.AcceptsReturn(true);
                    box.TextWrapping(TextWrapping::Wrap);
                    _renameCommitOnKeyUp = false;
                    // PreviewKeyDown (tunneling) runs before the box's own key handling — the one place
                    // we can both see Enter reliably and SUPPRESS the AcceptsReturn newline for the commit
                    // combo. We don't commit on the down event (it rebuilds the tree and tears out this
                    // box mid-keystroke); the matching KeyUp commits, exactly like TabHeaderControl.
                    box.PreviewKeyDown([this](const IInspectable&, const KeyRoutedEventArgs& e) {
                        if (e.Key() != VirtualKey::Enter)
                        {
                            return;
                        }
                        const auto mode = _appSettings.tabRenameCommitMode;
                        if (mode == TabRenameCommitMode::ClickAwayOnly)
                        {
                            return; // both keys just insert a newline; commit by clicking away
                        }
                        // Shift distinguishes the commit key from the newline key (both arrive as Enter).
                        auto shiftDown = false;
                        if (const auto w = CoreWindow::GetForCurrentThread())
                        {
                            shiftDown = WI_IsFlagSet(w.GetKeyState(VirtualKey::Shift), winrt::Windows::UI::Core::CoreVirtualKeyStates::Down);
                        }
                        const bool commit = (mode == TabRenameCommitMode::ClickAwayOrShiftEnter) ? shiftDown : !shiftDown;
                        if (commit)
                        {
                            _renameCommitOnKeyUp = true;
                            e.Handled(true); // suppress the newline + stop the down bubbling; KeyUp commits
                        }
                    });
                    box.KeyDown([this](const IInspectable&, const KeyRoutedEventArgs& e) {
                        if (e.Key() == VirtualKey::Escape)
                        {
                            _CancelRename();
                            e.Handled(true);
                        }
                    });
                    box.KeyUp([this](const IInspectable&, const KeyRoutedEventArgs& e) {
                        if (_renameCommitOnKeyUp)
                        {
                            _renameCommitOnKeyUp = false;
                            e.Handled(true);
                            _CommitRename(); // == clicking away (idempotent; the LostFocus that follows no-ops)
                        }
                    });
                    box.LostFocus([this](const IInspectable&, const RoutedEventArgs&) { _CommitRename(); });
                    // Focus + select-all once it is actually in the tree (Loaded) so the first
                    // keystroke replaces the old name. The box arrives as the SENDER — never capture
                    // the element into its own handler (the row handlers' self-capture rule below):
                    // box -> Loaded -> box was a refcount cycle leaking one TextBox per rename gesture.
                    box.Loaded([](const IInspectable& sender, const RoutedEventArgs&) {
                        if (const auto b = sender.try_as<TextBox>())
                        {
                            b.Focus(FocusState::Programmatic);
                            b.SelectAll();
                        }
                    });
                    _renameBox = box;
                    _treeHost.Children().Append(box);
                    continue;
                }

                const bool selected = (s.id == _selectedId);

                auto row = StackPanel{};
                row.Orientation(Orientation::Horizontal);
                row.Spacing(6);
                // State dot — the SAME filled Ellipse as the tab strip (StateDot == HeaderAgentStatusDot),
                // not the old per-state glyph, so the tree and the tab speak one visual language. The
                // StateLabel text appended below still names the state. A DORMANT session (live but its
                // claude hasn't started — a restored tab you haven't opened) shows the half-hollow twin.
                if (IsSessionDormant(s))
                {
                    auto dot = StateDotDormant(StateColor(s.state));
                    AgentSetTitledTip(dot, L"Not started yet", L"A half-hollow dot means this session's claude hasn't started \x2014 a restored tab you haven't opened yet. It only starts when the tab is first shown, so it costs nothing until you want it. Shift+Click the row, right-click \x2192 Activate Tab, or just open the tab to start it now; the dot fills once it is running.");
                    row.Children().Append(dot);
                }
                else
                {
                    auto dot = StateDot(StateColor(s.state));
                    AgentSetTitledTip(dot, L"Session state", L"The same color this session's card wears on the Triage Board, and the same dot its terminal tab carries \x2014 blue Running \xB7 gold Waiting-for-you \xB7 orange-red Needs-approval \xB7 crimson Error \xB7 green Done \xB7 gray Idle. The label beside it names the state; hover a board column header for what each one means.");
                    row.Children().Append(dot);
                }
                row.Children().Append(Text(OneLine(s.title.empty() ? std::wstring_view{ L"(untitled)" } : std::wstring_view{ s.title }), 13, false, 1.0));
                // Agentmaster (Codex-launch): a teal "codex" agent pill on a MANAGED Codex row, mirroring
                // the Board card — distinguishes it from a Claude row at a glance (Claude = no pill).
                if (s.kind == AgentKind::Codex)
                {
                    auto cp = Pill(L"codex", Color{ 0xFF, 0x4E, 0xC9, 0xB0 });
                    cp.Opacity(0.9);
                    AgentSetTitledTip(cp, L"Codex agent", L"This managed session runs the OpenAI Codex CLI instead of Claude. Agentmaster launches, resumes and tracks it like any session, but cannot drive its prompts \x2014 there is no queue or Tests Autorunner for Codex, and it reports only Running / Waiting / Idle.");
                    row.Children().Append(cp);
                }
                row.Children().Append(Text(StateLabel(s.state), 11, false, 0.5));
                // Agentmaster: a gray "outside" tag marks a session hosted in another window (only
                // possible in GLOBAL scope; in LOCAL every row is this window's). It sits at the end
                // of the row, after the state label.
                if (!isLocal(s))
                {
                    auto outside = Pill(L"outside", Color{ 0xFF, 0x8A, 0x8A, 0x8A });
                    outside.Opacity(0.85);
                    AgentSetTitledTip(outside, L"Another window", L"This session's tab lives in a different Agentmaster window \x2014 which is why it only appears here in GLOBAL scope. Double-click the row to bring that window forward and land on the tab.");
                    row.Children().Append(outside);
                }
                // Per-session timing (created-ago / active-for / last-activity-ago) from the transcript.
                {
                    const int64_t last = s.convLastActivityUnixMs ? s.convLastActivityUnixMs : s.lastActivityUnixMs;
                    if (auto t = TimingText(s.convCreatedUnixMs, last))
                    {
                        row.Children().Append(t);
                        _rowTimingBinds.push_back({ id, t }); // the 30s tick rewrites this text in place (perf — no rebuild)
                    }
                }

                auto rowBtn = Button{};
                rowBtn.Content(row);
                rowBtn.HorizontalAlignment(HorizontalAlignment::Stretch);
                rowBtn.HorizontalContentAlignment(HorizontalAlignment::Left);
                rowBtn.Margin(Thickness{ 16, 0, 0, 0 });
                rowBtn.Padding(Thickness{ 4, 2, 4, 2 });
                rowBtn.Background(Fill(selected ? 0x40 : 0x00, 0x80, 0x80, 0x80));
                rowBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });

                // Agentmaster (Linked Lenses): report hover so the page pills this session's terminal
                // tab while the Manager tab is active (the board-card twin, above). Capture id by value
                // + `this`, never the Button into its own handler (a self-capture leaks the element).
                rowBtn.PointerEntered([this, id](const IInspectable&, const PointerRoutedEventArgs&) { _ReportHover(id, true); });
                rowBtn.PointerExited([this, id](const IInspectable& sender, const PointerRoutedEventArgs& e) {
                    if (PointerStillWithin(sender, e))
                    {
                        return; // a child label's exit bubbled up — don't un-pill the tab as the mouse crosses the row's labels
                    }
                    _ReportHover(id, false);
                });
                // Single click = select; double click (within the OS threshold) = Activate
                // (jump to the live tab). A Button swallows DoubleTapped, so we time the
                // successive clicks ourselves.
                rowBtn.Click([this, id](const IInspectable&, const RoutedEventArgs&) {
                    // Shift+Click = Activate Tab IN PLACE (start a dormant session's claude without
                    // switching to it) — the click twin of the "Activate Tab (Shift+Click)" menu item.
                    // _activateDormantHandler no-ops when not dormant / not hosted here, so it's safe here.
                    if (ShiftHeld())
                    {
                        if (_activateDormantHandler)
                        {
                            _activateDormantHandler(winrt::hstring{ id });
                        }
                        return; // don't select / jump — Shift+Click is the in-place activate gesture
                    }
                    const auto nowTick = ::GetTickCount64();
                    const bool dbl = (id == _lastTreeClickId) && (nowTick - _lastTreeClickTick) <= ::GetDoubleClickTime();
                    _lastTreeClickId = id;
                    _lastTreeClickTick = nowTick;
                    if (dbl && _activateHandler)
                    {
                        _activateHandler(winrt::hstring{ id });
                    }
                    else
                    {
                        _SelectSession(id);
                        // Agentmaster (double-click fix): _SelectSession rebuilt the tree — every row
                        // Button is cleared + recreated (_RebuildTree), so this row's replacement isn't
                        // arranged until the next async layout pass and a follow-up second click (a
                        // double-click to Activate) would miss it. Force a synchronous layout so the
                        // replacement row has real bounds immediately and the second click lands — the
                        // board-card twin above. (An already-selected re-click early-outs in
                        // _SelectSession with no rebuild, so only this not-selected path needs it.)
                        if (_treeHost)
                        {
                            _treeHost.UpdateLayout();
                        }
                    }
                });
                // Enter = Activate (jump to live tab) — NEVER inject (Correctness Rule #2).
                // Delete = confirm-guarded delete; F2 = rename.
                rowBtn.KeyDown([this, id](const IInspectable&, const KeyRoutedEventArgs& e) {
                    const auto key = e.Key();
                    if (key == VirtualKey::Enter)
                    {
                        if (_activateHandler)
                        {
                            _activateHandler(winrt::hstring{ id });
                        }
                        e.Handled(true);
                    }
                    else if (key == VirtualKey::Delete)
                    {
                        _RequestArchive(id); // Del archives (shut down + keep restorable), not discard
                        e.Handled(true);
                    }
                    else if (key == VirtualKey::F2)
                    {
                        _OnRenameSession(id);
                        e.Handled(true);
                    }
                });
                // Right-click (or context key / long-press) menu: Rename / Archive / Open New Session Here.
                rowBtn.ContextFlyout(_MakeLazySessionMenu(id, _WorkDirOf(s), rowBtn)); // built at OPEN time (perf); the row anchors its Tags panel; Open-New-Here targets the EFFECTIVE work dir (the group this row sits under)
                AgentSetTitledTip(rowBtn, winrt::hstring{ s.title.empty() ? std::wstring{ L"(untitled)" } : s.title }, L"Click selects this session (the pane on the right follows it), double-click or Enter jumps to its live tab. F2 renames it, Del closes it, Shift+Click starts it if it hasn't started yet, and right-click has the rest.");
                // Agentmaster: tag + register the row so _Refresh can RESTORE keyboard focus onto it
                // after a rebuild (see _MakeCard for the board-lens twin). "t:" marks the tree lens.
                rowBtn.Tag(winrt::box_value(winrt::hstring{ L"t:" + id }));
                _treeRowsById[id] = rowBtn;
                _treeHost.Children().Append(rowBtn);
            }
        }
    }

    // Agentmaster: render the Explorer Tree's EXTERNAL scope — observe-only external claudes (real
    // Windows Terminal AND cmd-/console-hosted; the observer correlated them but will never bind,
    // Rule #9/#13), grouped by working dir, each row enriched from its transcript (title / host /
    // branch / timing). Left-click SELECTS a row -> the Auto Testing shows its conversation read-only
    // (we host no ConPTY, so it is never drivable). Right-click -> Open New Session Here (spawn a
    // managed session in that cwd) / Adopt (resume its conversation into a managed tab).
    void AgentManagerContent::_RebuildExternalTree()
    {
        if (_externalClaudes.empty())
        {
            _treeHost.Children().Append(Text(L"No external claudes detected \x2014 these are claudes running in real Windows Terminal or other hosts.", 12, false, 0.6));
            return;
        }

        // Ordered, de-duplicated working dirs (filesystem-aware, like the session tree). The
        // first-seen spelling is the display name; an empty cwd (denied/unreadable PEB) groups under
        // "(unknown)". Rows arrive pid-sorted from the observer, a stable order.
        const auto dirOf = [](const ::Agentmaster::ExternalClaudeRow& ex) -> std::wstring {
            return ex.cwd.empty() ? std::wstring{ L"(unknown)" } : ex.cwd;
        };
        std::vector<std::wstring> dirs;
        for (const auto& ex : _externalClaudes)
        {
            const std::wstring d = dirOf(ex);
            if (std::find_if(dirs.begin(), dirs.end(), [&](const std::wstring& x) { return PathEq(x, d); }) == dirs.end())
            {
                dirs.push_back(d);
            }
        }

        // Agentmaster: order the external dirs by the active (global) sort, exactly like the managed
        // tree — a dir's rank aggregates its externals (NEWEST/OLDEST by creation, MOST ACTIVE by
        // recency, A-Z by name). Rows within each dir are sorted the same way below.
        const auto sortMode = _appSettings.treeSort;
        {
            struct DirAgg
            {
                int64_t maxCreated{ 0 };
                int64_t minCreated{ INT64_MAX };
                int64_t bestLast{ 0 };
                uint32_t minPid{ UINT32_MAX }; // BY PID: a dir's rank is its lowest host-window pid
            };
            std::vector<std::pair<std::wstring, DirAgg>> ranked;
            ranked.reserve(dirs.size());
            for (const auto& dir : dirs)
            {
                DirAgg agg;
                for (const auto& ex : _externalClaudes)
                {
                    if (PathEq(dirOf(ex), dir))
                    {
                        const auto k = MakeSortKey(ex);
                        agg.maxCreated = (std::max)(agg.maxCreated, k.created);
                        agg.minCreated = (std::min)(agg.minCreated, k.created);
                        agg.bestLast = (std::max)(agg.bestLast, k.active ? INT64_MAX : k.last);
                        agg.minPid = (std::min)(agg.minPid, k.pid);
                    }
                }
                ranked.push_back({ dir, agg });
            }
            std::stable_sort(ranked.begin(), ranked.end(), [&](const std::pair<std::wstring, DirAgg>& A, const std::pair<std::wstring, DirAgg>& B) {
                const auto& a = A.second;
                const auto& b = B.second;
                switch (sortMode)
                {
                case ::Agentmaster::ExplorerSort::Newest:
                    if (a.maxCreated != b.maxCreated)
                        return a.maxCreated > b.maxCreated;
                    break;
                case ::Agentmaster::ExplorerSort::Oldest:
                    if (a.minCreated != b.minCreated)
                        return a.minCreated < b.minCreated;
                    break;
                case ::Agentmaster::ExplorerSort::MostActive:
                    if (a.bestLast != b.bestLast)
                        return a.bestLast > b.bestLast;
                    break;
                case ::Agentmaster::ExplorerSort::ByPid:
                    if (a.minPid != b.minPid)
                        return a.minPid < b.minPid;
                    if (a.bestLast != b.bestLast)
                        return a.bestLast > b.bestLast; // then most active
                    break;
                case ::Agentmaster::ExplorerSort::Alpha:
                    break;
                }
                return CiLess(A.first, B.first);
            });
            dirs.clear();
            for (auto& p : ranked)
            {
                dirs.push_back(std::move(p.first));
            }
        }

        for (const auto& dir : dirs)
        {
            const bool collapsed = _collapsedDirs.find(dir) != _collapsedDirs.end();
            int count = 0;
            for (const auto& ex : _externalClaudes)
            {
                if (PathEq(dirOf(ex), dir))
                {
                    ++count;
                }
            }

            // dir header (collapsible). Unlike the session tree it does NOT change the board scope:
            // externals are a global census, not part of the managed directory tree.
            auto dh = StackPanel{};
            dh.Orientation(Orientation::Horizontal);
            dh.Spacing(6);
            dh.Children().Append(Text(collapsed ? L"\x25B8" : L"\x25BE", 12, false, 0.8)); // ▸ / ▾
            dh.Children().Append(Text(winrt::hstring{ dir }, 13, true, 0.95));
            dh.Children().Append(Text(winrt::to_hstring(count), 12, false, 0.5));

            auto dirBtn = Button{};
            dirBtn.Content(dh);
            dirBtn.HorizontalAlignment(HorizontalAlignment::Stretch);
            dirBtn.HorizontalContentAlignment(HorizontalAlignment::Left);
            dirBtn.Background(Fill(0x00, 0x80, 0x80, 0x80));
            dirBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });
            dirBtn.Padding(Thickness{ 4, 2, 4, 2 });
            AgentSetTitledTip(dirBtn, L"Working directory", L"A folder agents outside Agentmaster are running in, and how many. Click to collapse or expand the group \x2014 unlike your own directories, an external folder doesn't scope the board (these sessions belong to no window).");
            const auto capturedDir = dir;
            dirBtn.Click([this, capturedDir](const IInspectable&, const RoutedEventArgs&) {
                if (_collapsedDirs.find(capturedDir) != _collapsedDirs.end())
                {
                    _collapsedDirs.erase(capturedDir);
                }
                else
                {
                    _collapsedDirs.insert(capturedDir);
                }
                _NotifyLensChanged(); // collapsed dirs are part of the per-window lens (M10)
                _Refresh();
            });
            _treeHost.Children().Append(dirBtn);

            if (collapsed)
            {
                continue;
            }

            // This dir's external rows, ordered by the active (global) sort. Pointers into
            // _externalClaudes stay valid — it is not mutated during the render.
            std::vector<const ::Agentmaster::ExternalClaudeRow*> dirRows;
            for (const auto& ex : _externalClaudes)
            {
                if (PathEq(dirOf(ex), dir))
                {
                    dirRows.push_back(&ex);
                }
            }
            std::stable_sort(dirRows.begin(), dirRows.end(), [&](const ::Agentmaster::ExternalClaudeRow* a, const ::Agentmaster::ExternalClaudeRow* b) {
                return SortKeyLess(sortMode, MakeSortKey(*a), MakeSortKey(*b));
            });
            for (const auto* exp : dirRows)
            {
                const auto& ex = *exp;

                const bool selExt = !ex.sessionId.empty() && ex.sessionId == _selectedExternalSessionId;

                // Title: the conversation's first prompt (from the transcript), else the cwd leaf,
                // else "claude". Truncated for the row.
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
                if (title.size() > 60)
                {
                    title = title.substr(0, 57) + L"\x2026";
                }

                auto row = StackPanel{};
                row.Orientation(Orientation::Horizontal);
                row.Spacing(6);
                // State dot — the SAME filled Ellipse as the tab strip (StateDot == HeaderAgentStatusDot).
                // A Claude external carries no PULL state -> gray (observe-only). For a Codex row (Phase
                // C2) the rollout-derived turn state colors it: blue running / gold waiting / gray idle.
                auto g = StateDot(ex.kind == AgentKind::Codex ? CodexStateColor(ex.codexState) : Color{ 0xFF, 0x9E, 0x9E, 0x9E });
                if (ex.kind == AgentKind::Codex)
                {
                    AgentSetTitledTip(g, L"Codex turn state", winrt::hstring{ L"Currently " } + CodexStateLabel(ex.codexState) + winrt::hstring{ L", read from this session's rollout transcript. Codex reports only Running, Waiting and Idle \x2014 its rollout records no approval or error event, so it has no needs-you or error state to show." });
                }
                else
                {
                    AgentSetTitledTip(g, L"Observed only", L"Agentmaster reads this external Claude session but doesn't follow its turns, so it has no state to show here. Adopt it (right-click) to bring its conversation into a tab that is tracked in full.");
                }
                row.Children().Append(g);
                // Agentmaster (Phase C1): a teal "codex" agent pill on Codex rows (Claude = default, no pill).
                if (ex.kind == AgentKind::Codex)
                {
                    auto cp = Pill(L"codex", Color{ 0xFF, 0x4E, 0xC9, 0xB0 });
                    cp.Opacity(0.9);
                    AgentSetTitledTip(cp, L"Codex agent", L"This external session runs the OpenAI Codex CLI. It is observed, not managed \x2014 right-click to fork a copy of its rollout, or resume it, into a tab here.");
                    row.Children().Append(cp);
                }
                {
                    auto titleText = Text(winrt::hstring{ title }, 13, false, 1.0);
                    // Agentmaster: surface the observer-tailed idle RECAP (away_summary) on hover — the
                    // external analog of the managed row's recap, read from the SAME transcript-tail
                    // region (ExternalClaudeRow.recap; see ProcessObserver). Full text (the tooltip wraps).
                    if (!ex.recap.empty())
                    {
                        AgentSetTitledTip(titleText,
                                          winrt::hstring{ title },
                                          winrt::hstring{ L"Recap \x2014 its own \x201C" L"what we did / what's next\x201D note, written after this session sat idle:\n" } + winrt::hstring{ ex.recap });
                    }
                    row.Children().Append(titleText);
                }

                // host tag: the foreign host this claude runs in — "Windows Terminal" (real WT) vs
                // "Agentmaster" / "Agentmaster Dev" (another of our instances) vs cmd / pwsh. Resolved by
                // the hosting terminal's package family / image path (ResolveExternalHostLabel), so our
                // fork is never mislabeled "WindowsTerminal" (its exe leaf) and dev/release are distinct.
                {
                    std::wstring hostLabel = ex.hostLabel;
                    if (hostLabel.empty())
                    {
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
                            hostLabel = (ex.host == RunningApp::WindowsTerminal) ? L"wt" : L"ext";
                        }
                    }
                    auto hp = Pill(winrt::hstring{ hostLabel }, Color{ 0xFF, 0x6E, 0x7B, 0x8A });
                    hp.Opacity(0.85);
                    AgentSetTitledTip(hp, L"Host", L"The terminal application this external session runs in \x2014 Windows Terminal, another Agentmaster install, or a plain cmd / pwsh console. Right-click the row to bring that window to the front.");
                    row.Children().Append(hp);
                }

                if (!ex.gitBranch.empty())
                {
                    row.Children().Append(Text(winrt::hstring{ L"[" } + winrt::hstring{ ex.gitBranch } + L"]", 11, false, 0.5));
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
                    // CURRENT model (transcript truth) > launch cmdline `--model`; short form. A Codex
                    // row's rollout model rides ex.model and passes through ShortModelName verbatim;
                    // the family words come from the cog's Model families box.
                    addPart(::Agentmaster::ShortModelName(ex.currentModel.empty() ? ex.model : ex.currentModel, ::Agentmaster::ParseModelFamilies(_appSettings.modelFamilies)));
                    addPart(ex.effort);
                    addPart(ex.sandbox); // Codex only (empty for Claude)
                    addPart(ex.approvalMode); // Codex only
                    if (ex.background)
                    {
                        addPart(L"bg");
                    }
                    if (!me.empty())
                    {
                        auto meText = Text(winrt::hstring{ me }, 11, false, 0.5);
                        AgentSetTitledTip(meText, L"How this session was started", L"Its model \xB7 reasoning effort \xB7 (for Codex, its sandbox and approval policy) \xB7 bg if it runs in the background. Read from the running process, not from a config file.");
                        row.Children().Append(meText);
                    }
                }
                // pid — its UNDERLINE is COLOR-CODED by the host window/shell (ex.hostPid): claudes
                // running in the same terminal window/tab share a host shell, so they get the same
                // underline color and are easy to identify at a glance, even across cwd groups.
                {
                    const uint32_t key = ex.hostPid ? ex.hostPid : ex.pid;
                    auto pidCol = StackPanel{};
                    pidCol.Spacing(1);
                    pidCol.Children().Append(Text(winrt::hstring{ L"pid " } + winrt::to_hstring(ex.pid), 11, false, 0.55));
                    auto underline = Border{};
                    underline.Height(2);
                    underline.CornerRadius(CornerRadius{ 1, 1, 1, 1 });
                    underline.HorizontalAlignment(HorizontalAlignment::Stretch); // span the "pid N" width
                    underline.Background(SolidColorBrush{ WindowKeyColor(key) });
                    AgentSetTitledTip(underline, L"Host window", winrt::hstring{ L"Every row with this underline color runs in the same terminal window or tab \x2014 shell process " } + winrt::to_hstring(key) + L" \x2014 so you can tell at a glance which external sessions share a window, even across different folders.");
                    pidCol.Children().Append(underline);
                    row.Children().Append(pidCol);
                }

                // timing (created-ago / active-for / last-activity-ago) — transcript ctime/mtime,
                // falling back to the process start time when there is no transcript yet.
                {
                    const int64_t created = ex.createdUnixMs ? ex.createdUnixMs : ex.startUnixMs;
                    if (auto t = TimingText(created, ex.lastActivityUnixMs))
                    {
                        row.Children().Append(t);
                    }
                }

                auto rowBtn = Button{};
                rowBtn.Content(row);
                rowBtn.HorizontalAlignment(HorizontalAlignment::Stretch);
                rowBtn.HorizontalContentAlignment(HorizontalAlignment::Left);
                rowBtn.Margin(Thickness{ 16, 0, 0, 0 });
                rowBtn.Padding(Thickness{ 4, 2, 4, 2 });
                rowBtn.Background(Fill(selExt ? 0x40 : 0x00, 0x80, 0x80, 0x80));
                rowBtn.BorderThickness(Thickness{ 0, 0, 0, 0 });

                // Left-click SELECTS this external -> the Auto Testing shows its conversation prompts
                // read-only (observe-only; we host no ConPTY so we can't drive it). Right-click -> the
                // Adopt / Open New Session Here / Bring Window To Front menu.
                rowBtn.ContextFlyout(_MakeLazyExternalMenu(ex)); // built at OPEN time (perf)
                const auto exId = ex.sessionId;
                const auto exCwd = ex.cwd;
                const auto exTitle = title;
                const auto exKind = ex.kind; // Phase C1
                const auto exRollout = ex.rolloutPath;
                rowBtn.Click([this, exId, exCwd, exTitle, exKind, exRollout](const IInspectable&, const RoutedEventArgs&) {
                    _SelectExternal(exId, exCwd, exTitle, exKind, exRollout);
                });
                AgentSetTitledTip(rowBtn, winrt::hstring{ title }, L"An agent running outside Agentmaster \x2014 found and read, never driven. Click to read its conversation in the pane on the right; right-click to adopt it into a tab here, start a session in its folder, or bring its window forward.");
                _treeHost.Children().Append(rowBtn);
            }
        }
    }

    // Agentmaster: the EXTERNAL-tree row right-click menu. For a CLAUDE row: Adopt (resume the
    // conversation into a managed, controllable tab), Open New Session Here (spawn a managed session
    // in the cwd), and Bring Window To Front. For a CODEX row (kind=Codex, observe-only in Phase C1):
    // Adopt + Open-New are omitted (both drive CLAUDE; Codex control is a later phase) — a disabled
    // note says so — and only the agent-agnostic Bring Window To Front is offered. All clickable items
    // defer one tick like _MakeSessionMenu so the closing flyout's focus restore doesn't race the
    // spawn / tree rebuild / foreground hand-off. Acts on the row's (pid, cwd) — an external/observe-
    // only row has no registry session id.
    // Agentmaster (perf — LAZY menus): the filler half of _MakeExternalTreeMenu (see _PopulateSessionMenu).
    void AgentManagerContent::_PopulateExternalTreeMenu(MenuFlyout& menu, const ::Agentmaster::ExternalClaudeRow& ex)
    {
        auto disp = _dispatcher;
        auto weak = get_weak();
        const uint32_t pid = ex.pid;
        const std::wstring cwd = ex.cwd;

        if (ex.kind == AgentKind::Codex)
        {
            // Codex-launch (lifecycle + state): Adopt resumes this codex's rollout into a MANAGED tab
            // (`codex resume <uuid>`; the original keeps running), and Open New Codex Session Here
            // launches a fresh managed codex in the cwd. Both route to _codexLaunchHandler (adopt flag).
            // No injector/Autorunner yet — you type into the tab directly (driving Codex is a later phase).
            const std::wstring sid = ex.sessionId;
            const std::wstring adoptTitle = ex.title;
            MenuFlyoutItem adopt;
            adopt.Text(L"Adopt");
            AgentSetTip(adopt, sid.empty() ?
                                   winrt::hstring{ L"This Codex session hasn't been prompted yet (no rollout) \x2014 Adopt launches a fresh managed Codex here" } :
                                   winrt::hstring{ L"Bring this Codex session's conversation under management \x2014 Fork a safe copy (codex fork) or Resume the same rollout" });
            // Two processes can't safely share one rollout, and the external is still running, so OFFER
            // the choice (the chosen "warn and let me choose"): Fork a copy (codex fork -> a NEW rollout,
            // the source untouched -> safe) vs. Resume anyway (codex resume -> the same rollout; stop the
            // original first). A never-prompted codex (no rollout) just launches fresh, no dialog.
            adopt.Click([weak, disp, pid, cwd, sid, adoptTitle](const IInspectable&, const RoutedEventArgs&) {
                auto act = [weak, pid, cwd, sid, adoptTitle]() {
                    auto self = weak.get();
                    if (!self || !self->_codexLaunchHandler)
                    {
                        return;
                    }
                    if (sid.empty())
                    {
                        self->_codexLaunchHandler(pid, winrt::hstring{ cwd }, true, false); // no rollout -> launch fresh
                        return;
                    }
                    const winrt::hstring label = adoptTitle.empty() ? winrt::hstring{ L"This Codex session" } : winrt::hstring{ L"\x201C" + adoptTitle + L"\x201D" };
                    self->_ConfirmChoice(
                        L"Adopt Codex session",
                        label + winrt::hstring{ L" is still running outside Agentmaster. Two processes can't safely share one rollout.\n\nFork a copy: branch its current state into a controllable session (codex fork) \x2014 safe, the original is untouched.\nResume anyway: take over the same rollout \x2014 stop the original first to avoid two writers." },
                        L"Fork a copy",
                        L"Resume anyway",
                        [weak, pid, cwd]() { if (auto s = weak.get()) { if (s->_codexLaunchHandler) { s->_codexLaunchHandler(pid, winrt::hstring{ cwd }, true, true); } } },
                        [weak, pid, cwd]() { if (auto s = weak.get()) { if (s->_codexLaunchHandler) { s->_codexLaunchHandler(pid, winrt::hstring{ cwd }, true, false); } } });
                };
                if (disp) { disp.TryEnqueue(act); } else { act(); }
            });
            menu.Items().Append(adopt);

            MenuFlyoutItem openHereCx;
            openHereCx.Text(L"Open New Codex Session Here");
            AgentSetTip(openHereCx, L"Launch a managed Codex session in this directory (a new, independent conversation)");
            openHereCx.Click([weak, disp, cwd](const IInspectable&, const RoutedEventArgs&) {
                if (disp)
                {
                    disp.TryEnqueue([weak, cwd]() { if (auto self = weak.get()) { if (self->_codexLaunchHandler) { self->_codexLaunchHandler(0, winrt::hstring{ cwd }, false, false); } } });
                }
                else if (auto self = weak.get())
                {
                    if (self->_codexLaunchHandler) { self->_codexLaunchHandler(0, winrt::hstring{ cwd }, false, false); }
                }
            });
            menu.Items().Append(openHereCx);

            MenuFlyoutItem copyIdCx;
            copyIdCx.Text(L"Copy Session Id");
            if (sid.empty())
            {
                copyIdCx.IsEnabled(false);
                AgentSetTip(copyIdCx, L"No rollout id yet (this Codex session hasn't been prompted)");
            }
            else
            {
                AgentSetTip(copyIdCx, L"Copy this Codex rollout's conversation id to the clipboard");
                copyIdCx.Click([sid](const IInspectable&, const RoutedEventArgs&) { CopyTextToClipboard(sid); });
            }
            menu.Items().Append(copyIdCx);
        }
        else
        {
            const std::wstring sid = ex.sessionId;
            const std::wstring adoptTitle = ex.title;
            MenuFlyoutItem adopt;
            adopt.Text(L"Adopt");
            AgentSetTip(adopt, sid.empty() ?
                                   winrt::hstring{ L"This Claude session hasn't been prompted yet (no transcript) \x2014 Adopt launches a fresh managed session here" } :
                                   winrt::hstring{ L"Bring this external Claude's conversation under management \x2014 Fork a safe copy (--fork-session) or Resume the same conversation" });
            // Two processes can't safely share one transcript, and the external is still running, so OFFER
            // the choice (the chosen "warn and let me choose"): Fork a copy (claude --fork-session -> a NEW
            // transcript, the source untouched -> safe) vs. Resume anyway (claude --resume -> the same
            // conversation; stop the original first). A never-prompted claude (no transcript) launches fresh.
            adopt.Click([weak, disp, pid, cwd, sid, adoptTitle](const IInspectable&, const RoutedEventArgs&) {
                auto act = [weak, pid, cwd, sid, adoptTitle]() {
                    auto self = weak.get();
                    if (!self || !self->_adoptExternalHandler)
                    {
                        return;
                    }
                    // Native-exe-only policy: Adopt resumes/forks the external's conversation into a NEW
                    // managed claude (a launch under the hood), so it needs a native claude.exe just like
                    // the Launch/Fork buttons. Without this gate the launch silently no-ops at the engine
                    // backstop ([launch-blocked]) and the button "does nothing". EnsureClaudeAvailable
                    // re-resolves first, so a claude installed since launch clears the gate automatically.
                    if (!::Agentmaster::EnsureClaudeAvailable())
                    {
                        self->_ShowClaudeMissing();
                        return;
                    }
                    if (sid.empty())
                    {
                        self->_adoptExternalHandler(pid, winrt::hstring{ cwd }, false); // no transcript -> launch fresh
                        return;
                    }
                    const winrt::hstring label = adoptTitle.empty() ? winrt::hstring{ L"This Claude session" } : winrt::hstring{ L"\x201C" + adoptTitle + L"\x201D" };
                    self->_ConfirmChoice(
                        L"Adopt Claude session",
                        label + winrt::hstring{ L" is still running outside Agentmaster. Two processes can't safely share one transcript.\n\nFork a copy: branch its current state into a controllable session (claude --fork-session) \x2014 safe, the original is untouched.\nResume anyway: take over the same conversation \x2014 stop the original first to avoid two writers." },
                        L"Fork a copy",
                        L"Resume anyway",
                        [weak, pid, cwd]() { if (auto s = weak.get()) { if (s->_adoptExternalHandler) { s->_adoptExternalHandler(pid, winrt::hstring{ cwd }, true); } } },
                        [weak, pid, cwd]() { if (auto s = weak.get()) { if (s->_adoptExternalHandler) { s->_adoptExternalHandler(pid, winrt::hstring{ cwd }, false); } } });
                };
                if (disp) { disp.TryEnqueue(act); } else { act(); }
            });
            menu.Items().Append(adopt);

            // Open New Session Here — offered in every scope (matches _MakeSessionMenu's LOCAL/GLOBAL
            // ordering): spawn a managed session in this external's cwd (a new, independent
            // conversation — distinct from Adopt, which resumes the external's existing conversation).
            // Launch-model picker: a SUBMENU — "Default" (the plain behavior) + one item per configured
            // model (AppSettings.launchModels; the shared AgentModelMenu.h recipe), each launching
            // this ONE session with `--model <id>`.
            MenuFlyoutSubItem openHere;
            openHere.Text(L"Open New Session Here");
            AgentSetTip(openHere, winrt::hstring{ L"Launch a managed Claude session in this directory (a new, independent conversation) \x2014 pick the model it starts on, or Default. " } + AgentModelEditHint());
            AgentFillModelPickItems(openHere.Items(), ::Agentmaster::ParseLaunchModels(_appSettings.launchModels), [weak, disp, cwd](winrt::hstring model) {
                // Spawning a managed Claude session needs a native claude.exe (native-exe-only policy) —
                // gate with the same re-resolve-then-prompt the Adopt action above uses, so this never
                // silently no-ops at the engine backstop when Claude isn't installed.
                auto act = [weak, cwd, model]() {
                    auto self = weak.get();
                    if (!self || !self->_spawnHandler)
                    {
                        return;
                    }
                    if (!::Agentmaster::EnsureClaudeAvailable())
                    {
                        self->_ShowClaudeMissing();
                        return;
                    }
                    self->_spawnHandler(winrt::hstring{ cwd }, winrt::hstring{}, model);
                };
                if (disp) { disp.TryEnqueue(act); } else { act(); }
            },
                                    _ModelSpecifyOpener());
            menu.Items().Append(openHere);

            // Copy Session Id — the resolved conversation id. Empty for a never-prompted external
            // (no transcript id yet, Rule #14) -> the item is disabled. Synchronous clipboard write,
            // no defer needed (matches the Archive page's "Copy id").
            MenuFlyoutItem copyId;
            copyId.Text(L"Copy Session Id");
            // `sid` is already declared at the top of this branch (the Adopt dialog gate reuses it).
            if (sid.empty())
            {
                copyId.IsEnabled(false);
                AgentSetTip(copyId, L"No conversation id yet (this Claude session hasn't been prompted)");
            }
            else
            {
                AgentSetTip(copyId, L"Copy this session's conversation id to the clipboard");
                copyId.Click([sid](const IInspectable&, const RoutedEventArgs&) {
                    CopyTextToClipboard(sid);
                });
            }
            menu.Items().Append(copyId);
        }

        // Bring Window To Front — the LAST option, for BOTH agents: surface the window HOSTING this
        // session (unminimize + foreground; a Windows Terminal-class host also gets the tab selected,
        // best-effort). Observe-only safe: window activation only — it never writes into the foreign
        // session (Rule #13). Defers a tick so the closing flyout's focus restore lands before
        // foreground is handed to the other window.
        MenuFlyoutItem bringFront;
        bringFront.Text(L"Bring Window To Front");
        AgentSetTip(bringFront, L"Unminimize + foreground the window hosting this session; a Windows Terminal host also gets its tab selected (best-effort)");
        bringFront.Click([weak, disp, pid, cwd](const IInspectable&, const RoutedEventArgs&) {
            if (disp)
            {
                disp.TryEnqueue([weak, pid, cwd]() { if (auto self = weak.get()) { self->_BringExternalToFront(pid, cwd); } });
            }
            else if (auto self = weak.get())
            {
                self->_BringExternalToFront(pid, cwd);
            }
        });
        menu.Items().Append(bringFront);
    }

    MenuFlyout AgentManagerContent::_MakeExternalTreeMenu(const ::Agentmaster::ExternalClaudeRow& ex)
    {
        MenuFlyout menu;
        _PopulateExternalTreeMenu(menu, ex);
        return menu;
    }

    // Agentmaster: Bring Window To Front (the EXTERNAL right-click's last item). Resolve the row's
    // host facts from the latest observer snapshot — hostPid roots the window walk when the claude
    // already exited; the title feeds the WT tab-match heuristics (the menu captured only pid +
    // cwd) — then hand the actual window work to a BACKGROUND thread: BringClaudeWindowToFront
    // takes a Toolhelp snapshot and does cross-process UI Automation reads (tens of ms, can block),
    // so it must never run on the UI thread. Fire-and-forget — no UI mutation afterwards, so
    // nothing posts back (and nothing captures `this` past the detach).
    void AgentManagerContent::_BringExternalToFront(uint32_t pid, const std::wstring& cwd)
    {
        uint32_t hostPid = 0;
        std::wstring sessionId;
        std::wstring title;
        for (const auto& ex : _externalClaudes)
        {
            if (ex.pid == pid)
            {
                hostPid = ex.hostPid;
                sessionId = ex.sessionId; // lets the worker read the transcript (custom title + prompt corpus) for the tab pick
                title = ex.title;
                break;
            }
        }
        // Nav audit: the user asked to surface an EXTERNAL claude's hosting window (the external row menu's
        // "Bring Window To Front"). The OS work (walk ancestors -> foreground -> UIA tab pick) runs on a
        // detached worker, so this records the intent + what was picked (pid / host shell / conversation).
        ::Agentmaster::LogNav(L"manager bring-to-front pid=" + std::to_wstring(pid) + L" host=" + std::to_wstring(hostPid) + L" " + ::Agentmaster::ShortId(sessionId) + L" cwd=" + cwd);
        std::thread([pid, hostPid, sessionId = std::move(sessionId), title = std::move(title), cwd]() {
            ::Agentmaster::BringClaudeWindowToFront(pid, hostPid, sessionId, title, cwd);
        }).detach();
    }

    // Agentmaster: advance the Explorer Tree scope LOCAL -> GLOBAL -> EXTERNAL -> LOCAL. The scope
    // is ONE state shared with the Triage Board's 2-way toggle and persisted per window in the lens
    // (ManagerState.treeScope); _SetTreeScope is the single mutator behind both buttons.
    void AgentManagerContent::_ToggleTreeScope()
    {
        switch (_treeScope)
        {
        case TreeScope::Local:
            _SetTreeScope(TreeScope::Global);
            break;
        case TreeScope::Global:
            _SetTreeScope(TreeScope::External);
            break;
        default:
            _SetTreeScope(TreeScope::Local);
            break;
        }
    }

    // Agentmaster: the ONE scope mutator behind BOTH toggles — the tree's 3-way cycle above and the
    // board's LOCAL/GLOBAL flip (which reads External as Global, so from EXTERNAL its click lands on
    // LOCAL). Handles the External enter/leave selection cleanup, reflects both buttons, pushes the
    // lens (the scope is persisted per window now), and refreshes — skippable when the caller
    // refreshes itself (e.g. _OnRenameSession, which refreshes once after its other mutations).
    void AgentManagerContent::_SetTreeScope(TreeScope scope, bool refresh)
    {
        if (_treeScope == scope)
        {
            return;
        }
        _treeScope = scope;
        // Entering EXTERNAL: externals are observe-only, so drop any managed session selection — the
        // Auto Testing then reads "nothing selected" until an external row is clicked (read-only).
        if (_treeScope == TreeScope::External && !_selectedId.empty())
        {
            _selectedId.clear();
            _selectedPromptId.clear();
        }
        // Leaving EXTERNAL: drop the external (read-only) selection so the Auto Testing returns to the
        // managed view cleanly.
        if (_treeScope != TreeScope::External && !_selectedExternalTitle.empty())
        {
            _selectedExternalSessionId.clear();
            _selectedExternalCwd.clear();
            _selectedExternalTitle.clear();
            _externalPlanLoadedFor.clear();
            _externalPlanPrompts.clear();
        }
        _UpdateTreeScopeButton();
        _UpdateBoardScopeButton();
        _NotifyLensChanged(); // the scope (and any selection it cleared) is part of the per-window lens
        if (refresh)
        {
            // _Refresh (not just _RebuildTree) so the board re-filters (LOCAL/GLOBAL), the selection
            // highlight tracks, and the Auto Testing re-renders when entering/leaving EXTERNAL.
            _Refresh();
        }
    }

    // Reflect the current scope on the toggle button's label.
    void AgentManagerContent::_UpdateTreeScopeButton()
    {
        if (_treeScopeBtn)
        {
            const wchar_t* label = (_treeScope == TreeScope::Global)     ? L"GLOBAL"
                                   : (_treeScope == TreeScope::External) ? L"EXTERNAL"
                                                                         : L"LOCAL";
            _treeScopeBtn.Content(winrt::box_value(label));
        }
    }

    // Reflect the shared scope on the BOARD toggle's label — 2-way: the board has no External mode,
    // so EXTERNAL (a tree-only view) reads as GLOBAL here (the board then shows every window's
    // sessions, which is what it renders in that scope).
    void AgentManagerContent::_UpdateBoardScopeButton()
    {
        if (_boardScopeBtn)
        {
            const wchar_t* label = (_treeScope == TreeScope::Local) ? L"LOCAL" : L"GLOBAL";
            _boardScopeBtn.Content(winrt::box_value(label));
        }
    }

    // Agentmaster: advance the Triage Board sort MOST ACTIVE -> NEWEST -> OLDEST -> A-Z -> MOST ACTIVE
    // (it never visits BY PID — pid grouping is meaningless once cards split across the state columns).
    // A SEPARATE global setting from the tree's sort (AppSettings::boardSort), so the board and tree
    // remember their own order. Like treeSort it is GLOBAL + persisted: mutate _appSettings.boardSort,
    // refresh the label, push it through the settings sink (the page persists settings.json), so the
    // choice survives restart and seeds every other / future window; _Refresh re-sorts THIS window's board now.
    void AgentManagerContent::_CycleBoardSort()
    {
        using ::Agentmaster::ExplorerSort;
        switch (_appSettings.boardSort)
        {
        case ExplorerSort::MostActive:
            _appSettings.boardSort = ExplorerSort::Newest;
            break;
        case ExplorerSort::Newest:
            _appSettings.boardSort = ExplorerSort::Oldest;
            break;
        case ExplorerSort::Oldest:
            _appSettings.boardSort = ExplorerSort::Alpha;
            break;
        case ExplorerSort::Alpha:
        case ExplorerSort::ByPid: // not produced by the board cycle, but treat as "wrap to the default"
        default:
            _appSettings.boardSort = ExplorerSort::MostActive;
            break;
        }
        _UpdateBoardSortButton();
        if (_settingsSink)
        {
            _settingsSink(_appSettings); // persist globally (settings.json) + re-materialize
        }
        _Refresh();
    }

    // Reflect the current (global) board sort on the toggle button's label.
    void AgentManagerContent::_UpdateBoardSortButton()
    {
        if (_boardSortBtn)
        {
            using ::Agentmaster::ExplorerSort;
            const wchar_t* label = (_appSettings.boardSort == ExplorerSort::Newest) ? L"NEWEST"
                                   : (_appSettings.boardSort == ExplorerSort::Oldest) ? L"OLDEST"
                                   : (_appSettings.boardSort == ExplorerSort::Alpha)  ? L"A\x2013Z"
                                                                                      : L"MOST ACTIVE"; // MostActive (default) + any stray ByPid
            _boardSortBtn.Content(winrt::box_value(label));
        }
    }

    // Agentmaster: advance the Explorer Tree sort NEWEST -> OLDEST -> MOST ACTIVE -> A-Z -> BY PID ->
    // NEWEST. The sort is a GLOBAL setting: mutate _appSettings.treeSort, refresh the label, then push
    // it through the settings sink (the page persists it to settings.json and re-materializes), so the
    // choice survives restart and seeds every other / future window. _Refresh re-sorts THIS window's
    // tree (and re-renders the board/plan) immediately.
    void AgentManagerContent::_CycleTreeSort()
    {
        using ::Agentmaster::ExplorerSort;
        switch (_appSettings.treeSort)
        {
        case ExplorerSort::Newest:
            _appSettings.treeSort = ExplorerSort::Oldest;
            break;
        case ExplorerSort::Oldest:
            _appSettings.treeSort = ExplorerSort::MostActive;
            break;
        case ExplorerSort::MostActive:
            _appSettings.treeSort = ExplorerSort::Alpha;
            break;
        case ExplorerSort::Alpha:
            _appSettings.treeSort = ExplorerSort::ByPid;
            break;
        case ExplorerSort::ByPid:
        default:
            _appSettings.treeSort = ExplorerSort::Newest;
            break;
        }
        _UpdateTreeSortButton();
        if (_settingsSink)
        {
            _settingsSink(_appSettings); // persist globally (settings.json) + re-materialize
        }
        _Refresh();
    }

    // Reflect the current (global) sort on the toggle button's label.
    void AgentManagerContent::_UpdateTreeSortButton()
    {
        if (_treeSortBtn)
        {
            using ::Agentmaster::ExplorerSort;
            const wchar_t* label = (_appSettings.treeSort == ExplorerSort::Oldest)       ? L"OLDEST"
                                   : (_appSettings.treeSort == ExplorerSort::MostActive) ? L"MOST ACTIVE"
                                   : (_appSettings.treeSort == ExplorerSort::Alpha)      ? L"A\x2013Z"
                                   : (_appSettings.treeSort == ExplorerSort::ByPid)      ? L"BY PID"
                                                                                         : L"NEWEST";
            _treeSortBtn.Content(winrt::box_value(label));
        }
    }

    // ---- Explorer-tree session actions (right-click menu, rename, delete) ----

    // Agentmaster (perf — LAZY menus): the eager builder is now the FILLER — it appends the session's
    // items into a caller-supplied menu. _MakeSessionMenu (below) still returns a fully built menu for
    // any caller that wants one up front; _MakeLazySessionMenu returns an EMPTY menu whose Opening
    // handler runs this, so a card/row pays for its menus only when one is actually opened — and the
    // "read at flyout-creation time" note below then means "read at OPEN time", which is fresher still.
    void AgentManagerContent::_PopulateSessionMenu(MenuFlyout& menu, const std::wstring& id, const std::wstring& cwd, const winrt::Windows::UI::Xaml::FrameworkElement& anchor)
    {
        auto disp = _dispatcher;
        auto weak = get_weak();

        // Agentmaster: resolve the session's kind + state ONCE so this menu can mirror the WT tab's
        // right-click session ops (New Session Here / Restart session / Fork session — kind-aware:
        // Codex spawns/forks codex, not claude) and offer the triage "Move to Idle/Done" (on a
        // Waiting-for-you card, and — as the error DISMISSAL — on an Error card).
        // Read at flyout-creation time — each _Refresh rebuilds the card so the menu tracks the latest
        // state; the click handlers re-resolve where it matters (the Move-to-Idle mutator re-checks under
        // the registry lock).
        std::optional<SessionInfo> info;
        if (_registry)
        {
            info = _registry->Get(id);
        }
        const bool isCodex = info && info->kind == AgentKind::Codex;
        const SessionState state = info ? info->state : SessionState::Idle;

        // A Segoe Fluent glyph icon for a menu item — so this menu reads like the WT tab's right-click
        // menu, which icons every item. The same glyphs the tab menu uses (Tab.cpp): Add \xE710,
        // RestartConnection \xE72C, Duplicate \xF5ED, Rename \xE8AC, Copy \xE8C8, Close \xE711.
        const auto glyphIcon = [](const wchar_t* glyph) {
            FontIcon fi;
            fi.FontFamily(FontFamily{ L"Segoe Fluent Icons" });
            fi.Glyph(glyph);
            return fi;
        };

        // All items defer one tick: a MenuFlyout restores focus to its target as it closes,
        // which would otherwise yank focus out of the freshly-shown rename editor / dialog (and the
        // spawn / tree rebuild for Open New Session Here).

        // Agentmaster (eager-init): Activate Tab — start a DORMANT session's claude IN PLACE (no focus
        // change), shown only when this session hasn't initialized yet (a window-restored / re-homed tab
        // the user never opened; SessionInfo::started == false) AND it is hosted in THIS window (a
        // single-session start can only target the window owning the control; a remote dormant session in
        // GLOBAL scope is woken via "Jump to Tab" or "Activate All Tabs"). FIRST item when present —
        // distinct from "Jump to Tab" (which switches to it + starts it as a side effect); this wakes it
        // where you are. Disappears once it starts.
        bool dormantLocal = false;
        if (info && IsSessionDormant(*info))
        {
            if (_localScopeProvider)
            {
                const auto localIds = _localScopeProvider(); // bind ONCE — find()/end() must be the same container
                dormantLocal = localIds.find(id) != localIds.end();
            }
            else
            {
                dormantLocal = true; // no provider wired => treat as local
            }
        }
        if (dormantLocal)
        {
            MenuFlyoutItem activate;
            activate.Text(L"Activate Tab (Shift+Click)");
            activate.Icon(glyphIcon(L"\xE768")); // Play — "start it"
            AgentSetTip(activate, L"Start this session's claude now, in place \x2014 it hasn't initialized yet (a restored tab you never opened). The view doesn't switch; use Jump to Tab for that. You can also Shift+Click the card or tree row to do this.");
            activate.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
                auto act = [weak, id]() { if (auto self = weak.get()) { if (self->_activateDormantHandler) { self->_activateDormantHandler(winrt::hstring{ id }); } } };
                if (disp)
                {
                    disp.TryEnqueue(act);
                }
                else
                {
                    act();
                }
            });
            menu.Items().Append(activate);
        }

        // Jump to Tab — Activate: switch to this session's live terminal tab (the page fans out to the
        // hosting WINDOW when the tab lives in another one). The menu twin of a double-click on the
        // card / tree row (and Enter on a tree row). First item — it's the most common action; a
        // separator sets the navigate action apart from the session-edit ops below.
        MenuFlyoutItem jump;
        jump.Text(L"Jump to Tab");
        jump.Icon(glyphIcon(L"\xE7B3")); // RedEye — matches the Auto Testing's "jump to the live tab" eye
        AgentSetTip(jump, L"Switch to this session's live terminal tab (jumps to its hosting window if it lives elsewhere)");
        jump.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
            if (disp)
            {
                disp.TryEnqueue([weak, id]() { if (auto self = weak.get()) { if (self->_activateHandler) { self->_activateHandler(winrt::hstring{ id }); } } });
            }
            else if (auto self = weak.get())
            {
                if (self->_activateHandler)
                {
                    self->_activateHandler(winrt::hstring{ id });
                }
            }
        });
        menu.Items().Append(jump);

        // Move to Idle/Done — Waiting-for-you AND Error triage. For a Waiting card it is the immediate,
        // user-driven twin of the timed WaitingForInput -> Idle decay (SessionScanner::_maybeDecayWaiting):
        // demote this card so it leaves the "Waiting-for-you" column for "Idle / Done". A deliberate state
        // transition layered on the hook-derived machine (the decay sets the SAME state the SAME way, so it
        // sticks — the scanner only re-promotes to Waiting/Running on NEW turn activity, never bouncing a
        // quiescent Idle back). For an ERROR card it is the manual DISMISSAL: Error is level-derived (the
        // scanner re-asserts it off the unchanged error tail every pass), so the mutator also sets the
        // errorDismissed ack — the recon-error gate then suppresses the re-derivation until the
        // conversation actually moves (a retry / a NEW error re-fires normally) — and clears
        // errorMessage/errorStatus (the "Empty/0 outside Error" invariant).
        if (state == SessionState::WaitingForInput || state == SessionState::Error)
        {
            MenuFlyoutItem moveIdle;
            moveIdle.Text(L"Move to Idle/Done");
            moveIdle.Icon(glyphIcon(L"\xE73E")); // CheckMark — "I've handled this; stop waiting on me / stop flagging this error"
            if (state == SessionState::Error)
            {
                AgentSetTip(moveIdle, L"Dismiss this errored card to the Idle / Done column \x2014 acknowledges the API error. It returns to Error if a new API error lands.");
            }
            else
            {
                AgentSetTip(moveIdle, L"Dismiss this \x201CWaiting-for-you\x201D card to the Idle / Done column \x2014 the manual version of the unread-timeout decay. It returns to Waiting-for-you on the session's next turn.");
            }
            moveIdle.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
                auto act = [weak, id]() {
                    auto self = weak.get();
                    if (!self || !self->_registry)
                    {
                        return;
                    }
                    // Re-check under the registry lock against the LIVE record: a new turn may have moved
                    // the state since the menu was built — never demote a session that is now Running.
                    // Stamp readUnixMs (mark it read) + clear manualUnread so a prior "Mark Unread" can't
                    // keep it pinned, mirroring the decay's read-gated semantics. An Error card
                    // additionally records the dismissal ack (so the scanner's level-derived Error can't
                    // bounce the flip back) and drops the preserved failure reason.
                    self->_registry->Update(id, [](SessionInfo& s) {
                        if (s.state == SessionState::WaitingForInput)
                        {
                            s.state = SessionState::Idle;
                            s.manualUnread = false;
                            s.readUnixMs = NowMs();
                        }
                        else if (s.state == SessionState::Error)
                        {
                            s.state = SessionState::Idle;
                            s.manualUnread = false;
                            s.readUnixMs = NowMs();
                            s.errorDismissed = true;
                            s.errorMessage.clear();
                            s.errorStatus = 0;
                        }
                    });
                };
                if (disp) { disp.TryEnqueue(act); } else { act(); }
            });
            menu.Items().Append(moveIdle);
        }

        // Move to Waiting-for-you — Idle/Done triage only (the reverse of "Move to Idle/Done"): a PLAIN
        // promote of a quiescent card back into the "Waiting-for-you" column. EXPLICITLY separate from
        // "Mark Unread" (the tab strip's sticky/flashy promote) — this sets NO sticky manualUnread and
        // raises no red-ring flash; it just moves columns. Refresh the decay anchor + leave it unread so it
        // behaves like a fresh turn-complete (waits the full Waiting-for-you timeout, then decays normally)
        // instead of instantly decaying off an ancient lastActivity.
        if (info && (state == SessionState::Idle || state == SessionState::Done))
        {
            MenuFlyoutItem moveWaiting;
            moveWaiting.Text(L"Move to Waiting-for-you");
            moveWaiting.Icon(glyphIcon(L"\xE823")); // Clock — put it back in the Waiting-for-you column
            AgentSetTip(moveWaiting, L"Move this card into the \x201CWaiting-for-you\x201D column \x2014 a plain move (no red-ring flash; unlike \x201CMark Unread\x201D it decays normally). It returns to Idle / Done after the unread timeout.");
            moveWaiting.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
                auto act = [weak, id]() {
                    auto self = weak.get();
                    if (!self || !self->_registry)
                    {
                        return;
                    }
                    // Re-check under the registry lock against the LIVE record: only promote a still-
                    // quiescent (Idle/Done) card — never bump a session that has since gone Running.
                    self->_registry->Update(id, [](SessionInfo& s) {
                        if (s.state == SessionState::Idle || s.state == SessionState::Done)
                        {
                            s.state = SessionState::WaitingForInput;
                            s.manualUnread = false; // plain move (NOT the sticky "Mark Unread")
                            s.lastActivityUnixMs = NowMs(); // restart the Waiting-for-you window (don't instant-decay)
                            s.readUnixMs = 0; // unread for this "turn", like a real turn-complete
                        }
                    });
                };
                if (disp) { disp.TryEnqueue(act); } else { act(); }
            });
            menu.Items().Append(moveWaiting);
        }

        menu.Items().Append(MenuFlyoutSeparator{});

        MenuFlyoutItem rename;
        rename.Text(L"Rename (F2)");
        rename.Icon(glyphIcon(L"\xE8AC")); // Rename (matches the WT tab menu)
        // Advertise the in-place editor's commit keys + that they're configurable. Which key commits
        // (Enter vs Shift+Enter) follows the GLOBAL TabRenameCommitMode setting; the other inserts a
        // newline (titles can be multi-line), Esc cancels, and clicking away always commits.
        AgentSetTip(rename, L"Rename this session \x2014 its Explorer name and tab title.\nBy default, Shift+Enter commits the new name and Enter inserts a line break (swap them in Settings (\x2699) \x2192 \x201CTab rename: commit with\x201D).\nEsc cancels; clicking away always commits.");
        rename.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
            if (disp)
            {
                disp.TryEnqueue([weak, id]() { if (auto self = weak.get()) { self->_OnRenameSession(id); } });
            }
            else if (auto self = weak.get())
            {
                self->_OnRenameSession(id);
            }
        });
        menu.Items().Append(rename);

        // Tags — the bookmark-tags panel for this session (the WT tab menu's "Tags" twin): name a
        // NEW tag (with the color picker) or toggle existing ones; each shows as a small bookmark on
        // the session's tab. The PAGE owns the panel (SetTagsHandler -> _OpenTagEditorForElement),
        // anchored under the right-clicked card / row (`anchor` — WEAK-captured at menu build; the
        // page guards a recycled/dead element and falls back to a default position). Deferred one tick
        // like Rename so the closing flyout's focus restore can't fight the panel's name-box focus.
        //
        // The anchor capture MUST stay weak — it was the 23 GB leak (the ToolTip leak's sequel, same
        // class — AgentTipHelpers.h). `anchor` IS this menu's host (the tree row / board card / its ⋯
        // button), and that host holds this menu strongly via ContextFlyout/Flyout — so a strong
        // `anchor` capture here closed a host -> flyout -> Click-delegate -> host REFCOUNT CYCLE that
        // C++/WinRT (pure refcounting, no cycle collector) can never free. Every _Refresh rebuild
        // (each registry notify + the 30 s backstop, board AND tree, ~N sessions a pass) then leaked
        // the ENTIRE discarded row/card subtree — TextBlocks + their DWrite layouts, icons, brushes,
        // this ~14-item menu — at ~85 MB/min on a busy fleet. The row handlers above already state the
        // rule: never capture an element into a handler its own subtree owns.
        MenuFlyoutItem tagsItem;
        tagsItem.Text(L"Tags");
        tagsItem.Icon(glyphIcon(L"\xE8A4")); // Bookmarks — matches the WT tab menu's Tags item
        AgentSetTip(tagsItem, L"Bookmark tags for this session \x2014 add a tag (pick its color) or toggle existing ones; each tag shows as a small bookmark ribbon on the session's tab.");
        const auto anchorWeak = anchor ? winrt::make_weak(anchor) : winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement>{};
        tagsItem.Click([weak, disp, id, anchorWeak](const IInspectable&, const RoutedEventArgs&) {
            auto act = [weak, id, anchorWeak]() {
                if (auto self = weak.get())
                {
                    if (self->_tagsHandler)
                    {
                        // A dead anchor resolves null — _OpenTagEditorForElement default-positions.
                        self->_tagsHandler(winrt::hstring{ id }, anchorWeak.get());
                    }
                }
            };
            if (disp) { disp.TryEnqueue(act); } else { act(); }
        });
        menu.Items().Append(tagsItem);

        // Copy — a submenu mirroring the per-tab link badge's copy button (DESIGN §9.7 / TAB_OVERLAY.md).
        // Placed directly below Rename (at the user's request). It routes through the SAME shared
        // CopySessionField action the overlay's copy menu uses, so the two menus can never drift: Session
        // Id / Path / Branch / the REAL Claude & Codex launch CLIs / the full Summary box / the whole
        // Transcript. Each item is a pure clipboard write (cases 0-4) or an off-thread read that hops back
        // to copy (Transcript/Summary) — none mutate the tree, so unlike the rename/spawn/archive items
        // elsewhere in this menu they need no defer (matches the old single "Copy Session Id").
        MenuFlyoutSubItem copySub;
        copySub.Text(L"Copy");
        copySub.Icon(glyphIcon(L"\xE8C8")); // Copy (matches the WT tab menu's "Copy >")
        AgentSetTip(copySub, L"Copy this session's id, path, branch, current (unsent) prompt, launch command line, transcript, or full summary");
        const auto addCopyItem = [&copySub, weak, id](const wchar_t* text, const wchar_t* tip, int which) {
            MenuFlyoutItem item;
            item.Text(text);
            AgentSetTip(item, tip);
            item.Click([weak, id, which](const IInspectable&, const RoutedEventArgs&) {
                if (auto self = weak.get())
                {
                    if (self->_registry)
                    {
                        // "Copy Current Prompt" (case 7) reads the unsent draft LIVE off the session's
                        // buffer when THIS window hosts its tab; the board/tree span the whole fleet, so
                        // for a session hosted elsewhere the provider answers "" and the copy falls back
                        // to the observer's recorded draft (ignored by every other case).
                        std::function<std::wstring()> liveDraft;
                        if (self->_liveDraftProvider)
                        {
                            liveDraft = [provider = self->_liveDraftProvider, id]() { return provider(id); };
                        }
                        // The Summary case renders with this window's GLOBAL summary-panel flags, so a
                        // copied Summary matches what the panels show (wrap/truncate); the tab-color
                        // mode drives the Path case's effective-work-dir resolution.
                        CopySessionField(*self->_registry, id, which, self->_dispatcher,
                                         self->_appSettings.summaryPanelWrapNewlines, self->_appSettings.summaryPanelTruncate,
                                         static_cast<int>(self->_appSettings.tabColorMode), liveDraft);
                    }
                }
            });
            copySub.Items().Append(item);
        };
        addCopyItem(L"Session Id", L"Copy the resumable conversation id (Codex: its rollout uuid)", 0);
        addCopyItem(L"Copy Path", L"Copy the session's working-directory path", 1);
        addCopyItem(L"Copy Branch Name", L"Copy the session's current git branch name", 2);
        // Copy Current Prompt (PENDING_INPUT.md) — the UNSENT draft in the session's input box. Claude
        // only: Codex's TUI has no ❯ rule-wrapped input box, so no draft is ever monitored for it.
        if (!isCodex)
        {
            addCopyItem(L"Copy Current Prompt", L"Copy what is typed into this session's input box but NOT yet sent \x2014 read live from the terminal when this window hosts the tab, else the last observed draft (nothing is copied when there is none)", 7);
        }
        // Offer ONLY the launch-CLI matching this session's agent (isCodex resolved above) — a Claude
        // session gets "Claude Launch CLI", a Codex session "Codex Launch CLI", never both.
        if (isCodex)
        {
            addCopyItem(L"Codex Launch CLI", L"Copy the full codex launch command line", 4);
        }
        else
        {
            addCopyItem(L"Claude Launch CLI", L"Copy the full claude.exe launch command line (with --settings hooks and flags)", 3);
        }
        addCopyItem(L"Summary", L"Copy the FULL session summary \x2014 the complete box (id, resume CLI, dir, folder, branch, duration, tasks, messages, files)", 6);
        addCopyItem(L"Transcript", L"Copy the whole conversation as text (your prompts + the agent's replies)", 5);
        addCopyItem(L"Transcript Followup", L"Copy the conversation folded per turn \x2014 ❯ each of your messages + ● the agent's end-of-turn reply \x2014 with a legend header, ready to paste into a follow-up session", 8);
        menu.Items().Append(copySub);

        // The three WT-tab-menu session ops (mirrored here at the user's request): New Session Here /
        // Restart session / Fork session — kind-aware (a Codex card spawns + forks codex, a Claude card
        // claude). Uses the row's cwd captured at build time (a session's workingDir is fixed at launch).
        // A separator sets these session ops apart from the Rename / Copy items above.
        menu.Items().Append(MenuFlyoutSeparator{});

        // New Session Here — spawn a NEW, independent managed session in this row's working dir.
        // Launch-model picker: the Claude item is a SUBMENU — "Default" (the plain behavior) + one
        // item per configured model (AppSettings.launchModels; the shared AgentModelMenu.h recipe).
        // A Codex row keeps the PLAIN item: the models are Claude models and a codex spawn takes no
        // --model, so a one-entry submenu would only add a hover for nothing.
        if (isCodex)
        {
            MenuFlyoutItem openHere;
            openHere.Text(L"Open New Codex Session Here");
            openHere.Icon(glyphIcon(L"\xE710")); // Add (matches the WT tab menu's "New Session Here")
            AgentSetTip(openHere, L"Launch a managed Codex session in this directory (a new, independent conversation)");
            openHere.Click([weak, disp, cwd](const IInspectable&, const RoutedEventArgs&) {
                auto act = [weak, cwd]() {
                    auto self = weak.get();
                    if (!self)
                    {
                        return;
                    }
                    // Codex spawn (adopt=false, fork ignored) — no native-exe gate (Codex isn't exe-only;
                    // the launcher falls back to a bare `codex` token + surfaces any error).
                    if (self->_codexLaunchHandler)
                    {
                        self->_codexLaunchHandler(0, winrt::hstring{ cwd }, false, false);
                    }
                };
                if (disp) { disp.TryEnqueue(act); } else { act(); }
            });
            menu.Items().Append(openHere);
        }
        else
        {
            MenuFlyoutSubItem openHere;
            openHere.Text(L"Open New Session Here");
            openHere.Icon(glyphIcon(L"\xE710")); // Add (matches the WT tab menu's "New Session Here")
            AgentSetTip(openHere, winrt::hstring{ L"Launch a managed Claude session in this directory (a new, independent conversation) \x2014 pick the model it starts on, or Default. " } + AgentModelEditHint());
            AgentFillModelPickItems(openHere.Items(), ::Agentmaster::ParseLaunchModels(_appSettings.launchModels), [weak, disp, cwd](winrt::hstring model) {
                auto act = [weak, cwd, model]() {
                    auto self = weak.get();
                    if (!self || !self->_spawnHandler)
                    {
                        return;
                    }
                    // Native-exe-only policy: spawning a managed Claude session needs a native claude.exe —
                    // gate (re-resolve-then-prompt) so it surfaces the install modal instead of silently
                    // no-op'ing at the engine backstop when Claude isn't installed.
                    if (!::Agentmaster::EnsureClaudeAvailable())
                    {
                        self->_ShowClaudeMissing();
                        return;
                    }
                    self->_spawnHandler(winrt::hstring{ cwd }, winrt::hstring{}, model);
                };
                if (disp) { disp.TryEnqueue(act); } else { act(); }
            },
                                    _ModelSpecifyOpener());
            menu.Items().Append(openHere);
        }

        // Restart session — rebuild THIS session's live ConPTY connection in place (the page resumes the
        // current conversation; never replays the launch commandline). Kind-agnostic at this seam: the
        // page's _RestartManagedSession handles both Claude (with its own claude.exe gate) and Codex.
        MenuFlyoutItem restart;
        restart.Text(L"Restart session");
        restart.Icon(glyphIcon(L"\xE72C")); // RestartConnection (matches the WT tab menu)
        AgentSetTip(restart, L"Restart this session \x2014 relaunch the agent and resume its current conversation in place (the terminal pane is reused).");
        restart.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
            if (disp)
            {
                disp.TryEnqueue([weak, id]() { if (auto self = weak.get()) { if (self->_restartSessionHandler) { self->_restartSessionHandler(winrt::hstring{ id }); } } });
            }
            else if (auto self = weak.get())
            {
                if (self->_restartSessionHandler)
                {
                    self->_restartSessionHandler(winrt::hstring{ id });
                }
            }
        });
        menu.Items().Append(restart);

        // Fork session — branch this conversation into a NEW, independent one (Claude: `--resume <id>
        // --fork-session`; Codex: `codex fork <rolloutUuid>`) opened in this window, the source untouched.
        // The page's _ForkManagedSessionById is the kind-aware fork shared with the WT tab's "Fork session".
        // Launch-model picker: the CLAUDE item is a SUBMENU — Default + one item per configured model
        // (a fork is a launch, so the pick rides `--model <id>` onto the forked session); the CODEX
        // item stays plain (`codex fork` takes no --model).
        if (isCodex)
        {
            MenuFlyoutItem fork;
            fork.Text(L"Fork session");
            fork.Icon(glyphIcon(L"\xF5ED")); // Duplicate (matches the WT tab menu's "Fork session")
            AgentSetTip(fork, L"Fork this session \x2014 branch its conversation into a new, independent session (named \x201C\x2026 (fork)\x201D); the original is untouched.");
            fork.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
                if (disp)
                {
                    disp.TryEnqueue([weak, id]() { if (auto self = weak.get()) { if (self->_forkManagedSessionHandler) { self->_forkManagedSessionHandler(winrt::hstring{ id }, winrt::hstring{}); } } });
                }
                else if (auto self = weak.get())
                {
                    if (self->_forkManagedSessionHandler)
                    {
                        self->_forkManagedSessionHandler(winrt::hstring{ id }, winrt::hstring{});
                    }
                }
            });
            menu.Items().Append(fork);
        }
        else
        {
            MenuFlyoutSubItem fork;
            fork.Text(L"Fork session");
            fork.Icon(glyphIcon(L"\xF5ED")); // Duplicate (matches the WT tab menu's "Fork session")
            AgentSetTip(fork, winrt::hstring{ L"Fork this session \x2014 branch its conversation into a new, independent session (named \x201C\x2026 (fork)\x201D); the original is untouched. Pick the model the fork starts on, or Default. " } + AgentModelEditHint());
            AgentFillModelPickItems(fork.Items(), ::Agentmaster::ParseLaunchModels(_appSettings.launchModels), [weak, disp, id](winrt::hstring model) {
                auto act = [weak, id, model]() {
                    if (auto self = weak.get())
                    {
                        if (self->_forkManagedSessionHandler)
                        {
                            self->_forkManagedSessionHandler(winrt::hstring{ id }, model);
                        }
                    }
                };
                if (disp) { disp.TryEnqueue(act); } else { act(); }
            },
                                    _ModelSpecifyOpener());
            menu.Items().Append(fork);
        }

        // Close — the LAST group, set apart by a separator and carrying the X glyph, mirroring the WT
        // tab's right-click "Close ▸" submenu (glyph \xE711). A SUBMENU (per the user's "Close -> Of
        // Same Folder"):
        //   • This Session — the plain Close: shut this tab down (FAVORITES.md: the "Close" verb that
        //     replaced Archive/Delete) but KEEP the record (always archived) so it stays in Sessions,
        //     resumable anytime — the conversation on disk is never deleted. Routed through _RequestArchive.
        //   • Of Same Folder — close EVERY live managed session sharing this session's EFFECTIVE work dir
        //     (`cwd` — the inferred dir while it infers, else the launch cwd; the SAME key the Explorer
        //     Tree groups it under and the board band paints). _CloseSessionsInFolder shows ONE confirm
        //     listing their titles, then the page does the cross-window batch close.
        // Both defer one tick (the closing flyout's focus restore mustn't race the dialog).
        menu.Items().Append(MenuFlyoutSeparator{});
        MenuFlyoutSubItem closeSub;
        closeSub.Text(L"Close");
        closeSub.Icon(glyphIcon(L"\xE711")); // Close (the X — matches the WT tab menu's "Close ▸")
        AgentSetTip(closeSub, L"Close this session, or every session that shares its folder. Closed sessions stay in Sessions and can be resumed anytime; the conversation on disk is never deleted.");

        MenuFlyoutItem closeThis;
        closeThis.Text(L"This Session");
        closeThis.Icon(glyphIcon(L"\xE711")); // Close (the X)
        AgentSetTip(closeThis, L"Close this session \x2014 shut its tab down. It stays in Sessions and can be resumed anytime; star it there to keep it in your favorites (the conversation on disk is never deleted).");
        closeThis.Click([weak, disp, id](const IInspectable&, const RoutedEventArgs&) {
            if (disp)
            {
                disp.TryEnqueue([weak, id]() { if (auto self = weak.get()) { self->_RequestArchive(id); } });
            }
            else if (auto self = weak.get())
            {
                self->_RequestArchive(id);
            }
        });
        closeSub.Items().Append(closeThis);

        // Of Same Folder — offered only when we have a folder to close (an empty effective dir would be a
        // dead item). `cwd` is the effective work dir already captured for "Open New Session Here", so
        // this needs no registry scan at menu-build time (the menu is rebuilt for every card each refresh
        // — keep it cheap; the enumeration happens lazily on click in _CloseSessionsInFolder).
        if (!cwd.empty())
        {
            MenuFlyoutItem closeFolder;
            closeFolder.Text(L"Of Same Folder");
            closeFolder.Icon(glyphIcon(L"\xE8B7")); // Folder — "everything in this folder"
            AgentSetTip(closeFolder, L"Close EVERY open session whose working directory is the same as this one \x2014 you'll see the full list first and can cancel. Each stays in Sessions, resumable anytime (nothing on disk is deleted).");
            closeFolder.Click([weak, disp, cwd](const IInspectable&, const RoutedEventArgs&) {
                if (disp)
                {
                    disp.TryEnqueue([weak, cwd]() { if (auto self = weak.get()) { self->_CloseSessionsInFolder(cwd); } });
                }
                else if (auto self = weak.get())
                {
                    self->_CloseSessionsInFolder(cwd);
                }
            });
            closeSub.Items().Append(closeFolder);

            // Other of Same Folder — everything in the folder EXCEPT this session (this one stays open) —
            // the WT tab menu's "Close other tabs" twin, scoped to the folder. Passes `id` as the exclude
            // to _CloseSessionsInFolder; a no-op (no dialog) when this is the only session in the folder.
            MenuFlyoutItem closeOtherFolder;
            closeOtherFolder.Text(L"Other of Same Folder");
            closeOtherFolder.Icon(glyphIcon(L"\xE8B7")); // Folder — "everything else in this folder"
            AgentSetTip(closeOtherFolder, L"Close every OTHER open session whose working directory is the same as this one \x2014 this session stays open. You'll see the full list first and can cancel. Each stays in Sessions, resumable anytime (nothing on disk is deleted).");
            closeOtherFolder.Click([weak, disp, cwd, id](const IInspectable&, const RoutedEventArgs&) {
                if (disp)
                {
                    disp.TryEnqueue([weak, cwd, id]() { if (auto self = weak.get()) { self->_CloseSessionsInFolder(cwd, id); } });
                }
                else if (auto self = weak.get())
                {
                    self->_CloseSessionsInFolder(cwd, id);
                }
            });
            closeSub.Items().Append(closeOtherFolder);
        }

        menu.Items().Append(closeSub);
    }

    MenuFlyout AgentManagerContent::_MakeSessionMenu(const std::wstring& id, const std::wstring& cwd, const winrt::Windows::UI::Xaml::FrameworkElement& anchor)
    {
        MenuFlyout menu;
        _PopulateSessionMenu(menu, id, cwd, anchor);
        return menu;
    }

    // Agentmaster (perf — LAZY menus): an EMPTY MenuFlyout that fills itself on Opening (the WT tab
    // context menu's fill-at-flyout-open idiom). A card carried two of these and a row one, each
    // ~13 items + 4 submenus + 2 launch-model pick lists + ~20 tooltips + a registry Get, built for
    // EVERY session on EVERY rebuild and almost never opened — ~50 full menus per refresh at fleet scale
    // (_MakeSessionMenu was ~12-15% of the UI thread's busy samples in the 2026-09-04 release profile).
    // Now the rebuild costs one empty flyout + one handler per site, and an opened menu reflects the
    // session's state AT OPEN time (previously frozen at rebuild time). Items are rebuilt on every
    // open (Items().Clear() first) so a menu can never show a stale "Move to Idle/Done" for a session
    // that has since moved on. The anchor (Tags-panel placement) is held weakly: a rebuilt card's old
    // menu is garbage by then, and a live one resolves its live anchor.
    MenuFlyout AgentManagerContent::_MakeLazySessionMenu(const std::wstring& id, const std::wstring& cwd, const winrt::Windows::UI::Xaml::FrameworkElement& anchor)
    {
        MenuFlyout menu;
        auto weak = get_weak();
        winrt::weak_ref<winrt::Windows::UI::Xaml::FrameworkElement> anchorWeak;
        if (anchor)
        {
            anchorWeak = winrt::make_weak(anchor);
        }
        menu.Opening([weak, id, cwd, anchorWeak](const IInspectable& sender, const IInspectable&) {
            const auto self = weak.get();
            auto m = sender.try_as<MenuFlyout>();
            if (!self || !m)
            {
                return;
            }
            try
            {
                m.Items().Clear();
                const winrt::Windows::UI::Xaml::FrameworkElement anchorNow = anchorWeak ? anchorWeak.get() : nullptr;
                self->_PopulateSessionMenu(m, id, cwd, anchorNow);
            }
            catch (...)
            {
                AgentLogCaughtException(L"_MakeLazySessionMenu Opening"); // an empty menu rather than a dead card
            }
        });
        return menu;
    }

    MenuFlyout AgentManagerContent::_MakeLazyExternalMenu(const ::Agentmaster::ExternalClaudeRow& ex)
    {
        MenuFlyout menu;
        auto weak = get_weak();
        const ::Agentmaster::ExternalClaudeRow row = ex; // the row's facts as of this rebuild (what the eager builder captured too)
        menu.Opening([weak, row](const IInspectable& sender, const IInspectable&) {
            const auto self = weak.get();
            auto m = sender.try_as<MenuFlyout>();
            if (!self || !m)
            {
                return;
            }
            try
            {
                m.Items().Clear();
                self->_PopulateExternalTreeMenu(m, row);
            }
            catch (...)
            {
                AgentLogCaughtException(L"_MakeLazyExternalMenu Opening");
            }
        });
        return menu;
    }

    // Agentmaster: the Auto-Testing message right-click menu. Copy (this prompt's text) is offered on
    // EVERY row; the per-prompt queue ops (Move up / Move down / Delete) appear only on UPCOMING rows
    // (a sent/historical row can't be reordered or unqueued). The queue ops act on `promptId` (the
    // right-clicked row, selecting it first) and — like _MakeSessionMenu — defer one tick so the
    // closing flyout's focus restore doesn't race the list rebuild. (No session-level item here — the
    // whole-session verbs live on the board card / tree row menu, _MakeSessionMenu.)
    MenuFlyout AgentManagerContent::_MakePromptMenu(const std::wstring& promptId, bool upcoming)
    {
        MenuFlyout menu;
        auto disp = _dispatcher;
        auto weak = get_weak();

        // Copy this prompt's text to the clipboard — every row, sent or upcoming. Reads the live
        // queue at click time so it copies the current body; no list rebuild, so no defer needed.
        MenuFlyoutItem copyItem;
        copyItem.Text(L"Copy");
        AgentSetTip(copyItem, L"Copy this prompt's text to the clipboard.");
        copyItem.Click([weak, promptId](const IInspectable&, const RoutedEventArgs&) {
            auto self = weak.get();
            if (!self || !self->_registry || self->_selectedId.empty())
            {
                return;
            }
            std::wstring text;
            if (const auto s = self->_registry->Get(self->_selectedId))
            {
                for (const auto& p : s->queue)
                {
                    if (p.id == promptId)
                    {
                        text = p.text.empty() ? p.label : p.text;
                        break;
                    }
                }
            }
            if (!text.empty())
            {
                CopyTextToClipboard(text);
            }
        });
        menu.Items().Append(copyItem);

        if (upcoming)
        {
            menu.Items().Append(MenuFlyoutSeparator{}); // divide Copy from the queue ops

            MenuFlyoutItem up;
            up.Text(L"Move up");
            AgentSetTip(up, L"Move this queued prompt earlier in the send order.");
            up.Click([weak, disp, promptId](const IInspectable&, const RoutedEventArgs&) {
                if (disp)
                {
                    disp.TryEnqueue([weak, promptId]() { if (auto self = weak.get()) { self->_selectedPromptId = promptId; self->_OnMovePrompt(-1); } });
                }
                else if (auto self = weak.get())
                {
                    self->_selectedPromptId = promptId;
                    self->_OnMovePrompt(-1);
                }
            });
            menu.Items().Append(up);

            MenuFlyoutItem down;
            down.Text(L"Move down");
            AgentSetTip(down, L"Move this queued prompt later in the send order.");
            down.Click([weak, disp, promptId](const IInspectable&, const RoutedEventArgs&) {
                if (disp)
                {
                    disp.TryEnqueue([weak, promptId]() { if (auto self = weak.get()) { self->_selectedPromptId = promptId; self->_OnMovePrompt(1); } });
                }
                else if (auto self = weak.get())
                {
                    self->_selectedPromptId = promptId;
                    self->_OnMovePrompt(1);
                }
            });
            menu.Items().Append(down);

            MenuFlyoutItem del;
            del.Text(L"Delete");
            AgentSetTip(del, L"Remove this prompt from the queue \x2014 it won't be sent.");
            del.Click([weak, disp, promptId](const IInspectable&, const RoutedEventArgs&) {
                if (disp)
                {
                    disp.TryEnqueue([weak, promptId]() { if (auto self = weak.get()) { self->_selectedPromptId = promptId; self->_OnDeletePrompt(); } });
                }
                else if (auto self = weak.get())
                {
                    self->_selectedPromptId = promptId;
                    self->_OnDeletePrompt();
                }
            });
            menu.Items().Append(del);
        }

        return menu;
    }

    void AgentManagerContent::_OnRenameSession(const std::wstring& id)
    {
        if (id.empty())
        {
            return;
        }
        // Agentmaster: the rename editor is IN-PLACE in the Explorer Tree — it only exists once
        // _RebuildTree renders this session's row. Invoked from a Triage-Board card (the board
        // shows the WHOLE fleet) that row may not currently render: the tree sits in EXTERNAL
        // scope, the session is hosted by ANOTHER window while the scope is LOCAL, or its
        // directory group is collapsed. Make the row renderable first — widen the lens, never the
        // data — so the editor always appears. The tree/F2 path (whose row is already visible)
        // passes every check unchanged.
        if (_treeScope == TreeScope::External)
        {
            // _SetTreeScope handles the leaving-EXTERNAL selection cleanup + both toggle labels +
            // the lens push; skip its refresh — this method refreshes once at the end.
            _SetTreeScope(TreeScope::Local, /*refresh*/ false);
        }
        if (_treeScope == TreeScope::Local && _localScopeProvider)
        {
            const auto localIds = _localScopeProvider();
            if (localIds.find(id) == localIds.end())
            {
                _SetTreeScope(TreeScope::Global, /*refresh*/ false); // hosted by another window — its row only renders in GLOBAL
            }
        }
        if (_registry)
        {
            if (const auto s = _registry->Get(id))
            {
                // Un-collapse the session's directory group (PathEq-aware: the collapsed set keeps
                // the first-seen spelling, which can differ from workingDir by case/slashes). The
                // group key is the EFFECTIVE work dir (_WorkDirOf) — the dir the tree renders this
                // row under — not necessarily the launch cwd.
                bool uncollapsed = false;
                for (auto it = _collapsedDirs.begin(); it != _collapsedDirs.end();)
                {
                    if (PathEq(*it, _WorkDirOf(*s)))
                    {
                        it = _collapsedDirs.erase(it);
                        uncollapsed = true;
                    }
                    else
                    {
                        ++it;
                    }
                }
                if (uncollapsed)
                {
                    _NotifyLensChanged(); // collapsed dirs are part of the per-window lens (M10)
                }
            }
        }
        _renamingId = id;
        _renameBox = nullptr; // bootstrap: force the next rebuild to create + focus the editor
        _Refresh();
    }

    void AgentManagerContent::_CommitRename()
    {
        if (_renamingId.empty())
        {
            return;
        }
        const auto id = _renamingId;
        std::wstring name = _renameBox ? std::wstring{ _renameBox.Text() } : std::wstring{};
        // Trim surrounding whitespace; an empty/whitespace name keeps the old title.
        const auto first = name.find_first_not_of(L" \t\r\n");
        const auto last = name.find_last_not_of(L" \t\r\n");
        name = (first == std::wstring::npos) ? std::wstring{} : name.substr(first, last - first + 1);

        _renamingId.clear();
        _renameBox = nullptr;
        if (!name.empty())
        {
            // The title is ONE value: the Explorer-tree name == the WT tab title == the persisted
            // SessionInfo.title. Route through the page so it updates the shared registry AND
            // retitles the session's tab in lockstep; the direct registry write is the fallback
            // when unwired (e.g. the standalone tests).
            if (_renameHandler)
            {
                _renameHandler(winrt::hstring{ id }, winrt::hstring{ name });
            }
            else if (_registry)
            {
                _registry->Update(id, [&](SessionInfo& s) { s.title = name; });
            }
        }
        _Refresh();
    }

    void AgentManagerContent::_CancelRename()
    {
        if (_renamingId.empty())
        {
            return;
        }
        _renamingId.clear();
        _renameBox = nullptr;
        _Refresh();
    }

    void AgentManagerContent::_RequestArchive(const std::wstring& id)
    {
        // "Delete"/"Archive" in the Manager routes to the page's archive seam, which PRESENTS THE
        // CONSEQUENCE (gated by the cog's confirmBeforeKill) and closes the tab — the SAME path as
        // clicking the tab's own X. We deliberately do not confirm here, to avoid a double dialog.
        if (id.empty())
        {
            return;
        }
        if (_archiveHandler)
        {
            _archiveHandler(winrt::hstring{ id });
        }
    }

    // Agentmaster (board/tree session menu — "Close ▸ Of Same Folder" / "Other of Same Folder"): forward
    // to the page, which owns the SHARED confirm+close (_ConfirmAndCloseClaudeSessionsInFolder — the same
    // entry the WT tab-strip "Close ▸" submenu uses, so the two surfaces can never drift). The page
    // enumerates the fleet's LIVE sessions in `folder` (skipping `excludeId` — set by "Other of Same
    // Folder" to this session's id so it stays open), shows ONE confirm LISTING the titles, then does the
    // cross-window batch close. A no-folder / no-handler click is a no-op.
    void AgentManagerContent::_CloseSessionsInFolder(const std::wstring& folder, const std::wstring& excludeId)
    {
        if (folder.empty() || !_closeFolderHandler)
        {
            return;
        }
        _closeFolderHandler(winrt::hstring{ folder }, winrt::hstring{ excludeId });
    }

    // A buttons-only confirm (XAML-Islands-safe: a text box inside a ContentDialog gets no
    // keypresses, but buttons work — see the _renameBox note). Runs onYes when the user accepts.
    // Used for the Close confirm (_RequestArchive) and Reopen-windows.
    void AgentManagerContent::_Confirm(const winrt::hstring& title, const winrt::hstring& body, const winrt::hstring& primary, std::function<void()> onYes)
    {
        ContentDialog dialog;
        dialog.Title(winrt::box_value(title));
        dialog.Content(winrt::box_value(body));
        dialog.PrimaryButtonText(primary);
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Close); // safe default = Cancel
        if (_root)
        {
            try
            {
                dialog.XamlRoot(_root.XamlRoot());
                dialog.RequestedTheme(_root.ActualTheme());
            }
            catch (...)
            {
            }
        }
        auto cb = std::move(onYes);
        dialog.PrimaryButtonClick([cb](const ContentDialog&, const ContentDialogButtonClickEventArgs&) {
            // Never let a confirm callback throw out of the XAML event handler: an escaped exception
            // would reach the app's unhandled-exception path (an assert/FailFast in Debug).
            try
            {
                if (cb)
                {
                    cb();
                }
            }
            CATCH_LOG();
        });
        try
        {
            dialog.ShowAsync();
        }
        CATCH_LOG(); // Agentmaster: a silently-swallowed ShowAsync = a user-action confirm dialog that never appears (a confusing no-op with no trace) — log it (matches the CATCH_LOG on the button-click above)
    }

    // Agentmaster: a THREE-way buttons-only choice (Primary / Secondary / Cancel) — the XAML-Islands-safe
    // dialog with both an extra button and a second callback. Used by Adopt to offer "Fork a copy" (safe:
    // a new transcript/rollout) vs. "Resume anyway" (take over the same conversation). Primary is the
    // DEFAULT (the safe Fork choice); Close (Cancel) does nothing. Both callbacks are exception-guarded
    // like _Confirm so nothing escapes into the XAML handler.
    void AgentManagerContent::_ConfirmChoice(const winrt::hstring& title, const winrt::hstring& body, const winrt::hstring& primary, const winrt::hstring& secondary, std::function<void()> onPrimary, std::function<void()> onSecondary)
    {
        ContentDialog dialog;
        dialog.Title(winrt::box_value(title));
        dialog.Content(winrt::box_value(body));
        dialog.PrimaryButtonText(primary);
        dialog.SecondaryButtonText(secondary);
        dialog.CloseButtonText(L"Cancel");
        dialog.DefaultButton(ContentDialogButton::Primary); // safe default = the primary (Fork) choice
        if (_root)
        {
            try
            {
                dialog.XamlRoot(_root.XamlRoot());
                dialog.RequestedTheme(_root.ActualTheme());
            }
            catch (...)
            {
            }
        }
        auto p = std::move(onPrimary);
        auto s = std::move(onSecondary);
        dialog.PrimaryButtonClick([p](const ContentDialog&, const ContentDialogButtonClickEventArgs&) {
            try
            {
                if (p)
                {
                    p();
                }
            }
            CATCH_LOG();
        });
        dialog.SecondaryButtonClick([s](const ContentDialog&, const ContentDialogButtonClickEventArgs&) {
            try
            {
                if (s)
                {
                    s();
                }
            }
            CATCH_LOG();
        });
        try
        {
            dialog.ShowAsync();
        }
        CATCH_LOG(); // Agentmaster: a silently-swallowed ShowAsync = a user-action confirm dialog that never appears (a confusing no-op with no trace) — log it (matches the CATCH_LOG on the button-click above)
    }

    // ---- Archived-sessions overlay: REMOVED (FAVORITES.md) ------------------
    // The in-content archive overlay AND the full-window Archive page are gone — the Sessions
    // browser is the sole history view. Closing a session keeps it (always archived), resumable
    // from Sessions and marked by Favorite.

    // Agentmaster: keep-awake control (tri-mode). SetThreadExecutionState's ES_CONTINUOUS flag is per-thread
    // and persists for the life of the calling thread (or until reset) — this runs on the window's UI thread,
    // which lives as long as the window, so no timer/poll loop is needed (unlike stay-awake.ps1, which loops
    // only because its host PowerShell would otherwise exit). The flag is system-wide while ANY thread holds
    // it; per-window holds compose fine (the PC stays awake while any window holds it). On window close the UI
    // thread exits and the per-thread flag is auto-released — so no explicit teardown is needed.
    //
    // _CycleKeepAwake advances the user's selected MODE; _RefreshKeepAwakeHold maps the mode to a desired hold.
}
