<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.6.0 release notes — STABLE (becomes Latest; NOT a pre-release).
     COMPREHENSIVE: contains the full changelog since the last stable (0.5.5) — the 0.6.0 new work
     up top, then each 0.5.6-0.5.9 pre-release's complete New/Fixes/Thanks (Install sections stripped).
     Range: v0.5.9..HEAD = 74 new commits; v0.5.5..HEAD = 222. RELEASING from HEAD = 69ae35f6e (code 2f6e1d3fa).
     Tag v0.6.0 -> 0.6.0.0. STABLE mechanics: publish --draft=false --latest (REPLACES 0.5.5 as Latest; no --prerelease).
     Assets: Agentmaster_0.6.0.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.0

**0.6.0 is a stable release** — the first stable since 0.5.5, bringing the entire 0.5.6 → 0.5.9 pre-release line to everyone plus a large batch of new work. This build becomes the new **default (Latest)** — no "allow pre-release" toggle needed.

## What's new

- **Bookmark tags.** Named, colored tags on any session show as ribbons on its tab, on its board card and in a Tags column of the Sessions browser.
- **Three tab-coloring modes.** A tab's color can follow its working folder, its own session, or the folder the session really works in.
- **Activate All Tabs and Shift+Click.** Restored tabs start lazily: Shift+Click starts a dormant tab in place, and Activate All Tabs wakes the fleet a few at a time.
- **A launch splash.** A light splash window covers the workspace restore at startup, so a launch no longer looks hung.
- **A rewritten rich tab tooltip.** Hovering a tab shows a summary-style card under it, hardened against the crashes the old one had.
- **Auto Testing becomes a developer tool.** The prompt-queue automation is renamed Auto Testing, and a release build's Manager pane shows a read-only session Summary instead.
- **Unsent drafts show as 3 dots.** A Claude tab holding a prompt you typed but never sent shows a pulsing 3-dot mark on the tab strip and the board.
- **API errors read Error.** A turn that fails on a rate limit, an overload or a 5xx shows Error with its message and status, and recovers on the next activity.
- **Settings in six tabs.** The Agent Manager's cog groups its settings into six tabs instead of one long scroll.
- **The summary follows the whole conversation.** It shows the sessions that led to the current one across /clear, /compact and plan restarts.
- **Git worktrees in the Launch picker.** The path picker lists your worktrees with their branches.
- **Edit a session's title in place.** Edit Title, or a slow double-click in the Sessions browser, renames a session.
- **Keep Awake.** A toolbar toggle keeps the PC and display awake: Off, Always, or only while a session runs.
- **Favorites replace the Archive.** A tab's lifecycle is Close and Favorite, and the Sessions page is the one browser for your history.
- **Context usage on board cards.** Each card shows how many tokens its conversation carries, so a session near auto-compact stands out.
- **Environment variables, global and per folder.** Settings merges them into every Claude and Codex session you launch.
- **A navigation trail and timestamped logs.** Every action writes one `[nav]` line to hooks.log, and every log line carries its time.
- **AGPL-3.0-or-later + commercial.** The project is dual-licensed.

## What's fixed

- **A launch no longer stalls on an invisible question.** The "reopen your N windows?" prompt could render invisibly and hang the launch; it is a visible prompt now.
- **Copy really copies.** Copy Summary and every overlay or board copy no longer fail silently from a focused tab.
- **A crash reopen keeps your triage state.** Running and Waiting-for-you survive it, and nothing is sent into a tab that has not started.
- **A 320 MB log flood is fixed.** A notify flood and redundant writes no longer swell hooks.log, and a worker thread's exception can no longer end the process.
- **Close confirmations can no longer be silenced for good.** And the splash no longer floats on top of other windows.
- **Resume, Fork and restore open the exact session you picked.** A timing heuristic no longer jumps to an unrelated tab or merges a restored tab into another session.
- **Forks keep their content.** A never-messaged fork re-forks from its source on restore.
- **No freeze when you switch keyboard layout, or interrupt a subagent turn.** Both used to lock the window, for seconds or for minutes.
- **"Last active" is accurate.** It comes from the last real conversation line, so a resume no longer makes an idle session read "just now".

## Install

**One command (PowerShell)** — installs 0.6.0 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.0
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). 0.6.0 is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable (no cert):** download `Agentmaster_0.6.0.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.0.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.0.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
