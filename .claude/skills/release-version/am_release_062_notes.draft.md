<!-- Agentmaster 0.6.2 release notes — STABLE (becomes Latest; NOT a pre-release).
     Range: v0.6.1..HEAD = 14 commits. Last stable is 0.6.1, so this IS the full changelog since last stable.
     RELEASING from d2f9937bc (tag v0.6.2). Tag v0.6.2 -> 0.6.2.0.
     STABLE mechanics: publish --draft=false --latest (REPLACES 0.6.1 as Latest; no --prerelease).
     Assets: Agentmaster_0.6.2.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.2

**0.6.2 is a stable release** building on 0.6.1. The headline is a **second major memory-leak fix** — a **23 GB Manager-rebuild leak** (following 0.6.1's tooltip-leak freeze) — alongside a batch of managed-tab, restart, and overlay fixes, plus three tab conveniences: a **"Remove colors"** coloring mode, an option to **hide the profile icon on tabs**, and a tab strip that **scrolls to reveal a selected session's tab**. This build becomes the new default **Latest**.

## Fixes

- **The 23 GB Manager memory leak (the big one).** After 0.6.1 fixed the tooltip-leak freeze, a *second* leak remained: a C++/WinRT reference cycle in the Triage-Board / Explorer-tree session context menu (the menu held its host card/row strongly, and the menu's **Tags** item held the host strongly back — an uncollectable cycle under COM refcounting). Because the Manager rebuilds its board and tree on every registry update (plus a 30 s backstop), each rebuild leaked the entire discarded card/row subtree — measured at roughly **85 MB/minute** under a busy fleet, reaching ~23 GB. The menu is now weak-anchored to its host, animation storyboards are stopped instead of left running forever, and every insert-only cache is bounded — so a long-lived Manager no longer grows.
- **"Restart session" actually restarts.** Restarting a managed session could print a `[process exited with code 259] … press Enter to restart` banner into the pane, then **falsely archive** the freshly-restarted session ~1.4 s later, and relaunch an **empty** conversation (a forked session silently losing its branch). Five distinct defects behind that were fixed; Restart now cleanly re-points the connection with no banner, no false archive, and a fork keeps its branch.
- **A marked triage state survives opening the tab.** Marking a background session **"Move to Waiting-for-you"** (or **Mark Unread**) and then activating it for the first time reset it straight back to **Idle** — because focusing a lazily-restored tab starts its `claude`, whose `SessionStart` hook unconditionally forced the state to Idle. A lazy-start `SessionStart` now **preserves** an at-rest "needs-you" triage (this also reinforces the crash-restore state preservation).
- **Overlay buttons register clicks on their icons.** Clicking the *glyph* of a per-tab overlay button (e.g. the summary-panel **pencil**) could miss because the icon ate the hit instead of the button — fixed, and the pencil now gives clear visual feedback (a state-colored glyph, plus an empty-panel placeholder) on every click.
- **Consistent tab-strip height.** A tab with a multi-line title no longer changes the tab-strip height depending on scroll position.

## New

- **"Remove colors" tab-coloring mode.** A fourth option under **Settings → Tabs → Tab coloring**: no tab is colored at all — but your saved per-directory / per-session colors are **kept** (not erased), so switching back to any colored mode restores exactly the colors your fleet had. ("Change tab color" is disabled while this mode is active.)
- **Hide the profile icon on tabs.** A new global **"Show icons on tabs"** setting (default **off** — an Agentmaster change from stock Windows Terminal) drops the profile icon from every tab, so a tab reads on its status dot + title. Toggles live.
- **The tab strip reveals a selected session's tab.** Selecting a session on the Triage Board or Explorer tree now scrolls the tab strip so that session's terminal tab is visible — completing the tab-strip half of the Linked-Lenses selection sync (previously the highlight pill could sit scrolled off-screen with many tabs). It's virtualization-aware and lands the tab clear of the overlay's scroll / **+** buttons.
- **Hourly update check.** With auto-update enabled, Agentmaster now re-checks for a new version **every hour while running**, not only at startup.

## Install

**One command (PowerShell)** — installs 0.6.2 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.2
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). 0.6.2 is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable (no cert):** download `Agentmaster_0.6.2.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.2.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.2.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
