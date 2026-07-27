// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Modifications (c) 2026 Eli Belash (Agentmaster), licensed under AGPL-3.0-or-later. See LICENSE.

#pragma once
#include "Pane.h"
#include "ColorPickupFlyout.h"
#include "Tab.h"
#include "Tab.g.h"

// fwdecl unittest classes
namespace TerminalAppLocalTests
{
    class TabTests;
};

namespace winrt::TerminalApp::implementation
{
    struct Tab : TabT<Tab>
    {
    public:
        Tab(std::shared_ptr<Pane> rootPane);

        // Called after construction to perform the necessary setup, which relies on weak_ptr
        void Initialize();

        winrt::Microsoft::Terminal::Control::TermControl GetActiveTerminalControl() const;
        winrt::Microsoft::Terminal::Settings::Model::Profile GetFocusedProfile() const noexcept;
        winrt::TerminalApp::IPaneContent GetActiveContent() const;

        void Focus(winrt::Windows::UI::Xaml::FocusState focusState);

        void Scroll(const int delta);

        std::shared_ptr<Pane> DetachRoot();
        std::shared_ptr<Pane> DetachPane();
        void AttachPane(std::shared_ptr<Pane> pane);

        void AttachColorPicker(winrt::TerminalApp::ColorPickupFlyout& colorPicker);

        std::pair<std::shared_ptr<Pane>, std::shared_ptr<Pane>> SplitPane(winrt::Microsoft::Terminal::Settings::Model::SplitDirection splitType,
                                                                          const float splitSize,
                                                                          std::shared_ptr<Pane> newPane);

        void ToggleSplitOrientation();
        void UpdateIcon(const winrt::hstring& iconPath, const winrt::Microsoft::Terminal::Settings::Model::IconStyle iconStyle);
        void HideIcon(const bool hide);

        void ShowBellIndicator(const bool show);
        void ActivateBellIndicatorTimer();

        float CalcSnappedDimension(const bool widthOrHeight, const float dimension) const;
        std::optional<winrt::Microsoft::Terminal::Settings::Model::SplitDirection> PreCalculateCanSplit(winrt::Microsoft::Terminal::Settings::Model::SplitDirection splitType,
                                                                                                        const float splitSize,
                                                                                                        winrt::Windows::Foundation::Size availableSpace) const;

        bool ResizePane(const winrt::Microsoft::Terminal::Settings::Model::ResizeDirection& direction);
        bool NavigateFocus(const winrt::Microsoft::Terminal::Settings::Model::FocusDirection& direction);
        bool SwapPane(const winrt::Microsoft::Terminal::Settings::Model::FocusDirection& direction);
        bool FocusPane(const uint32_t id);

        void UpdateSettings(const winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings& settings);
        void UpdateTitle();

        void Close();
        void Shutdown();
        void ClosePane();

        void SetTabText(winrt::hstring title);
        winrt::hstring GetTabText() const;
        void ResetTabText();
        void ActivateTabRenamer();

        // Agentmaster (tab tooltip): a rich, session-aware hover tooltip pushed by TerminalPage — its
        // registry observer (a managed Claude/Codex session) or its activity probe (an unmanaged tab).
        // TerminalPage builds the WHOLE tooltip body as a XAML element (a dark, summary-style card — see
        // TerminalPage::_UpdateTabAgentToolTip) since it owns the SessionInfo + registry; the Tab just
        // HOSTS it. While set it REPLACES the default title+keychord tooltip. `signature` is a cheap
        // content fingerprint: an identical push is a no-op (no re-host), so the per-change + per-tick
        // callers stay cheap. `swapWhileOpen` is a ONE-SHOT escape from the "Content only while closed"
        // rule for the async summary-body arrival — the one moment the hovering user WANTS the open card
        // to update (a single swap; the flicker rule targeted the old per-tick ago-churn, not this).
        // ClearAgentToolTip reverts to the default tooltip. UI thread only.
        void SetAgentToolTip(winrt::Windows::UI::Xaml::UIElement content, winrt::hstring signature, bool swapWhileOpen = false);
        void ClearAgentToolTip();
        winrt::hstring AgentToolTipSig() const noexcept { return _agentToolTipSig; } // Agentmaster (lazy tooltip): the hosted content's fingerprint — lets a per-tick producer SKIP building a card that would only be sig-discarded
        bool AgentToolTipAttached() const noexcept { return _agentToolTip != nullptr; } // Agentmaster: is a ToolTip currently ATTACHED to this tab? An owner recycle detaches + nulls it (crash #4), and a tooltip that is not attached cannot open — ToolTipService only starts watching the owner's hover when it is attached (RegisterToolTip)
        bool AgentToolTipOpen() const noexcept; // Agentmaster: is the rich card CURRENTLY on screen? A safe READ of IsOpen (invariant 2 forbids WRITING it) — the page needs it to keep its wheel-scroll state in lockstep with the card that is actually hosted, and to not yank a reader back to the top when PointerEntered re-fires mid-hover
        bool AgentToolTipHoverWired() const noexcept { return _agentToolTipHoverWired; } // Agentmaster (lazy tooltip): cheap already-armed probe so per-tick callers skip even the callback construction
        bool EnsureAgentToolTipHoverHook(std::function<void()> onHoverBuild, std::function<bool(int)> onWheel = nullptr); // Agentmaster (lazy tooltip): wire (once) the owner TabViewItem's PointerEntered -> the page's build-now callback, and its PointerWheelChanged -> the page's card-scroll callback (WHEEL SCROLL — the header is the only real pointer target: the card is hit-test-invisible); returns true only the ONE time it wires (the caller's arm-build cue)

