"""Page objects: Agentmaster's surfaces as a test reads and drives them.

Each page object knows WHERE its surface lives in the UI Automation tree and HOW a user operates it, so a test
says what it wants ("open the page", "right-click tab t1", "choose Close ▸ Close other tabs") and never deals
with element paths, popups or pixel positions. The facts behind them were read from dumps of Agentmaster 0.8.0
in the test guest (`python -m amdriver dump`, 2026-10-23):

- A window: the root (class CASCADIA_HOSTING_WINDOW_CLASS) → an InputSite pane → the `#TabView` (Home
  `#ManagerHomeButton`, the strip `#TabListView` of tab items named by their titles) and ONE content element
  holding everything else as direct children: the Agent Manager page's controls, the side panel's controls,
  the terminal (`TermControl`), the per-tab badge, and in-window popups (the + card, the folder list).
- A ContentDialog: a `window` element of class Popup named by its title, a child of the InputSite pane, with
  `#PrimaryButton` / `#SecondaryButton` / `#CloseButton`.
- Context menus and their submenus: each in its OWN top-level popup window (Xaml_WindowedPopupClass); menu
  items are `MenuFlyoutItem` / `MenuFlyoutSubItem`; a hidden item is absent from the tree, a disabled one has
  IsEnabled false.

Modules: window (MainWindow, the root of the rest), tabstrip, menus, dialogs, manager (the Agent Manager page),
sidepanel, queue (the page's Prompt Queue tab and the toolbar's global pause), badge (the per-tab badge's runner
parts), sessions (the Sessions page).
"""
