<!-- ⚠ NOT RELEASED (2026-11-03): the e2e gate FAILED. Build server build b20261103-1 (purged Release of 89cbb9e26)
     + the whole suite r20261103-135156-5b0994 = 205 tests, 190 passed, 15 failed. One failure is a confirmed
     regression against v0.8.1: SP-K2 - with Sum off, hovering a side-panel card opens the rich session card beside the
     panel (test_sum_off_card_hover_opens_no_rich_card: 3/3 here, passes on v0.8.1's b20261029-1; suspect 7bbb63137's
     side-panel AI work). The other 14 are flaky, pre-existing on v0.8.1, expected from a fix (PQR-K11) or Claude
     Code's own dialog (memory e2e-gate-2026-11-03-v082). No tag was pushed and no release.yml run was started.
     Before cutting: fix SP-K2 (or the user rules the card may show), rebuild, re-gate, then update the commit and the
     gate lines below and add the fix to the notes.
     Agentmaster 0.8.2-prerelease release notes — PRE-RELEASE (the stable 0.8.1 stays Latest).
     Range: v0.8.1..89cbb9e26 = 37 commits. 21 user-facing: 13 feat + 8 fix. 16 internal: 10 docs (the 0.8.1 notes
     archive, the Builds table, the private-data scrub's hash re-pointing, the Telegram SPEC, the Home-button freeze
     study, the voice billing facts), 1 test (LIF-06), 5 build-server commits (amb install, amb --help, capped e2e
     VMs, the no-slot admission fix, the disk-backed slot RAM measurement), plus c41e78ba0 (a build fix inside the
     range). Baseline = the previous NON-nightly release = the 0.8.1 stable (no release since).
     RELEASING from 89cbb9e26d2704b217c104980208de73d458fb70 (= origin/agentmaster tip; its ci.yml run 41594083557,
     x64 Release from scratch: success).
     Gate (the user's ask, 2026-11-03: "Build and run e2e on build server, if passes, release a new version as
     prerelease"): build server build <BUILD ID> (purged Release, release identity) + the whole e2e suite on it
     (<E2E REQUEST IDS>), the suite pinned to 89cbb9e26's e2e/ (Q:\agentmaster-e2e\suites\89cbb9e26\e2e).
     Version: tag v0.8.2-prerelease -> ver4 0.8.2.0, rawver 0.8.2-prerelease. The updater compares major.minor.patch,
     so 0.8.2 is offered over 0.8.1 to users who allow pre-releases. nightly=false: release.yml does NOT mark it a
     pre-release — mark it at publish. The installer's -Version maps to the TAG (0.8.2-prerelease).
     Tag:     git tag -a v0.8.2-prerelease -m "Agentmaster 0.8.2-prerelease" 89cbb9e26d2704b217c104980208de73d458fb70
              env -u GH_TOKEN -u GITHUB_TOKEN git push origin v0.8.2-prerelease
     Notes:   strip this comment (awk 'f{print} /^## Agentmaster 0.8.2-prerelease/{f=1; print}' <draft> > body.md), then
              gh release edit v0.8.2-prerelease -R Nucs/Agentmaster --notes-file body.md
     Publish: gh release edit v0.8.2-prerelease -R Nucs/Agentmaster --draft=false --prerelease --latest=false
              (NEVER --latest; the stable 0.8.1 stays Latest).
     Assets: Agentmaster_0.8.2.0.msixbundle, _x64.zip, _arm64.zip, _x64_symbols.zip, _arm64_symbols.zip, Agentmaster.cer. -->
## Agentmaster 0.8.2-prerelease

**0.8.2 is a pre-release** building on 0.8.1. Two Claude Code mods now ship inside Agentmaster and load into every Claude tab it starts: **file-links** (file paths in Claude's replies become clickable links) and **better-verbose-bash** (a shell call's reason heads its row in the verbose views). You can **name a window**, and its taskbar preview, Alt+Tab and Telegram show that name. The update prompt **shows the release notes**, the side panel gets the **AI search**, and Show Transcript draws **Markdown tables** and explains its levels. A queued prompt of over 800 characters on one line (a long Telegram message, a voice note's transcript) is **delivered again**. Enable **Settings → About → "Allow updating to pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.8.1** remains the default Latest for everyone else.

## New in 0.8.2

- **Claude Code mods ship with Agentmaster.** Every Claude tab Agentmaster starts (a new tab, a resume, a fork, a restart, a handover, a preset) loads two mods:
  - **file-links**: file paths and names in Claude's replies become clickable `file:///` links, for files that exist (Claude Code 2.1.287 or later).
  - **better-verbose-bash**: in the views that show a shell call in full (`--verbose`, ctrl+o), the reason Claude gave for the call heads its row (Claude Code 2.1.308 or later).

  A mod stands aside when you run your own copy of it (enabled in your Claude Code settings, or listed in `CLAUDE_CODE_PLUGIN_DIRS`), and when the tab's claude.exe is older than the mod needs. **Settings → Claude → MODS IN CLAUDE TABS** has a switch for each and says what the next tab will do with it.
- **Name a window.** Use **Rename window…** in the window menu (right-click a taskbar preview or the title bar's empty strip, or press Alt+Space), or the name in the Agent Manager header. A named window's caption is its name alone, so its taskbar preview, its Alt+Tab entry and its taskbar button show it. The name is saved, and comes back when the window reopens. In Telegram, the new-thread wizard's window step shows each named window by its name and accepts the name typed like the number; the AI search names windows the same way.
- **The update prompt shows the release notes.** The update prompt becomes a dark window of its own, with the release notes on the left: a tab per release on your channel, the version you run under a "▲ you" arrow, the updates in green, older releases dimmed. The choices on the right are the ones you know. Without WebView2 the notes show as plain text. You will see it at the first update offered after 0.8.2.
- **The AI search in the side panel.** An **AI** toggle inside the side panel's filter box turns the box into the AI search's request line, and the card list into its answer: the sessions it found, as side panel cards, each with the model's reason.
  - A click selects a session and peeks its summary; a double-click jumps to its tab, or resumes a closed session as a new tab; a right-click opens the session menu.
  - It is the same conversation as the Sessions page's AI search.
  - Its hidden Claude Code process starts only when you click into the box or send a request, and stops 120 minutes after its last use (**Settings → Sessions → AI SEARCH**).
- **Show Transcript, improved.**
  - GitHub-style **Markdown tables** in Claude's replies draw as real tables: a framed grid, a header band, striped rows, selectable cells, each column's alignment.
  - An **(i)** after the level tabs opens a guide comparing what each level shows, and each level's tab has a tooltip that sums it up.
  - **Compact** lists every tool call on one line with the reason Claude gave for it (`Bash: Count lines of transcript sources`), else what it acted on (`Read: app.cfg`).
  - The separators the view writes itself are a plain hyphen instead of an em dash, in **Copy ▸ Transcript** too; Claude's own text is untouched.
- **Telegram.**
  - **New topic icons:** Running ✍️ and Waiting for you 🔥 (existing topics change quietly), and the Search topic wears 🔎.
  - **📜 All Screen ▸ gets a Last Turn box:** checked, any level draws only the conversation's newest turn, numbered as in the whole conversation. It combines with File Diffs. `/allscreen [level] [diffs] [last]` does the same.
  - **The card's screen follows the status.** The card looks at its screen again after a change into a state at rest, and after its window was too busy to answer in time; only a changed picture is uploaded. A tab that starts (activated, launched or restarted) gets one picture once Claude Code's screen shows, never a black one.
- **File links through your own address.** The hosted mode's address no longer defaults to the author's site: it is the placeholder `https://example.com` until you enter yours, and the cog warns while the placeholder is in effect. **Settings → Messaging → FILE LINKS → Copy AI Impl Instructions** copies a brief, with your install's facts filled in, that an AI agent on your PC follows to set that address up (a Cloudflare Tunnel, Tailscale Funnel or a reverse proxy) and to test it.

## Fixes

- **A long one-line prompt was never delivered.** Claude Code shows a paste of over 800 characters with no line break as a bare `[Pasted text #N]`, which the verified send took for foreign text. Every such queued prompt (a long Telegram message, every voice note's transcript) failed as "not delivered" and left the placeholder in the input box. Such prompts are delivered now. Telegram's 👎 reply on a failed prompt also states the runner's real mode, instead of always "the runner paused".
- **A dim suggestion counted as a draft.** A guess that Claude Code draws dim in an empty input box (its own, or a mod's such as next-prompt) showed the 3 dots of an unsent draft. It was remembered, carried into forks and typed back after a restart. Dim text no longer counts as a draft.
- **Closing a "Waiting for you" tab leaves the session Idle.** Every close you make (the tab's ✕, the batch closes, a pane close) moves a Waiting-for-you session to Idle and clears its Mark Unread. A later resume no longer brings it back as Waiting, and its Telegram topic leaves as closed. A window close or a quit keeps the state.
- **The AI search failed at "thinking off".** On Claude Code 2.1.310 every request at off fails. The thinking toggle now rotates low → medium → high → xhigh → max, the default is low, and a stored off runs as low.
- **The Sessions search box keeps its text.** Turning AI on or off, and the ⊘ reset, no longer empty the box. With AI on, the typed text waits as the unsent request; with AI off, it filters the list.
- **No message box from the command line.** `agentmaster --help` (`-h`, `-?`, `/?`) and `--version` (`-v`), typed in a console or run by a script that reads the output, print there instead of opening the app's message box. `--version` adds the app's version.

## Worth knowing when you upgrade from 0.8.1

- ⚠️ **Two mods load into every Claude tab by default.** Turn either off under **Settings → Claude → MODS IN CLAUDE TABS**; a tab already open keeps what it started with. Your own installed copy of a mod always wins.
- ⚠️ **The hosted file-link mode needs your own public address.** Until you enter one, hosted links point at the placeholder and reach nothing.
- ⚠️ **The AI search has no "thinking off" any more.** A stored off runs as low.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → About → "Allow updating to pre-release versions."** The stable **0.8.1** stays the default Latest.

**One command (PowerShell)** — installs 0.8.2-prerelease (administrator rights not needed; it asks where Agentmaster should live: just for you, for all users of this PC, or as the app package):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.8.2-prerelease
```
Add `-Scope User` (or `Machine` / `Package`) to skip the question, and/or `-Launch` to start it right after.

**Portable (no cert):** download `Agentmaster_0.8.2.0_x64.zip` (or `_arm64`), unzip anywhere, run `Agentmaster.exe`. Its first launch offers a per-user install, or stays portable.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.8.2.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.8.2.0.msixbundle`.

**Debugging symbols:** the portable zips carry a *public* PDB beside each binary (every function named in a stack trace); `Agentmaster_0.8.2.0_x64_symbols.zip` (or `_arm64`) holds the *full* PDBs, Source-Linked to this release's commit. Extract it into the folder that holds `Agentmaster.exe`, or onto your debugger's symbol path (an MSIX install's folder is read-only).

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
