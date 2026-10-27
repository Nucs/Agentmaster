// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: content for the pinned, leftmost "Agent Manager" tab — the C1 "Linked
// Lenses" UI (DESIGN §9). Three regions over ONE shared model (the SessionRegistry):
//   * Triage Board (top)        — sessions as cards in hook-driven state columns
//   * Explorer Tree (bottom-left) — the M working directories -> their N sessions
//   * Auto Testing (bottom-right) — the selected session's prompt queue + Autorunner
// with bidirectional selection sync. Built imperatively (no IDL/XAML markup, like
// ScratchpadContent). The views are snapshot-driven: on any registry change we rebuild
// from SessionRegistry::Snapshot() on the UI thread.

#pragma once
#include "winrt/TerminalApp.h"
#include "BasicPaneEvents.h"
#include "AgentMaster/SessionModels.h"
#include "AgentMaster/LaunchPresets.h" // Agentmaster (PRESETS, HARNESSES.md): LaunchPresetsConfig — the Presets tab's by-value draft
#include "AgentMaster/MessagingFileLinks.h" // Agentmaster (MESSAGING.md §21.32.4): FileLinkLanAddress — the cog's FILE LINKS dropdown (pure, no Windows header)
#include "AgentLocalTooltip.h" // Agentmaster (LocalTooltip): the cog's designated-area tooltip panel (left-outside the card)

#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h> // Popup (the path-picker drop-down)
#include <winrt/Microsoft.UI.Xaml.Controls.h> // muxc::ColorPicker (the cog's "status flashing color" picker)

#include <vector>

#include <functional>
#include <map> // _boardTagColors (folded tag name -> "#AARRGGBB", the card ribbons' user-picked colors)
#include <memory>
#include <optional> // _MakeBoardTagChip's ribbon color (nullopt == the hollow "Untagged" outline)
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Agentmaster
{
    class SessionRegistry;
}

namespace Agentmaster::Updater
{
    struct UpdateInfo; // Updater.h (included by the .cpp only — winhttp/comctl must not fan out through this header)
}

namespace winrt::TerminalApp::implementation
{
    // Agentmaster (the shared LAUNCH BAR — AgentLaunchBar.h, included by the .cpp TUs that build or call it): the toolbar's
    // launch row is an AgentLaunchBar the content hosts. Forward-declared so this widely-included header does not pull the
    // bar's header into every TU; shared_ptr members + by-value declarations need no complete type here.
    class AgentLaunchBar;
    struct AgentLaunchBarHost;
    struct AgentLaunchRecentDirs;

    class AgentManagerContent : public winrt::implements<AgentManagerContent, IPaneContent>, public BasicPaneEvents
    {
    public:
        AgentManagerContent();
        ~AgentManagerContent(); // Agentmaster (M9): detach our observer from the shared registry

        // Wiring from the page (called right after construction).
        void SetRegistry(std::shared_ptr<::Agentmaster::SessionRegistry> registry);
        void SetSpawnHandler(std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> handler); // (workingDir, title, model) — model is the launch-model picker's per-LAUNCH `--model <id>` pick from an "Open New Session Here" submenu ("" = Default, the settings model)
        void SetActivateHandler(std::function<void(winrt::hstring)> handler); // (sessionId) -> jump to tab
        // Agentmaster (eager-init / "Activate Tab"): (sessionId) -> start a DORMANT session's claude IN
        // PLACE (no focus change) — distinct from SetActivateHandler ("Jump to Tab", which switches to it).
        void SetActivateDormantHandler(std::function<void(winrt::hstring)> handler);
        // Agentmaster (eager-init / "Activate All Tabs"): (allWindows) -> wake every dormant managed tab.
        // allWindows=false => this window only; true => this window + fan out to every other window.
        void SetActivateAllHandler(std::function<void(bool)> handler);
        // Agentmaster (eager-init / "Activate All Tabs" PACING): the page drives a DRIP-FED wake (500ms
        // apart, 4 per 10s), so the operation runs for many seconds — the page calls this true when its
        // drip starts and false when it drains, so the Manager makes the "in progress" state obvious:
        // the button relabels "Activating N tabs…" + goes disabled, and the launch/cwd box is disabled
        // (a clear "busy, hands off" cue). UI thread only.
        void SetActivateAllBusy(bool busy);
        // Agentmaster (Linked Lenses): report a pointer enter/leave on a managed session's board card
        // / tree row (id, entering). The PAGE owns the effective-hover bookkeeping (so it can't desync
        // from a missed PointerExited on a keyboard tab-switch) and pills that session's terminal tab
        // while the Manager tab is active — a live preview that follows the mouse, falling back to the
        // selected session.
        void SetHoverSessionHandler(std::function<void(winrt::hstring, bool)> handler);
        // Agentmaster (tab-strip reveal): (sessionId) -> a board card / tree row was CLICKED (a plain
        // single click that selects it). The page scrolls the tab strip so that session's tab is on screen
        // — also when the click did not CHANGE the selection (a re-click of the already-selected card),
        // which the lens-changed push cannot express: _SelectSession early-outs on the same id, so the page
        // would otherwise never hear about it. The page applies its own conditions (the Manager tab is the
        // active tab; the tab lives in that window) — see TerminalPage::_RevealClickedSessionTab.
        void SetRevealSessionTabHandler(std::function<void(winrt::hstring)> handler);
        void SetArchiveHandler(std::function<void(winrt::hstring)> handler); // (sessionId) -> Close (shut down, keep the record so it stays resumable in Sessions; FAVORITES.md)
        void SetCloseFolderHandler(std::function<void(winrt::hstring, winrt::hstring)> handler); // (folder, excludeId) -> "Close ▸ Of Same Folder" / "Other of Same Folder": close every live managed session whose effective work dir == folder, SKIPPING excludeId when non-empty (the content shows the ONE confirm listing titles; the page does the cross-window batch)
        // Agentmaster (session menu "Deactivate ▸" / "Close ▸" — the WT tab menu's two submenus, mirrored on the board
        // card, the tree row and the side-panel card): WHICH tabs one item of those submenus reaches, always counted from
        // the menu's session and its tab. The values ARE the WT tab menu's own scope codes (Tab.h kDeactivateScope*; the
        // page's wiring static_asserts each one), so a card item and the same item on the session's own tab can never
        // name different tabs. The strip scopes act in the window that HOSTS the session's tab, which need not be the
        // window the menu was opened in: the board, the tree and the side panel can list every window's sessions.
        enum class SessionMenuScope : int32_t
        {
            ThisTab = 0, // the session's own tab
            OtherTabs = 1, // every OTHER tab of the hosting window; the session's own tab is kept
            AllTabs = 2, // every tab of the hosting window, the session's own included
            TabsBefore = 3, // the tabs LEFT of the session's tab in the hosting window's strip; it and the tabs right of it are kept
            TabsAfter = 4, // the tabs RIGHT of it; it and the tabs left of it are kept
            SameFolder = 5, // every live session whose effective work dir is this session's, in EVERY window, this one included
            OtherSameFolder = 6, // the same folder batch minus this session, which is kept
            IdleTabs = 7, // Deactivate only ("Deactivate All Idle Tabs", 2026-10-24): every tab whose session is Idle (the gray dot) in the window the menu was opened in — or in every window, when other windows hold idle tabs too and the page's question is answered so (TerminalPage::_DeactivateIdleTabsAsking); never routed to the session's own window; Close has no twin
            FavoriteAllTabs = 8, // Close only ("★ Favorite & Close All Tabs"): AllTabs with every managed session of that window starred first; Deactivate has no twin. Was 7 until IdleTabs took that number (never stored: the code lives one click long)
        };
        // Agentmaster: the verb of a SessionMenuScope item — what happens to the tabs it reaches. Deactivate PARKS them
        // (the agent shuts down, the tab keeps its place and resumes the conversation when it is focused or activated;
        // nothing asks first, so a turn in flight is cut off). Close ENDS them through the usual close, which asks first
        // where the tab menu's close asks (every closed session stays resumable in Sessions).
        enum class SessionMenuVerb : int32_t
        {
            Deactivate = 0,
            Close = 1,
        };
        // Agentmaster: where a session's tab sits in THIS window's tab strip. The session menu grays its strip items from
        // it exactly as the WT tab menu grays its own (Tab::_EnableMenuItems): "Tabs to the Left" on the left-most tab,
        // "Tabs to the Right" on the right-most one, "Other Tabs" on a window's only tab. A session hosted in ANOTHER
        // window has no spot here, and its strip items then stay enabled — that window applies them, and a scope with no
        // tab in it does nothing (its hooks.log line says so). "Deactivate All Idle Tabs" does not read the spot: it acts
        // in the window the menu is open in, whatever window hosts the session, so its gray-out is SetIdleTabsProvider's.
        struct SessionTabSpot
        {
            uint32_t index{ 0 }; // the tab's index in the strip (0 == the left-most tab)
            uint32_t count{ 0 }; // how many tabs the strip holds, this one included (>= 1 for a spot that exists)
        };
        // Agentmaster (session menu "Deactivate ▸" / "Close ▸"): install the page's action behind every item of the two
        // submenus EXCEPT Close's two folder items, which keep the folder close this content already confirms and
        // forwards (SetCloseFolderHandler) — so those two behave exactly as before.
        //   handler — (sessionId, verb, scope): the page runs that item as the session's own tab menu would, in the window
        //             that hosts the tab (TerminalPage::_RunSessionMenuScope). Called on the UI thread, one dispatcher tick
        //             after the click (the closing menu's focus hand-back lands first). It must not throw: the item's
        //             dispatcher callback logs an escaping exception and drops it. Unwired => the items do nothing.
        //             Replaces any previous handler.
        // Returns nothing; throws nothing (a std::function move).
        void SetSessionMenuScopeHandler(std::function<void(winrt::hstring, SessionMenuVerb, SessionMenuScope)> handler);
        // Agentmaster (session menu "Deactivate ▸" / "Close ▸"): install the page's answer to "where does this session's tab
        // sit in THIS window's strip?" — read once each time a session menu opens, to gray the strip items like the tab menu.
        //   provider — (sessionId) -> the tab's SessionTabSpot, or nullopt when this window does not host the tab. A throw
        //              is caught and logged by the menu, which then treats the tab as hosted elsewhere (every strip item
        //              enabled). Unwired => the same. Replaces any previous provider.
        // Returns nothing; throws nothing (a std::function move).
        void SetSessionTabSpotProvider(std::function<std::optional<SessionTabSpot>(const std::wstring&)> provider);
        // Agentmaster (session menu "Deactivate ▸ Deactivate All Idle Tabs", 2026-10-24): install the page's answer to
        // "would that item park anything?" — read each time a session menu builds its Deactivate ▸, to gray the item like
        // the tab menu grays its own (TerminalPage.cpp's tab-flyout Opening handler).
        //   provider — () -> true when at least one tab in THIS window or another one would park now (TerminalPage::
        //              _IdleTabsToDeactivate / _IdleSessionsElsewhere: the lists the click acts on and offers). A throw is
        //              caught and logged by the menu, which then leaves the item enabled (the click reads the states again
        //              and parks nothing when there is nothing). Unwired => enabled. Replaces any previous provider.
        // Returns nothing; throws nothing (a std::function move).
        void SetIdleTabsProvider(std::function<bool()> provider);
        void SetRestoreHandler(std::function<void(winrt::hstring)> handler); // (sessionId) -> re-launch (resume) a closed session
        // Agentmaster: the Launch box accepts EITHER a working dir OR a session id. A FOUND session id
        // turns the launch button into "Resume session" (resume the conversation) and reveals a "Fork"
        // button (fork it into a new conversation). Both resolve (dir, title) in the content, so the
        // page just forwards to _ResumeSessionFromDisk / _ForkSessionFromDisk.
        void SetResumeSessionHandler(std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> handler); // (sessionId, dir, title)
        void SetForkSessionHandler(std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> handler); // (sessionId, dir, title)
        // Agentmaster (Triage Board / Explorer-tree session menu — mirror the WT tab's "Restart session"
        // / "Fork session"). Restart: (sessionId) -> rebuild the live ConPTY connection in place (the
        // page does it local-first, then fans out to the hosting window). Fork: (sessionId) -> the
        // kind-aware fork the WT tab menu uses (Claude --fork-session / Codex `codex fork`), opening the
        // fork tab in the acting window. Both are kind-agnostic at this seam — the page branches.
        void SetRestartSessionHandler(std::function<void(winrt::hstring)> handler); // (sessionId) -> restart a managed session's connection in place
        void SetForkManagedSessionHandler(std::function<void(winrt::hstring, winrt::hstring)> handler); // (sessionId, model) -> fork a managed session (kind-aware), like the WT tab's "Fork session"; model = the launch-model picker's per-LAUNCH `--model <id>` pick ("" = Default; always "" for Codex — its item is plain)
        void SetRenameHandler(std::function<void(winrt::hstring, winrt::hstring)> handler); // (sessionId, newTitle) -> rename in the registry + retitle the WT tab (the one title)
        void SetTagsHandler(std::function<void(winrt::hstring, winrt::Windows::UI::Xaml::FrameworkElement)> handler); // Agentmaster (bookmark tags): (sessionId, anchor) -> the page opens its tag editor panel under the clicked board card / tree row (the WT tab menu's "Tags" twin)
        // Agentmaster (transcript overlay — AgentTranscriptOverlay.h): the session menu's "Show Transcript"
        // (below "Copy ▸") / "Copy ▸ Transcript ▸ Show" and the EXTERNAL menu's "Show Transcript" open a
        // conversation in the page's window-level reading view. (sessionId, codex, codexRolloutId, title):
        // the content passes the facts itself because an EXTERNAL row is not in the registry. Unwired =>
        // the items are inert.
        void SetShowTranscriptHandler(std::function<void(winrt::hstring, bool, winrt::hstring, winrt::hstring)> handler);
        // Agentmaster (bookmark tags): the pointer entered/left one of a board card's bookmark
        // ribbons. begin(tag, anchor) -> the page shows its rich tag hover panel (every session
        // carrying the tag + its status dot; clicking a live row jumps to its tab) anchored at the
        // ribbon; end() cancels a pending open / grace-closes an open panel — the tab badges' + the
        // Sessions Tags column's exact hover behavior, fanned through the page because the panel
        // (and the carriers-of-a-tag data) live there. The Manager content sits inside the page's
        // visual tree, so the ribbon transforms into Root() space like any main-tree badge.
        void SetTagHoverHandlers(std::function<void(winrt::hstring, winrt::Windows::UI::Xaml::UIElement)> begin, std::function<void()> end);
        // Agentmaster: adopt an EXTERNAL (observe-only) claude from the Explorer Tree's EXTERNAL scope.
        // (pid, workingDir) -> the page resolves the conversation id from the transcript and resumes it
        // into a NEW managed, controllable tab (`claude --resume <id>`), or launches fresh if it has no
        // transcript. The original external process is left running (we never inject into / kill it).
        // (pid, cwd, fork): fork==true branches the conversation into a NEW transcript (safe while the
        // original is live); fork==false resumes the same conversation (true take-over). The user picks
        // in the Adopt dialog; an external with no transcript launches fresh either way.
        void SetAdoptExternalHandler(std::function<void(uint32_t, winrt::hstring, bool)> handler);
        // Agentmaster (Codex-launch): the EXTERNAL-tree menu for a Codex row. (pid, cwd, adopt, fork):
        // adopt=true brings that codex's rollout under management (fork=true => `codex fork` into a NEW
        // rollout [safe while the original is live]; fork=false => `codex resume` the same); adopt=false
        // launches a fresh managed Codex in cwd (fork ignored). Routes to _AdoptExternalCodex / _SpawnCodexSession.
        void SetCodexLaunchHandler(std::function<void(uint32_t, winrt::hstring, bool, bool)> handler);
        // Agentmaster: the set of session ids hosted in THIS window (the page's _claudeTabs).
        // Used by the Explorer Tree's LOCAL scope to show only this window's sessions; GLOBAL
        // ignores it and shows every window's sessions (the whole process-wide registry).
        void SetLocalScopeProvider(std::function<std::unordered_set<std::wstring>()> provider);
        // Agentmaster (focus-steal fix): returns true iff this Manager's hosting window is the OS
        // FOREGROUND window. _Refresh re-focuses the keyboard-focused board card/row after a rebuild,
        // and in XAML Islands Control.Focus() escalates to Win32 activation of the island's host window
        // — so a refresh that fires on a BACKGROUND Manager window (the Triage Board sitting behind a
        // Claude tab you're working in, in another window) would yank the OS foreground to the Manager
        // on every ~2s observer/registry tick. The page wires this to GetForegroundWindow()==hwnd; when
        // backgrounded the user isn't keyboard-navigating this board, so the focus-restore is skipped.
        void SetWindowForegroundProvider(std::function<bool()> provider);
        // Agentmaster (the GLOBAL PAUSE — DELIVERY.md §17): the "Pause Prompt Queue Runner" button's action.
        // The button only ASKS: the handler (the hosting page) sets the process-wide state on the ONE
        // scheduler, and the button's label then follows SetGlobalPauseShown. This content never decides
        // the state itself — a per-window flag here is what let two windows disagree.
        //   handler — called on the UI thread with the state the click asks for (true == pause all).
        void SetPauseHandler(std::function<void(bool)> handler);
        // Agentmaster (the GLOBAL PAUSE — DELIVERY.md §17): show the process-wide pause state on this
        // window's "Pause / Resume Prompt Queue Runner" button. The scheduler owns the state; the page
        // calls this when it seeds the content and whenever ANY window changes the pause, so every
        // window's button reads the same and the next click always asks for the opposite of the TRUE
        // state. UI thread only (it touches a XAML control). A no-op for the label while the button is
        // not built yet; the state is remembered either way.
        //   paused — true == every session's automatic send is held (the button reads "Resume Prompt Queue Runner").
        //   throws — winrt::hresult_error if the XAML control rejects the content (not expected).
        void SetGlobalPauseShown(bool paused);
        void SetSettings(const ::Agentmaster::AppSettings& settings); // seed the cog dialog's current values
        void SetSettingsHandler(std::function<void(::Agentmaster::AppSettings)> handler); // persist on Save
        // Agentmaster (cross-window settings broadcast): adopt GLOBAL settings that were changed in
        // ANOTHER window (the cog Save, or the Explorer-Tree / Triage-Board sort toggle) and re-apply the
        // bits THIS window renders live — refresh _appSettings (so future spawns use the latest globals)
        // + repaint the tree/board sort toggles + re-sort the views. Unlike SetSettings it does NOT
        // re-seed the Launch cwd box (that would stomp in-progress typing) and it forces an immediate
        // _Refresh. The engine's settings sink marshals onto this UI thread before calling it; the
        // SOURCE window is excluded by BroadcastSettingsChanged (it already applied the change itself).
        void ApplyExternalSettings(const ::Agentmaster::AppSettings& settings);
        // Agentmaster (M10 Increment 3; PERSISTENCE.md §13.5): the "Reopen Windows (N)" recover
        // button's action — reopen saved windows that are NOT currently open (the runtime analog of the
        // WindowEmperor's startup reopen loop). The content computes N itself
        // (::Agentmaster::RecoverableWindows) and shows the button only when N>0.
        void SetReopenWindowsHandler(std::function<void()> handler);
        // Agentmaster (Sessions page; SESSIONS.md / FAVORITES.md): the Manager's "Sessions" button opens
        // the full-window browser over EVERY on-disk Claude Code session — the sole history view.
        void SetOpenSessionsHandler(std::function<void()> handler);
        // Agentmaster (Sessions page; SESSIONS.md): the Settings cog's "Reset hidden sessions"
        // button — clear the user's "Hide from list" set (AppSettings.hiddenSessionIds). The page
        // (TerminalPage) owns the list + the Sessions browser, so the cog just fires the action there.
        void SetResetHiddenSessionsHandler(std::function<void()> handler);
        // Agentmaster (updater; Updater.h): "Update now" closes the app gracefully
        // (TerminalPage::RequestQuit) so the embedded installer can Add-AppxPackage the new build and
        // relaunch. Fired only AFTER the installer has been launched detached.
        void SetQuitForUpdateHandler(std::function<void()> handler);
        // Agentmaster: the Explorer Tree's "refresh" button (after the sort toggle) — reload the data
        // for the CURRENT scope. The content redraws immediately; this fires so the page can force the
        // Fleet Observer to re-survey now (re-enrich the registry + recompute the External census)
        // instead of waiting for the next tick. Optional — unwired, the button is a plain redraw.
        void SetRefreshHandler(std::function<void()> handler);
        // Agentmaster (PENDING_INPUT.md §8): the session menu's "Copy Current Prompt" wants the UNSENT
        // draft as the input box holds it right now. Only the window that HOSTS a session's tab can read
        // its buffer, and the board/tree show the WHOLE fleet — so this provider (wired by the page,
        // TerminalPage::_ReadLiveDraftForSession) answers "" for a session hosted elsewhere, dormant, or
        // unreadable, and the copy then falls back to the observer's recorded SessionInfo::pendingInput.
        // Optional — unwired, every copy simply uses the remembered draft.
        void SetLiveDraftProvider(std::function<std::wstring(const std::wstring&)> provider);
        // Agentmaster: force a UI redraw (board/tree/plan) from the current data — the page calls this
        // after an out-of-band reload (the observer survey lands asynchronously) so fresh data shows.
        void RefreshNow();
        // Agentmaster (tags perf, 2026-10-10): a bookmark tag was added to, removed from or recolored on a
        // session. Tags live in the SessionStore, never the registry, so no registry notify reports the
        // change; the page calls this after every tag edit of this window (TerminalPage::
        // _RefreshManagerBoardTags). It replaced a RefreshNow() there, which cost more than a tag needs:
        //   - HIDDEN (a tab is on screen — where the Tags panel is used most): only the dirty flag is set,
        //     and SetLensVisible(true) runs the one catch-up rebuild. RefreshNow's hidden branch also
        //     re-evaluated the keep-awake hold, which in the While-running mode deep-copies every live
        //     session; a tag cannot change that hold.
        //   - VISIBLE: only the BOARD is rebuilt (the cards' ribbons, the header's tag chips, and which
        //     cards pass the tag filter). The Explorer Tree and the Prompt Queue show no tag, so they keep
        //     their elements, their scroll position and their focus.
        // UI thread only. No-op before the layout is built.
        //   throws - what a board rebuild throws (XAML element creation; std::bad_alloc). Nothing while hidden.
        void NoteTagsChanged();
        // Agentmaster (perf — the hidden-lens rebuild tax): is this Manager's content ON SCREEN (the
        // window's Agent Manager page shown)? The page drives it from the page show/hide + the tab-switch
        // funnel. While HIDDEN, every refresh trigger — a registry notify, the 30s timing tick, a
        // tab-switch selection sync — only marks the
        // lens DIRTY instead of rebuilding the whole board + tree + plan (the full XAML teardown +
        // recreate of every card/row/menu, ~1-6s at fleet scale, measured on the ONE Emperor UI
        // thread that every window shares) for pixels nobody can see; becoming visible again runs the
        // ONE catch-up rebuild. Defaults to visible; the page host hides it right at creation
        // (TerminalPage::_CreateAgentManagerPage), since the page starts off screen until Home shows it.
        void SetLensVisible(bool visible);
        // Agentmaster (Linked Lenses — the per-tab -> Manager sync): select a managed session in the
        // lens from OUTSIDE. The page calls this when the user switches to that session's terminal tab,
        // so returning to the Manager tab shows the session you were just in selected (board card + tree
        // row highlighted + its Auto Testing). Equivalent to a single-click on the session's board card;
        // a no-op when the id is empty or already selected. Marshal to the UI thread is the caller's job.
        void SelectSession(winrt::hstring id);
        // Agentmaster (Linked Lenses — selection VISIBILITY sync): scroll the currently-selected board
        // card / Explorer-tree row into view. The page calls this when the user switches TO the Manager
        // tab: while it was hidden the selection followed the user's tab switches (SelectSession), so the
        // highlighted card can be scrolled off-screen on return — this synchronizes its visibility with
        // its highlight. A no-op when nothing managed is selected or the card/row isn't currently shown.
        void BringSelectedIntoView();
        // Agentmaster (Linked Lenses): the currently-selected managed session id (the board card /
        // tree row selection), or empty. The page reads it live to decide which tab wears the
        // "selected/active" pill when nothing is hovered.
        winrt::hstring SelectedSessionId() const noexcept { return winrt::hstring{ _selectedId }; }
        // Agentmaster (SIDE PANEL): advance the GLOBAL Triage-Board card sort (AppSettings::boardSort,
        // MOST ACTIVE -> NEWEST -> OLDEST -> A-Z -> wrap) — the SAME cycle + persist + re-sort path the
        // board header's own sort toggle runs (_CycleBoardSort: mutate, persist via the settings sink
        // — which preserves out-of-form fields, saves settings.json, and broadcasts to other windows —
        // then refresh this lens). Exposed so the side panel's order button shares ONE sort spec with
        // the board instead of growing a second, drift-prone setting. The sink runs synchronously on
        // the UI thread, so on return the page's AppSettings copy already carries the new order — the
        // caller can re-read boardSort and re-sort its own list immediately. UI thread only.
        void CycleBoardSort() { _CycleBoardSort(); }
        // Agentmaster (SIDE PANEL): build the full session context menu — the SAME lazily-populated
        // MenuFlyout the Triage-Board card / Explorer-tree row shows (_MakeLazySessionMenu: Jump to
        // Tab / Rename / Restart / Close ▸ / Open New Session Here ▸ model picker / Tags / Copy ▸ /
        // ...), filled at Opening from the live registry so a stale snapshot can never mis-target.
        // Exposed so a side-panel card offers the identical action set with zero drift — every item
        // routes through this content's already-wired callbacks (activate / rename / close / spawn),
        // exactly as if the user had right-clicked the board card. CAVEAT for hosts outside the
        // Manager page: Rename opens the Explorer tree's in-place editor, which lives on the (possibly
        // hidden) Agent Manager page — the rename works, but the editor is only visible after opening
        // the page (Home). `id` = the managed session; `cwd` = its effective working dir (feeds the
        // folder-wide items); `anchor` = the element the menu was opened on (the side-panel card) —
        // the menu's Tags item opens the tag panel right UNDER it, like a board card's; held weakly,
        // so a card rebuilt away before the click just falls back to the panel's default spot (the
        // window's top-left, which is also what an omitted anchor gets). Returns a menu the caller
        // shows via ShowAt; never null. UI thread only. Throws winrt::hresult_error only if the empty
        // flyout itself can't be created (the items are built later, guarded, at Opening).
        winrt::Windows::UI::Xaml::Controls::MenuFlyout BuildSessionMenuFor(const std::wstring& id,
                                                                           const std::wstring& cwd,
                                                                           const winrt::Windows::UI::Xaml::FrameworkElement& anchor = nullptr)
        {
            return _MakeLazySessionMenu(id, cwd, anchor);
        }
        // Agentmaster (SIDE PANEL "+" launcher, SIDE_PANEL.md §2e): the launch box's current text — what the side panel's
        // launcher card shows when it opens, so the two boxes read the same (the user's "keep the last, the same one the
        // Agent Manager has"). The text follows everything that writes this box: typing here, the selection pre-aim
        // (_SelectSession / _SelectExternal), the defaultLaunchDir seed, and the card's own edits (SetLaunchBoxText).
        //   returns — the untrimmed text; "" before the bar is built.
        //   throws  — what reading a XAML property throws on the UI thread (never in practice). UI thread only.
        std::wstring LaunchBoxText() const;
        // Agentmaster (SIDE PANEL "+" launcher): write the side panel card's box text into THIS launch box — the card
        // mirrors every edit and its close here — and re-validate it explicitly: the Agent Manager page is usually hidden
        // then, and TextChanged is unreliable off the live tree. The box is unfocused, so its drop-down never opens from
        // this. A no-op for identical text. UI thread only.
        //   text   — the card's box text (a folder or a session id).
        //   throws — what a XAML property write throws on the UI thread (the page's callers are contained).
        void SetLaunchBoxText(const std::wstring& text);
        // Agentmaster (SIDE PANEL "+" launcher): this window's recent-folders list — the ONE list the Manager's launch bar
        // reads and pushes to. The side panel's bar is built with the same pointer, so a folder launched from either is
        // at the top of both drop-downs and neither save can drop the other's push. Never null after construction.
        //   returns — the shared list (AgentLaunchBar.h has the type).
        std::shared_ptr<AgentLaunchRecentDirs> LaunchRecentDirs() const noexcept { return _launchRecentDirs; }

