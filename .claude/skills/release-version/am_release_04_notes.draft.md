<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     ARCHIVED AS PUBLISHED 2026-11-16: the body below this header is now the release's body exactly as
     GitHub published it, saved before the one-row rewrite of every release replaced it there. The
     body this file held until then differed from the published one (a draft revised after, or never
     applied as written); it is this file at the commit before this note.
     Agentmaster 0.4.0 release notes. Cumulative since the last PUBLIC release (v0.3.0, the
     current "Latest" on GitHub). Format matches the v0.3.0 notes: flat "- **Title.** desc."
     changelog bullets — now with a dedicated Fixes section. Asset names assume tag v0.4.0 -> 0.4.0.0.
     Drafted from a one-by-one read of every commit in v0.3.0..HEAD (~52; docs-only CLAUDE.md
     commits omitted as internal) cross-checked against the dev-session transcripts. -->
## Agentmaster 0.4.0

**0.4.0 deepens the per-session lens and hardens the engine.**

## What's new

- **A summary panel on every tab.** It shows the session's plan, tasks, numbered prompts and the files it read, created and edited.
- **A hover action row on the badge.** Open Path opens the working folder, and a copy menu copies the session id, path, branch, real launch command and summary.
- **The working folder and branch on the badge.** A second line shows the session's folder and its live git branch.
- **The Manager follows your tab, both ways.** Switching to a session's tab selects it in the Manager, and selecting a card drops its folder into the Launch box.
- **The Manager pills the session's tab.** On the Manager tab, the hovered or selected session's terminal tab wears an accent pill.
- **Keyboard navigation.** F2 renames the focused tab, Alt+Left jumps to the Agent Manager, and Alt+Right cycles through the window's tabs.
- **Commit a rename from the keyboard.** Shift+Enter commits a multi-line title by default, and clicking away always commits.
- **Permanent per-folder tab colors.** A folder keeps the same tab color across tabs, windows and restarts, and two folders never share one while colors remain.
- **Adopt an external session by forking a copy.** Fork a copy (the default) or Resume anyway, so a still-running session never gets two writers.
- **Resume where you left off.** Restoring a /clear-ed, plan-restarted or handed-over conversation opens its newest part, not its first.
- **Hide sessions and refresh the browser.** Hide from list drops a session from the Sessions browser (reversible), and ↻ re-scans.
- **A rich detail pane in Sessions.** Selecting a session shows its full summary box: plan, tasks, prompts, files and durations.
- **Move a tab to the start or end.** And a fork's title counts up: (fork), (fork 2), (fork 3).
- **Manager and menu polish.** Board cards are borderless at rest, tooltips open about 3× faster, and the Flight Plan opens at the latest message.

## What's fixed

- **No more /clear title flood.** A /clear no longer swaps the tab title back and forth forever, spinning the CPU and swelling hooks.log.
- **Claude launches on an npm-only install.** Agentmaster launches the native claude.exe by full path instead of a bare name.
- **Codex launches on a .cmd or .bat install.** Its launcher is resolved to a full path, and a .cmd runs through cmd /c.
- **Duplicating a Codex tab forks Codex.** It no longer spawns a stray Claude.
- **Autopilot recovers an eaten Enter.** It re-presses a lone Enter until the turn starts, never re-sending the text.
- **Delegating tabs read Running.** A turn handed to a subagent reads Running instead of Idle.
- **Sessions no longer stick on Running.** Claude's own idle heartbeat and a quiet transcript release a finished session.
- **"Needs you" clears after you answer.** An answered or approved session goes back to Running.
- **Closing a plain tab asks first.** A deliberate close of a shell tab with no managed session always confirms.
- **Keyboard focus stays on the clicked card.** A background rebuild no longer drops it.
- **Two folders no longer share a tab color.** Colors are collision-free, and existing maps are fixed once.
- **No empty summary box flash.** The panel stays hidden until it has content.
- **Codex resume targets the right rollout.** A reused process id no longer maps a tab to the wrong rollout.

## Install

Paste into PowerShell — installs **v0.4.0** (one UAC prompt, only to trust the signing cert):

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.4.0
```

Add `-Portable` for a cert-free, no-admin install, and/or `-Launch` to start it right after:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.4.0 -Portable -Launch
```

**Portable (recommended, no cert):** download `Agentmaster_0.4.0.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):**
1. Download `Agentmaster.cer` + `Agentmaster_0.4.0.0.msixbundle`.
2. Trust the cert once (admin PowerShell):
   `Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`
3. `Add-AppxPackage .\Agentmaster_0.4.0.0.msixbundle` (or double-click it).

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading from any earlier version keeps your data — pick **Production** in the first-launch profile picker (your existing `%USERPROFILE%\.agentmaster` carries over; Terminal settings are seeded in automatically). Portable-zip users: state lives inside the unzip folder — copy `%USERPROFILE%\.agentmaster` into `<unzip>\profile` to bring sessions along.

Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally [Codex](https://openai.com/codex/) — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
