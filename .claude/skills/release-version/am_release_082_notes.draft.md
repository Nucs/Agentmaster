<!-- RELEASED 2026-11-10 13:14:43Z as a PRE-RELEASE (the owner: "Release as 0.8.2 prerelease"), from 29b683148:
     https://github.com/Nucs/Agentmaster/releases/tag/v0.8.2-prerelease (release id 440382543; the stable 0.8.1 stays
     Latest). release.yml run 42818285009: prep, build x64, build arm64, bundle, release all green (~29.5 min); ci.yml
     run 42818279628 on the same push (the from-scratch x64 Release build + the symbols check, the clean-build gate in
     place of a local one): success. Assets checked: the 6 names, each the expected size; the x64 zip holds 16 PDBs,
     its TerminalApp.dll bakes 29b683148, its .am-version reads 0.8.2.0. The notes below were applied unchanged.
     ⚠ Shipped WITHOUT a new e2e gate, by the owner's go after being told: the last gate (2026-11-03, on 89cbb9e26,
     build server build b20261103-1 + the whole suite r20261103-135156-5b0994 = 205 tests, 190 passed, 15 failed) had
     one confirmed regression against v0.8.1, SP-K2 - with Sum off, hovering a side-panel card opens the rich session
     card beside the panel (test_sum_off_card_hover_opens_no_rich_card) - and no commit since mentions it; it was not
     re-checked on 29b683148 (the other 14: flaky, pre-existing on v0.8.1, PQR-K11's expected change, or Claude Code's
     own dialog; memory e2e-gate-2026-11-03-v082).
     Agentmaster 0.8.2-prerelease release notes - PRE-RELEASE (the stable 0.8.1 stays Latest).
     Range: v0.8.1..29b683148 = 73 commits (rewritten 2026-11-10; the 11-03 draft covered v0.8.1..89cbb9e26, 37).
     44 user-facing: 30 feat + 14 fix. 29 internal: docs (the Builds table, the notes archive, the private-data
     scrub's hash re-pointing, studies), the build server (amb install, admission, capped e2e VMs, measurements), a
     test, the dev identity's orange icon (dev builds only), and two build fixes no release ever carried (c41e78ba0,
     ad89700e3: agentmaster-cli failed to link since 0d3ed3b02). Baseline = the previous NON-nightly release = the
     0.8.1 stable (no release since).
     RELEASED from 29b683148 (= origin/agentmaster at the release: this draft's own commit, pushed as a fast-forward
     with the 35 before it; origin had stopped at 89cbb9e26). Its code is ad89700e3's (the three commits after it
     are docs), which runs on the owner's PC as production 0.8.0.12 and dev 0.0.1.44 (local builds, 2026-11-10).
     Gate: none on the release commit (see above); ci.yml's from-scratch build is the only build check.
     Version: tag v0.8.2-prerelease -> ver4 0.8.2.0, rawver 0.8.2-prerelease. The updater compares major.minor.patch,
     so 0.8.2 is offered over 0.8.1 to users who allow pre-releases. nightly=false: release.yml does NOT mark it a
     pre-release - mark it at publish. The installer's -Version maps to the TAG (0.8.2-prerelease).
     Tag:     git tag -a v0.8.2-prerelease -m "Agentmaster 0.8.2-prerelease" 29b683148ac5b12df05ebd07b241a086213674bf
              env -u GH_TOKEN -u GITHUB_TOKEN git push origin v0.8.2-prerelease
     Notes:   strip this comment (awk 'f{print} /^## Agentmaster 0.8.2-prerelease/{f=1; print}' <draft> > body.md), then
              gh release edit v0.8.2-prerelease -R Nucs/Agentmaster --notes-file body.md
     Publish: gh release edit v0.8.2-prerelease -R Nucs/Agentmaster --draft=false --prerelease --latest=false
              (NEVER --latest; the stable 0.8.1 stays Latest).
     Assets: Agentmaster_0.8.2.0.msixbundle, _x64.zip, _arm64.zip, _x64_symbols.zip, _arm64_symbols.zip, Agentmaster.cer. -->