        std::optional<winrt::Windows::UI::Color> GetTabColor();
        std::optional<winrt::Windows::UI::Color> GetRuntimeTabColor() const noexcept { return _runtimeTabColor; } // Agentmaster: the user-chosen override (drives per-dir color sync)
        // Agentmaster (tab color modes — NoColor/"Remove colors"): the color persistence should
        // record for this tab — the LIVE runtime color, or (while the NoColor mode has it
        // suspended) the PARKED one. Read by the window-record capture (the Manager tab's
        // per-window color) so a save taken while "Remove colors" is active persists the color the
        // tab HAD instead of voiding it; BuildStartupActions applies the same fold for a shell
        // tab's setColor action. Visual reads (GetTabColor) deliberately do NOT see the parked color.
        std::optional<winrt::Windows::UI::Color> GetPersistableTabColor() const noexcept { return _runtimeTabColor ? _runtimeTabColor : _suspendedTabColor; }
        void SetTabColorSuspended(bool suspended); // Agentmaster (NoColor): park/restore the runtime color — shed the visual, KEEP the value for persistence; raises no TabColorChanged
        void SetRuntimeTabColor(const winrt::Windows::UI::Color& color);
        void ResetRuntimeTabColor();
        // Agentmaster (PENDING_INPUT.md): the tab's CURRENT effective header background — what actually
        // renders behind the header content right now, which SHIFTS with the selected/unselected state.
        // Mirrors _ApplyTabColorOnUIThread exactly: a SELECTED tab shows the full tab color; a DESELECTED
        // tab shows it at 30% opacity, both layered over the tab-row color (so the effective lightness
        // matches what's drawn). With no custom tab color it falls back to `fallbackSource` (the session's
        // per-dir color) over the row. The pending "3 dots" contrast-pick reads this so the dots stay
        // legible whether the tab is focused or not (an unfocused colored tab is far darker than its full
        // color). UI thread only; read-only.
        winrt::Windows::UI::Color CurrentEffectiveTabBackground(const winrt::Windows::UI::Color& fallbackSource);

        void UpdateZoom(std::shared_ptr<Pane> newFocus);
        void ToggleZoom();
        bool IsZoomed();
        void EnterZoom();
        void ExitZoom();

        std::vector<Microsoft::Terminal::Settings::Model::ActionAndArgs> BuildStartupActions(BuildStartupKind kind) const;

        int GetLeafPaneCount() const noexcept;

        void TogglePaneReadOnly();
        void SetPaneReadOnly(const bool readOnlyState);
        void ToggleBroadcastInput();

        std::shared_ptr<Pane> GetActivePane() const;
        winrt::TerminalApp::TaskbarState GetCombinedTaskbarState() const;

        std::shared_ptr<Pane> GetRootPane() const { return _rootPane; }
        std::vector<uint32_t> GetMruPanes() const { return _mruPanes; }

        winrt::TerminalApp::TerminalTabStatus TabStatus()
        {
            return _tabStatus;
        }

        void SetDispatch(const winrt::TerminalApp::ShortcutActionDispatch& dispatch);

