<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request: "I want you to create a more one-row changes in the
     release notes, like we do in the head of all release notes ... sentences that start with a bold and then,
     essentially a sentence, maybe a sentence and a half, or a bit more if you really have to", and the layout the
     owner picked from three previews rendered by GitHub: "Head only". The body is a 1-2 sentence intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the unchanged Worth
     knowing and Install sections. The long body published until 2026-11-16 is this file at f2854acd6; none of it is
     on GitHub any more. Besides the head: nothing else changed.
     RELEASED 2026-11-10 13:14:43Z as a PRE-RELEASE (the owner: "Release as 0.8.2 prerelease"), from 29b683148:
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

**0.8.2 is a pre-release** building on 0.8.1. Enable **Settings → About → "Allow updating to pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.8.1** remains the default Latest for everyone else.

## What's new

- **Claude Code mods ship with Agentmaster.** Every Claude tab it starts turns file paths in Claude's replies into clickable links (file-links) and heads each shell call with its reason in the verbose views (better-verbose-bash).
- **Side panel filters.** A funnel beside + narrows the cards by folder, state, tag, recent activity or a flag such as Unread, and each window keeps its own filters.
- **Pin to Top.** A card's right-click menu pins it at the top of the side panel, with a pushpin, until you unpin it.
- **The side panel's ⋯ menu.** The header is now + · Sum · ⋯, and docking, scrolling to the selected tab, the card scope and order, and Collapse live in its ⋯ menu.
- **New tabs open next to their card's tab.** Open New Session Here and Fork session on a side panel card open the new tab right after the card's own tab, in the window that hosts it.
- **AI search in the side panel.** An AI toggle in the side panel's filter box takes a request in plain words and lists the sessions it finds as cards, each with the reason it fits.
- **Restart and Fork ask whether the turn goes on.** Restarting a session mid-turn or after an API error, or forking a turn you interrupted, asks "interrupted / uninterrupted", on the PC and in Telegram.
- **A turn cut by "Connection lost mid-response" restarts by itself.** Its session restarts as Restart uninterrupted, with no question and no tab switch, and the turn goes on.
- **Deactivate All Waiting-for-you.** The last item of every Deactivate ▸ menu parks every tab whose session is waiting for you, in this window or in all of them.
- **The reopen question can remember.** "Reopen your N previous Agentmaster windows?" gets a Remember my choice box, and Settings → Behavior → STARTUP answers it for good.
- **Name a window.** Rename window… gives a window a name that its caption, taskbar preview and Alt+Tab entry show, and that comes back when the window reopens.
- **Release notes in the update prompt.** The update prompt is now a window of its own that shows the notes of every release on your channel, with your version marked and the updates in green.
- **Show Transcript draws tables.** Markdown tables in Claude's replies render as real grids, an (i) guide compares the five levels, and Compact gives each tool call the reason Claude stated for it.
- **`/enter` in Telegram.** It presses Enter at the PC on the prompt already waiting in a session's input box, never into a menu, a dialog or an empty box, and says whether the prompt went out.
- **Voice searches in Telegram.** A voice note in the 🔎 Search topic runs as a search, transcribed with your sessions' folder and tab names as its vocabulary.
- **More folders for a new Telegram session.** A new thread's ➕ message adds folders before the session starts, each passed to Claude Code as `/add-dir` ahead of the first prompt.
- **New Telegram topic icons.** Running topics wear ✍️, Waiting-for-you topics 🔥, and the Search topic 🔎.
- **Last Turn in 📜 All Screen.** A Last Turn box draws only the conversation's newest turn, at any level, and `/allscreen … last` does the same.
- **The card's screen follows the status.** A Telegram card takes a fresh picture of the screen when its session comes to rest, and a tab that starts gets one once Claude Code's screen shows, never a black one.
- **File links through your own address.** The hosted file-link mode no longer points at the author's site, and Copy AI Impl Instructions (Settings → Messaging → FILE LINKS) hands an AI agent on your PC the steps to set up yours.

## What's fixed

- **Long prompts are delivered again.** A long one-line prompt, such as a long Telegram message or a voice note's transcript, no longer fails as "not delivered" or gets typed into the box twice.
- **Show Transcript no longer freezes every window.** A screen reader or another accessibility tool searching a very long transcript kept the UI busy for 40 minutes and more; the search now stays quick.
- **New sessions from Telegram for Android.** A thread opened from "All" on Telegram for Android now starts its new-session wizard, and `/newsession` also works in the main view and the Search topic.
- **A dim suggestion is no longer a draft.** A dim guess in an empty input box no longer shows the 3 dots of an unsent draft, travels into forks or gets typed back after a restart.
- **Closing a Waiting-for-you tab leaves it Idle.** A later resume no longer brings it back as Waiting, and its Telegram topic leaves as closed.
- **The AI search no longer fails at "thinking off".** Every request at off failed on Claude Code 2.1.310, so the thinking level now starts at low, and a stored off runs as low.
- **The Sessions search box keeps its text.** Turning AI on or off, or starting over with ⊘, no longer empties it.
- **`--help` and `--version` answer in the console.** Typed in a console or run by a script, they print there instead of opening the app's message box.
- **The side panel's unread ring is centered.** It sits evenly on a card's status dot instead of a pixel off.

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