## Agentmaster 0.8.2-prerelease

**0.8.2 is a pre-release** building on 0.8.1. Two Claude Code mods now ship inside Agentmaster and load into every Claude tab it starts: **file-links** (file paths in Claude's replies become clickable links) and **better-verbose-bash** (a shell call's reason heads its row in the verbose views). The side panel gets **filters**, **Pin to Top**, a **⋯ menu** and the **AI search**. **Restart and Fork ask** whether a cut-off turn should go on, and a turn cut by a lost connection **restarts by itself**. You can **name a window**, the update prompt **shows the release notes**, and Telegram gains **/enter**, **voice searches** and **more folders for a new session**. Long prompts are **delivered again**, and Show Transcript no longer **freezes every window** when a screen reader or another accessibility tool searches it. Enable **Settings → About → "Allow updating to pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.8.1** remains the default Latest for everyone else.

## New in 0.8.2

- **Claude Code mods ship with Agentmaster.** Every Claude tab Agentmaster starts (a new tab, a resume, a fork, a restart, a handover, a preset) loads two mods:
  - **file-links**: file paths and names in Claude's replies become clickable `file:///` links, for files that exist (Claude Code 2.1.287 or later).
  - **better-verbose-bash**: in the views that show a shell call in full (`--verbose`, ctrl+o), the reason Claude gave for the call heads its row (Claude Code 2.1.308 or later).

  A mod stands aside when you run your own copy of it (enabled in your Claude Code settings, or listed in `CLAUDE_CODE_PLUGIN_DIRS`), and when the tab's claude.exe is older than the mod needs. **Settings → Claude → MODS IN CLAUDE TABS** has a switch for each and says what the next tab will do with it.