        // Agentmaster: reservedLeading = how many pinned, non-bulk-closable tabs occupy the front of
        // the strip (the Manager tab => 1, else 0). They are skipped by _RemoveTabs, so they must not
        // count as closable neighbors when enabling "Close tabs to the left" / "Close other tabs".
        void UpdateTabViewIndex(const uint32_t idx, const uint32_t numTabs, const uint32_t reservedLeading = 0);
        void SetActionMap(const Microsoft::Terminal::Settings::Model::IActionMapView& actionMap);

        void ThemeColor(const winrt::Microsoft::Terminal::Settings::Model::ThemeColor& focused,
                        const winrt::Microsoft::Terminal::Settings::Model::ThemeColor& unfocused,
                        const til::color& tabRowColor);

        Microsoft::Terminal::Settings::Model::TabCloseButtonVisibility CloseButtonVisibility();
        void CloseButtonVisibility(Microsoft::Terminal::Settings::Model::TabCloseButtonVisibility visible);

        void DisableCloseAndMoveMenuItems(); // Agentmaster
        void DisableTabRename(); // Agentmaster
        void SetAgentCopyMenuVisible(bool visible, bool isCodex); // Agentmaster: show/hide the "Copy >" session-field submenu (managed agent-session tabs only; page-driven at flyout-open). isCodex (meaningful only when visible) offers ONLY the matching launch-CLI item — Codex CLI for a Codex session, Claude CLI otherwise — never both
        void SetAgentMarkUnreadVisible(bool visible); // Agentmaster: show/hide the "Mark Unread" item (managed agent-session tabs only; page-driven at flyout-open)
        void SetAgentTriageMoveState(bool visible, bool toIdle, bool fromError); // Agentmaster (Waiting-for-you + Error triage): show/hide the status-adaptive "Move to Idle/Done" / "Move to Waiting-for-you" item + set its label/icon by direction (toIdle == this session is Waiting-for-you OR Error, so offer the demote — fromError picks the error-dismissal tooltip; else it is Idle/Done, so offer the plain promote). Managed agent-session tabs only; page-driven at flyout-open
        void SetAgentFavoriteState(bool visible, bool isFavorite); // Agentmaster (FAVORITES.md): show/hide the "Favorite"/"Unfavorite" item + set its label by the session's current star (managed agent-session tabs only; page-driven at flyout-open)
        void SetAgentTagVisible(bool visible); // Agentmaster (bookmark tags): show/hide the "Tag" item (managed agent-session tabs only; page-driven at flyout-open, like Favorite)
        void SetAgentActivateVisible(bool visible); // Agentmaster (eager-init): show/hide the "Activate Tab" item (shown only when this tab's managed session is DORMANT — its claude hasn't started; page-driven at flyout-open from ConnectionState())
        void SetNewSessionModels(const std::vector<std::pair<std::wstring, std::wstring>>& models, bool isCodex, std::function<void(std::function<void(winrt::hstring)>)> specify = nullptr); // Agentmaster (launch-model picker): swap the plain "New Session Here" item for a submenu — Default + one item per configured {displayName, modelId} — and (re)populate it; page-driven at flyout-open from ParseLaunchModels(AppSettings.launchModels). isCodex (or an empty list) keeps the PLAIN item (the models are Claude models — a codex spawn takes no --model)
        void SetForkSessionModels(const std::vector<std::pair<std::wstring, std::wstring>>& models, bool isManagedClaude, std::function<void(std::function<void(winrt::hstring)>)> specify = nullptr); // Agentmaster (launch-model picker): same swap for "Fork session" — the submenu (Default + models) shows ONLY for a managed CLAUDE tab with a non-empty list (a fork is a launch, so --model applies); a Codex tab (codex fork takes no --model) and a plain shell tab (whose "Fork session" is really WT's duplicate-tab) keep the PLAIN item. Page-driven at flyout-open
        void SetFavoriteAndCloseAllVisible(bool visible); // Agentmaster (FAVORITES.md): show/hide the "★ Favorite & close all tabs" close-submenu item (shown only when the window hosts >=1 managed session; page-driven at flyout-open)
        void SetColorPickerEnabled(bool enabled); // Agentmaster (tab color modes — NoColor/"Remove colors"): gray/restore the "Change tab color..." context-menu item AND gate AttachColorPicker (the openTabColorPicker action), so NO tab — managed, shell, or the Manager tab — can be recolored while the mode is NoColor. TOGGLEABLE (unlike the Manager tab's one-shot disables) — seeded at tab registration, swept strip-wide on a mode change (_ReapplyManagedTabColors), re-asserted by the managed paint seam (_ApplySessionTabColor) + at flyout-open

