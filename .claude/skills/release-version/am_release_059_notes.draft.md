<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.5.9 release notes — PRE-RELEASE.
     Style: matches 0.5.7 (tight intro, no "fork of Windows Terminal" preamble, no "## Capabilities",
     "## New in X.Y.Z" + "## Fixes" + "## Install (pre-release)").
     Range: v0.5.8..HEAD = 89 commits (13 docs/refactor/chore/test omitted from user notes).
     RELEASING from HEAD = c42442a89. Tag v0.5.9 -> 0.5.9.0.
     PRERELEASE mechanics: publish --prerelease --latest=false (the stable 0.5.5 stays Latest).
     Asset names: Agentmaster_0.5.9.0.msixbundle, Agentmaster_0.5.9.0_x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.5.9 (pre-release)

**0.5.9 is a pre-release** building on 0.5.8 — a large one. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.5.5** remains the default for everyone else.

## What's new

- **Unsent drafts show as 3 dots.** A Claude tab holding a prompt you typed but never sent shows a pulsing 3-dot mark on the tab strip and the board, across windows.
- **API errors become an Error state.** A turn that fails on a rate limit, an overload or a 5xx reads Error with its message and status, and recovers on the next activity.
- **Settings in six tabs.** The Agent Manager's cog groups launch, autopilot, tabs, behavior, environment and updates into six tabs.
- **The summary follows the conversation across files.** It shows the sessions that led to the current one across /clear, /compact and plan restarts, and refreshes as the transcript grows.
- **Git worktrees in the Launch picker.** The path picker lists your worktrees with their branches and marks the current one.
- **Edit a session's title in place.** Edit Title, or a slow double-click in the Sessions browser, renames a session, and the browser gains a Ctx column.
- **Dormant dots, Activate All Tabs and Move to a state.** A half-hollow dot marks a session not started yet, Activate Tab / All Tabs start sessions, and Move to Idle / Done / Waiting-for-you sit on the card and tab menus.
- **A configurable unread ring.** The status dot's flash ring takes any color and opacity (red at 80% by default).
- **Crown or Star for favorites.** Settings picks the marker a favorited session's tab wears.
- **Overlay opacity at rest and on hover.** A two-thumb slider sets each one.
- **Shift+Home toggles the Agent Manager.** It jumps to the Manager tab and back, wherever focus sits.
- **A persistence-aware close-window confirm.** Closing a window offers Close All Windows, and "Don't ask me again" is gone.
- **Rewound branches leave the history.** Branches you backed out of with a double Esc no longer show in the summary, the title or the Transcript copy.
- **A launch-timing trace.** `[startup]` lines in hooks.log stamp every launch phase on one clock.
- **More accurate prompt jumps.** Alt+↑ / Alt+↓ check Claude Code's prompt glyph before jumping.
- **Per-window Manager tab color and a tidier tab menu.** The Manager tab's color is kept per window, and the tab menu is grouped with separators.
- **AGPL-3.0-or-later + commercial.** The project is now dual-licensed.

## What's fixed

- **Resume, Fork and restore open the exact session you picked.** A timing heuristic could jump to an unrelated live tab or merge a restored tab into another session; it is gone.
- **App-wide crash hardening.** A tooltip timer firing on a detached element no longer crashes the app, and no transcript reader's failure can escape its worker.
- **Forks keep their content.** A never-messaged fork re-forks from its source on restore, and its summary shows the parent's transcript.
- **The board behaves in the background.** A background board no longer steals the foreground, a double-click activates any card, and a click keeps a column's scroll.
- **Tooltips stop flickering.** Tips no longer close and reopen on every pointer move, hide at once, and never eat a click.
- **Summaries are more accurate.** Teammate protocol chatter is dropped while real teammate reports stay.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.5.5** stays the default Latest.

**One command (PowerShell)** — install 0.5.9 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.5.9
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.5.9.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.5.9.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.5.9.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
