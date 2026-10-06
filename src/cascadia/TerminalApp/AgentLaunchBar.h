// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the LAUNCH BAR: the agent toggle, the working-directory box (its validation underline and its
// path-picker drop-down) and the Launch / Fork buttons. ONE implementation for every surface that starts a session
// from a typed folder or a typed session id.
//
// WHAT: the row that reads "launch a [● Claude] session in [ box ] [Launch Claude] [Fork]" on the Agent Manager page.
// Its whole behavior lived inside AgentManagerContent (AgentManagerContent.Launch.cpp, plus the toolbar and drop-down
// builds in _BuildLayout). It is moved here VERBATIM so a second host, the side panel's "+" launcher card
// (TerminalPage.AgentSidePanel.cpp, SIDE_PANEL.md §2e), runs the exact same code: the same box validation (green =
// a session id found on disk, amber = a folder Launch will create, red = Launch disabled), the same drop-down
// (Browse…, RECENT / RECENT MATCHES, GIT WORKTREES, MATCHES / SUBFOLDERS), the same keys (Esc closes the drop-down,
// Enter commits without launching), the same Launch hover menu (models, Specify…, presets), the same launch, resume,
// fork and "Create & Launch" paths.
//
// THE TWO HOSTS (AgentLaunchBarHost):
//   - the Agent Manager page: the Claude <-> Codex toggle, the drop-down parented into the page's own root, its
//     "Claude not detected" modal, its staged toolbar reflow on every width change (layoutChanged), and the recent
//     folders loaded once per window (AgentManagerContent::LaunchRecentDirs hands the SAME list to the side panel);
//   - the side panel's "+" card: Claude only (a fixed "● Claude" chip — Codex launches from there are planned), the
//     drop-down parented into the page Root(), the page's own "Claude not detected" dialog, and three extra hooks the
//     Manager does not need: launched (the card closes after a launch), textEdited (the card's box mirrors the
//     Manager's box) and the dialog anchor (the card, so a "Couldn't create folder" dialog is dark).
//
// THE RULES THIS CLASS KEEPS (each one has a comment at its code):
//   - The drop-down opens only on a USER focus (Pointer / Keyboard), a tap or a keystroke — never on a programmatic
//     focus — and an Esc / Enter / blur dismissal sticks until the box is refocused or tapped.
//   - The drop-down rows never take focus (IsTabStop false, AllowFocusOnInteraction false): a row that took focus
//     would let the box's LostFocus tear the drop-down down before the click lands.
//   - A programmatic Text write re-validates EXPLICITLY: TextChanged does not fire reliably while the box is off the
//     live tree (a hidden Manager page, a closed card), so the underline would stay on the previous text's state.
//   - The drop-down's geometry is never computed inside a layout pass: a resize goes through QueuePathPickerRelayout
//     (one coalesced dispatcher tick), because mutating a parented Popup mid-pass is the E_LAYOUTCYCLE fail-fast.
//   - Every caught exception is logged (Rule #18) with the context of the step that failed.
//
// OWNERSHIP: a host keeps the bar in a std::shared_ptr for its own lifetime (the Manager content, the page). The
// bar owns its XAML elements; every handler it wires on them captures a weak_ptr to the bar, so an element that
// outlives the bar (the host still holds it in its tree) runs handlers that do nothing. The host callbacks the
// bar calls capture the host WEAKLY (a winrt weak_ref), so a bar never keeps its host alive. The destructor only
// runs the noexcept, guarded Shutdown (a page teardown can run on a pool thread — the ~TerminalPage cascade rule).
//
// WinRT XAML types, so this lives in TerminalApp, not the plain-C++ AgentMaster/ engine directory. Include from TUs
// that include pch.h first (the projections come from the pch, like AgentSessionToolTip.h).

#pragma once

#include <cstdint> // uint32_t — the recent-folders limit getter
#include <functional> // the host callbacks
#include <memory> // std::shared_ptr / enable_shared_from_this — stable identity for weak-capturing handlers
#include <string> // paths, ids, titles
#include <vector> // the recent-folders list