        til::event<winrt::delegate<void()>> RequestFocusActiveControl;

        til::event<winrt::Windows::Foundation::EventHandler<winrt::Windows::Foundation::IInspectable>> Closed;
        til::event<winrt::Windows::Foundation::EventHandler<winrt::Windows::Foundation::IInspectable>> CloseRequested;
        til::property_changed_event PropertyChanged;

        til::typed_event<TerminalApp::TerminalPaneContent> RestartTerminalRequested;

        til::typed_event<TerminalApp::Tab, IInspectable> ActivePaneChanged;
        til::event<winrt::delegate<>> TabRaiseVisualBell;
        til::event<winrt::delegate<>> TabColorChanged; // Agentmaster: runtime tab color set/reset -> page syncs the per-directory color
        til::event<winrt::delegate<>> MoveTabToStartRequested; // Agentmaster: context-menu "Move to start" -> page relocates this tab to the first movable slot
        til::event<winrt::delegate<>> MoveTabToEndRequested; // Agentmaster: context-menu "Move to end" -> page relocates this tab to the last slot
        til::event<winrt::delegate<winrt::hstring /*modelId*/>> NewSessionHereRequested; // Agentmaster: context-menu "New Session Here" -> page spawns a managed agent session in this tab's working dir. modelId = the launch-model picker's per-LAUNCH `--model <id>` pick ("" = Default — the plain item and the submenu's Default both send it)
        til::event<winrt::delegate<winrt::hstring /*modelId*/>> ForkSessionRequested; // Agentmaster (launch-model picker): a "Fork session" SUBMENU pick -> page forks THIS tab's managed Claude session with that model (_ForkManagedSessionById; "" = Default). Raised ONLY by the submenu form (shown solely on managed Claude tabs); the plain item keeps the upstream duplicate-tab action dispatch, which reaches the same fork seam
        til::event<winrt::delegate<>> MarkUnreadRequested; // Agentmaster: context-menu "Mark Unread" -> page flashes this tab's red ring until visited (even if it's the focused tab)
        til::event<winrt::delegate<>> TriageMoveRequested; // Agentmaster (Waiting-for-you + Error triage): context-menu "Move to Idle/Done" / "Move to Waiting-for-you" -> page moves this tab's managed session between WaitingForInput/Error and Idle/Done (direction re-derived from the live state; an Error demote is the error DISMISSAL); EXPLICITLY separate from "Mark Unread" (no sticky flag, no ring flash)
        til::event<winrt::delegate<>> FavoriteRequested; // Agentmaster (FAVORITES.md): context-menu "Favorite"/"Unfavorite" -> page toggles this tab's session star (SessionStore), the same toggle as the Sessions page's ★ column
        til::event<winrt::delegate<>> TagEditorRequested; // Agentmaster (bookmark tags): context-menu "Tags" -> page opens the tag panel for this tab's session (name a new tag + toggle the existing ones; an islands-safe Popup — a Flyout-hosted text box gets no keypresses)
        til::event<winrt::delegate<winrt::hstring /*tag*/, winrt::Windows::UI::Xaml::UIElement /*anchor*/>> TagBadgeHoverBegin; // Agentmaster (bookmark tags): the pointer entered a header bookmark badge -> page shows the rich tag hover panel (sessions carrying the tag + status, click == jump) anchored at the badge
        til::event<winrt::delegate<>> TagBadgeHoverEnd; // Agentmaster (bookmark tags): the pointer left a header bookmark badge -> page schedules the hover panel's grace-timer close (canceled if the pointer crosses into the panel)
        til::event<winrt::delegate<>> ActivateSessionRequested; // Agentmaster (eager-init): context-menu "Activate Tab" -> page starts this tab's DORMANT session's claude IN PLACE (TermControl::InitializeWithSize), no focus change
        til::event<winrt::delegate<>> CloseTabsBeforeRequested; // Agentmaster: context-menu "Close > Close tabs to the left" -> page closes every tab left of this one (skipping the pinned Manager tab)
        til::event<winrt::delegate<>> CloseAllTabsRequested; // Agentmaster: context-menu "Close > Close all tabs" -> page closes every tab in this window (skipping the pinned Manager tab)
        til::event<winrt::delegate<>> FavoriteAndCloseAllTabsRequested; // Agentmaster (FAVORITES.md): context-menu "Close > ★ Favorite & close all tabs" -> page stars every managed session, then closes every tab
        til::event<winrt::delegate<int32_t /*which*/>> CopySessionFieldRequested; // Agentmaster: context-menu "Copy > <field>" -> page copies that field of this tab's managed session via the shared CopySessionField action (the same options as the per-tab overlay's copy button); `which` is the copy-menu code (see AgentCopyActions.h)
        til::event<winrt::delegate<winrt::hstring /*title*/, winrt::hstring /*body*/, winrt::TerminalApp::IPaneContent /*content*/>> TabToastNotificationRequested;
        til::typed_event<IInspectable, IInspectable> TaskbarProgressChanged;

