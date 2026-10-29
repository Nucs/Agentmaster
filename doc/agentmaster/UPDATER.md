# The in-app updater

A GitHub-release self-updater for the packaged release install and for portable copies. Code:
`AgentMaster/Updater.h` (header-only, plain Win32 like `ProfileBootstrap.h`, so the WindowsTerminal EXE
includes it without the engine lib), called from `WindowEmperor` and from the Settings cog's About tab. Its
two parts, which it includes at its end, are the prompt window (`UpdaterPromptWindow.h`) and that window's
release notes (`UpdaterReleaseNotes.h`).

## When it checks

- **At startup:** `WindowEmperor` runs `RunStartupUpdateCheck` after the profile resolves and before the
  "Reopen your N previous Agentmaster windows?" prompt: a bounded GitHub-API query (≤ 6 s, on a worker,
  so a slow network can't wedge the launch).
- **Every hour while running:** `RunPeriodicUpdateCheck` (the shared core is `RunUpdateCheckAndPrompt`).
  `WindowEmperor` arms a Win32 `WM_TIMER` on its message window (`_setupUpdateAutocheck`; a XAML
  `DispatcherTimer` never starts in this mode); each tick runs on a detached thread, so neither the
  round-trip nor the modal prompt blocks the UI. An in-flight guard (a `shared_ptr<atomic<bool>>` the
  worker captures instead of `this`) prevents a second prompt.
- **Who checks:** packaged release installs and portable copies (`IsUpdaterChannel`); dev and plain
  unpackaged builds skip unless `AGENTMASTER_UPDATE_STARTUP` is set.

## The prompt

- When a strictly newer eligible release exists, a dark window of Agentmaster's own asks, with the release
  notes beside the choices (*The prompt window*); the TaskDialog it replaced is its fallback. Both offer
  **Update now** / **Postpone**, over six radios choosing what Postpone means:
  - **Remind me next restart** (the default, and what Cancel / X / Esc / any failure resolve to): writes
    nothing durable; it latches the process-scoped `AGENTMASTER_UPDATE_DECLINED=<tag>` env var, so the
    startup and hourly checks are skipped, before any network call, until the next launch (which clears
    an inherited latch). The cog's manual check stays live. Each skipped tick still logs.
  - **Remind me tomorrow:** the next local 08:00 (within 24 h; DST-safe; the radio names the instant,
    e.g. "Remind me tomorrow (Sun, 8:00 AM)").
  - **3 days · 7 days · 30 days · Skip this version.**
- A postponed or skipped version stays silent until it expires; the same gates apply to the startup and
  hourly checks.

## The prompt window

`ShowUpdatePrompt` builds the TaskDialog as before and hands it to `ShowUpdatePromptWindow`
(`UpdaterPromptWindow.h`), which shows the same words and choices in a window of its own and answers with
the TaskDialog's button and radio ids (`PromptWindowAnswer`), so one switch maps both. The TaskDialog shows
only when the window cannot be created, or with `AGENTMASTER_UPDATE_PROMPT=taskdialog`.

- **Layout** (`ComputePromptLayout`, pure): the release notes on the left, taking every extra pixel; on the
  right, at a fixed 344 DIP, the TaskDialog's information icon, main instruction, text and six radios,
  verbatim; below, a band with **Release page →** (the offered release's page), **Update now** (the default)
  and **Postpone**. It opens at 1040 × 640 DIP (at most 92% of the work area), centred on its owner, or at
  startup on the monitor under the mouse; it can shrink to a 380-DIP notes column beside the full right
  column. A client below that minimum is laid out at the minimum, so nothing overlaps.
- **The notes:** a tab per release on the user's update channel (`ReleaseAllowedOnChannel`, drafts left
  out), oldest to newest. Older releases are dimmed, the installed one sits under a `▲ you` arrow (in the gap
  before the first update, labelled with the version, when the installed version is no release on the
  channel), every update is green with a dot; a pre-release or nightly tab carries a `pre` / `nightly` badge
  and its short version (`ReleaseTabLabel`). The strip scrolls (‹ ›, the wheel, Left / Right / Home / End),
  opens at yours and the updates, and shows the newest release. A tab's notes open under a header: Update /
  Your version / Older, the tag · kind · date (the user's short date), and Open on GitHub →.
- **Where they come from:** the offered release's notes show at once (`UpdateInfo::notes`, which the check
  already has). A worker reads GitHub's release list while the window is up (`FetchReleaseNotesList`: 30 per
  page, paging until a page reaches the installed version, at most 3 pages, 8 s per phase), renders it, and
  leaves the result in a mailbox (`ReleaseNotesMailbox`); a status line says "Loading the other releases…"
  until then, or why the list failed. At most 30 tabs: the oldest go first, every update stays
  (`BuildReleaseTimeline`; one tab per tag, the list's copy winning over the offered one).
- **Markdown → HTML:** cmark (vcpkg 0.31.1, the library the transcript overlay parses with) in its safe mode
  (`CMARK_OPT_DEFAULT`): raw HTML is omitted, a `javascript:`, `vbscript:`, `file:` or non-image `data:`
  destination is emptied. Each release's Install section is cut first (`StripInstallSection`: an ATX heading
  "Install", any case, to the next heading of its level; a heading inside a code fence is code), because
  Update now installs for the user. Notes over 200,000 characters are cut at a line end, with a pointer to
  the release page.
