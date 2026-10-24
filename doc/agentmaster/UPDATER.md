# The in-app updater

A GitHub-release self-updater for the packaged release install and for portable copies. Code:
`AgentMaster/Updater.h` (header-only, plain Win32 like `ProfileBootstrap.h`, so the WindowsTerminal EXE
includes it without the engine lib), called from `WindowEmperor` and from the Settings cog's About tab.

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

- A TaskDialog when a strictly newer eligible release exists: **Update now** / **Postpone**, over six
  radios choosing what Postpone means:
  - **Remind me next restart** (the default, and what Cancel / X / Esc / any failure resolve to): writes
    nothing durable; it latches the process-scoped `AGENTMASTER_UPDATE_DECLINED=<tag>` env var, so the
    startup and hourly checks are skipped, before any network call, until the next launch (which clears
    an inherited latch). The cog's manual check stays live. Each skipped tick still logs.
  - **Remind me tomorrow:** the next local 08:00 (within 24 h; DST-safe; the radio names the instant,
    e.g. "Remind me tomorrow (Sun, 8:00 AM)").
  - **3 days · 7 days · 30 days · Skip this version.**
- A postponed or skipped version stays silent until it expires; the same gates apply to the startup and
  hourly checks.

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