        // Agentmaster (Fleet Observer O6; OBSERVER.md §11c): the External (WindowsTerminal) claude
        // census — observe-only sessions the observer detected in a real Windows Terminal (NOT our
        // tabs, NO registry session, NO Auto Testing). The page's _ObserverProbe pushes the observer's
        // External() table here each tick; the content shows them as a collapsible "External (N)"
        // board group. Diffs against the current list, so an unchanged push is a no-op (no rebuild).
        void SetExternalClaudes(std::vector<::Agentmaster::ExternalClaudeRow> rows);

        // Agentmaster (M10; PERSISTENCE.md §13): the per-window Manager LENS (selection / scope /
        // selected prompt / collapsed dirs / splitter sizes). GetManagerState reads it;
        // SetManagerState seeds it on restore (re-applies the splitter sizes, then refreshes);
        // SetLensChangedHandler installs a callback the content fires — carrying the current lens —
        // whenever the lens mutates, so the hosting window debounce-saves its window record.
        ::Agentmaster::ManagerState GetManagerState() const;
        void SetManagerState(const ::Agentmaster::ManagerState& state);
        void SetLensChangedHandler(std::function<void(::Agentmaster::ManagerState)> handler);

        // IPaneContent
        winrt::Windows::UI::Xaml::FrameworkElement GetRoot();
        void UpdateSettings(const winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings& settings);
        winrt::Windows::Foundation::Size MinimumSize();
        void Focus(winrt::Windows::UI::Xaml::FocusState reason = winrt::Windows::UI::Xaml::FocusState::Programmatic);
        void Close();
        winrt::Microsoft::Terminal::Settings::Model::INewContentArgs GetNewTerminalArgs(BuildStartupKind kind) const;

        winrt::hstring Title(); // "Agent Manager", or "Agent Manager Dev" on the AgentmasterDev package (defined in the .cpp — needs ProfileBootstrap)
        uint64_t TaskbarState() { return 0; }
        uint64_t TaskbarProgress() { return 0; }
        bool ReadOnly() { return false; }
        winrt::hstring Icon() const;
        Windows::Foundation::IReference<winrt::Windows::UI::Color> TabColor() const noexcept { return nullptr; }
        winrt::Windows::UI::Xaml::Media::Brush BackgroundBrush();

        // See BasicPaneEvents for most generic event definitions

    private:
        void _BuildLayout();
        // The lens rebuild every refresh trigger funnels through (the hidden-lens gate lives here).
        //   boardOnly - false (every caller but one): the board, the tree, the plan, the Summary tab, the
        //               toolbar counts and the keep-awake hold. true (NoteTagsChanged): the board alone,
        //               from a fresh live snapshot — for a change only the board can show. The tree then
        //               keeps the state of the last full rebuild; any registry change still brings its own
        //               full one. While the lens is hidden both forms only mark it dirty.
        //   throws    - what the XAML rebuilds throw; std::bad_alloc.
        void _Refresh(bool boardOnly = false);
        void _RefreshFromRegistryEvent(); // Agentmaster (perf): the registry-notify path into _Refresh — trailing-throttled (min gap between event-driven full rebuilds; the trailing edge always runs); UI thread
        void _RebuildBoard(const std::vector<::Agentmaster::SessionInfo>& sessions);
        void _RebuildTree(const std::vector<::Agentmaster::SessionInfo>& sessions);
        // Agentmaster: the Explorer Tree's EXTERNAL scope — render the Fleet Observer's observe-only
        // external claudes (_externalClaudes) grouped by working dir, each row carrying an Adopt / Open
        // New Session Here / Bring Window To Front right-click menu. Reads the member table, not the
        // registry snapshot.
        void _RebuildExternalTree();
        void _RebuildPlan(const std::vector<::Agentmaster::SessionInfo>& sessions);
        // Agentmaster: select an EXTERNAL (observe-only) row in the tree's EXTERNAL scope -> the
        // Auto Testing shows that conversation's prompts READ-ONLY (we host no ConPTY, so we can't
        // drive it). _LoadExternalPlan reads the transcript prompts on a background thread (one-shot
        // per id) and posts them back via the dispatcher; _RebuildExternalPlan renders them.
        // `kind` (Claude vs Codex) + `rolloutPath` (Codex's date-sharded .jsonl, carried on the row)
        // select the right read-only-plan reader: Claude -> ReadTranscriptInfo(cwd,id); Codex ->
        // ReadCodexRolloutInfo(rolloutPath). (Phase C1.)
        void _SelectExternal(const std::wstring& sessionId, const std::wstring& cwd, const std::wstring& title, ::Agentmaster::AgentKind kind, const std::wstring& rolloutPath);
        void _LoadExternalPlan(const std::wstring& sessionId, const std::wstring& cwd, ::Agentmaster::AgentKind kind, const std::wstring& rolloutPath);
        void _RebuildExternalPlan();
        // Agentmaster: pin the Auto Testing's scroll to the BOTTOM the first time a given subject is
        // viewed (the latest SENT message + the UPCOMING queue sit at the bottom of the list, so a
        // freshly-opened plan defaults to "where the conversation left off"). `subjectKey` identifies
        // what the list currently shows — a managed session id, or "x:<id>" for an external's
        // read-only conversation; empty means nothing scrollable. We only scroll when the subject
        // CHANGES from the last one we pinned (_planAutoScrolledFor): a mere _Refresh on the same
        // subject (a background state change, an OSC title float) must NOT yank the user's scroll
        // position. Deferred to a clean tick + UpdateLayout so ScrollableHeight is valid (the list was
        // just (re)populated this frame).
        void _PinPlanToBottomOnSubjectChange(const std::wstring& subjectKey);

        // Agentmaster: Explorer Tree scope toggle, cycling LOCAL -> GLOBAL -> EXTERNAL (this
        // window's tabs / all windows / observe-only externals). _ToggleTreeScope advances the mode
        // + rebuilds (and clears selection when entering EXTERNAL so the Auto Testing reads
        // nothing-selected); _UpdateTreeScopeButton refreshes the toggle button's label.
        void _ToggleTreeScope();
        void _UpdateTreeScopeButton();
        // Agentmaster: Explorer Tree sort toggle, after the scope toggle, cycling NEWEST -> OLDEST ->
        // MOST ACTIVE -> A-Z. The sort is a GLOBAL setting (AppSettings::treeSort): _CycleTreeSort
        // advances it, persists it through the settings sink (so every window + a relaunch pick it
        // up), and rebuilds; _UpdateTreeSortButton refreshes the toggle button's label.
        void _CycleTreeSort();
        void _UpdateTreeSortButton();
        // Agentmaster: Triage Board sort toggle, after the board's LOCAL/GLOBAL scope toggle, cycling
        // MOST ACTIVE -> NEWEST -> OLDEST -> A-Z (the tree's set minus BY PID; default MOST ACTIVE). A
        // SEPARATE GLOBAL setting from treeSort (AppSettings::boardSort): _CycleBoardSort advances +
        // persists it through the settings sink (shared by every window, adopted on relaunch) and
        // re-sorts the board; _UpdateBoardSortButton refreshes the toggle's label.
        void _CycleBoardSort();
        void _UpdateBoardSortButton();
        // Agentmaster (bookmark tags): the Triage Board's TAG FILTER chips, in the board header right
        // after the "Clear" button — one bookmark-ribbon chip per tag the board's currently-visible
        // sessions carry, exactly the Sessions browser's chip look (_RebuildSessionsTagChips).
        // _RebuildBoardTagChips re-lists them from _boardTags (filled per rebuild, already narrowed to
        // the scope) each _RebuildBoard; _ToggleBoardTagFilter flips one and re-renders.
        //
        // ⚠ Semantics are **OR**, deliberately UNLIKE the Sessions page's AND: picking `release` and
        // `hotfix` shows cards carrying EITHER. _BoardTagFilterAccepts is the one predicate the board's
        // column loop asks (empty filter => everything passes). The picked set is part of the per-window
        // lens (ManagerState::boardTagFilter), so it survives a reopen.
        void _RebuildBoardTagChips(const std::unordered_map<std::wstring, int64_t>& activityBySession);
        // The ONE chip builder both the tag chips and the leading "Untagged" chip go through, so they
        // cannot drift apart visually. A set ribbonColor paints that tag's bookmark; nullopt paints the
        // hollow outline that means "carries none". A plain Button (not a ToggleButton): the SELECTED
        // look is a 1px accent BORDER with no background change, driven from `isChecked` at build time
        // (the row rebuilds on every click), which a ToggleButton's background-painting Checked VSM
        // could not give without re-templating.
        winrt::Windows::UI::Xaml::Controls::Button _MakeBoardTagChip(
            const std::wstring& label,
            const std::optional<winrt::Windows::UI::Color>& ribbonColor,
            bool isChecked,
            const std::wstring& tipTitle,
            const winrt::hstring& tipBody,
            std::function<void()> onToggle);
        void _ToggleBoardTagFilter(const std::wstring& tag);
        void _ToggleBoardUntagged(); // the leading "Untagged" chip — show/hide the cards carrying no tag
        void _ClearBoardTagFilter(); // the trailing ✕ — drop every pick AND put "Untagged" back on (the default view)
        bool _BoardTagFilterAccepts(const std::wstring& sessionId) const;

        void _SelectSession(const std::wstring& id);
        void _ClearSelection(); // Agentmaster: the board header's "Clear" button — deselect the managed OR external selection
        // Agentmaster (Linked Lenses): forward a pointer enter/leave on a managed card/row to the
        // page (id, entering). Stateless here — the page does the effective-hover matching (which
        // absorbs the enter-B-before-leave-A ordering when sliding between rows) so the content holds
        // no hover state to desync.
        void _ReportHover(const std::wstring& id, bool entering);
        // Agentmaster (tab-strip reveal): a board card / tree row single-click asks the page to scroll
        // that session's tab into view (the page decides whether it applies). Called after the click's
        // own _SelectSession + forced layout; stateless here, a no-op until the page wires the handler.
        void _RequestSessionTabReveal(const std::wstring& id);
        void _SetScope(const std::wstring& dir);
        std::optional<::Agentmaster::SessionInfo> _Selected(const std::vector<::Agentmaster::SessionInfo>& sessions) const;

