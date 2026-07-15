<!-- Agentmaster 0.6.5 release notes — PRE-RELEASE (NOT Latest; v0.6.1 stays Latest).
     PRERELEASE NOTES RULE: show ONLY this prerelease's OWN explicit delta (not a comprehensive
     changelog — the full accumulation happens when a STABLE release is cut). See the release-notes skill §2.
     0.6.5's explicit delta over 0.6.4 = the tab/overlay/restart batch held back in the abandoned 0.6.2
     pre-release + the newer state/triage commits. It deliberately does NOT re-list the 23 GB leak fix +
     hourly update (shipped in 0.6.3) or the resume crash-loop fix (shipped in 0.6.4).
     Mainline tip 369e02ef8. PRERELEASE mechanics: publish --draft=false --prerelease --latest=false (0.6.1 stays Latest).
     Assets: Agentmaster_0.6.5.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.5 (pre-release)

**0.6.5 is a pre-release that ships the tab / overlay / restart batch** that was held back in the abandoned 0.6.2 pre-release, plus newer session-state and triage improvements. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.6.1** remains the default for everyone else.

## New in 0.6.5

- **"Remove colors" tab-coloring mode.** A fourth option under **Settings → Tabs → Tab coloring**: no tab is colored at all — but your saved per-directory / per-session colors are **kept** (not erased), so switching back to any colored mode restores exactly the colors your fleet had. It's strip-wide (every non-managed tab's color is *suspended*, never voided) and "Change tab color" is disabled while the mode is active.
- **Hide the profile icon on tabs.** A new global **"Show icons on tabs"** setting (default **off** — an Agentmaster change from stock Windows Terminal) drops the profile icon from every tab, so a tab reads on its status dot + title. Toggles live.
- **The tab strip reveals a selected session's tab.** Selecting a session on the Triage Board or Explorer tree now scrolls the tab strip so that session's terminal tab is visible — completing the tab-strip half of the Linked-Lenses selection sync (previously the highlight pill could sit scrolled off-screen with many tabs). It's virtualization-aware and lands the tab clear of the overlay's scroll / **+** buttons.
- **A still-running shell, background agent, or teammate keeps the session "Running."** Work that *outlives the turn* — a live shell job, a `run_in_background` agent, or a Claude teammate writing to the lead's side files for minutes after the lead's turn ends — now correctly reads **Running** instead of dropping to Idle / Done / Waiting-for-you, and settles back to Waiting only once that background work actually quiets.
- **Dismiss an Error card by hand.** The triage **Move to Idle** / **Move to Done** actions now extend to the **Error** state on both the Triage Board and the Explorer tree, so you can clear an errored session you've already dealt with.
- **Ctrl+Home** toggles the Agent Manager (home / back), rebound from Shift+Home.

## Fixes

- **"Restart session" actually restarts.** Restarting a managed session could print a `[process exited with code 259] … press Enter to restart` banner into the pane, then **falsely archive** the freshly-restarted session ~1.4 s later, and relaunch an **empty** conversation (a forked session silently losing its branch). Five distinct defects behind that were fixed; Restart now cleanly re-points the connection with no banner, no false archive, and a fork keeps its branch.
- **A marked triage state survives opening the tab.** Marking a background session **"Move to Waiting-for-you"** (or **Mark Unread**) and then activating it for the first time reset it straight back to **Idle** — because focusing a lazily-restored tab starts its `claude`, whose `SessionStart` hook unconditionally forced the state to Idle. A lazy-start `SessionStart` now **preserves** an at-rest "needs-you" triage (this also reinforces the crash-restore state preservation).
- **Overlay buttons register clicks on their icons.** Clicking the *glyph* of a per-tab overlay button (e.g. the summary-panel **pencil**) could miss because the icon ate the hit instead of the button — fixed, and the pencil now gives clear visual feedback (a state-colored glyph, plus an empty-panel placeholder) on every click.
- **Teammate & background-agent traffic no longer pollutes your history or fakes the cache hint.** When a Claude teammate or a background agent delivers a message to a session, that delivery fires a real prompt-submit on the receiving session — which used to record the wrapper text into that session's **Sent/Typed** history (and persist it to `sessions.json`) and light the Triage-Board **⚡ "still cached"** hint even though the teammate's turn ran in its own context. Corpus-audited across both delivery shapes, that machine traffic is now noise-gated out of the Typed record, and the ⚡ hint keys strictly on the receiving session's own API turns.
- **Consistent tab-strip height.** A tab with a multi-line title no longer changes the tab-strip height depending on scroll position.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.6.1** stays the default Latest.

**One command (PowerShell)** — installs 0.6.5 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.5
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.6.5.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.5.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.5.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