namespace Agentmaster
{
    class SessionRegistry; // the shared fleet (session id -> working dir + title); complete in the .cpp
}

namespace winrt::TerminalApp::implementation
{
    // The recent-folders list (recent-dirs.json) of ONE window, newest first. Shared by every launch bar of the
    // window through a std::shared_ptr, so a folder launched from the side panel's card is at the top of the Agent
    // Manager's drop-down at once, and the other way round: one list, so the two bars can never save over each
    // other's additions. Loaded by the Agent Manager content at construction (LoadRecentDirs) and written back WHOLE
    // after every push (SaveRecentDirs). ⚠ Other WINDOWS keep their own copy, exactly as before this was shared:
    // the last window to push writes the file.
    struct AgentLaunchRecentDirs
    {
        std::vector<std::wstring> dirs; // newest first; capped at AppSettings::recentDirsLimit on every push
    };

    // Everything a launch bar needs from the surface that hosts it. The callbacks run on the UI thread, from a click,
    // a key-up or a hover-menu pick; each may be empty, which makes its action a no-op (the bar never throws for a
    // missing callback). Every callback must capture its host weakly (see OWNERSHIP in the file header).
    struct AgentLaunchBarHost
    {
        // True: the agent element is the Claude <-> Codex toggle BUTTON (the Agent Manager). False: a fixed
        // "● Claude" chip, not clickable, with a tooltip that says Codex launches from this surface are planned
        // (the side panel's card). With false the bar never enters Codex mode, so spawnCodex is never called.
        bool codexToggle{ true };
        // The window's shared recent-folders list. Null is allowed: the bar then makes its own empty list (nothing is
        // offered as RECENT until the first launch, and each push still saves the file).
        std::shared_ptr<AgentLaunchRecentDirs> recentDirs;
        // The panel the drop-down Popup is appended to, top/left aligned in grid row 0. Its coordinate space is the one
        // the drop-down's offsets are computed in, and its width is the "window" of the drop-down's width rule (out to
        // 90% of it). The Agent Manager passes its page root; the side panel passes the page Root().
        winrt::Windows::UI::Xaml::Controls::Grid popupHost{ nullptr };
        // How many grid rows the drop-down Popup spans in popupHost: 1 for the Agent Manager (its row 0 is the
        // toolbar), 3 for the page Root() (the whole grid, the tag editor's recipe). Any positive value works: the
        // Popup's offsets are measured from the top-left of its slot, which is popupHost's top-left either way.
        int32_t popupRowSpan{ 1 };
        // The element whose XamlRoot and ActualTheme the "Couldn't create folder" dialog takes. Must be in a forced-Dark
        // subtree so the dialog is dark: the Agent Manager's root, or the side panel's card.
        winrt::Windows::UI::Xaml::FrameworkElement dialogAnchor{ nullptr };
        // The UI thread's dispatcher (the Browse dialog's defer, the box's LostFocus check, the drop-down re-fit).
        winrt::Windows::System::DispatcherQueue dispatcher{ nullptr };
        // The shared session registry, read at call time (the Agent Manager receives its registry after the bar is
        // built). Null answers: a session id resolves through its transcript only, and RECENT lists no live folders.
        std::function<std::shared_ptr<::Agentmaster::SessionRegistry>()> registry;
        // AppSettings::recentDirsLimit, read at call time (0 / unset => 10, the shipped default).
        std::function<uint32_t()> recentDirsLimit;
        // AppSettings::launchModels ("Display name | model-id" lines), read when the Launch hover menu opens.
        std::function<std::wstring()> launchModels;
        // Start a NEW Claude session in `dir`. `target`: "" = Default (a plain click), else a model id or "@<presetId>"
        // from the Launch hover menu. The folder already exists (a "Create & Launch" created it) and is in the recent
        // list when this runs.
        std::function<void(const std::wstring& dir, const std::wstring& target)> spawnClaude;
        // Resume the FOUND session `id` (its transcript exists) in `dir`, titled `title` ("" = derive one).
        std::function<void(const std::wstring& id, const std::wstring& dir, const std::wstring& title)> resumeSession;
        // Fork the FOUND session `id` into a new conversation in `dir`, titled from `title`.
        std::function<void(const std::wstring& id, const std::wstring& dir, const std::wstring& title)> forkSession;
        // Start a NEW managed Codex session in `dir` (codexToggle hosts only).
        std::function<void(const std::wstring& dir)> spawnCodex;
        // No native claude.exe was found: show the host's "Claude not detected" notice. Nothing launches.
        std::function<void()> claudeMissing;
        // The "Specify…" opener the Launch hover menu hands to AgentFillModelPickItems (AgentModelMenu.h). Empty =
        // the menu has no "Specify…" item.
        std::function<void(std::function<void(winrt::hstring)>)> specifyModel;
        // PRESETS: show the host's "Add presets" dialog (where presets are added). The bar hands it to the hover menu
        // only while the user has no preset at all — it asks ::Agentmaster::ShouldOfferAddPresets() each time the menu
        // opens — so the "Add presets…" item takes the presets' place exactly then. Empty = never such an item.
        std::function<void()> addPresets;
        // The bar's own width just changed (the agent toggle's label, the Launch label, Fork shown or hidden). The
        // Agent Manager re-runs its staged toolbar reflow here; the side panel does not need it (its card re-places
        // itself on its own SizeChanged).
        std::function<void()> layoutChanged;
        // A launch, resume or fork was handed to its callback above, and the bar finished its own follow-up (a
        // resumed / forked id is cleared from the box). The side panel closes its card and re-syncs the box text.
        std::function<void()> launched;
        // The box text changed (typing, a drop-down pick, Browse, a programmatic set). The side panel mirrors it into
        // the Agent Manager's box, so the two boxes show the same text.
        std::function<void(const std::wstring& text)> textEdited;
    };