        // The TabViewIndex is the index this Tab object resides in TerminalPage's _tabs vector.
        WINRT_PROPERTY(uint32_t, TabViewIndex, 0);
        // The TabViewNumTabs is the number of Tab objects in TerminalPage's _tabs vector.
        WINRT_PROPERTY(uint32_t, TabViewNumTabs, 0);

        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, Title, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(winrt::hstring, Icon, PropertyChanged.raise);
        WINRT_OBSERVABLE_PROPERTY(bool, ReadOnly, PropertyChanged.raise, false);
        WINRT_PROPERTY(winrt::Microsoft::UI::Xaml::Controls::TabViewItem, TabViewItem, nullptr);

        WINRT_OBSERVABLE_PROPERTY(winrt::Windows::UI::Xaml::FrameworkElement, Content, PropertyChanged.raise, nullptr);

    private:
        // Agentmaster: widened from upstream's 165 — the rename box (now multi-line) was capping
        // at ~half a wide tab's width. This is a MaxWidth, so a narrow Equal-mode tab still clamps
        // the box to the tab; it only lets the box use more room when the tab is wide.
        static constexpr double HeaderRenameBoxWidthDefault{ 360 };
        static constexpr double HeaderRenameBoxWidthTitleLength{ std::numeric_limits<double>::infinity() };

