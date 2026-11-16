<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     ARCHIVED AS PUBLISHED 2026-11-16: the body below this header is now the release's body exactly as
     GitHub published it, saved before the one-row rewrite of every release replaced it there. The
     body this file held until then differed from the published one (a draft revised after, or never
     applied as written); it is this file at the commit before this note.
     Agentmaster 0.5.0 release notes. Cumulative since v0.4.1 (last published).
     Format matches prior notes: flat "- **Title.** desc." bullets, New + Fixes sections.
     REWRITTEN from a full read of ALL 99 commits in v0.4.1..HEAD (HEAD d2dfde801) cross-referenced
     against am_release_050_qa.md — the prior draft only covered the first ~43 commits and missed
     the interactive-pwsh host, sortable board, card title bands, HUD search box, Home/Jump-Back,
     Mark-Unread, batch-close dialog, the Sessions-browser additions, the tab-tearout/restart crash
     fixes, and the 7 newest commits. ~22 docs/README commits folded or omitted. The uncommitted
     tooltip-polish WIP (62 lines) folds into "Tooltips sharpened". Asset names assume tag v0.5.0 -> 0.5.0.0. -->
## Agentmaster 0.5.0

**0.5.0 is a big release.**

## What's new

- **An in-app updater.** Agentmaster checks GitHub Releases at startup and from Settings → Updates, and offers Update now, Postpone or Skip, with an opt-in for pre-releases.
- **Sessions never dead-end.** Every managed session runs inside an interactive PowerShell, so quitting the agent leaves a live prompt in its folder instead of a dead pane.
- **A sortable Triage Board, synced across windows.** The board sorts by Most active, Newest, Oldest or A–Z, and every global setting change reaches every window at once.
- **Color-coded board cards.** A card's title sits in a band of its folder's color, the color its tab wears.
- **A redesigned tab badge with a clickable Autopilot.** The badge's parts explain themselves on hover, and Autopilot is a button that cycles Off → Semi → Full.
- **The terminal search box joins the HUD.** Ctrl+Shift+F opens it in the badge's look, beside the badge, with a "Highlight all matches" toggle.
- **Home and Jump Back on the tab strip.** Home returns to the pinned Manager tab when it scrolls away, and Jump Back returns to the session you came from.
- **Mark Unread.** A tab's right-click item flashes its red attention ring until you visit it, even on the focused tab.
- **A refined attention flash.** A session that leaves Running while its tab is not focused grows a thin red ring that blinks with every other flagged tab.
- **More reach from the tab menu.** New Session Here and Fork session (renamed from Duplicate) open next to the clicked tab, and a Copy › submenu joins them.
- **One dialog for a batch close.** Closing several managed tabs or a window asks once: Delete All, Archive All or Cancel All.
- **The Copy submenu everywhere.** Session Id, Path, Branch, the CLIs, Summary and Transcript, from the board, the tab menu and the badge alike.
- **A faster Sessions browser.** A double-click jumps to an open session's tab or offers Resume / Fork for a closed one, and an Open filter lists the live sessions.
- **Board cards get a ⋯ menu and Jump to Tab.** And the selected card scrolls into view when you return to the Manager.
- **A smarter launch box.** A bare word fuzzy-matches your recent folders, and a path filters its subfolders by the typed leaf, like an address bar.
- **Trash a session.** Beside Archive, a trash action drops a session's record; the conversation on disk stays.
- **Flight Plan compose focus and prompt history.** Focus returns to the compose box after a send, and Up recalls earlier prompts.
- **Tab options honored, and legible tabs.** The close-button and middle-click settings apply to tabs, and tabs keep a 130 px minimum width.
- **A silent profile on first launch.** The first run picks the install's default profile without asking.
- **Sharper tooltips.** Every Manager, cog, menu, badge and page control says what it does on hover.

## What's fixed

- **State no longer flaps between Idle and Running.** A long tool or an API retry keeps the dot Running instead of flickering.
- **Clicks on content no longer drag the window.** Only the real title bar moves it.
- **Tab tear-out fixes.** Tearing out a never-focused tab no longer crashes, and the new window hosts the moved tab.
- **Restart on a never-shown restored tab no longer crashes.** The session simply starts when the tab is first shown.
- **The summary panel no longer crashes or shows empty.** A vigorous resize no longer hits a layout cycle, and a message that starts with a newline shows.
- **A complete Ctrl+F passthrough.** Claude's TUI gets both the key-down and the key-up.
- **"Launch Claude" no longer sticks disabled.** The Launch box re-validates every folder Agentmaster drops into it.
- **Installs over a conflicting registration work.** The installer and updater remove a registration that blocks with 0x80073CFB, keeping your data, and retry.
- **Smaller fixes.** The pinned Manager tab can no longer be renamed, and the Sessions range popup closes when the pointer leaves it.

## Install

**One command (PowerShell)** — install or upgrade to 0.5.0 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.5.0
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). *Already on 0.5.0+? Just use **Settings → Check for updates** (or the startup prompt).*

**Portable (no cert):** download `Agentmaster_0.5.0.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.5.0.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.5.0.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