    // ONE launch bar: build it with Create, put its parts into the host's layout (AgentElement, CwdColumn, LaunchButton,
    // ForkButton — in that order on the Agent Manager page), and call Shutdown on the UI thread when the host goes away.
    // UI thread only: every member reads or writes XAML objects of the host's thread.
    class AgentLaunchBar final : public std::enable_shared_from_this<AgentLaunchBar>
    {
    public:
        // Build a bar and all its XAML: the agent element, the box (seeded with %USERPROFILE%) with its underline, the
        // Launch and Fork buttons, the Launch hover-menu timer, and the drop-down Popup (appended to host.popupHost now).
        //   host    — what the bar calls and where it parents its drop-down (moved in).
        //   returns — the new bar (never null).
        //   throws  — std::bad_alloc on allocation failure, or winrt::hresult_error when XAML refuses an element (the
        //             host's build is on the UI thread; the Agent Manager's constructor and the side panel's first "+"
        //             click both let it propagate to their own containment).
        static std::shared_ptr<AgentLaunchBar> Create(AgentLaunchBarHost host);

        // Runs Shutdown (noexcept). Releases the XAML references; nothing here can throw.
        ~AgentLaunchBar();

        AgentLaunchBar(const AgentLaunchBar&) = delete; // one bar per set of controls: a copy would share them
        AgentLaunchBar& operator=(const AgentLaunchBar&) = delete;

        // The agent element to place before the box: the toggle Button (codexToggle) or the fixed chip Border.
        // Never null after Create.
        winrt::Windows::UI::Xaml::FrameworkElement AgentElement() const noexcept;
        // The Claude <-> Codex toggle button, or null on a codexToggle=false bar (the Agent Manager's staged reflow
        // measures it as part of the row).
        winrt::Windows::UI::Xaml::Controls::Button AgentToggleButton() const noexcept { return _launchAgentBtn; }
        // The working-directory box itself — the Agent Manager keeps it as _cwdBox (its reflow sizes it, the selection
        // pre-aims it). Write its Text through SetBoxText, or re-validate after a direct write (see the rules above).
        winrt::Windows::UI::Xaml::Controls::TextBox CwdBox() const noexcept { return _cwdBox; }
        // The box + its 2-px validation underline, stacked: THE element to place in the host's row.
        winrt::Windows::UI::Xaml::Controls::StackPanel CwdColumn() const noexcept { return _cwdCol; }
        // "Launch Claude" / "Create & Launch Claude" / "Resume session" (and the Codex twins); disabled on a red box.
        winrt::Windows::UI::Xaml::Controls::Button LaunchButton() const noexcept { return _launchBtn; }
        // "Fork" — visible only while the box holds a FOUND session id.
        winrt::Windows::UI::Xaml::Controls::Button ForkButton() const noexcept { return _forkBtn; }
        // The drop-down's panel (the Popup's child): a host that closes on an outside press treats a press under it as
        // inside (the side panel's card).
        winrt::Windows::UI::Xaml::Controls::Border PathPanel() const noexcept { return _pathPanelBorder; }