        winrt::Windows::UI::Xaml::FocusState _focusState{ winrt::Windows::UI::Xaml::FocusState::Unfocused };
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _duplicateTabMenuItem{};
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _splitTabMenuItem{};
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _moveToNewWindowMenuItem{};
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _moveRightMenuItem{};
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _moveLeftMenuItem{};
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _moveToStartMenuItem{}; // Agentmaster
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _moveToEndMenuItem{}; // Agentmaster
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _exportTabMenuItem{};
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _findMenuItem{};
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _restartConnectionMenuItem{};
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _newSessionHereMenuItem{}; // Agentmaster: spawn a managed agent session in this tab's working dir (the PLAIN form — shown for a Codex tab / an empty model list; see _newSessionHereSubMenu)
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutSubItem _newSessionHereSubMenu{}; // Agentmaster (launch-model picker): the SUBMENU form of "New Session Here" — Default + one item per configured model; the page repopulates it at flyout-open (SetNewSessionModels) and exactly one of the pair is visible
        std::wstring _newSessionModelsKey; // Agentmaster (launch-model picker): change-gate for the submenu rebuild — the joined {name,id} list last built, so an unchanged settings list skips the item churn at every flyout-open
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutSubItem _forkSessionSubMenu{}; // Agentmaster (launch-model picker): the SUBMENU form of "Fork session" — Default + one item per configured model, raising ForkSessionRequested; the page repopulates it at flyout-open (SetForkSessionModels) and exactly one of {_duplicateTabMenuItem, this} is visible
        std::wstring _forkModelsKey; // Agentmaster (launch-model picker): the fork submenu's rebuild change-gate (the _newSessionModelsKey idiom)
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _markUnreadMenuItem{}; // Agentmaster: "Mark Unread" — flash this tab's red ring until visited; kept as a member so the page can show/hide it per managed-session-tab (collapsed by default)
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _triageMoveMenuItem{}; // Agentmaster (Waiting-for-you + Error triage): status-adaptive "Move to Idle/Done" / "Move to Waiting-for-you" — member so the page sets its visibility + label/icon per the session's state at flyout-open (collapsed by default)
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _favoriteMenuItem{}; // Agentmaster (FAVORITES.md): "Favorite"/"Unfavorite" — toggle this tab's session star; member so the page sets visibility + label per managed-session-tab (collapsed by default)
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _tagMenuItem{}; // Agentmaster (bookmark tags): "Tag" — open the tag panel for this tab's session; member so the page can show/hide it per managed-session-tab (collapsed by default)
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _activateSessionMenuItem{}; // Agentmaster (eager-init): "Activate Tab" — start this tab's DORMANT session in place; member so the page shows it only when the session's claude hasn't started (collapsed by default)
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _closeOtherTabsMenuItem{};
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _closeTabsBeforeMenuItem{}; // Agentmaster: "Close tabs to the left" (the left-hand twin of _closeTabsAfterMenuItem)
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _closeTabsAfterMenuItem{};
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _closeAllTabsMenuItem{}; // Agentmaster: "Close all tabs" — the whole-window twin of "Close other tabs"
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _favoriteAndCloseAllTabsMenuItem{}; // Agentmaster (FAVORITES.md): "★ Favorite & close all tabs" — star every managed session, then close all; member so the page can show/hide it per whether the window hosts a managed session (collapsed by default)
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _closePaneMenuItem{};
        // Agentmaster: the "Move tab" / "Close" sub-menus and the "Close tab" item are kept
        // as members (not locals in _CreateContextMenu) so the pinned Manager tab can gray
        // them out via DisableCloseAndMoveMenuItems().
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutSubItem _moveSubMenu{};
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutSubItem _closeSubMenu{};
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _closeTabMenuItem{};
        // Agentmaster: kept as a member (not a local in _CreateContextMenu) so the pinned Manager
        // tab can gray out the "Rename Tab" item via DisableTabRename(). _renameDisabled also gates
        // ActivateTabRenamer() so the double-tap / openTabRenamer-action paths are blocked too.
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _renameTabMenuItem{};
        bool _renameDisabled{ false };
        // Agentmaster (tab color modes — NoColor/"Remove colors"): the "Change tab color..." item, kept
        // as a member so SetColorPickerEnabled can gray/restore it per the GLOBAL tab-color mode (a
        // TOGGLE, unlike the Manager tab's one-shot disables). _colorPickerDisabled also gates
        // AttachColorPicker() so the openTabColorPicker action / command-palette path is blocked too.
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _chooseColorMenuItem{};
        bool _colorPickerDisabled{ false };
        // Agentmaster: the "Copy >" submenu (session id / path / branch / current prompt / Claude & Codex
        // launch CLI / summary / transcript) mirroring the per-tab overlay's copy button. Kept as a member so the page
        // can show/hide it (SetAgentCopyMenuVisible) per whether this tab currently hosts a managed agent
        // session; built collapsed in _CreateContextMenu, its items raise CopySessionFieldRequested.
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutSubItem _copySessionSubMenu{};
        // Agentmaster: the two launch-CLI items inside _copySessionSubMenu, kept as members so
        // SetAgentCopyMenuVisible can reveal ONLY the one matching the session's agent (Claude vs Codex).
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _copyClaudeCliItem{ nullptr };
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _copyCodexCliItem{ nullptr };
        // Agentmaster (PENDING_INPUT.md): "Copy Current Prompt" — the unsent-draft item, a member for the
        // same reason: SetAgentCopyMenuVisible shows it only for a CLAUDE session (Codex has no input box).
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutItem _copyCurrentPromptItem{ nullptr };
        uint32_t _reservedLeadingTabs{ 0 }; // Agentmaster: count of pinned, non-bulk-closable leading tabs (the Manager tab); fed by UpdateTabViewIndex, read by _EnableMenuItems
        winrt::TerminalApp::ShortcutActionDispatch _dispatch;
        Microsoft::Terminal::Settings::Model::IActionMapView _actionMap{ nullptr };
        winrt::hstring _keyChord{};