        // Agentmaster (M10): fire _lensChangedHandler with the current lens (GetManagerState) so the
        // window debounce-saves its record; _ApplyLayoutToTracks pushes _layout's fractions into the
        // live row/column definitions (used when seeding a restored per-window layout).
        void _NotifyLensChanged();
        void _ApplyLayoutToTracks();

        // Action-bar handlers (operate on _selectedId / _selectedPromptId).
        // (The Launch button's action, its PRESETS hover menu and "Create & Launch" moved into the shared AgentLaunchBar —
        // _OnLaunch / _ShowLaunchMenu / _EnsureLaunchDirExists there; the content reaches them through _launch.)
        void _OnAddPrompt();
        void _OnSendNow(); // the "!" icon — confirms, then _DoSendNow
        void _DoSendNow(); // actual inject, after the Send-now confirm
        // Agentmaster: after queueing / sending a prompt, return keyboard focus to the compose box
        // so the user can keep typing the next one (clicking the icon button stole focus). A no-op
        // if the box isn't built / present.
        void _FocusPromptBox();
        // Agentmaster (prompt history — shell/REPL idiom): on the live DRAFT, pressing Up once the caret
        // reaches the FIRST VISUAL ROW of the compose box recalls the selected session's previously SENT
        // prompts (newest first); while BROWSING history Up/Down walk older/newer FREELY (no caret gate,
        // Esc cancels back to the draft) and Down off the newest entry restores the in-progress draft (the
        // draft itself only ever moves the caret on Down — nothing is newer). _BuildPromptHistory snapshots
        // the sent prompts (Flight + Typed), newest first, consecutive-duplicate-collapsed; _ApplyPromptHistoryText
        // writes a recalled body (guarded so its TextChanged doesn't reset navigation) + parks the caret at
        // the end; _ResetPromptHistory leaves navigation (called when the user edits, the box is cleared, or
        // the selection changes). _PromptCaretOnFirstRow gates the enter-history trigger (visual row, so it
        // respects word-wrap).
        std::vector<std::wstring> _BuildPromptHistory() const;
        void _ApplyPromptHistoryText(const std::wstring& text);
        void _ResetPromptHistory();
        bool _PromptCaretOnFirstRow() const;
        // Agentmaster (PENDING_INPUT.md §8a — "pull in the unsent prompt"): when the user clicks / tabs
        // into the compose box WITH EDIT INTENT and the box is EMPTY, prefill it with the selected
        // session's UNSENT input-box draft (the "3 dots" text) so a prompt typed into the terminal but
        // not sent can be queued here without retyping it. Same two sources as "Copy Current Prompt":
        // the LIVE buffer read via _liveDraftProvider, falling back to the observer's
        // SessionInfo::pendingInput. Never clobbers text already in the box, and a ONE-SHOT latch
        // (_promptPrefill*) keeps the same draft from being re-inserted after the user cleared or
        // queued it — a fresh draft in the session offers itself again. Claude sessions only.
        void _MaybePrefillPromptFromDraft();
        // The compose box's placeholder is the feature's only discoverability: while the selected
        // session HAS an unsent draft and the box is empty, it says so ("click to pull in…"). Called
        // from _RebuildPlan (which knows the selection), guarded so an unchanged string isn't rewritten.
        void _UpdateComposePlaceholder(bool sessionHasDraft);
        // Agentmaster (PENDING_INPUT.md §8b — the CONTINUATION check + the conditional pull button):
        // resolve "the current prompt" for the SELECTED session exactly the way the copy menus do —
        // the LIVE buffer read (_liveDraftProvider) if this window can reach the tab, else the
        // observer's remembered SessionInfo::pendingInput (which is authoritative for a live session,
        // so there is deliberately no fallback onto the durable store — see the .cpp).
        // `allowLive` is false on the per-keystroke path, where a buffer read is not affordable.
        // (Returns the text + optionally which source answered; deliberately not the engine's
        // CurrentPromptPick, so this widely-included header stays free of PendingInput.h.)
        std::wstring _ResolveSelectedSessionDraft(bool allowLive, bool* fromLive = nullptr);
        // Show/hide the compose box's "pull it in" button by comparing the box against that draft
        // (EvaluateDraftPull): offered only when there IS a draft, it DIFFERS from the box, and taking
        // it would not destroy composed text (the box is empty / contained in the draft / at least 80%
        // similar over 50 chars). Called from _RebuildPlan, the box's TextChanged, and after every pull.
        void _UpdateDraftPullButton();
        // The button's click: take the draft into the compose box (replacing what is there — the
        // explicit, user-initiated version of the §8a pull, so it bypasses the one-shot latch but
        // re-arms it).
        void _OnPullDraftClicked();
        // Write a resolved draft into the compose box (caret at the end) + arm the one-shot latch on
        // it, and log the pull. `how` is the nav verb: fill (an empty box) / extend (a continuation) /
        // button (the explicit click). False == the .Text() write threw (nothing was changed).
        bool _TakeDraftIntoPromptBox(const std::wstring& draft, bool fromLive, const wchar_t* how);
        void _OnMovePrompt(int delta);
        void _OnDeletePrompt();
        // Agentmaster (queue pop): Ctrl+Shift+Up on an EMPTY compose box pops the selected session's
        // LAST still-Pending queued prompt back into the editor — the envelope's ("Add to queue")
        // inverse: un-queue the most recent upcoming row to edit/re-aim it before it fires. Returns
        // true when it popped (the keystroke is then marked handled); a non-empty box / no selection /
        // nothing Pending returns false so the TextBox keeps its native Ctrl+Shift+Up (selection
        // extension). The find+capture+erase run in ONE registry Update (TakeLastPendingPrompt) so a
        // racing scheduler send can't be un-recorded.
        bool _PopLastQueuedPromptIntoBox();
        // Set the SELECTED session's Tests Autorunner mode — the user's explicit choice, so it cancels a
        // pending interrupt resume (DELIVERY.md §14) whichever mode is picked. Switching to Auto also
        // resets the per-run send counter, releases the question-guard hold (the user is attending) and
        // re-stamps a blocked box-state clock. Writes the shared registry, so the scheduler sees it at
        // once; a no-op with nothing selected and for a managed Codex (it can never be driven).
        //   mode   — AutorunnerMode::Full ("Auto") or AutorunnerMode::Off ("Paused").
        //   throws — std::bad_alloc on allocation failure only.
        void _OnAutorunnerChanged(::Agentmaster::AutorunnerMode mode);
        // Agentmaster: the Auto-Testing header's Tests Autorunner toggle. _CycleAutorunner flips the
        // selected session between its two modes (Paused <-> Auto); _UpdateAutorunnerButton paints
        // the button's colored state dot + label (dim/disabled when no live session is selected).
        void _CycleAutorunner();
        // Takes the whole AutorunnerState: an INTERRUPT-HELD Off (DELIVERY.md §14) paints differently
        // from a user's Off — "Paused -> Auto on your next message".
        void _UpdateAutorunnerButton(const ::Agentmaster::AutorunnerState& autorunner, bool enabled);
        // Agentmaster: the Auto-Testing pane's two-state [Summary | Auto Testing] tab toggle (its top
        // line). _SelectPlanPaneTab(summary) sets the GLOBAL choice (AppSettings::autoTestingShowsSummary),
        // persists + broadcasts it through the settings sink (the treeSort idiom), and re-paints;
        // _UpdatePlanPaneTab reflects the current choice — accents the selected segment and shows that
        // tab's content (Summary host vs the Auto Testing body), no-op while the controls are null.
        void _SelectPlanPaneTab(bool summary);
        void _UpdatePlanPaneTab();
        // Agentmaster (Summary tab): render the selected managed Claude session's summary — the SAME
        // RenderSessionSummaryBox the Sessions page / per-tab overlay use — into the Summary tab, with
        // the user MESSAGES newest-first and the whole box on one inner scrollbar. _RefreshSummaryTab
        // (cheap; called on every plan refresh + tab switch) resolves the subject and renders from the
        // single-entry cache or kicks the off-thread analyze; _LoadSummaryForSession does the background
        // analyze+render; _RenderSummaryBox splits the box text into mono lines + full-width rules
        // (mirrors the Sessions page's SessAppendSummaryBox).
        void _RefreshSummaryTab();
        // `forkParentId` is the session's fork source (SessionInfo::forkParentId; empty for any other
        // session): a fork with no messages of its own yet has no transcript, so its source's is summarized
        // instead — what its summary panel shows (ResolveClaudeDisplayTranscript) — rather than "No messages
        // recorded". The cache key stays (id, mtime): the fork's first message moves its last-activity
        // stamp, which re-analyzes from the fork's own transcript.
        void _LoadSummaryForSession(const std::wstring& id, const std::wstring& dir, const std::wstring& forkParentId, int64_t mtime);
        void _RenderSummaryBox(const std::wstring& text);
        void _OnSaveTemplate();
        void _OnApplyTemplate(bool toWholeDirectory);
        void _RefreshTemplateCombo();

        // Agentmaster (the shared LAUNCH BAR, AgentLaunchBar.h): the toolbar's launch row — the Claude<->Codex toggle,
        // the cwd box with its validation underline and its path-picker drop-down (recent dirs / fuzzy RECENT MATCHES /
        // git worktrees / subfolders, sized against the WINDOW), Launch with its PRESETS hover menu, and Fork — is ONE
        // AgentLaunchBar (_launch), the same code the side panel's "+" launcher card runs (SIDE_PANEL.md §2e). The
        // content keeps only its HOST half: the callbacks the bar calls (_MakeLaunchBarHost), the staged toolbar
        // reflow (_ReflowLaunchBar, below), and three forwarders its other TUs call.
        // Build the bar's host for THIS content: the Claude<->Codex toggle, the drop-down parented into _root (row 0),
        // the "Couldn't create folder" dialog on _root, the shared recent list (_launchRecentDirs), and callbacks that
        // forward to the page-wired handlers (_spawnHandler / _resumeSessionHandler / _forkSessionHandler /
        // _codexLaunchHandler), the Manager's own modal (_ShowClaudeMissing), the "Specify…" prompt
        // (_ModelSpecifyOpener) and _ReflowLaunchBar. Every callback holds the content WEAKLY and reads its member at
        // call time — the page wires the handlers AFTER the bar is built, and a handler still unset makes its action a
        // no-op. Returns the host by value (complete in AgentLaunchBar.h). Throws std::bad_alloc only.
        AgentLaunchBarHost _MakeLaunchBarHost();
        // Close the bar's drop-down (a forwarder: SetActivateAllBusy closes it before greying the box out). No-op
        // before the bar is built.
        void _ClosePathPicker();
        // Re-fit the bar's OPEN drop-down to a resized window on the next dispatcher tick (a forwarder: _root's
        // SizeChanged queues it — a layout pass, which is exactly why the re-fit itself is deferred; the bar's
        // _QueuePathPickerRelayout has the rule). Never throws.
        void _QueuePathPickerRelayout();
        // Agentmaster: validate the Launch box's content as a working dir OR a session id, painting the
        // underline (green = a FOUND session id; red = an unknown id or a missing dir) and enabling the
        // launch button (disabled on red) + the Fork button (shown only for a found session id). A forwarder to
        // the bar: call it after every programmatic _cwdBox.Text write (the selection pre-aim, the defaultLaunchDir
        // seed, the claude.exe recovery) — TextChanged does not fire reliably while the page is off the live tree.
        void _ValidateLaunchBox();
        // Agentmaster (responsive launch bar): stage-collapse the toolbar's top row as the pane narrows —
        // (A) full label -> (B) drop "launch a"/"session in" -> (C) shrink the cwd box to its floor -> (D)
        // wrap the launch buttons (_launchBtns) onto their own line. Measures each piece's DesiredSize
        // against the live pane width and picks the least-collapsed stage that fits. Called from _root's
        // SizeChanged AND whenever a button's content/visibility changes (the bar's layoutChanged callback —
        // its validation and its agent toggle — plus _UpdateReopenButton / _UpdateActivateAllButton). No
        // layout loop — every change is inside _root, whose size the pane (not its content) owns.
        void _ReflowLaunchBar();
        // (Fork, the session-id resolve, the recent-folders push + collect, the drop-down rows and Browse… moved into
        // the shared AgentLaunchBar with the rest of the launch row.)

        // Build one session card for the Triage Board.
        winrt::Windows::UI::Xaml::Controls::Button _MakeCard(const ::Agentmaster::SessionInfo& s);
        // Agentmaster (Waiting-for-you countdown bar): drain every tracked card's 1px bottom bar in
        // place (ScaleX = fraction of the waiting window still remaining) and start/stop the 1s timer
        // that drives it depending on whether any bar is tracked. _RebuildBoard re-seeds the tracks.
        void _UpdateCardProgress();
        void _SyncProgressTimer();
        // Agentmaster (UI_THREAD.md §12 — the board's PERSISTENT columns): one Triage Board column. A window
        // builds each column ONCE, the first time a rebuild shows it, and keeps it for the window's life; a
        // rebuild only rewrites the header's count and refills `cards`. Until 2026-10-14 every rebuild made
        // all the columns again — a new ScrollViewer per column per rebuild — and the replaced ScrollViewers
        // were never freed: the frozen release of 2026-10-14 held 2,920 of them, each holding its whole card
        // stack, and the XAML event manager's lists grew with them until one rebuild took ~4 s and rebuilds
        // ran back to back (the window read "Not Responding"). A kept ScrollViewer also keeps its scroll
        // offset by itself, so the per-rebuild offset capture and its restore-on-Loaded handler are gone.
        struct BoardColumnView
        {
            // The column box: a _boardHost child while the column is shown (Error and External leave the
            // board while they are empty; the other four are always shown).
            winrt::Windows::UI::Xaml::Controls::Border root{ nullptr };
            // The header's card count, rewritten only when the number changes.
            winrt::Windows::UI::Xaml::Controls::TextBlock count{ nullptr };
            // External only: the header's collapse chevron (▸ collapsed / ▾ expanded). Null for a state column.
            winrt::Windows::UI::Xaml::Controls::TextBlock chevron{ nullptr };
            // The card list's ScrollViewer: ONE per column for the window's life (see the struct comment).
            winrt::Windows::UI::Xaml::Controls::ScrollViewer scroller{ nullptr };
            // The card list, cleared and refilled by every rebuild.
            winrt::Windows::UI::Xaml::Controls::StackPanel cards{ nullptr };
            // The count the header shows now; -1 until the first rebuild sets it.
            int shownCount{ -1 };
            // External only: the collapsed state the column shows now (mirrors _externalCollapsed after a
            // rebuild), so a rebuild touches the chevron, the scroller and the alignment only on a change.
            bool shownCollapsed{ false };
        };
        // Agentmaster (UI_THREAD.md §12): the persistent column for `key`, built on its first use with a
        // pinned header over a vertically scrolling card list. The column fills the board height, so a tall
        // column scrolls within the board instead of clipping past the bottom edge (the board's own
        // ScrollViewer has its vertical scroll disabled).
        //   key     — the column key: a state column's title ("Running", "Waiting-for-you", "Needs-approval",
        //             "Error", "Idle / Done") or "External".
        //   title   — the header's title text (for a state column, the same as `key`).
        //   state   — the state column's SessionState: its header dot color and its header tooltip. Ignored
        //             for "External" (gray dot, its own tooltip, a header that collapses the column).
        //   returns — the column, a reference into _boardColumns that stays valid while the content lives
        //             (the map never erases an entry).
        //   throws  — winrt::hresult_error from XAML construction, which propagates to the rebuild's caller
        //             exactly as the old per-rebuild construction did.
        BoardColumnView& _EnsureBoardColumn(const std::wstring& key, const winrt::hstring& title, ::Agentmaster::SessionState state);
        // Agentmaster (UI_THREAD.md §12): rewrite a column header's count. A no-op when the number did not
        // change, so a rebuild that changes nothing in a column writes nothing to its header.
        //   view  — the column.
        //   count — the number of cards the column holds now.
        //   throws — winrt::hresult_error from the TextBlock write only.
        void _SetBoardColumnCount(BoardColumnView& view, int count);
        // Agentmaster (UI_THREAD.md §12): make _boardHost show exactly the columns of `order`, in that order,
        // with the fewest removes / inserts / moves (KeyedListDiff.h — the side panel's card-list recipe). A
        // column that stays on the board is never taken out of the tree, so its ScrollViewer keeps its
        // offset. In the normal case nothing changes and nothing is touched; only the Error and External
        // columns come and go. A plan that ends out of step with its mirror (a bug) is repaired by clearing
        // the host and appending `order` again, and logged.
        //   order  — the keys of the columns to show, in board order (each key built by _EnsureBoardColumn).
        //   throws — winrt::hresult_error from the collection edits, or std::bad_alloc.
        void _SyncBoardColumnOrder(const std::vector<std::wstring>& order);
        // Agentmaster (O6, UI_THREAD.md §12): refill the persistent "External (N)" column — real-WindowsTerminal /
        // console claudes the observer detected, observe-only. Its header (a Button) toggles _externalCollapsed;
        // collapsed, the column shows the header alone in a box that hugs it. The caller adds the column to the
        // board only while _externalClaudes is not empty.
        //   view   — the External column (_EnsureBoardColumn(L"External", …)).
        //   throws — winrt::hresult_error from XAML construction / writes.
        void _FillExternalColumn(BoardColumnView& view);
        winrt::Windows::UI::Xaml::Controls::Button _MakeExternalCard(const ::Agentmaster::ExternalClaudeRow& ex);

        // Draggable pane splitters (resize + on-hover cursor + persisted sizes).
        // `vertical` == a vertical bar dividing the bottom COLUMNS (↔, resizes Tree/Plan);
        // `!vertical` == a horizontal bar dividing the root ROWS (↕, resizes Board/Bottom).
        winrt::Windows::UI::Xaml::Controls::Border _MakeSplitter(bool vertical);
        void _OnSplitterPressed(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e, bool vertical);
        void _OnSplitterMoved(const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e, bool vertical);
        void _OnSplitterReleased(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e);

        // Agentmaster (inferred working dir): the session's EFFECTIVE working directory under the
        // CURRENT tab-color mode — ::Agentmaster::EffectiveWorkingDir(_appSettings.tabColorMode, s).
        // The ONE dir every Manager surface groups/scopes/displays/opens-here by, so the tree, the
        // board, the Auto Testing, and the tab color always tell the same story: the inferred dir
        // when the Inferred mode detected the session working OUTSIDE its launch cwd, else the cwd.
        // Launch/resume/fork mechanics keep reading s.workingDir (the terminal's real cwd).
        std::wstring _WorkDirOf(const ::Agentmaster::SessionInfo& s) const;