        // Paint the box's underline and set the Launch / Fork buttons for the box's current text (the rules in
        // _ValidateLaunchBox). Call after any programmatic Text write. Throws what a XAML property write throws on
        // the UI thread (never in practice; the hosts' callers are contained).
        void ValidateLaunchBox();
        // Repaint the agent element (● Claude / ● Codex). Throws as ValidateLaunchBox.
        void UpdateLaunchAgentButton();
        // Exactly what a click on Launch does: stop the hover-menu timer (a click is a decision), then launch with the
        // Default target. The side panel's key-up Enter on a focused Launch button calls it. Throws what the host
        // callbacks throw (a click's own exposure).
        void ClickLaunch();
        // Exactly what a click on Fork does. Throws what the host callbacks throw.
        void ClickFork();
        // Close the drop-down (no dismissal latch: the next keystroke reopens it). Never throws in practice.
        void ClosePathPicker();
        // True while the drop-down is open. Never throws (a failed read answers false).
        bool PathPickerOpen() const noexcept;
        // Re-fit an OPEN drop-down to a resized window on the next dispatcher tick (coalesced). Call from a
        // SizeChanged handler; never re-fits synchronously. Never throws (logged, the latch re-armed).
        void QueuePathPickerRelayout();
        // The box is about to leave the screen (the side panel's card closes): close the drop-down, latch the dismissal
        // and normalize the text — what the box's own blur does one tick later, done now so the caller reads the
        // normalized text at once. Never throws (logged).
        void DismissForHide() noexcept;
        // The box's current text, untrimmed. Empty when the box is not built. Never throws in practice.
        std::wstring BoxText() const;
        // Replace the box's text when it differs, put the caret at the end, and re-validate explicitly (the box may be
        // off the live tree, where TextChanged is unreliable). A no-op for identical text.
        //   text   — the new text (a folder or a session id; not normalized here).
        //   throws — what a XAML property write throws on the UI thread (the hosts' callers are contained).
        void SetBoxText(const std::wstring& text);
        // Stop the Launch hover-menu timer (UI-thread-affine: guarded, a pool-thread teardown throws
        // RPC_E_WRONG_THREAD there) and close an open drop-down. Safe to call twice and from the destructor.
        void Shutdown() noexcept;

        // Add the Agent Manager's six Button STATE brushes to `resources` — ButtonBackground / *PointerOver / *Pressed
        // and ButtonForeground / *PointerOver / *Pressed, the values AgentManagerContent seeds on its root — each one
        // only when the dictionary does not hold that key yet (a host's own seed wins). Why: under the forced-Dark island
        // theme the default {ThemeResource ButtonBackground*} brushes resolve to null, and a Button with a null
        // background is hit-testable only on its text and shows no hover. The bar seeds its drop-down panel with it (a
        // host may parent the drop-down outside any seeded subtree — the page Root()), and the side panel seeds its
        // card, so Launch, Fork and the drop-down rows look and click exactly as on the Agent Manager page.
        //   resources - the dictionary to fill (an element's Resources()).
        //   throws    - winrt::hresult_error when XAML refuses the dictionary write (never in practice; the callers'
        //               builds are contained).
        static void SeedButtonStateBrushes(const winrt::Windows::UI::Xaml::ResourceDictionary& resources);