- **The page** (`BuildReleaseNotesPage`) is a fixed document with no release in it. Its content-security
  policy runs only the script and the style that carry a fresh per-page nonce (`default-src 'none'`, images
  over https). Its script asks for the model (`{"type":"ready"}`), renders every model the window posts
  (`PostWebMessageAsJson`, `BuildReleaseNotesModelJson`), and sends links (`open`) and Esc (`escape`) back.
  The window opens only http(s) addresses, in the user's browser, and cancels every navigation but the page's
  own load; new windows, the context menu, DevTools, the status bar and the browser's own keys (F5, Ctrl+P,
  Ctrl+F, …) are off; Ctrl+wheel zooms. The page uses the dialog's message font, face and size.
- **WebView2:** the SDK is the `Microsoft.Web.WebView2` package the repository already restores for WinUI
  (1.0.1661.34, `WebView2PackageRoot`). TerminalAppLib and the EXE put its headers on every unit's include
  path (the window is inline code, and two units of one module compiling it differently would break the one
  definition rule); TerminalApp.dll and the EXE link its static loader (`WebView2LoaderStatic.lib`: nothing to
  ship). The runtime is the Evergreen WebView2 Runtime that Windows 11 carries (most Windows 10 PCs have it
  through Edge or Microsoft 365). Its data folder is `<profile>\webview2\`, never the package's own storage.
  It runs on the caller's thread inside the window's modal loop: the startup's main thread and the cog's UI
  thread already are single-threaded COM apartments, and the hourly check's worker becomes one for the
  prompt (`PromptApartment`).
- **Without WebView2** — no runtime, a thread that is no single-threaded apartment, a start that fails or
  takes over 15 s, a page that does not load, a WebView2 process that stops, or `AGENTMASTER_UPDATE_PROMPT=plain`
  — the notes column shows the notes as plain text in a read-only box (the updates newest first, the Install
  section cut, `BuildReleaseNotesPlainText`); the window and its choices stay the same. The reason is logged.
- **Dark** like every Agentmaster surface: a dark title bar (DWM; on Windows 11 the caption's own colors), the
  Agent Manager's palette (#2e2e2e), Windows' `DarkMode_Explorer` theme on the push buttons and scroll bars,
  and custom-drawn radios (Windows draws a themed radio's label black whatever color it is given). High
  Contrast switches the window to the system colors: no custom drawing, no dark theme, and the page follows
  the forced colors.
- **Keyboard:** Tab cycles the notes, the radio group, the buttons and the link (the page hands the keyboard
  back at its first and last control, `MoveFocusRequested`); the arrows move inside the radio group and
  between the tabs; Enter = Update now (the default, as in the TaskDialog); Esc, ✕ and Alt+F4 = Cancel =
  "Remind me next restart", from the page too. Keys meant for the page never reach the dialog navigation.
- **The console echo** (CLI.md §2b) reads like the TaskDialog's, with this window's link and a line for the
  notes, through `DialogEcho::ShowCustomDialog` (a dialog Agentmaster draws itself has no Win32 dialog call
  for the coverage test to find, so it must come through there).
- **Closing:** the worker is detached (it posts nothing once the window is going); WebView2 is closed (a view
  retired inside one of its own callbacks is closed one message later); the owner is enabled again before the
  window is destroyed; then the window pumps for up to 3 s for WebView2 calls still under way, so a browser
  process started for the prompt never outlives it — the hourly check's thread ends right after.
- **Logging** (`[update]`): `prompt window: shown (notes: WebView2 <version> starting, rendered by cmark
  0.31.1)`, `prompt window: the notes page is ready (WebView2 <version>, <n> ms)`, `prompt window: notes as
  plain text (<why>)`, `release notes: <n> release(s) on the channel from <p> page(s), <u> update(s), yours
  <tag> (<n> ms)` or `release notes: the list could not be loaded (<why>)`, `prompt window: opened <url>`,
  `prompt window: could not be created (<why>) — the task dialog shows instead`.
- **Tests:** `TestUpdateReleaseNotes` (`tests_update_notes.cpp`): the list filter, the timeline, the Install
  cut, cmark's safe HTML, the model, the page's policy and nonce, the plain text, the layout at 100% and 150%
  and below its minimum, `ShowCustomDialog`. The page was also rendered headlessly from the real release
  list on 2026-10-29: the tabs, the arrow, the keyboard, links and Esc behaved, with no console error.
  The same day a scratch probe built from these headers ran the REAL check and the REAL prompt in a Windows 11
  desktop (claude-desktops, Tiny11 25H2, WebView2 156): the dark window with its tabs and notes (the list in
  ~0.7 s, the page ready in ~0.8 s), Postpone with "3 days" answering Postpone3, Up inside the radio group, Tab
  into the page and Left between its tabs without touching the radios, Esc from the page and ✕ answering
  "Remind me next restart", the minimum size, the plain-text path and the TaskDialog fallback; no WebView2
  process outlived a prompt. That run found the radios' glyphs drawn classic after a click (the control had no
  theme handle to lend during the repaint): the window now draws them with a theme handle of its own.
- **Check after the build:** a dev build with `AGENTMASTER_UPDATE_STARTUP=1` sees every release as newer and
  prompts at startup. Check the dark window and its tabs; Tab, Esc and Enter; a resize and a move to a
  monitor of another scale; `AGENTMASTER_UPDATE_PROMPT=plain`; the cog's Check for updates (an owned
  window); and the hourly prompt (its own thread).

## Update now

- Agentmaster writes its built-in installer — `am-update.cmd` + `am-update.ps1`, embedded in the app,
  never fetched — into the active profile and runs it; the app then quits (`TerminateProcess`, like the
  single-instance handoff), and the workspace reopens on the relaunch.
- **The release package:** the script downloads the `.msixbundle` and `Agentmaster.cer`, trusts the
  certificate (elevating only if needed), `Add-AppxPackage`s the bundle (with the VCLibs-dependency
  fallback) and relaunches. Only the release family is published, so a dev install that updates becomes
  the release install.
- **A portable copy** (`IsPortableUpdateTarget`: unpackaged + the `.portable` marker) updates in place: its
  version is the exe-side `.am-version` stamp (`CurrentInstallVersion` → `PortableVersionFromDir`; absent =
  0.0.0, so it adopts the newest release); `ParseReleaseObj` picks the release's arch-matched portable zip
  (`kPortableZipSuffix`) and `InstallableForThisInstall` routes every consumer to the zip; `am-update.ps1
  -Portable` downloads the zip, stops what runs under the unzip, swaps its binaries keeping `settings\`,
  `profile\`, `profile.path` and `install.path` (the zip's `agentmaster-<ver>` or `terminal-<ver>` folder
  unwrapped), restamps `.am-version` and relaunches. No certificate, no administrator, no registration.
- **An all-users copy** elevates only its file swap. When that elevation is declined (1223), refused by
  policy (1260) or fails, the script asks: `[A]` again · `[U]` install the release just for this user
  (the default; no rights; `-ProfileDir` keeps the profile) · `[C]` cancel. The package update's
  certificate question offers `[U]` too.
- **Never without the app:** after a failure, a cancel or a kept version, `am-update.ps1` starts the
  installed version again (`Restart-PreviousApp`, counting only this account's processes) and leaves
  `<profile>\am-update-restarted.txt`, which that run reads as "Remind me next restart"
  (`ConsumeRestartedAfterUpdateMarker`, 10 min). The generated `.cmd` launchers start with `chcp 65001`.
- After an update, `RefreshInstalledRegistration` re-stamps the per-user install's Apps & Features entry.

## Channels

- **Stable** releases always; **pre-releases** only with `allowUpdatePrerelease`; **nightlies** only with
  `allowUpdateNightly`.
- A nightly is a release whose TAG contains "nightly" (`IsNightlyTag`, case-insensitive; the tag decides,
  whatever the prerelease flag; a `/releases/latest` check keeps one off the stable channel).
  `ReleaseAllowedOnChannel` gates each tier separately, and the list scan picks the newest eligible
  release, so a pre-release-only user skips a newer nightly. `CompareVersion` is numeric: a nightly must
  raise `X.Y.Z` past the installed version to be offered. The prompt repeats the nightly warning.
- Turning the nightly switch on asks first (memory leaks, CPU issues, crashes; "use it to contribute and
  help, but be willing to have your work suddenly interrupted"): the toggle handler reverts the switch,
  asks, and re-applies it only on accept (`_ApplyAllowNightly`).

## The cog's UPDATES section (About)

- **Check for updates**, a "vX.Y.Z available!" label, "Allow pre-release versions" and "Allow updating to
  nightly builds (unstable)". Both switches apply at once: a freshest-disk read-modify-write plus a silent
  re-check (`[nav] update-prerelease -> on/off`).
- The check worker (a detached thread) and its UI completion (`_ApplyUpdateCheckResult`) are fully
  guarded, with button and flag recovery; a fresh cog open resets `_interactiveUpdateInFlight` and the
  button label; the pre-release seed latch clears on every path.

## State

- In `settings.json`, inside the engine's envelope: `allowUpdatePrerelease` and `allowUpdateNightly` (each
  written by its toggle) and `updateSkippedVersion` / `updatePostponedUntilUnixMs` (written by the
  prompt's EXE-safe read-modify-write). Both cog-Save preserve blocks restore all four from disk.
- `ReadPrefs` falls back to a stray top-level key; `WriteUpdateState` migrates strays into the envelope,
  refuses to rebuild an unparseable non-empty file, and writes atomically (temp + flush + `MoveFileExW`).

## Hardening and logging

- Every `Updater.h` entry point is no-throw with a safe default: the channel gate → not a channel; the
  prompt → remind next restart; applying a decision → the latched remind; launching the installer or
  uninstaller → not launched (the app never quits with nothing running); reading prefs → defaults. The
  comctl hyperlink callback and `HttpsGet`'s read loop are guarded (its three WinHTTP handles close on
  the throw path); `ParseVersion` clamps each component.
- `IsTrustedAssetUrl` admits only `https://github.com/<repo>/releases/download/…`, in `ParseReleaseObj`
  and in `LaunchInstaller`, because the installer downloads and runs what those URLs name.
- Every check logs `[update]` lines to hooks.log through `Updater::LogUpdate` (the EXE-safe twin of
  `AppendStateLog`): the timer armed, each tick's begin and outcome tagged `(startup)` / `(periodic)` /
  `(cog)` / `(cog-silent)` (postponed with time left, up to date, available, skipped / declined), every
  prompt decision, installer launches and failures.
- No silent catch: every catch on this surface logs (with the target path for the file helpers), except
  the logger's own catch and a catch whose log line itself failed to build, both annotated in place.