        // Agentmaster (tab tooltip): the rich, session-aware tooltip pushed by TerminalPage (see
        // SetAgentToolTip). TerminalPage builds the whole body element (a dark, summary-style card); the
        // Tab only HOSTS it on the reused ToolTip. While _agentToolTipActive, _UpdateToolTip hosts
        // _agentToolTipContent instead of the default title+keychord; _agentToolTipSig (a content
        // fingerprint from the page) skips re-hosting identical content each tick.
        bool _agentToolTipActive{ false };
        winrt::Windows::UI::Xaml::UIElement _agentToolTipContent{ nullptr };
        winrt::hstring _agentToolTipSig{};
        // The ONE reused ToolTip object (swap its Content only while CLOSED; re-creating + re-SetToolTip on
        // each ~2s refresh would flicker/replace the framework's open tip while hovered). Configured once:
        // pinned Dark + Placement Bottom + hit-test-invisible, anchored to the TAB via PlacementRect (an
        // automatic hover tooltip otherwise places itself relative to the POINTER). FRAMEWORK-MANAGED —
        // ToolTipService owns open/close; we NEVER drive IsOpen (that was the crash source — see
        // _UpdateAgentToolTip and doc/agentmaster/HANDOVER_tab-tooltip.md). Detached + nulled on an owner
        // recycle/unload/shutdown.
        winrt::Windows::UI::Xaml::Controls::ToolTip _agentToolTip{ nullptr };
        bool _agentToolTipUnloadWired{ false }; // the owner Unloaded -> detach handler is wired once per tab
        // Agentmaster (LAZY tab tooltip — the CPU fix): the tooltip card is built ON HOVER, not per tick.
        // The page's build-now callback (weak-captured) runs on the owner TabViewItem's PointerEntered —
        // which fires well BEFORE ToolTipService's open delay, so the Content swap still happens while the
        // tip is closed (the safe path). One hook per tab, wired once; the callback resolves the tab's
        // CURRENT session at hover time, so a /resume re-home never leaves it stale.
        std::function<void()> _agentToolTipHoverCb{ nullptr };
        std::function<bool(int)> _agentToolTipWheelCb{ nullptr }; // Agentmaster (WHEEL SCROLL): the page scrolls the OPEN card's body by this many wheel units; returns true when it consumed the notch
        uint64_t _agentToolTipWheelLogTick{ 0 }; // Agentmaster (WHEEL SCROLL): throttle for the one-line-per-gesture [tooltip-wheel] trace (a spin is many events)
        bool _agentToolTipHoverWired{ false }; // the PointerEntered hook is wired once per tab
        bool _agentToolTipSwapOpenOnce{ false }; // one-shot: let the NEXT _UpdateAgentToolTip swap Content while OPEN (the async summary-body arrival)

        winrt::Microsoft::Terminal::Settings::Model::ThemeColor _themeColor{ nullptr };
        winrt::Microsoft::Terminal::Settings::Model::ThemeColor _unfocusedThemeColor{ nullptr };
        til::color _tabRowColor;

        Microsoft::Terminal::Settings::Model::TabCloseButtonVisibility _closeButtonVisibility{ Microsoft::Terminal::Settings::Model::TabCloseButtonVisibility::Always };

        std::shared_ptr<Pane> _rootPane{ nullptr };
        std::shared_ptr<Pane> _activePane{ nullptr };
        std::shared_ptr<Pane> _zoomedPane{ nullptr };

        winrt::Microsoft::Terminal::Settings::Model::IconStyle _lastIconStyle;
        winrt::hstring _lastIconPath{};
        std::optional<winrt::Windows::UI::Color> _runtimeTabColor{};
        // Agentmaster (tab color modes — NoColor/"Remove colors"): the runtime color PARKED while
        // the mode is active — the visual is shed (_runtimeTabColor cleared) but the value is kept
        // here so persistence (GetPersistableTabColor / BuildStartupActions) still records it and
        // leaving the mode restores it (SetTabColorSuspended(false)). Never rendered.
        std::optional<winrt::Windows::UI::Color> _suspendedTabColor{};
        winrt::TerminalApp::TabHeaderControl _headerControl{};
        winrt::TerminalApp::TerminalTabStatus _tabStatus{};

        winrt::TerminalApp::ColorPickupFlyout _tabColorPickup{ nullptr };
        winrt::event_token _colorSelectedToken;
        winrt::event_token _colorClearedToken;
        winrt::event_token _pickerClosedToken;

        struct ContentEventTokens
        {
            winrt::TerminalApp::IPaneContent::BellRequested_revoker BellRequested;
            winrt::TerminalApp::IPaneContent::TitleChanged_revoker TitleChanged;
            winrt::TerminalApp::IPaneContent::TabColorChanged_revoker TabColorChanged;
            winrt::TerminalApp::IPaneContent::TaskbarProgressChanged_revoker TaskbarProgressChanged;
            winrt::TerminalApp::IPaneContent::ConnectionStateChanged_revoker ConnectionStateChanged;
            winrt::TerminalApp::IPaneContent::ReadOnlyChanged_revoker ReadOnlyChanged;
            winrt::TerminalApp::IPaneContent::FocusRequested_revoker FocusRequested;
            winrt::TerminalApp::IPaneContent::NotificationRequested_revoker NotificationRequested;