    private:
        // Use Create: the bar must live in a std::shared_ptr before any handler can weak-capture it.
        explicit AgentLaunchBar(AgentLaunchBarHost host);

        // Build every XAML element and wire its handlers (called once, by Create, after the shared_ptr exists).
        // Throws what XAML element creation throws (Create's contract).
        void _Build();

        // --- The agent element + the launch actions (moved from AgentManagerContent.Launch.cpp) ---
        void _UpdateLaunchAgentButton();
        // PRESETS (HARNESSES.md §8.2): the Launch button's hover menu — exactly the "Open New Session Here" picker
        // (Default, Specify…, the launch models, then the visible presets), opened after a short hover
        // (_launchHoverTimer) so a pointer merely crossing the bar opens nothing. Offered only while the button
        // launches a NEW Claude session (not in Codex mode, not when the box holds a session id, not while the button
        // is disabled). A pick launches through _OnLaunch(target). Never throws (a failed open is logged).
        void _ShowLaunchMenu();
        // The Launch button's action. `target` (PRESETS): the hover menu's pick — "" = Default (a plain click), a model
        // id, or "@<presetId>" — handed to spawnClaude for a NEW session in the typed folder. A session id in the box
        // resumes that session on its own snapshot and ignores `target`. Throws what the host callbacks throw.
        //   target - the launch target ("" for a plain click).
        void _OnLaunch(const std::wstring& target = {});
        // "Create & Launch": ensure the typed working dir exists, creating it (and any missing parents) when it is a
        // not-yet-existing absolute path. Returns false (and shows a buttons-only error dialog) when the create failed,
        // so the caller aborts the launch. An existing dir is a no-op pass-through. Never throws (filesystem errors
        // arrive as an error_code; the dialog's failures are logged).
        //   dir     - the typed working dir (normalized here).
        //   returns - true when the dir exists now and the launch may proceed.
        bool _EnsureLaunchDirExists(const std::wstring& dir);
        // The Fork button (visible for a found session id) -> host.forkSession. Throws what the callback throws.
        void _OnForkFromBox();
        // Validate the box's content as a working dir OR a session id, painting the underline (green = a FOUND session
        // id; red = an unknown id or a dir that can't be used; amber = a dir Launch will create) and enabling the
        // Launch button (disabled on red) + the Fork button (shown only for a found session id).
        void _ValidateLaunchBox();
        // Resolve a session id to its (working dir, title): the registry first, the transcript's own cwd as the
        // fallback. Returns false when no dir could be found (the caller does nothing). Never throws in practice.
        //   id    - the bare session id.
        //   dir   - receives the working dir ("" on failure).
        //   title - receives the registry title ("" = derive one from the dir).
        bool _ResolveSessionDirTitle(const std::wstring& id, std::wstring& dir, std::wstring& title);
        // Move `dir` to the front of the shared recent-folders list (deduped with PathEq), cap it, and save the file.
        //   dir - the folder just launched / browsed to; "" is ignored.
        void _PushRecentDir(const std::wstring& dir);
        // Recent/known working dirs for the drop-down. `current` is the box text (excluded from the result). A
        // non-empty `query` (a bare token typed with no root path) switches the result from plain MRU order to
        // case-insensitive fuzzy matches ranked by Levenshtein closeness (closest first); an empty query keeps the
        // historical MRU order. Returns at most recentDirsLimit folders.
        std::vector<std::wstring> _CollectRecentDirs(const std::wstring& current, const std::wstring& query) const;