        // Explorer-tree session actions: right-click context menu (Rename / Archive, then Open New
        // Session Here in the row's cwd as the last item) + double-click to activate. `anchor` is the
        // element the menu is attached to (board card / tree row / the card's ⋯ button) — the Tags
        // item hands it to the page so the tag editor opens right under what was clicked.
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _MakeSessionMenu(const std::wstring& id, const std::wstring& cwd, const winrt::Windows::UI::Xaml::FrameworkElement& anchor = nullptr);
        // Agentmaster: the EXTERNAL-tree row right-click menu. Adopt -> resume the external's conversation
        // into a managed, controllable tab (via _adoptExternalHandler); Open New Session Here -> spawn a
        // managed session in the external's cwd (an independent conversation); Bring Window To Front (last)
        // -> surface the external's HOSTING window (_BringExternalToFront). Observe-only externals carry no
        // registry session, so this menu acts on the row's facts, not a session id. For a CODEX row
        // (kind=Codex, observe-only in Phase C1) Adopt + Open New Session Here are omitted (those drive
        // CLAUDE) and an "Open rollout file" item is offered instead; Bring Window To Front (agent-
        // agnostic window activation) stays.
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _MakeExternalTreeMenu(const ::Agentmaster::ExternalClaudeRow& ex);
        // Agentmaster: the EXTERNAL menu's Bring Window To Front — resolve the row's host facts
        // (hostPid / sessionId / title) from the latest _externalClaudes snapshot, then surface its
        // hosting window on a BACKGROUND thread (ProcessInspect::BringClaudeWindowToFront:
        // restore-if-minimized + foreground + best-effort WT tab select — the worker reads the
        // transcript for the custom title + prompt corpus the tab pick matches against).
        // Observe-only safe: window activation, never input.
        void _BringExternalToFront(uint32_t pid, const std::wstring& cwd);
        // Auto-Testing message right-click menu: per-prompt Move up / Move down / Delete (only on
        // UPCOMING rows — a sent row can't be reordered) + Archive session (always). Queue ops act
        // on `promptId` (the right-clicked row), not the current selection.
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _MakePromptMenu(const std::wstring& promptId, bool upcoming);
        void _OnRenameSession(const std::wstring& id); // begin an in-place rename of the row
        void _CommitRename(); // apply the in-place editor's text to the session title
        void _CancelRename(); // discard the in-place editor (Esc)
        void _RequestArchive(const std::wstring& id); // route to the page's Close seam (presents the confirm + closes the tab; keeps the record so it stays resumable in Sessions — FAVORITES.md)
        void _CloseSessionsInFolder(const std::wstring& folder, const std::wstring& excludeId = {}); // "Close ▸ Of Same Folder" (excludeId empty) / "Other of Same Folder" (excludeId == this session): enumerate every live managed session in `folder` (skipping excludeId), show ONE confirm listing their titles, then hand (folder, excludeId) to the page's cross-window batch-close (_closeFolderHandler)

        // Settings cog: an in-content modal overlay (NOT a ContentDialog — a text box inside a
        // ContentDialog receives no keypresses in XAML Islands; see the _renameBox note). Built
        // into the main visual tree so its TextBoxes work; shown/hidden by toggling Visibility.
        void _BuildSettingsOverlay();
        void _ShowSettings(); // populate controls from _appSettings, then reveal the overlay
        void _HideSettings();
        void _SaveSettings(); // read controls -> _appSettings -> _settingsSink, then hide
        // Agentmaster: the cog's TOP TAB strip — Sessions · Autorunner · Behavior · Tabs & Overlay ·
        // Claude · About. A button-tab swaps one scrollable panel per group (the _SwitchEnvTab idiom;
        // NOT a Pivot — themes unreliably under XAML Islands). _settingsTabButtons / _settingsTabPanels
        // are parallel-indexed; Save/Cancel is a fixed footer outside the tabs.
        void _SwitchSettingsTab(int index); // show one tab's content scroller + restyle the strip

        // Agentmaster (ENV_VARS.md): the cog's "Environment variables" area — a Global / Per-directory
        // two-tab editor (multi-line NAME=VALUE, one per line) feeding ResolveSessionEnv at spawn. Built
        // once into the settings panel; _Load seeds it on cog open; _Save writes AppSettings.env (global)
        // + flushes the per-directory draft to dir-env.json. The per-dir draft is held in memory until
        // Save so Cancel discards (the cog's form semantics), mirroring _appSettings for the global side.
        void _BuildEnvVarsArea(const winrt::Windows::UI::Xaml::Controls::StackPanel& panel);
        void _LoadEnvVarsArea(); // seed global text from _appSettings.env + load dir-env.json into the draft
        void _SaveEnvVarsArea(); // global -> _appSettings.env; flush _dirEnvDraft -> SaveDirEnv
        void _SwitchEnvTab(bool perDir); // toggle Global <-> Per-directory panels + restyle the tab buttons

        // Agentmaster (PRESETS, doc/agentmaster/HARNESSES.md §8.1): the cog's "Presets" tab — the providers
        // (Base URL + key) and the preset list with its editor. Every launch-value box is EMPTY unless the
        // user typed into it, and an empty box shows, in gray, the value that applies instead and where it
        // comes from (Agentmaster env, Windows env, template, provider default) — the ONE resolver the launch
        // uses (ResolvePresetField), so the gray text can never disagree with what launches. Edits go into
        // an in-memory DRAFT (_presetsDraft + the key drafts), written on Save and dropped on Cancel — the
        // per-directory env area's form semantics.
        void _BuildPresetsArea(const winrt::Windows::UI::Xaml::Controls::StackPanel& panel); // build the tab's controls once
        void _LoadPresetsArea(); // _ShowSettings: draft from launch-presets.json (nothing added by itself: no file = an empty list), key drafts cleared, editor on the first preset
        void _SavePresetsArea(); // _SaveSettings: write launch-presets.json + store / remove typed provider keys in Credential Manager
        void _RebuildPresetProviders(); // (re)build one block per provider (Base URL box, key box, stored-key line)
        void _RebuildPresetPicker(); // refill the preset picker from the draft, keeping the selection
        void _SelectPresetForEdit(const std::wstring& id); // fill the editor from one draft preset (no draft writes while filling)
        void _RefreshPresetEditor(); // placeholders, field visibility by provider, the tested line, the launch preview
        void _AddPreset(const std::wstring& templateId); // "New": a blank preset ("" ) or one linked to a template
        void _DuplicatePreset(); // copy the selected preset (same template link, its typed values, " (copy)")
        void _DeletePreset(); // drop the selected preset (a template's preset can be added again from TEMPLATES; nothing comes back by itself)
        void _RefreshPresetTemplateRows(); // TEMPLATES: each row's Add / disabled "Added" + its detail line, the "Add all tested" button, and the empty-list line — after every list change (_RebuildPresetPicker calls it)
        // TEMPLATES' Add and "Add all tested templates": append one LINKED preset per template id to the draft
        // (AddLaunchPresetsFromTemplates — no typed field, visible, a fresh "p-" id), select the first one, and rebuild
        // the picker + editor once. Nothing is written before Save. An empty or all-unknown list does nothing.
        //   templateIds - the template ids to add, in order.
        void _AddPresetsFromTemplates(const std::vector<std::wstring>& templateIds);
        void _AddCustomProvider(); // a new custom Anthropic-compatible provider (name, env prefix, Base URL)
        ::Agentmaster::LaunchPreset* _DraftPreset(); // the selected preset inside the draft (nullptr = none)
        ::Agentmaster::LaunchProviderEntry* _DraftProviderEntry(const std::wstring& id, bool create); // a provider's entry in the draft (created on demand)
        ::Agentmaster::PresetEnvLookup _PresetsAgentmasterEnv(); // Agentmaster's Global env as typed in the (possibly unsaved) env editor
        std::wstring _PresetsKeyFor(const std::wstring& providerId, std::wstring* source); // the key the preview uses: typed > stored > Agentmaster env > Windows
        void _RebuildEnvDirList(); // (re)populate the per-dir selector from session dirs ∪ dir-env keys, filtered
        void _SelectEnvDir(const std::wstring& normKey, const std::wstring& displayPath); // load a dir's env into the per-dir editor
        // Lex an env editor's text -> recolor its wrapping border + write its status line (the SAME
        // green/amber/red palette _ValidateLaunchBox uses). `perDir` picks which editor/border/status.
        void _RefreshEnvLex(bool perDir);
        void _RefreshLaunchModelsLex(); // launch-model picker: live-lex the "Launch models" editor (LexLaunchModelsText) — recolor its border + refresh the counts/first-issue status line (the _RefreshEnvLex twin)

        // Agentmaster (updater; Updater.h): the Settings cog's UPDATES section. Runs a GitHub
        // release check OFF the UI thread (WinHTTP), then marshals back. interactive==true (the
        // "Check for updates" button): show the prompt (Update now / Postpone [next restart · tomorrow-08:00 ·
        // 3·7·30 days · skip]) on a found update, else report "up to date" / "couldn't reach
        // GitHub". interactive==false (the silent check kicked when the cog opens): only set the
        // "vX.Y.Z available!" label (dark green) when a newer release exists; stay quiet otherwise.
        // "Update now" launches the embedded am-update installer (detached) and asks the app to quit
        // via _quitForUpdateHandler so the package isn't in use while it upgrades + relaunches.
        void _CheckForUpdates(bool interactive);
        void _ApplyUpdateCheckResult(bool interactive, const std::wstring& stateDir, const ::Agentmaster::Updater::UpdateInfo& info); // the check's UI completion (label/changelog/prompt+apply); split out so the worker's recovery catch stays small
        void _ApplyAllowNightly(bool on); // the nightly switch's INSTANT-APPLY commit (freshest-disk RMW + re-kick the silent check) — ON is reached only through the accepted warning confirm

        // Agentmaster (native-exe-only policy): the "Claude not detected" modal — shown when a
        // launch/fork is attempted with no native claude.exe (::Agentmaster::ClaudeAvailable() false).
        // Carries why (native required, the Node CLI is unsupported), the install path (`claude install`
        // + a docs link), a Browse… (locate claude.exe), and Re-check. Built into the tree like the
        // settings overlay (toggled by Visibility), so its content behaves in XAML Islands.
        void _BuildClaudeMissingOverlay();
        void _ShowClaudeMissing();
        void _HideClaudeMissing();
        // Agentmaster (launch-model picker -> "Specify..."): the type-any-model-id prompt, shown as a
        // dimmed modal over _root like the two overlays above (a ContentDialog text box gets no
        // keypresses under XAML Islands). The CARD is built by the shared AgentBuildSpecifyModelCard
        // so this prompt, the Sessions page's, and the WT tab menu's are literally the same control.
        // `onPicked` fires ONLY on commit, with the trimmed id; cancel/backdrop/Escape launch nothing.
        // Opened by every "Open New Session Here"/"Fork session" submenu here and by the Settings
        // tab's successor-model combos.
        //   onPicked - receives the committed, trimmed model id (never ""), on the UI thread.
        //   seed     - the id the box opens with; "" (every caller today) = the model last committed in
        //              this prompt in ANY window (model-list.json's "last" key), or an empty box if none.
        // Returns at once; the prompt opens on a later dispatcher tick, where a failure to build or show
        // it is logged ([exc] in hooks.log) and nothing opens. A content with no root or dispatcher yet
        // opens nothing.
        // Throws std::bad_alloc only (TryEnqueue reports a refusal by its result, not by throwing).
        void _PromptForModel(std::function<void(winrt::hstring)> onPicked, const std::wstring& seed = {});
        void _HideSpecifyModel();
        // The opener every model submenu on this surface passes to AgentFillModelPickItems — ONE
        // definition, so the board/tree/External menus can't wire it three different ways. Spelled
        // out rather than using AgentModelMenu.h's AgentSpecifyModelOpener alias because that header
        // (and the prompt's) must not fan out through this one — see the UpdateInfo note above.
        std::function<void(std::function<void(winrt::hstring)>)> _ModelSpecifyOpener();
        // Agentmaster (PRESETS, HARNESSES.md §8.2): the "Add presets…" opener every model menu on this surface passes
        // to AgentFillModelPickItems (the board / tree / External menus). Asked when the menu is built: it returns the
        // opener ONLY while the user has no preset at all (::Agentmaster::ShouldOfferAddPresets — the one shared
        // question), and an empty function otherwise, so the item is left out. Spelled out for the same reason as
        // _ModelSpecifyOpener. The Launch button's hover menu asks the same question itself (AgentLaunchBar) and calls
        // _ShowAddPresetsHelp through its host.
        // Returns the opener (it reaches this content weakly: a click after teardown does nothing), or an empty
        // function. Reads the cached presets config; throws std::bad_alloc only.
        std::function<void()> _AddPresetsOpener();
        // Show where presets are added: a buttons-only ContentDialog over the Agent Manager with one OK button (the
        // user's request), its text AddPresetsHelpText(true) — without the "open the Agent Manager" step, because the
        // Settings button is on screen. Deferred one dispatcher tick past the invoking menu's close. Never throws: a
        // dialog that cannot show (another ContentDialog is open, no XamlRoot yet) is logged ([exc] in hooks.log),
        // and nothing else happens.
        void _ShowAddPresetsHelp();
        // COMMANDS.md §6b: give a successor-model combo its "Specify..." behavior (the row itself is
        // seeded by seedModelCombo). Wired ONCE at build time — reseeding only refills Items().
        void _WireSuccessorModelCombo(const winrt::Windows::UI::Xaml::Controls::ComboBox& combo, std::vector<std::wstring>* ids);
        // Browse for claude.exe (IFileOpenDialog, .exe filter) -> persist it as the claudeExePath
        // override (through _settingsSink) + ::Agentmaster::RefreshClaudeExe; hides the missing-overlay
        // and re-validates the launch box if claude is now available. Runs the Win32 modal OFF the click
        // tick (the XAML-Islands deferral rule). `fromSettings` true => also refresh the Settings fields.
        void _BrowseForClaudeExe(bool fromSettings);

        // Agentmaster (FAVORITES.md): the Archived overlay/page + their methods were removed — the
        // Sessions browser is the sole history view (closed sessions stay there, resumable, starred).
        // Agentmaster: keep-awake control — a TRI-MODE button (Off / Always / While-Running).
        // _CycleKeepAwake advances the mode (Off -> Always -> WhileRunning -> Off); _RefreshKeepAwakeHold
        // computes the DESIRED execution-state hold for the current mode (Always => always hold; WhileRunning
        // => hold iff a live session is actively Running) and, on a transition only, calls SetThreadExecutionState
        // (ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED to hold, ES_CONTINUOUS alone to release). It is
        // also called from _Refresh so WhileRunning tracks the fleet live; pass the already-fetched snapshot to
        // avoid a second Snapshot() copy. _UpdateKeepAwakeButton repaints the button to reflect mode + hold.
        void _CycleKeepAwake();
        void _RefreshKeepAwakeHold(const std::vector<::Agentmaster::SessionInfo>* sessions = nullptr);
        void _UpdateKeepAwakeButton();
        // Agentmaster (M10 Increment 3): the "Reopen Windows (N)" recover button. _UpdateReopenButton
        // sets its label to the recoverable-window count and shows it only when N>0; _OnReopenWindows
        // confirms, then fires _reopenWindowsHandler (the page reopens every not-currently-open record).
        void _UpdateReopenButton();
        void _OnReopenWindows();
        // Agentmaster (eager-init / "Activate All Tabs (N)"): set the button's label to THIS window's
        // dormant managed-tab count and show it only when N>0 (the hide-when-idle idiom, like the reopen
        // button); refreshed on the _Refresh cadence. _OnActivateAllTabs decides the scope: if other
        // windows ALSO have dormant tabs it prompts (This window / All windows), else it just wakes this
        // window's. Both counts come from _DormantCounts (snapshot + the local-scope provider).
        void _UpdateActivateAllButton();
        void _OnActivateAllTabs();
        // {thisWindowDormant, fleetDormant} — live MANAGED sessions whose claude hasn't started yet
        // (IsSessionDormant), split by whether they are hosted in THIS window (per _localScopeProvider).
        std::pair<int, int> _DormantCounts() const;
        // A buttons-only confirm (XAML-Islands-safe) for consequential actions; runs onYes on accept.
        void _Confirm(const winrt::hstring& title, const winrt::hstring& body, const winrt::hstring& primary, std::function<void()> onYes);
        // Agentmaster: a buttons-only THREE-way choice (XAML-Islands-safe): primary / secondary / Cancel.
        // Used by Adopt to offer Fork-a-copy (safe) vs Resume-anyway (take-over). Primary is the default
        // (the safe choice); Cancel does nothing. onPrimary / onSecondary run on the respective click.
        void _ConfirmChoice(const winrt::hstring& title, const winrt::hstring& body, const winrt::hstring& primary, const winrt::hstring& secondary, std::function<void()> onPrimary, std::function<void()> onSecondary);

        std::shared_ptr<::Agentmaster::SessionRegistry> _registry;
        // Agentmaster (M9): our observer's token on the shared (process-wide) registry, so this
        // window's lens detaches cleanly on teardown instead of dangling a strong
        // DispatcherQueue ref there. (An ::Agentmaster::ObserverToken == uint64_t; kept as
        // uint64_t to avoid including SessionRegistry.h in this header.)
        uint64_t _observerToken{ 0 };
        winrt::Windows::System::DispatcherQueue _dispatcher{ nullptr };
        // Agentmaster (Waiting-for-you "unread" model): a 30s timer that re-runs _Refresh() so the
        // TIME-derived card adornments (the ⚡ "still cached" hint + the "ago" timing) stay current in
        // quiet periods with no registry events. Stopped in the destructor.
        winrt::Windows::UI::Xaml::DispatcherTimer _cardRefreshTimer{ nullptr };
        // Agentmaster (perf — the CPU-hotspot fix): COALESCE + trailing-THROTTLE for the registry-notify
        // refresh. The observer used to TryEnqueue one FULL _Refresh (deep registry Snapshot + board +
        // tree + plan rebuild) PER notify — with 19 active sessions (presence flips, hook events, recon
        // synths) that was a steady per-window rebuild storm on the UI thread. _refreshQueued keeps at
        // most ONE dispatcher hop in flight (a notify burst folds into it; cleared BEFORE the refresh so
        // a notify landing DURING a rebuild schedules the next one — no lost updates); the one-shot
        // _refreshDelayTimer enforces a minimum gap between event-driven rebuilds, always running the
        // TRAILING refresh so the final state is never dropped. User-action refreshes (RefreshNow, scope/
        // sort toggles, the 30s idle timer) stay direct and immediate.
        std::shared_ptr<std::atomic<bool>> _refreshQueued{ std::make_shared<std::atomic<bool>>(false) };
        int64_t _lastRegistryRefreshMs{ 0 };
        winrt::Windows::UI::Xaml::DispatcherTimer _refreshDelayTimer{ nullptr };
        // Agentmaster (UI_THREAD.md §12 — the adaptive gap): how long the last real rebuild took, in ms
        // (_Refresh measures every rebuild that runs, whoever asked for it; a refresh gated off while the
        // page is hidden does not count). _RefreshFromRegistryEvent waits ManagerRefreshGapMs(this) between
        // registry-driven rebuilds, so a slow page leaves the UI thread room for input instead of rebuilding
        // back to back (RefreshPacing.h). 0 until the first rebuild.
        int64_t _lastRefreshCostMs{ 0 };
        // Agentmaster (UI_THREAD.md §12): when the last `[manager] slow rebuild` line was written
        // (GetTickCount64 ms; 0 = never), so a page that stays slow logs once a minute, not per rebuild.
        int64_t _lastSlowRebuildLoggedMs{ 0 };