            // These events literally only apply if the content is a TermControl.
            winrt::Microsoft::Terminal::Control::TermControl::KeySent_revoker KeySent;
            winrt::Microsoft::Terminal::Control::TermControl::CharSent_revoker CharSent;
            winrt::Microsoft::Terminal::Control::TermControl::StringSent_revoker StringSent;

            winrt::TerminalApp::TerminalPaneContent::RestartTerminalRequested_revoker RestartTerminalRequested;
        };
        std::unordered_map<uint32_t, ContentEventTokens> _contentEvents;

        winrt::event_token _rootClosedToken{};

        std::vector<uint32_t> _mruPanes;
        uint32_t _nextPaneId{ 0 };

        bool _receivedKeyDown{ false };
        bool _iconHidden{ false };
        bool _changingActivePane{ false };

        winrt::hstring _runtimeTabText{};
        bool _inRename{ false };
        winrt::Windows::UI::Xaml::Controls::TextBox::LayoutUpdated_revoker _tabRenameBoxLayoutUpdatedRevoker;

        void _Setup();

        SafeDispatcherTimer _bellIndicatorTimer;
        void _BellIndicatorTimerTick(const Windows::Foundation::IInspectable& sender, const Windows::Foundation::IInspectable& e);

        void _UpdateHeaderControlMaxWidth();

        void _CreateContextMenu();
        winrt::hstring _CreateToolTipTitle();

        void _DetachEventHandlersFromContent(const uint32_t paneId);
        void _AttachEventHandlersToContent(const uint32_t paneId, const winrt::TerminalApp::IPaneContent& content);
        void _AttachEventHandlersToPane(std::shared_ptr<Pane> pane);

        void _UpdateActivePane(std::shared_ptr<Pane> pane);
        void _UpdateMenuItemStates();

        winrt::hstring _GetActiveTitle() const;

        void _RecalculateAndApplyReadOnly();

        void _UpdateProgressState();

        void _UpdateConnectionClosedState();
        void _RestartActivePaneConnection();

        winrt::Windows::UI::Xaml::Media::Brush _BackgroundBrush();

        void _MakeTabViewItem();

        void _AppendMoveMenuItems(winrt::Windows::UI::Xaml::Controls::MenuFlyout flyout);
        winrt::Windows::UI::Xaml::Controls::MenuFlyoutSubItem _AppendCloseMenuItems(winrt::Windows::UI::Xaml::Controls::MenuFlyout flyout);
        void _EnableMenuItems();
        void _UpdateSwitchToTabKeyChord();
        void _UpdateToolTip();
        void _UpdateAgentToolTip(); // Agentmaster: host the page-built rich session tooltip card on the reused ToolTip; FRAMEWORK-MANAGED open/close (Content swapped only while closed; we never drive IsOpen)
        void _WireAgentToolTipUnload(); // Agentmaster: wire (once) the owner TabViewItem Unloaded -> _DetachAgentToolTip, so a MUX recycle can't strand a stale ToolTip ref (the sole handler left after the manual-open path was removed)
        void _DetachAgentToolTip(); // Agentmaster: detach the framework-managed tooltip from its owner + drop our ref (owner recycle/unload, ClearAgentToolTip, Shutdown); no IsOpen driven — the framework closes any open popup itself

        void _RecalculateAndApplyTabColor();
        void _ApplyTabColorOnUIThread(const winrt::Windows::UI::Color& color);
        void _ClearTabBackgroundColor();
        void _RefreshVisualState();

        bool _focused() const noexcept;
        void _updateIsClosable();

        void _addBroadcastHandlers(const winrt::Microsoft::Terminal::Control::TermControl& control, ContentEventTokens& events);

        void _chooseColorClicked(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void _renameTabClicked(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void _duplicateTabClicked(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void _splitTabClicked(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void _closePaneClicked(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void _exportTextClicked(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);
        void _findClicked(const winrt::Windows::Foundation::IInspectable& sender, const winrt::Windows::UI::Xaml::RoutedEventArgs& e);

        void _bubbleRestartTerminalRequested(TerminalApp::TerminalPaneContent sender, const winrt::Windows::Foundation::IInspectable& args);

        friend class ::TerminalAppLocalTests::TabTests;
    };
}