        // --- The path-picker drop-down (moved from AgentManagerContent.Launch.cpp) ---
        // A transparent, focus-neutral row: a click drills the box into `fullPath`. `branch` (RECENT rows) appends
        // "— <branch>". Throws what XAML element creation throws.
        winrt::Windows::UI::Xaml::Controls::Button _MakePathRow(const std::wstring& fullPath, const winrt::hstring& glyph, const winrt::hstring& displayText, const std::wstring& branch = {});
        // A "GIT WORKTREES" row — "<name> — <path> — <branch>". Mirrors _MakePathRow (transparent + focus-neutral);
        // a click drills the box into the worktree. Throws what XAML element creation throws.
        winrt::Windows::UI::Xaml::Controls::Button _MakeWorktreeRow(const std::wstring& fullPath, const std::wstring& name, const std::wstring& branch, bool isCurrent);
        // The "Browse…" row pinned to the TOP of the drop-down (the first option) — a native folder dialog whose pick
        // both fills the box AND joins the recents (_BrowseForLaunchDir). Throws what XAML element creation throws.
        winrt::Windows::UI::Xaml::Controls::Button _MakeBrowseRow();
        // Open the native folder dialog (deferred off the click tick) and drop the pick into the box + the recents.
        void _BrowseForLaunchDir();
        // Re-list the drop-down for the box's current text (Browse…, RECENT / RECENT MATCHES, GIT WORKTREES,
        // MATCHES / SUBFOLDERS, the empty-state hint). Throws what XAML element creation throws.
        void _RebuildPathPicker();
        // Clear the dismissal latch, re-fit, re-list and open the drop-down.
        void _OpenPathPicker();
        // Anchor the drop-down under the box and size it against the CURRENT window — the width rule (from the box's
        // left edge out to 90% of the host's width, 560-DIP floor) plus the HEIGHT CAP: the list grows with its rows up
        // to 90% of the window's height, but never past the window's bottom edge (less an 8-DIP gap). Why both height
        // limits: the list starts below the box, so on a short window a full 90% would run off the bottom, and an
        // overflowing popup is CLIPPED — its last rows AND the lower end of its scrollbar land outside the window, where
        // they can't be scrolled into view at all. So the bottom-edge clamp wins on a short window; a 120-DIP floor
        // (~Browse… + a header + two rows) only overrides it on a window too short to fit even that. "Window" = the
        // XAML island (XamlRoot().Size() — it fills the client area, tab row included), measured in the island
        // coordinates TransformToVisual(nullptr) yields. Never throws: the anchor, width and height steps are guarded
        // one by one and log via AgentLogCaughtException (Rule #18), each keeping its previous value. ⚠ It reads LIVE
        // layout: never call it synchronously from a SizeChanged / LayoutUpdated handler (mid-pass geometry is stale,
        // and mutating a parented Popup inside a layout pass is the E_LAYOUTCYCLE fail-fast the tab-strip tag badges
        // hit) — go through _QueuePathPickerRelayout instead.
        void _LayoutPathPicker();
        // Re-fit the OPEN drop-down to a resized window on the next dispatcher tick (via _LayoutPathPicker). A no-op
        // while the list is closed (every open re-fits anyway) or while a re-fit is already queued, so a drag-resize's
        // burst of SizeChanged events costs at most one re-fit per tick. Never throws: a failed enqueue (the
        // dispatcher shutting down with the window) is logged and RE-ARMS the queued latch — a latch left stuck would
        // silently stop the open list from ever following the window size again.
        void _QueuePathPickerRelayout();
        void _ClosePathPicker(); // close the drop-down Popup (no latch change)
        void _PickPath(const std::wstring& dir); // a row click: drive the box to `dir` (drilling into a folder) and keep it focused
        void _NormalizeCwdBox(); // platform-sensitive NormPath of the box (on commit / blur / pick / launch)

        // --- Host access (each one null-safe, so a missing callback is a no-op) ---
        // The host's registry, or null. Never throws (a throwing getter is logged and answers null).
        std::shared_ptr<::Agentmaster::SessionRegistry> _Registry() const noexcept;
        // AppSettings::recentDirsLimit through the host (0 when unknown — every caller maps 0 to 10). Never throws.
        uint32_t _RecentDirsLimitSetting() const noexcept;
        // AppSettings::launchModels through the host ("" when unknown). Never throws (logged, "").
        std::wstring _LaunchModelsSetting() const noexcept;
        // Show the host's "Claude not detected" notice. Never throws (a host failure is logged).
        void _ClaudeMissing() noexcept;
        // Tell the host the bar's width may have changed (host.layoutChanged). Never throws (logged): it also runs
        // from the noexcept scope guard at the end of _ValidateLaunchBox.
        void _NotifyLayoutChanged() noexcept;
        // Tell the host a launch / resume / fork was handed off (host.launched). Never throws (logged).
        void _NotifyLaunched() noexcept;
        // Tell the host the box text changed (host.textEdited). Never throws (logged).
        void _NotifyTextEdited() noexcept;

