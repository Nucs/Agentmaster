<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     ARCHIVED AS PUBLISHED 2026-11-16: the body below this header is now the release's body exactly as
     GitHub published it, saved before the one-row rewrite of every release replaced it there. The
     body this file held until then differed from the published one (a draft revised after, or never
     applied as written); it is this file at the commit before this note.
     Agentmaster 0.6.8 release notes — PRE-RELEASE (NOT Latest; v0.6.1 stays Latest).
     PRERELEASE NOTES RULE: show ONLY this prerelease's OWN explicit delta vs the previous release (0.6.7).
     ~46 commits v0.6.7..c236f9012, themed. Mainline tip = c236f9012 (origin in sync).
     Headliners: tab-title naming conventions (659ea2207+5615b8dbd), in-app Debug Mode (c236f9012+ddba06a1a),
     one-click Claude install (2c292ac8f), worktree-shared tab color (829db210a), toast click->foreground+jump
     (c04f9cf9a+4e6da3cc6), LocalTooltip panels, home-dir infer; + a big stability/crash + perf batch.
     Omitted (internal/dev/no-op): logging/nav, exception-forensics, the non-virtualizing swap+revert (net-zero),
     dev-only splash/brand text, docs. PRERELEASE mechanics: --draft=false --prerelease --latest=false (0.6.1 stays Latest).
     Assets: Agentmaster_0.6.8.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.8 (pre-release)

**0.6.8 is a large pre-release.** Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.6.1** remains the default for everyone else.

## What's new

- **Tab-title naming.** Settings → Tabs → "Tab title naming" picks how an untitled tab's title comes from its folder (its last word by default), with a live preview.
- **Debug Mode.** Settings → About → Enable Debug Mode turns on the developer-only Auto Testing in a release install.
- **One-click Claude install.** When no claude.exe is found, the "Claude Code not found" prompt installs or migrates Claude in one click.
- **A git repo and its worktrees share one tab color.** Worktrees wear their repository's color, so they read as one at a glance.
- **Click a notification to jump to its session.** A toast click brings the window to the front and switches to the session's tab.
- **Home-folder sessions infer their working folder.** A session launched in your home folder takes the folder its tool calls concentrate in for its color, grouping and Open New Session Here.
- **Hover help in a fixed panel.** The Settings cog, the Sessions page and the Manager show their hover help in a side panel instead of floating tooltips.

## What's fixed

- **Closing a window or dragging a tab no longer crashes.** Every UI-thread teardown path is guarded, and so is the tab strip's drag-start lookup.
- **No runaway auto-scroll.** Releasing a selection outside the terminal no longer leaves it scrolling.
- **Truer notifications.** A "waiting for you" toast waits while a shell, background agent or teammate still works, and a session gets at most one toast per 20 s.
- **/model no longer flips a session to Running.** A local slash command on an idle or waiting session leaves its state alone.
- **Long tab titles show in full.** The title limit is now 255 characters instead of a 30-character display cap.
- **"Copy Launch CLI" pastes and runs.** The copied command starts with PowerShell's call operator.
- **A lighter UI.** Tab tooltip cards are built on hover instead of every tick, and the Manager's refresh and the sessions autosave are throttled.
- **Closing the splash aborts the launch.** The starting instance shuts down instead of starting in the background.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.6.1** stays the default Latest.

**One command (PowerShell)** — installs 0.6.8 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.8
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.6.8.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.8.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.8.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