        // Agentmaster (perf — the hidden-lens rebuild tax, SetLensVisible): _lensVisible mirrors "the
        // Agent Manager page is on screen"; _refreshDirty records a refresh that was REQUESTED while
        // hidden, so the first SetLensVisible(true) runs exactly one catch-up _Refresh(). Every
        // refresh trigger (registry notify, timing tick, selection sync, RefreshNow) funnels through
        // _Refresh, which is where the gate lives — no caller needs to know.
        bool _lensVisible{ true };
        bool _refreshDirty{ false };

        // Agentmaster (perf — the 30s timing tick): the "created …, active … ago" timing TextBlocks of the live cards +
        // rows, bound to their session ids, so the periodic tick REWRITES their text in place (a
        // handful of TextBlock.Text sets) instead of tearing down + recreating the whole board and
        // tree (which is what the tick used to do — a full _Refresh every 30s, ~1-6s of UI-thread
        // work at fleet scale, purely to move an "ago" string). Re-seeded by the rebuilds: cards are
        // cleared + refilled in _RebuildBoard/_MakeCard, rows in _RebuildTree. _cardWarmShown remembers
        // whether each card RENDERED the ⚡ "still server-cached" glyph, so the tick can detect a
        // lapsed/started cache window (the other time-derived adornment) and fall back to one full
        // rebuild only when a glyph must actually appear or vanish.
        struct TimingBind
        {
            std::wstring id;
            winrt::Windows::UI::Xaml::Controls::TextBlock text{ nullptr };
        };
        std::vector<TimingBind> _cardTimingBinds;
        std::vector<TimingBind> _rowTimingBinds;
        std::unordered_map<std::wstring, bool> _cardWarmShown;
        void _RefreshTimingTexts(); // the 30s tick's in-place update (falls back to _Refresh when a ⚡ glyph flips)

        // Agentmaster (perf): _UpdateReopenButton's "Reopen Windows (N)" count comes from
        // RecoverableWindows(), which ENUMERATES + PARSES every windows\<id>.json on disk — it ran on
        // every _Refresh (every registry notify). Cached with a short TTL; a fresh count is at most
        // kReopenCountTtlMs stale, which is invisible for a button that reads a number.
        int64_t _reopenCountCheckedMs{ 0 };
        int _reopenCountCached{ 0 };

        // Agentmaster (perf): ParseModelFamilies(_appSettings.modelFamilies) was re-parsed per card per
        // rebuild for the card's short model name; parsed once per distinct settings string instead.
        std::wstring _modelFamiliesSrc{ L"\x01unset" }; // a value no settings string can equal => the first call always parses
        std::vector<std::wstring> _modelFamiliesCache;
        const std::vector<std::wstring>& _ModelFamilies();

        // Agentmaster (perf — LAZY context menus): a card carried TWO fully built session menus (the
        // right-click ContextFlyout + the hover "⋯" button's Flyout) and a tree row one more — each
        // ~13 items + 4 submenus + 2 launch-model pick lists + ~20 tooltips, plus a registry Get and a
        // local-scope query — built EAGERLY for EVERY session on EVERY rebuild, i.e. ~50 full menus
        // per refresh at fleet scale that were almost never opened (_MakeSessionMenu was ~12-15% of
        // the UI thread's busy samples). These return an EMPTY MenuFlyout that populates itself in its
        // Opening handler (the same "fill at flyout-open" idiom the WT tab context menu uses), so a
        // menu costs nothing until it is actually opened — and then reflects the session's state AT
        // OPEN time rather than at rebuild time. The eager builders stay as the fillers.
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _MakeLazySessionMenu(const std::wstring& id, const std::wstring& cwd, const winrt::Windows::UI::Xaml::FrameworkElement& anchor = nullptr);
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _MakeLazyExternalMenu(const ::Agentmaster::ExternalClaudeRow& ex);
        void _PopulateSessionMenu(winrt::Windows::UI::Xaml::Controls::MenuFlyout& menu, const std::wstring& id, const std::wstring& cwd, const winrt::Windows::UI::Xaml::FrameworkElement& anchor);
        // Agentmaster (session menu "Deactivate ▸" / "Close ▸" — the WT tab menu's two submenus): build ONE of the two
        // for _PopulateSessionMenu. Both list the tab menu's scopes in ONE order, so they line up row for row: this tab ·
        // tabs to the left · tabs to the right · other tabs · of same folder · other of same folder · all tabs (Close ends
        // with the tab menu's "★ Favorite & Close All Tabs"). Items are grayed by the tab menu's own rules: "Deactivate
        // Tab" while the session is dormant, the strip items by `spot`. Every click runs one dispatcher tick later through
        // _sessionMenuScopeHandler — except Close's two folder items, which keep _CloseSessionsInFolder (the existing
        // confirm-then-cross-window folder close).
        //   verb           — which submenu: Deactivate (park, never asks) or Close (end, asks where the tab menu asks).
        //   id             — the menu's session.
        //   cwd            — its effective work dir (inferred, else the launch cwd); the two folder items are left out when
        //                    it is empty, since there is no folder to match.
        //   sessionDormant — its agent has not started (a restored or parked tab): grays "Deactivate Tab", which would
        //                    have nothing to stop. Ignored for Close.
        //   spot           — where its tab sits in THIS window's strip, or nullopt when another window hosts it (then no
        //                    strip item is grayed: the hosting window applies the scope).
        // Returns the submenu, never null. Throws winrt::hresult_error when XAML refuses to build an element; the lazy
        // menu's Opening handler contains it (an empty menu rather than a dead card).
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutSubItem _MakeSessionScopeSubmenu(SessionMenuVerb verb, const std::wstring& id, const std::wstring& cwd, bool sessionDormant, const std::optional<SessionTabSpot>& spot);
        void _PopulateExternalTreeMenu(winrt::Windows::UI::Xaml::Controls::MenuFlyout& menu, const ::Agentmaster::ExternalClaudeRow& ex);
        // Agentmaster (transcript overlay): append the EXTERNAL menu's "Show Transcript" (after "Copy Session
        // Id") — the external's conversation in the page's reading view. Disabled, with a tip saying why,
        // when the row has no id yet (a never-prompted claude / codex has no transcript or rollout).
        // Parameters: menu — the external row's menu; sid — the conversation id (Claude) or rollout uuid
        // (Codex), may be empty; codex — the row is a Codex rollout; title — the row's title (may be empty).
        void _AppendExternalShowTranscriptItem(winrt::Windows::UI::Xaml::Controls::MenuFlyout& menu, const std::wstring& sid, bool codex, const std::wstring& title);

        // Agentmaster (Waiting-for-you countdown bar): the live 1px bottom bars to drain. Each holds the
        // bar's ScaleTransform (ScaleX = fraction of the waiting window still remaining, origin LEFT) +
        // the countdown anchor + the full duration. Re-seeded by _RebuildBoard (cleared at its top,
        // pushed by _MakeCard); _progressTimer ticks ~1s and sets each ScaleX in place (no rebuild —
        // a cheap render-transform write). The timer runs only while >=1 bar is tracked.
        struct CardProgress
        {
            // The bar element (its RenderTransform is a ScaleTransform whose ScaleX = fraction). Stored
            // as the Border rather than the ScaleTransform so this header needs no winrt Media include
            // (which collides ::IInspectable with winrt's in this TU); _UpdateCardProgress resolves the
            // transform in the .cpp where Media is in scope.
            winrt::Windows::UI::Xaml::Controls::Border bar{ nullptr };
            int64_t lastActivityUnixMs{ 0 }; // the turn's last activity (the countdown start)
            int64_t timeoutMs{ 0 }; // waitingForYouTimeoutMinutes * 60000 (the full bar duration)
        };
        std::vector<CardProgress> _cardProgress;
        winrt::Windows::UI::Xaml::DispatcherTimer _progressTimer{ nullptr };

        std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> _spawnHandler; // (workingDir, title, model) — model "" = Default (the launch-model picker)
        std::function<void(winrt::hstring)> _activateHandler;
        std::function<void(winrt::hstring)> _activateDormantHandler; // Agentmaster (eager-init): "Activate Tab" -> start a dormant session's claude in place (no focus change)
        std::function<void(bool)> _activateAllHandler; // Agentmaster (eager-init): "Activate All Tabs" -> wake every dormant tab (allWindows=false this window only / true + fan out)
        std::function<void(winrt::hstring, bool)> _hoverSessionHandler; // Agentmaster (Linked Lenses): push a managed card/row pointer enter/leave (id, entering) so the page pills its tab
        std::function<void(winrt::hstring)> _revealSessionTabHandler; // Agentmaster (tab-strip reveal): a managed card/row single-click -> the page scrolls that session's tab into view, even on a re-click of the already-selected card (null until the page wires it)
        std::function<void(winrt::hstring)> _archiveHandler;
        std::function<void(winrt::hstring, winrt::hstring)> _closeFolderHandler; // Agentmaster ("Close ▸ Of Same Folder" / "Other of Same Folder"): (folder, excludeId) -> the page closes every live managed session in that folder (cross-window) SKIPPING excludeId, the content having already shown the one confirm
        std::function<void(winrt::hstring, SessionMenuVerb, SessionMenuScope)> _sessionMenuScopeHandler; // Agentmaster (session menu "Deactivate ▸" / "Close ▸"): (sessionId, verb, scope) -> the page runs that item as the session's own tab menu would, in the window hosting the tab (SetSessionMenuScopeHandler); null until wired — the items then do nothing
        std::function<std::optional<SessionTabSpot>(const std::wstring&)> _sessionTabSpotProvider; // Agentmaster (session menu "Deactivate ▸" / "Close ▸"): (sessionId) -> where its tab sits in THIS window's strip, nullopt when another window hosts it — grays the strip items like the tab menu (SetSessionTabSpotProvider); null => nothing grayed
        std::function<bool()> _idleTabsProvider; // Agentmaster (session menu "Deactivate All Idle Tabs"): () -> would that item park a tab in this window or another one now — grays the item like the tab menu (SetIdleTabsProvider); null => enabled
        std::function<void(winrt::hstring)> _restoreHandler;
        std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> _resumeSessionHandler; // Agentmaster: launch box holds a FOUND session id -> resume it (id, dir, title)
        std::function<void(winrt::hstring, winrt::hstring, winrt::hstring)> _forkSessionHandler; // Agentmaster: launch box Fork -> fork the session id (id, dir, title)
        std::function<void(winrt::hstring)> _restartSessionHandler; // Agentmaster: Triage Board / Explorer-tree "Restart session" -> rebuild a managed session's connection in place (page does it cross-window)
        std::function<void(winrt::hstring, winrt::hstring)> _forkManagedSessionHandler; // Agentmaster: Triage Board / Explorer-tree "Fork session" -> kind-aware fork of a managed session (the WT tab menu's fork), opening the fork in the acting window. (sessionId, model) — model "" = Default (the launch-model picker)
        std::function<void(winrt::hstring, winrt::hstring)> _renameHandler; // Agentmaster: Explorer-tree rename -> page (registry title + tab title in lockstep)
        std::function<void(winrt::hstring, winrt::Windows::UI::Xaml::FrameworkElement)> _tagsHandler; // Agentmaster (bookmark tags): board-card / tree-row "Tags" -> page opens the tag editor panel (sessionId, the clicked element as the anchor)
        std::function<void(winrt::hstring, bool, winrt::hstring, winrt::hstring)> _showTranscriptHandler; // Agentmaster (transcript overlay): "Show Transcript" / "Transcript ▸ Show" -> page opens the reading view (sessionId, codex, codexRolloutId, title)
        std::function<void(winrt::hstring, winrt::Windows::UI::Xaml::UIElement)> _tagHoverBeginHandler; // Agentmaster (bookmark tags): a card ribbon's pointer-enter -> the page's rich tag hover panel (tag, the ribbon as the anchor)
        std::function<void()> _tagHoverEndHandler; // Agentmaster (bookmark tags): the ribbon's pointer-exit -> cancel/grace-close that panel
        std::function<void(uint32_t, winrt::hstring, bool)> _adoptExternalHandler; // Agentmaster: EXTERNAL-tree Adopt (pid, cwd, fork) -> page forks/resumes the external's conversation into a managed tab
        std::function<void(uint32_t, winrt::hstring, bool, bool)> _codexLaunchHandler; // Agentmaster (Codex-launch): EXTERNAL-codex (pid, cwd, adopt, fork): Adopt (adopt=true; fork picks fork/resume) / Open-New-Codex (adopt=false)
        std::function<std::unordered_set<std::wstring>()> _localScopeProvider; // Agentmaster: this window's hosted session ids (for the Explorer Tree LOCAL scope)
        std::function<std::wstring(const std::wstring&)> _liveDraftProvider; // Agentmaster (PENDING_INPUT.md §8): read a session's unsent input-box draft from its LIVE buffer for "Copy Current Prompt" ("" when the tab isn't hosted/readable here — the remembered draft then wins)
        std::function<bool()> _windowForegroundProvider; // Agentmaster (focus-steal fix): is this Manager's window the OS foreground window? Gates _Refresh's focus-restore so a background rebuild can't steal foreground.
        std::function<void(bool)> _pauseHandler; // Agentmaster (DELIVERY.md §17): asks the page to set the process-wide pause (SetPauseHandler)
        std::function<void(::Agentmaster::AppSettings)> _settingsSink;
        std::function<void()> _reopenWindowsHandler; // Agentmaster (M10): the "Reopen Windows" recover-button action
        std::function<void()> _openSessionsHandler; // Agentmaster (Sessions page): open the full-window global Sessions browser (TerminalPage-hosted)
        std::function<void()> _resetHiddenSessionsHandler; // Agentmaster (Sessions page): the Settings cog's "Reset hidden sessions" action — clear AppSettings.hiddenSessionIds (TerminalPage-side)
        std::function<void()> _quitForUpdateHandler; // Agentmaster (updater): "Update now" -> quit the app gracefully (page's RequestQuit) so the installer can replace it
        std::function<void()> _refreshHandler; // Agentmaster: Explorer Tree refresh -> page re-surveys the Fleet Observer (reload the current scope's data)
        std::function<void(::Agentmaster::ManagerState)> _lensChangedHandler; // Agentmaster (M10): push lens changes to the hosting window
        ::Agentmaster::AppSettings _appSettings{}; // current settings (seeded by SetSettings; edited via the cog)
        // Agentmaster (the GLOBAL PAUSE — DELIVERY.md §17): the process-wide pause state as this window
        // last heard it — a DISPLAY mirror, written only by SetGlobalPauseShown. The scheduler is the
        // owner; the button's click reads this to ask for the opposite and never flips it itself.
        bool _globalPaused{ false };

        std::wstring _selectedId;
        std::wstring _scopeDir; // board filter: empty == all directories
        std::wstring _selectedPromptId;
        // Agentmaster: the EXTERNAL row selected in the tree's EXTERNAL scope (its resolved
        // conversation id + cwd + title), and the prompts read from its transcript for the read-only
        // Auto Testing. _externalPlanLoadedFor == the id whose prompts are loaded (empty while loading
        // or none selected). Mutually exclusive with _selectedId (a managed selection clears these).
        std::wstring _selectedExternalSessionId;
        std::wstring _selectedExternalCwd;
        std::wstring _selectedExternalTitle;
        ::Agentmaster::AgentKind _selectedExternalKind{ ::Agentmaster::AgentKind::Claude }; // Phase C1: which reader the read-only plan uses (Claude transcript vs Codex rollout)
        std::wstring _selectedExternalRolloutPath; // Codex: the rollout .jsonl path for the read-only plan (Claude leaves empty)
        std::wstring _externalPlanLoadedFor;
        std::vector<std::wstring> _externalPlanPrompts;
        // Agentmaster: the ONE session scope behind BOTH toggles — the Explorer Tree's 3-way cycle
        // (after the "EXPLORER TREE" title) and the Triage Board's 2-way LOCAL/GLOBAL (after the
        // "TRIAGE BOARD" title; the board has no External mode — it reads External as Global, and
        // a click there flips the shared scope to LOCAL).
        //   Local    == this window's sessions only (the page's _claudeTabs)
        //   Global   == every window's sessions (the whole process-wide registry)
        //   External == the Fleet Observer's observe-only external claudes (_externalClaudes), grouped
        //               by cwd; right-click a row for Open New Session Here / Adopt.
        // PERSISTED per window in the lens (ManagerState.treeScope) — no longer in-memory-only —
        // so a reopened window keeps its scope; both buttons reflect the one state.
        enum class TreeScope { Local, Global, External };
        TreeScope _treeScope{ TreeScope::Local };
        // Agentmaster: the ONE scope mutator behind both toggles — handles the External enter/leave
        // selection cleanup, reflects BOTH buttons, pushes the lens (persisted), and refreshes
        // (skippable when the caller refreshes itself, e.g. _OnRenameSession). _UpdateBoardScopeButton
        // paints the board toggle's LOCAL/GLOBAL label (External shows as GLOBAL).
        void _SetTreeScope(TreeScope scope, bool refresh = true);
        void _UpdateBoardScopeButton();
        std::unordered_set<std::wstring> _collapsedDirs;
        bool _suppressAutorunnerEvent{ false };
        // Agentmaster (O6): the observer's External (WindowsTerminal) claudes, pushed by the page's
        // _ObserverProbe; rendered as a collapsible "External (N)" board group. _externalCollapsed
        // hides the cards (the header keeps the count).
        std::vector<::Agentmaster::ExternalClaudeRow> _externalClaudes;
        bool _externalCollapsed{ false };