        AgentLaunchBarHost _host; // the surface's callbacks and anchors (see AgentLaunchBarHost)
        std::shared_ptr<AgentLaunchRecentDirs> _recentDirs; // MRU of launched working dirs (persisted) — the host's shared list, or a private one
        winrt::Windows::System::DispatcherQueue _dispatcher{ nullptr }; // host.dispatcher, or the creating thread's

        // The Claude<->Codex agent toggle before the box (codexToggle hosts; null otherwise).
        winrt::Windows::UI::Xaml::Controls::Button _launchAgentBtn{ nullptr };
        // The fixed "● Claude" chip (codexToggle=false hosts; null otherwise).
        winrt::Windows::UI::Xaml::Controls::Border _agentChip{ nullptr };
        // false = launch a Claude (default, unchanged); true = launch a managed Codex in the typed dir. Only the
        // toggle flips it, so a chip bar stays false for its whole life.
        bool _launchCodex{ false };
        winrt::Windows::UI::Xaml::Controls::TextBox _cwdBox{ nullptr }; // the working dir / session id box
        winrt::Windows::UI::Xaml::Controls::StackPanel _cwdCol{ nullptr }; // [box / underline], the element a host places
        winrt::Windows::UI::Xaml::Controls::Border _cwdUnderline{ nullptr }; // validation underline (green=found session id, red=missing dir / unknown id, amber=will create, transparent=neutral)
        winrt::Windows::UI::Xaml::Controls::Button _launchBtn{ nullptr }; // "Launch Claude" (dir) / "Resume session" (a found session id); disabled on a red box
        winrt::Windows::UI::Xaml::Controls::Button _forkBtn{ nullptr }; // "Fork" — visible only when the box holds a FOUND session id
        // PRESETS: the hover-intent delay before the Launch button's menu opens (started on PointerEntered, stopped on
        // PointerExited — a pass over the button never opens it) and the menu currently shown (non-null while open, so a
        // second hover does not stack a second menu).
        winrt::Windows::UI::Xaml::DispatcherTimer _launchHoverTimer{ nullptr };
        winrt::Windows::UI::Xaml::Controls::MenuFlyout _launchMenu{ nullptr };
        winrt::Windows::UI::Xaml::Controls::Primitives::Popup _pathPopup{ nullptr }; // the drop-down Popup (in host.popupHost)
        winrt::Windows::UI::Xaml::Controls::Border _pathPanelBorder{ nullptr }; // the drop-down's dark panel (the Popup's child)
        // The drop-down list's scroller. Its MaxHeight IS the list's height cap (the Border around it adds only fixed
        // padding + a 1-DIP edge): seeded 380 at build, then re-capped by _LayoutPathPicker at every open and window
        // resize (up to 90% of the window height). Write the cap ONLY there — a fixed value set anywhere else would
        // quietly win until the next open.
        winrt::Windows::UI::Xaml::Controls::ScrollViewer _pathScroll{ nullptr };
        winrt::Windows::UI::Xaml::Controls::StackPanel _pathListHost{ nullptr }; // the drop-down's rows
        bool _pathPickerUserDismissed{ false }; // Esc/Enter/blur dismiss the picker; (re)focusing/tapping the box clears it
        // True while a deferred window-resize re-fit of the open drop-down is queued (_QueuePathPickerRelayout) —
        // coalesces a drag-resize's SizeChanged burst to one re-fit per tick. Cleared by the queued tick BEFORE it
        // re-fits (so a resize landing after its reads still queues another) and on a failed enqueue; if it ever stuck
        // true, the open list would stop following the window size.
        bool _pathPickerRelayoutQueued{ false };
        bool _shutDown{ false }; // Shutdown ran: a second call (the destructor's) does nothing
    };
}