- **The side panel: filters, Pin to Top and a ⋯ menu.**
  - **Filters.** A funnel button beside **+** opens a panel that stays open while you pick: **By Folder** (the **+** card's folder picker, with or without subfolders), **By State**, **By Tag**, **Active within**, and four flags (Unsent draft · Unread · Pinned to top · Favorite). A card shows when it matches every filter in use, and any one of the picks inside each. While a filter is on, a row under the filter box shows the count, a chip per pick and **Clear**. Each window keeps its own filters, and they come back when the window reopens.
  - **Pin to Top.** A card's right-click menu, right under Jump to Tab, pins the card at the top of the list with a pushpin. Pinning it again brings it back to the top; **Unpin** lets it go. It is separate from Telegram's `/pin`.
  - **The ⋯ menu.** The header is now **+ · Sum · ⋯**: Dock on the left / right, ⌖ Scroll to the selected tab, Card scope ▸, Card order ▸ and Collapse are the ⋯ menu's items. The header mirrors with the dock: docked left, it reads **⋯ Sum +** from the window's edge.
  - **Open New Session Here** and **Fork session** on a card open the new tab right after the card's own tab, in the window that hosts it, as the tab menu's own items do.
- **The AI search in the side panel.** An **AI** toggle inside the side panel's filter box turns the box into the AI search's request line, and the card list into its answer: the sessions it found, as side panel cards, each with the model's full reason.
  - A click selects a session and peeks its summary; a double-click jumps to its tab, or resumes a closed session as a new tab; a right-click opens the session menu.
  - While the assistant works, a loading icon shows, and the list changes only when the request ends: the last answer's cards stay dimmed until then. Both AI strips (here and on the Sessions page) show a breathing dot and a sweeping light while it works.
  - It is the same conversation as the Sessions page's AI search. Its hidden Claude Code process starts only when you click into the box or send a request, and stops 120 minutes after its last use (**Settings → Sessions → AI SEARCH**).
- **Restart and Fork ask whether the turn goes on.** 0.8.1 asked only when you restarted a turn you had interrupted, and when a fork would continue its source's turn. Now:
  - **Restarting a session in the middle of a turn** asks **Restart interrupted / uninterrupted**, on the PC and in Telegram. Uninterrupted lets the restarted session continue the turn; interrupted stops it with a regular interrupt.
  - **A turn that died on an API error** is part of the same choice: uninterrupted takes the error back and the new session continues the turn by itself; interrupted replaces the error with a regular interrupt. A fork of a turn you interrupted asks too, and every such question reads "interrupted / uninterrupted".
  - The AI search assistant (on the Sessions page, in the side panel and in Telegram's Search topic) asks before it forks a running session.
  - **A turn cut by "API Error: Connection lost mid-response" restarts by itself**, as Restart uninterrupted: no question, no tab switch, and the turn goes on.
- **Deactivate All Waiting-for-you.** The last item of every Deactivate ▸ menu (the tab menu, and a card's or tree row's session menu) parks every tab whose session is waiting for you, asking **This window / All windows** like Deactivate All Idle Tabs.
- **The reopen question can remember.** "Reopen your N previous Agentmaster windows?" gets a **Remember my choice** box, and **Settings → Behavior → STARTUP** answers it for good: Ask every time, Reopen them all, or Start with a single window.
- **Name a window.** Use **Rename window…** in the window menu (right-click a taskbar preview or the title bar's empty strip, or press Alt+Space), or the name in the Agent Manager header. A named window's caption is its name alone, so its taskbar preview, its Alt+Tab entry and its taskbar button show it. The name is saved, and comes back when the window reopens. In Telegram, the new-thread wizard's window step shows each named window by its name and accepts the name typed like the number; the AI search names windows the same way.
- **The update prompt shows the release notes.** The update prompt becomes a dark window of its own, with the release notes on the left: a tab per release on your channel, the version you run under a "▲ you" arrow, the updates in green, older releases dimmed. The choices on the right are the ones you know. Without WebView2 the notes show as plain text. You will see it at the first update offered after 0.8.2.
- **Show Transcript, improved.**
  - GitHub-style **Markdown tables** in Claude's replies draw as real tables: a framed grid, a header band, striped rows, selectable cells, each column's alignment.
  - An **(i)** after the level tabs opens a guide comparing what each level shows, and each level's tab has a tooltip that sums it up.
  - **Compact** lists every tool call on one line with the reason Claude gave for it (`Bash: Count lines of transcript sources`), else what it acted on (`Read: app.cfg`).
  - The separators the view writes itself are a plain hyphen instead of an em dash, in **Copy ▸ Transcript** too; Claude's own text is untouched.
- **Telegram.**
  - **`/enter`** presses one Enter at the PC on the prompt already waiting in a session's input box, never into a menu, a dialog or an empty box, and answers whether the prompt was submitted, kept, or why not.
  - **A voice note in the 🔎 Search topic is a search**: it is transcribed with your sessions' folder and tab names as its vocabulary, and read like a typed request.
  - **More folders for a new session.** A new thread's **➕** message adds folders before the session starts. Each reaches Claude Code as `/add-dir` before the first prompt, its answer chosen in the wizard: for this session, remember it (asked to confirm first), or no. Claude Code also reads each added folder's CLAUDE.md.
  - **New topic icons:** Running ✍️ and Waiting for you 🔥 (existing topics change quietly), and the Search topic wears 🔎.
  - **📜 All Screen ▸ gets a Last Turn box:** checked, any level draws only the conversation's newest turn, numbered as in the whole conversation. It combines with File Diffs. `/allscreen [level] [diffs] [last]` does the same.
  - **The card's screen follows the status.** The card looks at its screen again after a change into a state at rest, and after its window was too busy to answer in time; only a changed picture is uploaded. A tab that starts (activated, launched or restarted) gets one picture once Claude Code's screen shows, never a black one.
- **File links through your own address.** The hosted mode's address no longer defaults to the author's site: it is the placeholder `https://example.com` until you enter yours, and the cog warns while the placeholder is in effect. **Settings → Messaging → FILE LINKS → Copy AI Impl Instructions** copies a brief, with your install's facts filled in, that an AI agent on your PC follows to set that address up (a Cloudflare Tunnel, Tailscale Funnel or a reverse proxy) and to test it.

## Fixes

- **Long prompts are delivered again.** Claude Code shows a paste of over 800 characters, or one with more than two line breaks, as a placeholder: `[Pasted text #N +M lines]`, or a bare `[Pasted text #N]` when it has no line break. The verified send took the bare form for foreign text, so every long one-line queued prompt (a long Telegram message, every voice note's transcript) failed as "not delivered" and left the placeholder in the input box; and a long prompt's send could lose track of its own placeholder after the Enter, then type the prompt again or leave it in the box. The send now recognizes the placeholder Claude Code drew for it, presses Enter into it, and gives a slow redraw 3 seconds before filling the box again. Telegram's 👎 reply on a failed prompt also states the runner's real mode, instead of always "the runner paused".
- **Show Transcript froze every window.** With a very long transcript level open (tens of thousands of blocks), a search by an accessibility client (a screen reader, or a clipboard or assistant tool that reads apps) kept Agentmaster's UI busy for 40 minutes and more, while the terminals went on drawing. The view now hands such a client one item per chunk, and the search stays quick.
- **A new Telegram session from anywhere.** On Telegram for Android, the message that opens a new thread from "All" arrives without its thread, so the thread's wizard never started; it starts now. `/newsession` also works in the main view, the Search topic and every new thread.
- **A dim suggestion counted as a draft.** A guess that Claude Code draws dim in an empty input box (its own, or a mod's such as next-prompt) showed the 3 dots of an unsent draft. It was remembered, carried into forks and typed back after a restart. Dim text no longer counts as a draft.
- **Closing a "Waiting for you" tab leaves the session Idle.** Every close you make (the tab's ✕, the batch closes, a pane close) moves a Waiting-for-you session to Idle and clears its Mark Unread. A later resume no longer brings it back as Waiting, and its Telegram topic leaves as closed. A window close or a quit keeps the state.
- **The AI search failed at "thinking off".** On Claude Code 2.1.310 every request at off fails. The thinking toggle now rotates low → medium → high → xhigh → max, the default is low, and a stored off runs as low.
- **The Sessions search box keeps its text.** Turning AI on or off, and the ⊘ reset, no longer empty the box. With AI on, the typed text waits as the unsent request; with AI off, it filters the list.
- **No message box from the command line.** `agentmaster --help` (`-h`, `-?`, `/?`) and `--version` (`-v`), typed in a console or run by a script that reads the output, print there instead of opening the app's message box. `--version` adds the app's version.
- **The side panel's unread ring** sits centered on a card's status dot, 2 px wide on every side, instead of a pixel off.

## Worth knowing when you upgrade from 0.8.1

- ⚠️ **Two mods load into every Claude tab by default.** Turn either off under **Settings → Claude → MODS IN CLAUDE TABS**; a tab already open keeps what it started with. Your own installed copy of a mod always wins.
- ⚠️ **Two environment variables join your Global environment, once:** `CLAUDE_CODE_ADDITIONAL_DIRECTORIES_CLAUDE_MD=1` (Claude Code reads the CLAUDE.md of every folder a session adds) and `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1` (a resumed session continues a turn that a restart, a crash or an app close cut off). Edit or delete them under **Settings → Sessions → ENVIRONMENT VARIABLES → Global**: a deleted one never comes back, and your own value for either name always wins.
- ⚠️ **A turn cut by "Connection lost mid-response" restarts its session by itself**, and the turn goes on.
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
