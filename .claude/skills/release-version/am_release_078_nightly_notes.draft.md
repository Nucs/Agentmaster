<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.7.8-prerelease-nightly release notes — NIGHTLY (the tier BELOW pre-release).
     Range: v0.7.7-prerelease..f5d1784ad = 34 commits (25 user-facing; 4 docs incl. the 0.7.7 notes
     archive, 2 test/build, 3 internal — the hot-deploy tooling 47ac26952 and two compile fixes
     d53213ff6 / 7ac3d50e6). Baseline = the previous release of ANY kind = the 0.7.7 pre-release
     (release-notes §2). RELEASING from f5d1784ad (= origin/agentmaster tip after pushing the 33
     unpushed mainline commits).
     Pre-validation: no local build for this cut (project rule: a build needs the user's permission).
     The "(NOT BUILT)" labels in the log are point-in-time and stale — the local Release layout's
     TerminalApp.dll (install 0.6.8.13) was built 2026-09-27 18:54, after the last code commit
     e43edd63b (09:21), and the Debug layout at 12:39 — so CI's from-scratch Release build is the
     remaining clean-build gate. Engine harness NOT re-run for this cut; last known at the 0.6.8.13
     tree: 4860 checks, 3 known test-side TestTaskbarBadge failures.
     NIGHTLY mechanics: the tag CONTAINS "nightly" -> release.yml strips the suffix to digits for the MSIX
     Identity Version (ver4 stays numeric 0.7.8.0, so ASSET NAMES carry no suffix) and auto-marks the release
     prerelease. The in-app updater offers it ONLY to users who accepted the cog's warning-gated "Allow
     updating to nightly builds (unstable)" switch — pre-release-opted users skip it. The one-command
     installer needs the FULL suffixed version (-Version maps to the TAG).
     Publish: gh release edit v0.7.8-prerelease-nightly -R Nucs/Agentmaster --draft=false --prerelease --latest=false
       (NEVER --latest; the stable 0.7.0 stays Latest).
     Assets: Agentmaster_0.7.8.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.7.8-prerelease-nightly (nightly)

> ⚠️ **NIGHTLY build** — an unstable development version. It may have memory leaks, CPU issues, and crashes. Install it only to help test, and be willing to have your work interrupted.

Nightlies are **not** offered to pre-release users — enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** (a warning confirm gates it) or install from the assets below. The stable **0.7.0** remains Latest for everyone else.

## What's new

- **The Agent Manager is a page behind the Home button.** The pinned Manager tab is gone: the board, tree and Auto Testing open from the tab strip's Home button, and any tab, Jump Back or a card double-click leaves the page.
- ⚠️ **Alt+Left and Ctrl+Home are Windows Terminal's again.** Both reach your shell and Claude as in stock Windows Terminal; the Home and Jump Back buttons are the way in and out.
- **A fleet side panel beside the terminal.** A panel that belongs to the window docks left of the terminal and lists this window's session cards, with a filter box, the board's sort and a LOCAL / GLOBAL toggle.
- **An unread count on the taskbar button.** A red number counts the sessions waiting for you across every window, and a turn that ends in a background window's selected tab now rings and counts.
- **A fork brings your unsent prompt along.** Forking a session types the source's unsent prompt into the fork's box, pasted blocks included, without sending it.
- **Copy hands over the pasted text.** Copy Current Prompt puts the pasted content on the clipboard instead of a `[Pasted text #N +M lines]` label, whenever it can prove which paste the label stands for.
- **A MOST RECENT sort.** The board, side panel and tree can sort by turn starts and ends, so a session busy with tool calls no longer floats above the one whose turn just ended.
- **The Launch folder picker uses the window.** The path picker under the Launch box grows up to 90% of the window's height and re-fits when the window is resized.

## What's fixed

- **No more 8-hour freeze on a tab switch.** The summary jump could spin forever on a conversation that repeated a prompt's opening words hundreds of times; the loop is now bounded.
- **Sessions no longer strand on Running after their turn ended.** The transcript reader keeps catching up on a large, unchanged file instead of parking mid-history.
- **Hooks no longer time out at launch and exit.** On Claude Code 2.1.139+ the hook forwarder runs PowerShell directly instead of Git Bash and your `BASH_ENV`.
- **The scanner no longer burns a third of a core.** One file-system watch on Claude's projects folder replaces its walks over every project folder.
- **Claude Code's Ctrl+G editor works again.** When the PATH nears cmd.exe's 8,191-character limit, Agentmaster trims the copy it hands every session.
- **Prompts Claude Code 2.1.277+ refused are cleaned first.** Everything Agentmaster types into a Claude box is cleaned of the "invisible" characters that make Claude cancel the Enter.
- **Hebrew and other non-ASCII prompts arrive intact.** The hook forwarder reads Claude's UTF-8 as UTF-8, and a non-ASCII Windows user name no longer breaks hook delivery.
- **/compact, /clear and resume behave.** A compaction no longer drops a running session to Idle, the context size updates when it ends, and summaries span the whole session.
- **Closing a tab no longer starts another Claude.** Focus goes to the nearest tab that starts nothing, or to the Agent Manager page.
- **Unsent drafts are read under Claude Code's fullscreen view.** The draft reader looks at the alternate screen too, and never mistakes Ctrl+G's editor or a pager for the input box.
- **Selecting a far-off tab scrolls the strip to it.** A tab already on screen never slides out from under your pointer, and moving the focused tab no longer marks its neighbor read.

## Install (nightly)

> ⚠️ **This is a nightly.** It is offered as an automatic update **only** if you enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** — enabling "Allow pre-release versions" is *not* enough. The stable **0.7.0** stays the default Latest.

**One command (PowerShell)** — installs this nightly (admin not required: as admin it trusts the self-signed cert silently; otherwise it asks — elevate once (UAC), a no-admin per-user portable install, or cancel):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.7.8-prerelease-nightly
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.7.8.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained — its first launch offers a per-user install or stays portable.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.7.8.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.7.8.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