        winrt::Windows::UI::Xaml::Controls::Grid _root{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _focusSink{ nullptr }; // Agentmaster: invisible, caret-less keyboard-focus sink so the pane-root key handlers (alt+right, ctrl+tab, ctrl+alt+<n>) fire on tab switch — see Focus()
        winrt::Windows::UI::Xaml::Controls::StackPanel _boardHost{ nullptr }; // horizontal columns
        winrt::Windows::UI::Xaml::Controls::TextBlock _boardScope{ nullptr }; // Agentmaster: "[scope: <dir>]" — shown ONLY while a directory is scoped (the old "[all directories]" placeholder is gone; it was display-only)
        winrt::Windows::UI::Xaml::Controls::Button _showAllBtn{ nullptr }; // Agentmaster: the board's "Show all" — collapsed while already showing all (empty scope), shown once a dir is scoped
        winrt::Windows::UI::Xaml::Controls::Button _boardScopeBtn{ nullptr }; // Agentmaster: the board's LOCAL/GLOBAL toggle after the "TRIAGE BOARD" title — same state as _treeScopeBtn (External reads GLOBAL)
        winrt::Windows::UI::Xaml::Controls::Button _boardSortBtn{ nullptr }; // Agentmaster: the board's MOST ACTIVE/NEWEST/OLDEST/A-Z sort toggle after the scope toggle (global, persisted; AppSettings::boardSort, separate from _treeSortBtn)
        winrt::Windows::UI::Xaml::Controls::Button _boardRefreshBtn{ nullptr }; // Agentmaster: the board's ↻ refresh button after the sort toggle (re-scan + redraw the whole tab; twin of _treeRefreshBtn)
        winrt::Windows::UI::Xaml::Controls::Button _clearSelBtn{ nullptr }; // Agentmaster: the board's "Clear" button next to LOCAL/GLOBAL — deselect the current card/row; hidden while nothing is selected (synced by _RebuildBoard, like _showAllBtn)
        // Agentmaster (bookmark tags): the board header's TAG FILTER chips row, mounted in the header
        // Grid's star column right AFTER the "Clear" button so it takes whatever width is left and
        // horizontally scrolls past a screenful (the header can't wrap — the board's own ScrollViewer
        // scrolls only the columns). The scroller collapses whole while no visible session is tagged
        // and nothing is picked, so an untagged fleet's header looks exactly as it did before.
        winrt::Windows::UI::Xaml::Controls::ScrollViewer _boardTagChipsScroll{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _boardTagChipsPanel{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _treeScopeBtn{ nullptr }; // Agentmaster: the LOCAL/GLOBAL/EXTERNAL toggle after the "EXPLORER TREE" title
        winrt::Windows::UI::Xaml::Controls::Button _treeSortBtn{ nullptr }; // Agentmaster: the NEWEST/OLDEST/MOST ACTIVE/A-Z sort toggle after the scope toggle (global, persisted)
        winrt::Windows::UI::Xaml::Controls::Button _treeRefreshBtn{ nullptr }; // Agentmaster: the ↻ refresh button after the sort toggle (reload the current scope's data)
        winrt::Windows::UI::Xaml::Controls::StackPanel _treeHost{ nullptr };
        // Agentmaster: id -> the live board card / tree row Button, repopulated on every _Refresh
        // (cleared + refilled by _RebuildBoard / _RebuildTree). Used ONLY to RESTORE keyboard focus
        // onto the same card/row after a rebuild: _Refresh recreates every element on any registry
        // notification — including a title-only change — which would otherwise drop focus off the
        // clicked card (the selection highlight survives via _selectedId, the focused element does
        // not). The focused element is identified by its "b:<id>" / "t:<id>" Tag.
        std::unordered_map<std::wstring, winrt::Windows::UI::Xaml::Controls::Button> _boardCardsById;
        std::unordered_map<std::wstring, winrt::Windows::UI::Xaml::Controls::Button> _treeRowsById;
        // Agentmaster (bookmark tags): the LIVE sessions' tags + the user-picked tag colors,
        // refreshed at the top of every _RebuildBoard so _MakeCard can hang each card's bookmark
        // ribbons out of its title band. Same-window fresh (a tag edit triggers a board
        // refresh via the page); another window's edits land on its next rebuild — the tab badges'
        // accepted cross-window staleness.
        // Since 2026-10-10 the tags come from _boardTagsCache (below), not from one store read per card
        // per rebuild; the colors are one LoadAllTagColors per rebuild, which the store answers from its
        // own cache for the cost of one stat.
        std::unordered_map<std::wstring, std::vector<std::wstring>> _boardTags;
        std::map<std::wstring, std::wstring> _boardTagColors; // folded tag name -> "#AARRGGBB" (empty when no live session is tagged)
        // Agentmaster (tags perf, 2026-10-10): every shown session's tag list as last read — an EMPTY
        // list included, since "untagged" is what most sessions cache — valid for the tag epoch it was
        // read at (_boardTagsEpoch). _RebuildBoard used to call GetSessionTags for every card on every
        // rebuild: one file stat and a record copy per card, up to four rebuilds a second while the
        // Manager is on screen (~466 cards in the release's biggest window). Now a rebuild reads a
        // session's tags only when that session is new to the board, or after
        // ::Agentmaster::SessionTagsEpoch() moved — a tag or a tag color changed somewhere in this
        // process, any window — which drops the whole cache (the side panel's rule, SIDE_PANEL.md §2c).
        // ⚠ NOT seen: a tag written by ANOTHER process to the same profile, until the epoch next moves or
        // the session leaves the board and returns. No other process writes tags today (the CLI only
        // reads), and two instances on one profile already draw the profile-mutex warning.
        // Pruned to the shown sessions on each rebuild, so it cannot grow with every session that ever
        // crossed the board.
        std::unordered_map<std::wstring, std::vector<std::wstring>> _boardTagsCache;
        // The tag epoch _boardTagsCache was read at. Starts at a value SessionTagsEpoch never returns, so
        // the first rebuild always reads.
        uint64_t _boardTagsEpoch{ UINT64_MAX };
        // Agentmaster (bookmark tags): the board's picked TAG FILTER — display-cased names in click
        // order; empty == no filter. OR semantics (a card passes when it carries ANY picked tag), the
        // deliberate difference from the Sessions browser's AND — see _BoardTagFilterAccepts. Part of
        // the per-window lens (ManagerState::boardTagFilter), so it is restored on reopen; a picked tag
        // is NEVER silently dropped when its last carrier goes away (its chip stays, at 0, so it can
        // always be clicked off) — the startup case where the fleet hasn't loaded yet would otherwise
        // erase a restored filter.
        std::vector<std::wstring> _boardTagFilter;
        // Agentmaster (bookmark tags): the "Untagged" chip — the (N+1)th bucket, leading the chips row,
        // ON by default (which is what keeps an untouched board showing everything). It is NOT part of
        // _boardTagFilter's OR: an untagged session is judged by THIS flag alone, a tagged one by the
        // picks alone — so unchecking it is how you say "only tagged cards", and it stays meaningful
        // whether or not any tag is picked. Also per-window persisted (ManagerState::boardShowUntagged).
        bool _boardShowUntagged{ true };
        // Agentmaster (UI_THREAD.md §12): column key -> the window's persistent column (BoardColumnView). Filled
        // on first use by _EnsureBoardColumn; never erased, so a reference into it stays valid. At most six
        // entries. The scroll offset each column used to capture and restore across a rebuild
        // (_boardColumnScrollers / _boardColumnOffsets, until 2026-10-14) needs no bookkeeping now: the
        // column's one ScrollViewer keeps it.
        std::unordered_map<std::wstring, BoardColumnView> _boardColumns;
        // Agentmaster (UI_THREAD.md §12): the keys of _boardHost's children, in order — the mirror that
        // _SyncBoardColumnOrder plans its edits against.
        std::vector<std::wstring> _boardColumnOrder;
        winrt::Windows::UI::Xaml::Controls::StackPanel _planHeaderHost{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _planListHost{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ScrollViewer _planScroll{ nullptr }; // Agentmaster: hosts _planListHost — pinned to the bottom on first view of a subject (see _PinPlanToBottomOnSubjectChange)
        std::wstring _planAutoScrolledFor; // Agentmaster: the subject key (session id / "x:<extId>") we last auto-scrolled the Auto Testing to bottom for; only a CHANGE re-pins (a same-subject _Refresh keeps the user's scroll)
        // Agentmaster (the shared LAUNCH BAR, AgentLaunchBar.h): the toolbar's launch row — built in _BuildLayout from
        // _MakeLaunchBarHost and owned for the content's whole life; its parts sit in _launchBar / _launchBtns and its
        // drop-down Popup in _root. Its Launch button, Fork button, underline, drop-down, hover menu and Codex flag live
        // inside it now (they were members here). Shut down in the destructor (its hover timer is UI-affine).
        std::shared_ptr<AgentLaunchBar> _launch;
        // This window's recent-folders list (recent-dirs.json): loaded in the constructor before _BuildLayout builds the
        // bar, read and pushed by _launch, and handed to the side panel's "+" launcher (LaunchRecentDirs) so both bars of
        // the window share ONE list.
        std::shared_ptr<AgentLaunchRecentDirs> _launchRecentDirs;
        // The bar's working-dir box (_launch->CwdBox()), kept under its historical name: _ReflowLaunchBar sizes it, the
        // selection pre-aims it (_SelectSession / _SelectExternal), SetSettings seeds it, SetActivateAllBusy greys it out.
        // After a direct Text write call _ValidateLaunchBox (TextChanged is unreliable while the page is hidden).
        winrt::Windows::UI::Xaml::Controls::TextBox _cwdBox{ nullptr };
        // Agentmaster (responsive launch bar): the toolbar's top row + its collapsible pieces, promoted to
        // members so _ReflowLaunchBar can stage-collapse them as the pane narrows. _toolbarCol is the
        // vertical [title row | actions row] stack; _launchBar is the title+launch row; _launchBtns groups
        // Launch/Fork/Reopen/Activate so wrapping moves ONE element to its own line. _agentmasterText/_dashText
        // are always shown; _launchAText ("launch a") + _sessionInText ("session in") collapse first.
        winrt::Windows::UI::Xaml::Controls::StackPanel _toolbarCol{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _launchBar{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _launchBtns{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _agentmasterText{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _dashText{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _launchAText{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _sessionInText{ nullptr };
        bool _launchBtnsWrapped{ false }; // true once _launchBtns has been moved to its own (2nd) line
        double _lastRootWidth{ 0.0 }; // last _root width seen in SizeChanged (drives reflow off the hot path)
        double _measLaunchA{ 56.0 }; // cached DesiredSize of "launch a" (read 0 once collapsed; kept from when visible)
        double _measSessionIn{ 62.0 }; // cached DesiredSize of "session in"
        // Agentmaster (Codex-launch): the bar's Claude<->Codex agent toggle before the box (_launch->AgentToggleButton()),
        // kept so _ReflowLaunchBar can measure it as part of the row.
        winrt::Windows::UI::Xaml::Controls::Button _launchAgentBtn{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _addPromptBox{ nullptr };
        // Agentmaster (prompt history): navigation state for the compose box's Up/Down recall.
        // _promptHistoryIndex == -1 means "not navigating" (the live draft); 0 == the newest sent
        // prompt, growing older. _promptHistory is the newest-first snapshot taken when navigation
        // begins; _promptHistoryDraft is the text that was being composed when it began (restored on
        // Down past the newest). _promptHistoryNavigating guards our own .Text() writes so the box's
        // TextChanged handler doesn't mistake a recall for a user edit and reset navigation.
        int _promptHistoryIndex{ -1 };
        std::vector<std::wstring> _promptHistory;
        std::wstring _promptHistoryDraft;
        bool _promptHistoryNavigating{ false };
        // Agentmaster (PENDING_INPUT.md §8a): the ONE-SHOT latch behind _MaybePrefillPromptFromDraft —
        // the (session, draft text) pair last pulled into the compose box. Re-focusing an empty box
        // offers that same draft only ONCE: after the user queues it (the box clears) or deletes it,
        // clicking back in must NOT silently re-insert it (which would double-queue the prompt). A
        // CHANGED draft (or a different session) doesn't match the latch, so it is offered normally.
        std::wstring _promptPrefillSessionId;
        std::wstring _promptPrefillText;
        // Agentmaster (PENDING_INPUT.md §8b): the compose box's conditional "pull in the unsent
        // prompt" button — shown only while the selected session's draft differs from the box AND
        // taking it would not destroy composed text (_UpdateDraftPullButton / EvaluateDraftPull). It
        // shares column 2 with the templates (paper) icon, so appearing/disappearing never changes the
        // compose row's HEIGHT (see the build site for why it is not a fourth icon on the left).
        winrt::Windows::UI::Xaml::Controls::Button _pullDraftBtn{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _autorunnerBtn{ nullptr }; // Agentmaster: Autorunner mode toggle, now in a thin strip atop the Auto Testing TAB body (was the old FLIGHT PLAN header)
        // Agentmaster: the Auto-Testing pane's two-state [Summary | Auto Testing] segmented tab toggle
        // (its top line) + the two swappable tab bodies. _summaryHost holds the Summary tab; _autoTestBody
        // holds the Autorunner strip + the prompt queue / compose box. Visibility is driven by
        // AppSettings::autoTestingShowsSummary via _UpdatePlanPaneTab.
        winrt::Windows::UI::Xaml::Controls::Button _summaryTabBtn{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _autoTestTabBtn{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Grid _summaryHost{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Grid _autoTestBody{ nullptr };
        // Agentmaster (Summary tab): the scrollable host + box panel for the Summary tab. _summaryScroll
        // is the ONE inner scrollbar (the narrow pane scrolls a long box); _summaryBoxHost holds the
        // rendered RenderSessionSummaryBox lines (mono TextBlocks + full-width rules), mirroring the
        // Sessions page's SessAppendSummaryBox. Loaded off-thread + cached single-entry by (id, mtime);
        // user messages are rendered newest-first. _summaryShown* tracks what is CURRENTLY on screen
        // (incl. "\x01loading" / "\x01none" sentinels) so a frequent _Refresh doesn't rebuild the box
        // (and lose scroll) when nothing changed; _summaryLoadingId dedupes the in-flight analyze (one
        // per id — a busy session's mtime churn can't stack loads).
        winrt::Windows::UI::Xaml::Controls::ScrollViewer _summaryScroll{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _summaryBoxHost{ nullptr };
        std::wstring _summaryCacheId;
        int64_t _summaryCacheMtime{ -1 };
        std::wstring _summaryCacheText;
        std::wstring _summaryShownId;
        int64_t _summaryShownMtime{ -1 };
        std::wstring _summaryLoadingId;
        winrt::Windows::UI::Xaml::Controls::StackPanel _templatesRow{ nullptr }; // Agentmaster: the Templates row — collapsed by default, toggled by the paper icon
        winrt::Windows::UI::Xaml::Controls::Button _pauseBtn{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _settingsBtn{ nullptr }; // the cog (next to Pause)
        winrt::Windows::UI::Xaml::Controls::Button _sessionsBtn{ nullptr }; // Agentmaster (Sessions page): "Sessions" -> the global on-disk sessions browser (the sole history view; FAVORITES.md)
        winrt::Windows::UI::Xaml::Controls::Button _reopenBtn{ nullptr }; // Agentmaster (M10): "Reopen Windows (N)" -> reopen saved-but-not-open windows (shown only when N>0)
        winrt::Windows::UI::Xaml::Controls::Button _activateAllBtn{ nullptr }; // Agentmaster (eager-init): "Activate All Tabs (N)" -> wake this window's dormant tabs (shown only when N>0)
        bool _activateAllBusy{ false }; // Agentmaster (eager-init pacing): true while the page's drip is waking this window's tabs — relabels/disables _activateAllBtn + disables _cwdBox (SetActivateAllBusy)
        winrt::Windows::UI::Xaml::Controls::Button _keepAwakeBtn{ nullptr }; // Agentmaster: tri-mode "Keep Awake" button -> SetThreadExecutionState keeps the PC + display from sleeping
        // Agentmaster: the user-selected keep-awake mode. Off = sleep normally; Always = always hold the
        // execution-state flag; WhileRunning = hold ONLY while a live session is actively Running (so the
        // machine can sleep once every agent is idle/waiting/done — and never mid-turn).
        enum class KeepAwakeMode { Off, Always, WhileRunning };
        KeepAwakeMode _keepAwakeMode{ KeepAwakeMode::Off };
        bool _keepAwakeHeld{ false }; // whether SetThreadExecutionState is CURRENTLY holding the flag (drives transition-only OS calls + the button color)
        // Last-rendered button appearance — _UpdateKeepAwakeButton repaints ONLY on a (mode, held) transition,
        // so the frequent _Refresh -> _RefreshKeepAwakeHold calls don't rebuild the content + resources (or
        // flicker the button) every tick. _keepAwakeRendered forces the very first paint.
        KeepAwakeMode _keepAwakeRenderedMode{ KeepAwakeMode::Off };
        bool _keepAwakeRenderedHeld{ false };
        bool _keepAwakeRendered{ false };
        // ---- "Claude not detected" overlay (native-exe-only policy gate) ----
        winrt::Windows::UI::Xaml::Controls::Grid _claudeMissingOverlay{ nullptr }; // dimmed modal layer; shown when launch/fork is blocked by no native claude.exe
        winrt::Windows::UI::Xaml::Controls::Grid _specifyModelOverlay{ nullptr }; // dimmed modal layer for the "Specify a model..." prompt (the card is rebuilt per open — fresh MRU + per-invocation callback)
        winrt::Windows::UI::Xaml::Controls::TextBlock _claudeMissingStatus{ nullptr }; // the live detection status line (updated by Browse / Re-check)
        winrt::Windows::UI::Xaml::Controls::Button _claudeMissingInstallBtn{ nullptr }; // one-click install: `claude install` (npm-legacy) or the claude.ai native bootstrap (none); label/visibility set in _ShowClaudeMissing from ClaudeInstallKind()
        // ---- Settings overlay (the cog dialog) ----
        winrt::Windows::UI::Xaml::Controls::Grid _settingsOverlay{ nullptr }; // dimmed modal layer over _root
        // Agentmaster (LocalTooltip, AgentLocalTooltip.h): the cog's hover-description panel — every
        // AgentSetTip text inside the card renders HERE (outside the card at its top-left: same top,
        // right edge glued to the card's left edge, growing only leftward) instead of as a floating
        // ToolTip (suppressed inside the card's scope; a too-narrow window falls back to floating).
        AgentLocalTooltip _settingsLocalTip;
        // Agentmaster (LocalTooltip): the Manager TAB's hover-description panel — the toolbar /
        // Triage Board / Explorer Tree / Auto Testing regions' tips render in a click-through panel
        // nested in the board's top-right corner (scoped per region, NOT on _root, so the settings /
        // claude-missing overlay cards stay outside it — see _BuildLayout's wiring block).
        AgentLocalTooltip _managerLocalTip;
        // Agentmaster: the cog's TOP TABS. The strip buttons + their content panels are parallel-indexed
        // (button i <-> panel i); _SwitchSettingsTab SWAPS panel i into the single _settingsScroll (the
        // ContentPresenter-swap idiom) and restyles the strip. Each section in _BuildSettingsOverlay fills
        // one of these panels.
        std::vector<winrt::Windows::UI::Xaml::Controls::Button> _settingsTabButtons;
        std::vector<winrt::Windows::UI::Xaml::Controls::StackPanel> _settingsTabPanels; // one content panel per tab
        // PER-TAB "Reset" (the footer button left of Cancel): index-aligned with the two vectors
        // above — a tab that supplies a handler gets the button while it is showing, a tab that
        // passes nullptr (every tab but Commands today) simply doesn't. The mechanism is generic
        // on purpose so wiring another tab is one lambda at its addSettingsTab call; nothing else
        // changes. A handler resets that tab's CONTROLS to the shipped defaults (AppSettings{}),
        // NOT the disk: the change only lands on Save, so Cancel still discards it and no confirm
        // dialog is needed (a ContentDialog over this in-content modal is the XAML-Islands trap).
        std::vector<std::function<void()>> _settingsTabResets;
        winrt::Windows::UI::Xaml::Controls::Button _settingsResetBtn{ nullptr }; // footer, left of Cancel; shown only for a tab with a reset handler
        winrt::Windows::UI::Xaml::Controls::ScrollViewer _settingsScroll{ nullptr }; // single scroller; its Content is the active tab's panel
        int _settingsActiveTab{ 0 }; // index of the showing tab (reset to 0 on each _ShowSettings)
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setSkipPermissions{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setTrustWorkspace{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _setModel{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _setLaunchModels{ nullptr }; // launch-model picker: multi-line "Display name | model-id" list (one per line) feeding every "Open New Session Here" / "Fork session" submenu
        winrt::Windows::UI::Xaml::Controls::Border _setLaunchModelsBorder{ nullptr }; // wraps _setLaunchModels; recolored by the lexer (the _setEnvBorder idiom)
        winrt::Windows::UI::Xaml::Controls::TextBlock _setLaunchModelsStatus{ nullptr }; // the counts + first-issue line under the editor (the _setEnvStatus idiom)
        winrt::Windows::UI::Xaml::Controls::TextBox _setModelFamilies{ nullptr }; // current-model adornment: the comma-separated Anthropic FAMILY words ShortModelName recognizes for a bare --model alias (AppSettings::modelFamilies; blank => built-ins)
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setIncludeCoAuthored{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ComboBox _setDefaultMode{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _setMaxAutoSends{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setStopOnError{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setPauseOnHuman{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setPreserveDraft{ nullptr }; // PENDING_INPUT.md §9 — the DRAFT SWAP
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setDraftSwapCtrlS{ nullptr }; // PENDING_INPUT.md §9 — use Claude's Ctrl+S stash as the swap's first rung
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setVerifySend{ nullptr }; // DELIVERY_PLAN.md R4 — verified placement (fill → read-back → CR)
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setRestoreDraft{ nullptr }; // PENDING_INPUT.md §10 — the restore RE-FILL: type a remembered unsent draft back into a reopened session's box
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setCarryDraftOnFork{ nullptr }; // PENDING_INPUT.md §11 — the FORK CARRY: copy the source's unsent draft (pastes expanded) into a new fork's box; SESSIONS tab (not autorunner-gated — release builds fork too)
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setResumeApiErrorTurns{ nullptr }; // HOOKS.md "API-error turns" — AppSettings::resumeApiErrorTurns: before a resume, rewind a turn an API error ended so the resumed claude continues it; SESSIONS tab, read fresh by each resume seam (no live push needed)
        // SESSIONS_DB.md §4 / §7.6 — the session database (SESSIONS tab, "SESSION DATABASE"): the mode (Off / Shadow /
        // On), the refresh tick's interval (D16), DuckDB's threads and memory limit (§10), and a status line read from
        // the engine's refresher when the cog opens.
        winrt::Windows::UI::Xaml::Controls::ComboBox _setSessionDbMode{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _setSessionDbRefresh{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _setSessionDbThreads{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _setSessionDbMemory{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _setSessionDbStatus{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setConfirmKill{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setReapOrphans{ nullptr }; // OBSERVER.md §13a: shut down a session whose terminal host died (reapOrphanedSessions)
        // RENDERING.md: draw every terminal on the CPU (WARP) instead of the GPU (AppSettings::softwareRendering,
        // default OFF). Behavior tab -> RENDERING; the page applies a change live to every window on Save.
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setSoftwareRendering{ nullptr };
        // RENDERING.md §4c ("Hidden tabs"): Behavior tab -> RENDERING. Stop drawing off-screen terminals
        // (AppSettings::pauseHiddenTerminals), give back their GPU device (releaseHiddenTerminalDevices)
        // after this many seconds hidden (releaseHiddenTerminalDevicesAfterSeconds; blank = 60, 0 = at
        // once). The page applies a change live to every window on Save.
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setPauseHiddenTerminals{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setReleaseHiddenDevices{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _setReleaseHiddenSeconds{ nullptr };
        // Agentmaster (RENDERING.md §4c): grey out the release switch while the pause is off, and the
        // seconds box while either is off — the settings they set then do nothing. Called from both
        // switches' Toggled and from the cog's seed (a programmatic IsOn may not raise Toggled). UI
        // thread. Never throws: null-safe on every control, and a failed XAML property call is logged
        // (Rule #18) and leaves the enabled state stale until the next toggle or cog open.
        void _SyncHiddenRenderingControls();
        // NOTIFICATIONS tab (System notifications): the Running -> X Windows-toast switches. The master
        // toggle gates the rest (the dependents grey out while it's off); the five checkboxes pick WHICH
        // target states notify (all checked == the "Running to anything else" default rule).
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setNotifyEnabled{ nullptr }; // master: show Windows notifications (AppSettings::notificationsEnabled, default ON)
        winrt::Windows::UI::Xaml::Controls::CheckBox _setNotifyWaiting{ nullptr }; // Running -> WaitingForInput ("waiting for you")
        winrt::Windows::UI::Xaml::Controls::CheckBox _setNotifyNeedsApproval{ nullptr }; // Running -> NeedsApproval ("needs approval")
        winrt::Windows::UI::Xaml::Controls::CheckBox _setNotifyIdle{ nullptr }; // Running -> Idle
        winrt::Windows::UI::Xaml::Controls::CheckBox _setNotifyDone{ nullptr }; // Running -> Done
        winrt::Windows::UI::Xaml::Controls::CheckBox _setNotifyError{ nullptr }; // Running -> Error
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setNotifySuppressFocused{ nullptr }; // skip the toast when the session's tab is focused in the active window (default ON)
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setNotifySound{ nullptr }; // play the Windows notification sound (OFF => <audio silent/>; default ON)
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setTaskbarUnreadBadge{ nullptr }; // the unread-session count on the taskbar button (AppSettings::taskbarUnreadBadge, NOTIFICATIONS.md section 8; default ON; deliberately NOT greyed by the toast master)
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setTaskbarUnreadFrame{ nullptr }; // the per-window frame on the taskbar / Alt+Tab preview of a window with unread tabs (AppSettings::taskbarUnreadThumbnailFrame, NOTIFICATIONS.md section 9; default ON; independent of the count and the toast master)
        // MESSAGING tab (MESSAGING.md — the fleet in Telegram): the master switch, the bot token (typed here, stored in
        // Windows Credential Manager on Save — never in a profile file), the paired Telegram user id, the idle grace
        // (D25), the Search topic's thinking level, "Check bot" (a getMe round trip off the UI thread) and a status line
        // read from the engine's service when the cog opens.
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setMsgEnabled{ nullptr };
        winrt::Windows::UI::Xaml::Controls::PasswordBox _setMsgToken{ nullptr }; // the typed token; empty = keep the stored one
        winrt::Windows::UI::Xaml::Controls::TextBox _setMsgOwner{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _setMsgGrace{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ComboBox _setMsgThinking{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _setMsgCheck{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _setMsgForget{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _setMsgStatus{ nullptr };
        bool _msgForgetToken{ false }; // "Forget token" was clicked: Save removes the stored token
        // MESSAGING.md §21.11: the topics' default mode (verbose = the work too; off = the conversation only), Claude's
        // Markdown as rich messages, and the optional SECRETARY bot (a second bot that sends the user's turns as the user):
        // its token goes to Credential Manager on Save like the topics bot's; empty = keep the stored one.
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setMsgVerbose{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setMsgRich{ nullptr };
        // MESSAGING.md §21.42: whether every session's card carries the terminal's screen as a picture
        // (AppSettings::messagingCardScreenshots, on by default): read again at each card update, uploaded when it changed.
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setMsgCardScreens{ nullptr };
        // MESSAGING.md §21.39: 📜 All Screen's default transcript level (AppSettings::messagingAllScreenLevel): what a bare /allscreen
        // draws and the level the card's picker marks with ★. Its items are Msg::kAllScreenLevels in order (index = row).
        winrt::Windows::UI::Xaml::Controls::ComboBox _setMsgAllScreenLevel{ nullptr };
        winrt::Windows::UI::Xaml::Controls::PasswordBox _setMsgSecretaryToken{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _setMsgSecretaryForget{ nullptr };
        bool _msgForgetSecretaryToken{ false }; // "Forget secretary token" was clicked: Save removes it
        void _CheckTelegramBot(); // the "Check bot" button: getMe with the typed (else stored) token on a worker thread; the result lands in _setMsgStatus
        // MESSAGING.md §21.32.4: the FILE LINKS section — how Claude's links to files on this PC reach the phone: Off, a LAN
        // address of this PC (the dropdown lists them), or through cc.sysd.cc (Cloudflare -> the tunnel on this PC -> the fastrun
        // relay -> this install's server). The station and the port name this install to the relay; the excluded folders are
        // never linked. A live line previews a link's shape and says what is wrong with a box; the status line is the service's
        // FileLinksStatusText, read when the cog opens.
        winrt::Windows::UI::Xaml::Controls::ComboBox _setMsgFilesMode{ nullptr }; // 0 Off, 1 home network, 2 cc.sysd.cc
        winrt::Windows::UI::Xaml::Controls::TextBox _setMsgFilesStation{ nullptr }; // "" = the install's default station
        winrt::Windows::UI::Xaml::Controls::TextBox _setMsgFilesPort{ nullptr }; // "" = the install's default port
        winrt::Windows::UI::Xaml::Controls::ComboBox _setMsgFilesLanBase{ nullptr }; // item 0 = automatic; the rest = _msgFilesLanUrls
        winrt::Windows::UI::Xaml::Controls::TextBox _setMsgFilesHostedBase{ nullptr }; // "" = https://cc.sysd.cc
        winrt::Windows::UI::Xaml::Controls::TextBox _setMsgFilesExclude{ nullptr }; // one folder per line
        winrt::Windows::UI::Xaml::Controls::TextBlock _setMsgFilesPreview{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBlock _setMsgFilesStatus{ nullptr };
        std::vector<::Agentmaster::Msg::FileLinkLanAddress> _msgFilesLanAddresses; // this PC's private IPv4 addresses, read when the cog opens
        std::wstring _msgFilesHostName; // this PC's DNS name, read when the cog opens (the dropdown's last choice)
        std::vector<std::wstring> _msgFilesLanUrls; // parallel to _setMsgFilesLanBase's items: "" = automatic (the first address)
        /// <summary>
        /// Refills the "links point to" dropdown with this PC's LAN URLs on the port the box names (else the default), keeping
        /// the address of <paramref name="keepUrl"/> selected — a port change keeps the same address — or appending it, marked,
        /// when it is no address of this PC now (an old Wi-Fi address, a name typed into settings.json).
        /// </summary>
        /// <param name="keepUrl">The URL to keep selected ("" = automatic).</param>
        void _RebuildFileLinkLanChoices(const std::wstring& keepUrl);
        /// <summary>
        /// The FILE LINKS preview line: what a link will look like with the boxes as they are, and what in them is not valid (a
        /// station, a port or an address the service would replace with the default). Also enables the dropdown in the home
        /// network mode only and the hosted address in the cc.sysd.cc mode only.
        /// </summary>
        void _RefreshFileLinksPreview();
        // COMMANDS tab (COMMANDS.md §6a): the /handover slash-command family — each command's
        // Enable toggle + Command-name box + a status line reading the LIVE vs CONFIGURED state
        // ("Active as /x" / "becomes /y after restart" / "disabled after restart"). Rename +
        // disable apply at the NEXT START (the definition files + CommandWatch bindings are set up
        // once at engine init); the LIVE names come from the engine-owned materialized-name
        // markers read fresh from disk at cog open (_cmdLive*; "" == disabled this run).
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setCmdHandoverEnabled{ nullptr }; // /handover family: enable the NEW-TAB handover command
        winrt::Windows::UI::Xaml::Controls::TextBox _setCmdHandoverName{ nullptr }; // its typed name (normalized slug; default "handover")
        winrt::Windows::UI::Xaml::Controls::TextBlock _setCmdHandoverStatus{ nullptr }; // live-vs-configured status line
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setCmdHandoverHereEnabled{ nullptr }; // enable the IN-PLACE (replace-this-tab) handover command
        winrt::Windows::UI::Xaml::Controls::TextBox _setCmdHandoverHereName{ nullptr }; // its typed name (default "handover-here")
        winrt::Windows::UI::Xaml::Controls::TextBlock _setCmdHandoverHereStatus{ nullptr }; // live-vs-configured status line
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setCmdStandbyEnabled{ nullptr }; // enable the STANDBY (fill, don't send — §5b) handover command
        winrt::Windows::UI::Xaml::Controls::TextBox _setCmdStandbyName{ nullptr }; // its typed name (default "handover-standby")
        winrt::Windows::UI::Xaml::Controls::TextBlock _setCmdStandbyStatus{ nullptr }; // live-vs-configured status line
        std::wstring _cmdLiveHandoverName; // the name ACTIVE this run ("" == disabled) — the disk marker at cog open
        std::wstring _cmdLiveHandoverHereName;
        std::wstring _cmdLiveStandbyName;
        void _UpdateCommandsTabStatus(); // re-render the status lines (names + §6b shaping + §6c location) from the current (unsaved) control state
        void _RefreshCommandDefinitionState(); // re-sample the definition files from disk into _cmdDefState* (cog open / after a Reinstall)
        // §6b successor shaping (COMMANDS.md): per-command successor-model combos ("Default" +
        // the launchModels list, items rebuilt at every cog open so a launch-models edit shows;
        // the parallel id vectors map SelectedIndex -> the stored model id, [0] == "" Default,
        // a stored id no longer listed appended as "(custom) <id>" so it round-trips), the
        // family title find/replace regex pair + the HANDOVER file-match regex (validated live
        // via RegexUtil — the shaping status line calls out an invalid pattern), and the
        // delete-after-hand-off toggle. Model/title/delete apply to the NEXT handover right
        // after Save; the file-match pattern is binding-time (applies after restart).
        winrt::Windows::UI::Xaml::Controls::ComboBox _setCmdModelHandover{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ComboBox _setCmdModelHandoverHere{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ComboBox _setCmdModelStandby{ nullptr };
        std::vector<std::wstring> _cmdModelIdsHandover; // index-aligned with _setCmdModelHandover's items
        std::vector<std::wstring> _cmdModelIdsHandoverHere;
        std::vector<std::wstring> _cmdModelIdsStandby;
        winrt::Windows::UI::Xaml::Controls::TextBox _setCmdTitleFind{ nullptr }; // successor-title rewrite: the FIND regex ("" == default "(handover)" naming)
        winrt::Windows::UI::Xaml::Controls::TextBox _setCmdTitleReplace{ nullptr }; // …and its REPLACE text ($1 backrefs honored)
        winrt::Windows::UI::Xaml::Controls::TextBox _setCmdFileMatch{ nullptr }; // the HANDOVER file-match regex ("" == leaf contains "handover"; restart-applied)
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setCmdDeleteAfter{ nullptr }; // delete a HANDOVER md after its successor spawned + delivery secured (never the pointer tier); default OFF
        winrt::Windows::UI::Xaml::Controls::TextBox _setCmdDeleteDeadline{ nullptr }; // …and how long (MINUTES, default 1440 == 24 h) it waits for that successor to actually start
        winrt::Windows::UI::Xaml::Controls::TextBlock _setCmdShapingStatus{ nullptr }; // the shaping summary/validation line (invalid regexes called out)
        // COMMANDS.md §6c — WHERE the briefings are written: a free-typed FOLDER with a preset
        // drop-down beside it ("scratchpad" == the shipped default). Rendered into both definition
        // files on Save (RefreshHandoverCommandWritePath), so it applies to the NEXT handover with
        // no restart — unless the file was hand-edited, which _setCmdDefState reports and
        // _setCmdReinstallBtn is the confirmed escape hatch for.
        winrt::Windows::UI::Xaml::Controls::TextBox _setCmdWritePath{ nullptr }; // the folder ("" / "scratchpad" == the session scratchpad)
        winrt::Windows::UI::Xaml::Controls::Button _setCmdWritePathPresets{ nullptr }; // its preset menu (scratchpad · ./ · ./docs · …)
        winrt::Windows::UI::Xaml::Controls::TextBlock _setCmdDefState{ nullptr }; // what the two definition files on disk currently are (ours / yours)
        winrt::Windows::UI::Xaml::Controls::Button _setCmdReinstallBtn{ nullptr }; // confirmed overwrite of both definitions with the shipped text
        // The two definition files' state, sampled from disk at cog OPEN (and after a Reinstall) —
        // not per keystroke: _UpdateCommandsTabStatus runs on every edit and this costs two file
        // reads + hashes. Rendered by that same status pass.
        ::Agentmaster::ShippedCommandFileState _cmdDefStateHandover{ ::Agentmaster::ShippedCommandFileState::Missing };
        ::Agentmaster::ShippedCommandFileState _cmdDefStateHere{ ::Agentmaster::ShippedCommandFileState::Missing };
        ::Agentmaster::ShippedCommandFileState _cmdDefStateStandby{ ::Agentmaster::ShippedCommandFileState::Missing };
        winrt::Windows::UI::Xaml::Controls::ComboBox _setRenameCommit{ nullptr }; // how the tab rename box commits via the keyboard (None / +Shift+Enter / +Enter); GLOBAL
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setWaitingNever{ nullptr }; // Waiting-for-you "unread" model: ON => never time-decay (stay Waiting until read); disables the slider
        winrt::Windows::UI::Xaml::Controls::Slider _setWaitingDecaySlider{ nullptr }; // Waiting-for-you -> Idle timeout, 1..10080 minutes (1m..7d); the "Never" toggle above owns 0, the textbox (left) is the source of truth and may exceed 7d (slider then sits maxed)
        winrt::Windows::UI::Xaml::Controls::TextBox _setWaitingDecayText{ nullptr }; // the same timeout as a compact "12h5m" free-text box to the LEFT of the slider; red border when unparsable; may exceed the slider's 7d max
        winrt::Windows::UI::Xaml::Controls::Border _setWaitingDecayBorder{ nullptr }; // a normally-transparent ring AROUND _setWaitingDecayText that turns red on an unparsable value (an outer ring, so it survives the TextBox focus visual-states that would override a direct BorderBrush)
        bool _waitingDecaySyncing{ false }; // reentrancy guard for the textbox<->slider value sync (suppresses the TextChanged/ValueChanged echo so they don't loop)
        winrt::Windows::UI::Xaml::Controls::TextBox _setServerCache{ nullptr }; // Claude's server-side prompt-cache lifetime in minutes (drives the card's ⚡ "still cached" hint); default 5
        winrt::Windows::UI::Xaml::Controls::TextBox _setLaunchDir{ nullptr };
        winrt::Windows::UI::Xaml::Controls::TextBox _setRecentDirsLimit{ nullptr }; // how many recent Launch dirs the path-picker keeps
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setShowTabCloseButton{ nullptr }; // TABS: show the close (x) button on tabs (OFF => force every tab to "Never"); GLOBAL
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setCloseTabOnMiddleClick{ nullptr }; // TABS: close a tab on middle-mouse click (OFF => disable both the manual hook + WinUI's native middle-close); GLOBAL
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setShowTabIcon{ nullptr }; // TABS: show the profile icon on tabs (OFF, the default => IconStyle::Hidden, takes no strip space); GLOBAL
        // TABS — the TAB SIZE section (AppSettings::tabWidthMode / tabMinWidth / tabMaxWidth /
        // tabTitleFontSize / tabTitleFontWeight / tabMinHeight). GLOBAL; the page applies them live on Save
        // (TerminalPage::_ApplyTabSizingSettings). The four sliders show their value in their Header
        // (_UpdateTabSizeHeaders); the two width sliders push each other along so min <= max always holds.
        winrt::Windows::UI::Xaml::Controls::ComboBox _setTabWidthMode{ nullptr }; // "Tab width": Windows Terminal setting (default) / Equal width / Fit to title / Compact — items in TabStripWidthMode order
        winrt::Windows::UI::Xaml::Controls::Slider _setTabMinWidth{ nullptr }; // the narrowest a tab may get, px (default 130); disabled while the mode is Compact (it doesn't apply there)
        winrt::Windows::UI::Xaml::Controls::Slider _setTabMaxWidth{ nullptr }; // the widest a tab may get, px (default 240)
        winrt::Windows::UI::Xaml::Controls::Slider _setTabTitleFontSize{ nullptr }; // the tab title font size (default 12; the rename box follows it)
        winrt::Windows::UI::Xaml::Controls::ComboBox _setTabTitleFontWeight{ nullptr }; // the tab title weight — Normal (default) / SemiBold / Bold — items in TabTitleFontWeight order
        winrt::Windows::UI::Xaml::Controls::Slider _setTabMinHeight{ nullptr }; // the shortest a tab may get, px (default 32); never below the tab's own content height
        winrt::Windows::UI::Xaml::Controls::TextBlock _setTabWidthHint{ nullptr }; // tab title fit: the dim line under the two width sliders that says, for the mode the "Tab width" combo shows NOW, what the minimum / maximum do and whether a long title widens its tab — so Equal width's "every tab the same share, a long title ends in …" is never a surprise (rendered by _UpdateTabWidthHint)
        bool _tabSizeSyncing{ false }; // reentrancy guard: one width slider pushing the other must not bounce back through the other's push
        void _UpdateTabSizeHeaders(); // re-render the four sliders' "<label> — <value>" Headers from their CURRENT values (the build, every ValueChanged, the seed)
        void _SyncTabMinWidthEnabled(); // the "Tab width" combo's dependents: enable the min-width slider unless the combo says Compact (where a minimum would stop unselected tabs shrinking), then re-render the mode hint (_UpdateTabWidthHint)
        void _UpdateTabWidthHint(); // re-render _setTabWidthHint for the combo's CURRENT (unsaved) mode; null-safe, so the build can call it before the hint exists
        winrt::Windows::UI::Xaml::Controls::ComboBox _setTitleNaming{ nullptr }; // TABS: HOW an untitled session's tab title derives from its working dir — last word (default) / folder as-is / two folders / capitals (TabTitleNaming); GLOBAL, applies at the next launch/adopt
        winrt::Windows::UI::Xaml::Controls::ComboBox _setTitleCase{ nullptr }; // TABS: the case transform on the derived title — Default / Lowercase / Uppercase (TabTitleCase); GLOBAL
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setTitleUnderscores{ nullptr }; // TABS: convert any whitespace in the derived title to '_' (AppSettings::tabTitleSpacesToUnderscores); GLOBAL
        winrt::Windows::UI::Xaml::Controls::TextBlock _titleNamingPreview{ nullptr }; // TABS: the live example preview under the naming controls — made-up paths re-derived through the REAL DeriveSessionTitle from the CURRENT (unsaved) control state on every change
        void _UpdateTitleNamingPreview(); // re-render _titleNamingPreview from the current naming controls
        winrt::Windows::UI::Xaml::Controls::ComboBox _setFavoriteIcon{ nullptr }; // TABS: the FAVORITE marker glyph on a live session's tab — Crown (default) / Star (FAVORITES.md §5a); GLOBAL
        winrt::Windows::UI::Xaml::Controls::TextBox _setMaxTags{ nullptr }; // TABS: the GLOBAL cap on distinct bookmark tags (default 20, hard ceiling 40 — ClampMaxTags); GLOBAL
        winrt::Windows::UI::Xaml::Controls::Slider _setTooltipTagsOpacity{ nullptr }; // TABS: the OPACITY (10..100%) of the bookmark-tag chips in the rich tab TOOLTIP (AppSettings::tooltipTagsOpacity, default 0.9); GLOBAL. Header shows the live percent.
        winrt::Windows::UI::Xaml::Controls::ComboBox _setTabColorMode{ nullptr }; // TABS: HOW managed tabs are colored — shared per working dir (default) / individual per tab / inferred working dir / "Remove colors" (no tab colored, saved colors kept) (TabColorMode); GLOBAL, applied live on Save + cross-window broadcast
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setInferGitRoot{ nullptr }; // TABS: "Use .git folder to infer" — the inferred working dir snaps to the enclosing git root (AppSettings::inferGitRoot, default ON); enabled only while the coloring mode is Inferred; GLOBAL
        winrt::Microsoft::UI::Xaml::Controls::ColorPicker _setFlashRingPicker{ nullptr }; // TABS: the "status flashing color" — the tab status-dot unread flash ring; alpha channel = opacity (IsAlphaEnabled). Lives in a Flyout opened by the swatch button (compact); stored as AppSettings::flashRingColor ("#AARRGGBB"); GLOBAL
        winrt::Windows::UI::Xaml::Controls::Border _flashRingSwatch{ nullptr }; // TABS: the compact color-preview swatch (Flyout opener content) — updated on seed + the picker's ColorChanged
        // PENDING_INPUT.md: the unsent-draft "3 dots" color, a LIGHT/DARK contrast pair (shown LIGHT-on-dark
        // / DARK-on-light, auto-picked from the tab's per-dir color). Same swatch-button-opens-a-flyout idiom
        // as the flash-ring row; stored as AppSettings::pendingDotsLightColor / pendingDotsDarkColor; GLOBAL.
        winrt::Microsoft::UI::Xaml::Controls::ColorPicker _setPendingLightPicker{ nullptr }; // TABS: pending dots color shown on a DARK tab/card background
        winrt::Windows::UI::Xaml::Controls::Border _pendingLightSwatch{ nullptr }; // TABS: its compact color-preview swatch
        winrt::Microsoft::UI::Xaml::Controls::ColorPicker _setPendingDarkPicker{ nullptr }; // TABS: pending dots color shown on a LIGHT tab/card background
        winrt::Windows::UI::Xaml::Controls::Border _pendingDarkSwatch{ nullptr }; // TABS: its compact color-preview swatch
        // TABS: "Overlay opacity" — the per-tab overlay's REST + HOVER opacities (TAB_OVERLAY.md), on ONE
        // dual-thumb gradient track (transparent left -> solid right). Left dot = rest, right dot = hover;
        // they can't cross (rest <= hover). Drag-driven (CapturePointer + PointerMoved, the resize-grip
        // idiom). Seeded from AppSettings on open; read into tabOverlayRest/HoverOpacity on Save. GLOBAL.
        winrt::Windows::UI::Xaml::Controls::Canvas _overlayOpacityTrack{ nullptr };
        winrt::Windows::UI::Xaml::Shapes::Ellipse _overlayRestThumb{ nullptr }; // left dot — rest opacity (the more-transparent end)
        winrt::Windows::UI::Xaml::Shapes::Ellipse _overlayHoverThumb{ nullptr }; // right dot — hover opacity (the more-solid end)
        winrt::Windows::UI::Xaml::Controls::TextBlock _overlayOpacityLabel{ nullptr }; // live "Rest N%  ·  Hover M%" readout
        double _overlayRestVal{ 0.50 }; // current slider value (0..1) — rest opacity
        double _overlayHoverVal{ 1.0 }; // current slider value (0..1) — hover opacity
        bool _overlayDragRest{ false }; // a drag of the rest dot is in flight
        bool _overlayDragHover{ false }; // a drag of the hover dot is in flight
        double _overlayDragStartX{ 0.0 }; // window-X at PointerPressed (delta-accumulation, the grip idiom)
        double _overlayDragStartVal{ 0.0 }; // the dragged dot's value at PointerPressed
        void _LayoutOverlayOpacitySlider(); // position both dots + refresh the label from _overlayRest/HoverVal (no layout dependency — fixed track width)
        winrt::Windows::UI::Xaml::Controls::TextBlock _setProfileDir{ nullptr }; // the ACTIVE per-install profile dir (read-only; Change… applies on restart)
        winrt::Windows::UI::Xaml::Controls::Button _setResetHidden{ nullptr }; // BEHAVIOR: "Reset hidden sessions" — clears the Sessions browser's "Hide from list" set (fires _resetHiddenSessionsHandler; relabeled per open)
        winrt::Windows::UI::Xaml::Controls::TextBox _setEnv{ nullptr }; // ENV area: GLOBAL multi-line NAME=VALUE editor (one per line)
        // ENV area (ENV_VARS.md): the two-tab "Environment variables" editor. Global vs Per-directory tab
        // buttons swap two panels; each editor is wrapped in a Border whose color = the live lexer status,
        // with a status TextBlock beneath it.
        winrt::Windows::UI::Xaml::Controls::Button _setEnvTabGlobal{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Button _setEnvTabDir{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _envGlobalPanel{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _envDirPanel{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Border _setEnvBorder{ nullptr }; // wraps _setEnv; recolored by the lexer
        winrt::Windows::UI::Xaml::Controls::TextBlock _setEnvStatus{ nullptr }; // global editor bottom status line
        winrt::Windows::UI::Xaml::Controls::TextBox _setEnvDirFilter{ nullptr }; // type-to-filter the dir list
        winrt::Windows::UI::Xaml::Controls::ListBox _setEnvDirList{ nullptr }; // selectable known/working dirs (● = has env)
        winrt::Windows::UI::Xaml::Controls::TextBox _setEnvDir{ nullptr }; // PER-DIR multi-line NAME=VALUE editor (selected dir)
        winrt::Windows::UI::Xaml::Controls::Border _setEnvDirBorder{ nullptr }; // wraps _setEnvDir; recolored by the lexer
        winrt::Windows::UI::Xaml::Controls::TextBlock _setEnvDirStatus{ nullptr }; // per-dir editor bottom status line
        // The in-memory per-directory env draft (NormDirKey -> env-text), seeded from dir-env.json on cog
        // open, edited across dir switches, and flushed on Save (Cancel discards). Mirrors dir-colors'
        // representation (a flat vector of pairs) so no new include is needed.
        std::vector<std::pair<std::wstring, std::wstring>> _dirEnvDraft;
        std::wstring _envEditingDirKey; // NormDirKey of the dir currently loaded into _setEnvDir ("" = none)
        bool _envTabIsDir{ false }; // which tab is showing (false = Global)
        // PRESETS tab state (see _BuildPresetsArea). The draft is the working copy; nothing reaches disk or
        // Credential Manager before Save.
        ::Agentmaster::LaunchPresetsConfig _presetsDraft;
        std::wstring _presetsSelectedId; // the preset the editor shows ("" = none)
        std::map<std::wstring, std::wstring> _presetKeyDraft; // provider id -> a key typed this session (never logged)
        std::unordered_set<std::wstring> _presetKeyDelete; // provider ids whose stored key Save removes
        bool _presetsLoading{ false }; // true while controls are filled from the draft: their change handlers then write nothing
        int _presetsTabIndex{ -1 }; // the tab's index (its placeholders refresh when it is shown: the env editor may have changed)
        winrt::Windows::UI::Xaml::Controls::StackPanel _presetsProvidersHost{ nullptr }; // the provider blocks
        winrt::Windows::UI::Xaml::Controls::ComboBox _presetsPicker{ nullptr }; // which preset the editor shows
        std::vector<std::wstring> _presetsPickerIds; // parallel to _presetsPicker's items
        winrt::Windows::UI::Xaml::Controls::Button _presetsNewBtn{ nullptr }; // "New ▾" (blank / from a template)
        // TEMPLATES, the one-click adds at the top of the tab: one row per shipped template, built once in
        // _BuildPresetsArea; their states follow the draft (_RefreshPresetTemplateRows).
        struct PresetTemplateRow
        {
            std::wstring templateId; // the template the row's button adds
            winrt::Windows::UI::Xaml::Controls::Button add{ nullptr }; // "Add", or a disabled "Added" while a preset links to the template
            winrt::Windows::UI::Xaml::Controls::TextBlock detail{ nullptr }; // model id · tested line (· in your presets)
        };
        std::vector<PresetTemplateRow> _presetTemplateRows;
        winrt::Windows::UI::Xaml::Controls::Button _presetsAddAllBtn{ nullptr }; // "Add all tested templates (N)"; disabled once every tested template is in the list
        winrt::Windows::UI::Xaml::Controls::TextBlock _presetsEmptyLine{ nullptr }; // "No presets yet…" under the picker row, shown only while the list is empty
        winrt::Windows::UI::Xaml::Controls::StackPanel _presetEditor{ nullptr }; // the editor (collapsed when the list is empty)
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _presetShowInMenus{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ComboBox _presetProviderCombo{ nullptr };
        std::vector<std::wstring> _presetProviderIds; // parallel to _presetProviderCombo: "" = the template's, else a provider id / kAnthropicProviderId
        std::map<std::wstring, winrt::Windows::UI::Xaml::Controls::TextBox> _presetFieldBoxes; // field key -> its box
        winrt::Windows::UI::Xaml::Controls::TextBlock _presetTestedLine{ nullptr }; // "Tested …" / "Not tested yet" / "Your own preset"
        winrt::Windows::UI::Xaml::Controls::TextBlock _presetPreview{ nullptr }; // the compiled launch, key masked
        winrt::Windows::UI::Xaml::Controls::TextBlock _setClaudeDetected{ nullptr }; // Agentmaster: the AUTO-DETECTED native claude.exe (read-only; "Not detected" when none)
        winrt::Windows::UI::Xaml::Controls::TextBox _setClaudeExePath{ nullptr }; // Agentmaster: explicit claude.exe override (blank = auto-detect; must be an .exe)
        winrt::Windows::UI::Xaml::Controls::Button _setClaudeInstallBtn{ nullptr }; // Agentmaster: one-click install/migrate — shown only when no native claude.exe is detected (label from ClaudeInstallKind())
        winrt::Windows::UI::Xaml::Controls::TextBox _setCleanupDays{ nullptr }; // ENV_VARS.md §8: cleanupPeriodDays in the user's GLOBAL ~/.claude/settings.json (history retention; read/written via the ClaudeUserSettings repo, NOT AppSettings)
        // ---- UPDATES (Agentmaster updater; Updater.h) ----
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setAllowPrerelease{ nullptr }; // include GitHub pre-releases in the update check (default OFF; INSTANT-APPLY — the switch RMWs settings.json on flip, no Save needed)
        bool _seedingAllowPrerelease{ false }; // latch: a cog-open programmatic IsOn seed must not re-fire the switch's instant-apply RMW
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setAllowNightly{ nullptr }; // include NIGHTLY builds (tag contains "nightly" — unstable dev versions, a tier below pre-release); turning ON is gated behind a warning confirm, then the same INSTANT-APPLY RMW
        bool _seedingAllowNightly{ false }; // latch: programmatic IsOn writes (cog-open seed / the handler's revert-then-ask / the confirm's re-apply) must not re-fire the Toggled handler
        winrt::Windows::UI::Xaml::Controls::Button _setCheckUpdates{ nullptr }; // "Check for updates" -> the same prompt the startup check shows
        winrt::Windows::UI::Xaml::Controls::TextBlock _setUpdateStatus{ nullptr }; // status label ("vX.Y.Z available!" dark green / "up to date" / "Checking…")
        bool _interactiveUpdateInFlight{ false }; // guard so a double-click of "Check for updates" can't fire two prompts
        winrt::Windows::UI::Xaml::Controls::HyperlinkButton _setCurrentChangelog{ nullptr }; // opens THIS build's release page (github .../releases/tag/v<current>)
        winrt::Windows::UI::Xaml::Controls::HyperlinkButton _setUpdateChangelog{ nullptr }; // opens the AVAILABLE update's release page; shown only after a check found one
        std::wstring _lastUpdateChangelogUrl; // the available update's release page (drives _setUpdateChangelog's click)
        winrt::Windows::UI::Xaml::Controls::Button _setUninstallBtn{ nullptr }; // Agentmaster (updater): "Uninstall Agentmaster…" -> remove THIS install (per-user; profile data kept), then quit. Shown only for packaged installs.
        // ---- DEVELOPER (Agentmaster debug escape hatch; ProfileBootstrap.h IsDebugPackage) ----
        winrt::Windows::UI::Xaml::Controls::ToggleSwitch _setDebugMode{ nullptr }; // About-tab "Enable Debug Mode" — the durable --debug twin (makes a Release build answer Profiles::IsDevOrDebugPackage() true, the gate for debug-only features; NO feature reads it since 2026-10-09, when the Prompt Queue Runner left it — kept for later ones; applies after restart). FORM-owned (AppSettings.debugMode); forced ON+disabled on a Dev build.
        winrt::Windows::UI::Xaml::Controls::TextBlock _setDebugModeNote{ nullptr }; // caption under the toggle — "Applies after restart." / dev-build / active-now state
        winrt::Windows::UI::Xaml::Controls::TextBox _templateNameBox{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ComboBox _templateCombo{ nullptr };
        std::vector<::Agentmaster::PlanTemplate> _templates;

        // ---- Resizable splitters (persisted geometry) ----
        // The definitions each splitter resizes + the panes they bound (read for live size).
        ::Agentmaster::ManagerLayout _layout;
        winrt::Windows::UI::Xaml::Controls::RowDefinition _boardRow{ nullptr };
        winrt::Windows::UI::Xaml::Controls::RowDefinition _bottomRow{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ColumnDefinition _treeCol{ nullptr };
        winrt::Windows::UI::Xaml::Controls::ColumnDefinition _planCol{ nullptr };
        // Drag state. One splitter drags at a time; sizes are pinned at PointerPressed so the
        // boundary tracks the cursor 1:1 (no feedback loop from the live re-layout).
        enum class DragKind
        {
            None,
            Rows, // dragging the horizontal bar (Board vs Bottom)
            Cols // dragging the vertical bar (Tree vs Auto Testing)
        };
        DragKind _dragKind{ DragKind::None };
        double _dragOrigin{ 0 }; // root-relative pointer coord on the drag axis at press
        double _dragSizeA{ 0 }; // first track's px size at drag start
        double _dragSizeB{ 0 }; // second track's px size at drag start

        // ---- Explorer-tree interactions ----
        // Button swallows DoubleTapped, so we synthesize a double-click by timing successive
        // clicks on the same row (GetDoubleClickTime threshold) -> Activate; single -> select.
        std::wstring _lastTreeClickId;
        unsigned long long _lastTreeClickTick{ 0 };
        // Triage Board cards use the same click-timing trick (a Button swallows DoubleTapped):
        // double-click a card -> Activate (jump to the live terminal tab); single -> select.
        std::wstring _lastCardClickId;
        unsigned long long _lastCardClickTick{ 0 };
        // In-place rename: a TextBox swapped into the row being renamed. A ContentDialog can't
        // host a text box in XAML Islands (it receives no keypresses), so we edit inline like
        // the tab renamer; while the editor is live the tree skips rebuilds to keep focus+text.
        std::wstring _renamingId;
        winrt::Windows::UI::Xaml::Controls::TextBox _renameBox{ nullptr };
        // Agentmaster: like TabHeaderControl, the inline rename box is multi-line; a commit combo
        // (Enter / Shift+Enter per AppSettings::tabRenameCommitMode) is flagged in PreviewKeyDown
        // (which suppresses the newline) and committed on the matching KeyUp — Rule #11 keeps this
        // tree-rename path behaving like the WT tab-rename box.
        bool _renameCommitOnKeyUp{ false };
    };
}
