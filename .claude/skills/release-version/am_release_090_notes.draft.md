<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request: "I want you to create a more one-row changes in the
     release notes, like we do in the head of all release notes ... sentences that start with a bold and then,
     essentially a sentence, maybe a sentence and a half, or a bit more if you really have to", and the layout the
     owner picked from three previews rendered by GitHub: "Head only". The body is a 1-2 sentence intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the unchanged Worth
     knowing and Install sections. The long body published until 2026-11-16 is this file at 57c9fca4d; none of it is
     on GitHub any more. Besides the head: the auto-restart line of Worth knowing names its setting instead of "(see above)".
     RELEASED 2026-11-13 20:02:45Z as STABLE, the new Latest, from 158d52016:
     https://github.com/Nucs/Agentmaster/releases/tag/v0.9.0 (release id 444658786; 0.8.1 is no longer Latest;
     /releases/latest answers v0.9.0). The owner: "Lets release! stable release", then "Pause releasing" while the draft
     sat checked and unpublished (nothing was published or changed meanwhile), then "Release, green light".
     release.yml run 43489333252: prep, build x64 (23 min), build arm64 (30 min), bundle, release all green (~31 min);
     ci.yml run 43489329606 on the same push (the from-scratch x64 Release build + the symbols check): success. Assets
     checked: the 6 names, each the expected size (bundle 54,786,365 B; x64 zip 41,015,110 B; arm64 zip 42,567,204 B;
     x64 symbols 183,149,259 B; arm64 symbols 187,333,956 B; cer 768 B); the x64 zip (downloaded by asset id) holds 16
     PDBs, its agentmaster-0.9.0.0/.am-version reads 0.9.0.0 and its TerminalApp.dll bakes 158d52016.
     NOTES REPLACED after the publish, the same evening, at the owner's request: "Reread the release notes, merge the
     release notes (all of them) since last release (non prerelease), no mentioning of prerelease versions but 0.9.0".
     The first published body (0.9.0's own delta on top, then the 0.8.2-prerelease body folded in under its own banner,
     release-notes §6 as it read then) is this file at commit 882d6af44. This body is ONE merged changelog over
     v0.8.1..158d52016: one New, one Fixes, one "Worth knowing when you upgrade", and no version named but 0.9.0 (other
     products' versions stay: Claude Code, Windows, Windows Terminal). Merged where 0.9.0 changed what 0.8.2 shipped:
     the automatic restart (7711df891 + 6d14f793c + 82a626ecf) is one item; the side panel's header is described as it
     ends (⋯ · Sum · funnel · +); the filters carry the By Folder submenu, Pause filter and the card's Filter menu; the
     AI search item carries ⊘'s refill and the copy button. Left out as superseded: 0.8.2's "The card's screen follows
     the status" (the looks and the launch watch, 35426a13d / 60cf43c87, removed by b4321c3dc: a picture only on a 🖼️
     tap). Checked against v0.8.1 itself: 0.8.1's cards already carried a screenshot at every update (69792f881 is in
     v0.8.1), and the automatic restart, the ⋯ menu, the filters and the side panel's AI search are not in it.
     Agentmaster 0.9.0 release notes - STABLE (replaces 0.8.1 as Latest).
     Range: v0.8.1..158d52016 = 103 commits. Baseline = the last stable 0.8.1 (no nightly since); the one pre-release
     between, 0.8.2-prerelease (29b683148, 73 commits: 44 user-facing), is merged in, never named.
     v0.8.2-prerelease..158d52016 = 30 commits: 14 user-facing (10 feat + 4 fix: 6d14f793c, 82a626ecf, d3e617f84,
     f5e12b5e3, fe5303802, e98ff0253, 34df0a309, ee8b44cba, b4321c3dc, f03433c9d, 67defd929, 0f014a873, 2e04d18de,
     772c34d3b) and 16 internal (the build server's second site and `amb test`, the durable outbox's design and its
     benchmark tools, the README's screenshots, the Builds table, the skills, the 0.8.2 release record).
     Its code is b8d5224ae's (the commits after it are docs), which production runs as 0.8.0.13 since 2026-11-13 13:57
     local: the build server's PURGED Release build w20261113-6, installed by `amb install`.
     Gate (the owner's pick: "Publish when CI is green"): ci.yml's from-scratch x64 Release build of cda97ae71 (the same
     code) succeeded (run 43401098408); the purged Release build w20261113-6; the m5 harness on 907bde046 (w20261113-4:
     12,907 checks, only the 3 known TaskbarBadge failures; the engine code is the same).
     ⚠ NO e2e since 2026-11-03 (190/205 on 89cbb9e26; SP-K2 still open: with Sum off, hovering a side-panel card opens
     the rich session card). ⚠ Production's first Telegram census windows after the update: 559 and 438 calls per 10 min
     (531 + 378 card edits; later 10-99, mean 76; no 429): MESSAGING.md §21.49 check 1 says the 🖼️ rollout must not
     burst; cause not established.
     Version: tag v0.9.0 -> ver4 0.9.0.0, ver3 0.9.0 (the owner's pick over 0.8.3). The updater compares
     major.minor.patch, so 0.9.0 is offered over 0.8.1 AND over 0.8.2-prerelease (a stable 0.8.2 would not have been).
     The fork also carries the old upstream Windows Terminal tag v0.9.433.0 (no release attached): no clash.
     Tag:     git tag -a v0.9.0 -m "Agentmaster 0.9.0" 158d52016 ; env -u GH_TOKEN -u GITHUB_TOKEN git push origin v0.9.0
     Notes:   strip this comment (awk 'f{print} /^## Agentmaster 0.9.0$/{f=1; print}' <draft> > body.md), then
              gh release edit v0.9.0 -R Nucs/Agentmaster --notes-file body.md
     Publish: gh release edit v0.9.0 -R Nucs/Agentmaster --draft=false --latest --prerelease=false (done)
     Assets: Agentmaster_0.9.0.0.msixbundle, _x64.zip, _arm64.zip, _x64_symbols.zip, _arm64_symbols.zip, Agentmaster.cer. -->
## Agentmaster 0.9.0

**0.9.0 is a stable release** and the new default **Latest**. Agentmaster offers it as an update by itself, or install it from the assets below.

## What's new

- **Claude Code mods ship with Agentmaster.** Every Claude tab it starts turns file paths in Claude's replies into clickable links (file-links) and heads each shell call with its reason in the verbose views (better-verbose-bash).
- **Side panel filters.** A funnel narrows the cards by folder, state, tag, recent activity or a flag such as Unread, and a card's own menu filters by its folder, state, tag, branch or fork family.
- **Pin to Top.** A card's right-click menu pins it at the top of the side panel, with a pushpin, until you unpin it.
- **The side panel's ⋯ menu.** Docking, scrolling to the selected tab, the card scope and order, and Collapse now live in one ⋯ menu in the panel's header.
- **A collapsed side panel takes no room.** Its strip floats hidden over the terminal's edge and shows when the pointer comes near, so the terminal keeps its whole width.
- **New tabs open next to their card's tab.** Open New Session Here and Fork session on a side panel card open the new tab right after the card's own tab, in the window that hosts it.
- **AI search in the side panel.** An AI toggle in the side panel's filter box takes a request in plain words and lists the sessions it finds as cards, each with the reason it fits.
- **Restart and Fork ask whether the turn goes on.** Restarting or forking a session whose turn is running, was interrupted or died on an API error asks "interrupted / uninterrupted", on the PC and in Telegram.
- **Sessions restart by themselves.** A response Claude Code cut off restarts its session at once and a temporary API failure after 1 to 15 minutes, so the turn goes on, while an error that needs you never does.
- **Telegram keeps every message.** Posts wait in a journal on disk and go out again after a network error, a restart, an update or a crash, and a topic deleted in Telegram comes back with its history.
- **Telegram screenshots on a tap.** A 🖼️ button on a session's card puts the terminal's current screen on it, instead of the card reading the terminal at every update.
- **`/enter` in Telegram.** It presses Enter at the PC on the prompt already waiting in a session's input box, never into a menu, a dialog or an empty box, and says whether the prompt went out.
- **Voice searches in Telegram.** A voice note in the 🔎 Search topic runs as a search, transcribed with your sessions' folder and tab names as its vocabulary.
- **More folders for a new Telegram session.** A new thread's ➕ message adds folders before the session starts, each passed to Claude Code as `/add-dir` ahead of the first prompt.
- **New Telegram topic icons.** Running topics wear ✍️, Waiting-for-you topics 🔥, and the Search topic 🔎.
- **Last Turn in 📜 All Screen.** A Last Turn box draws only the conversation's newest turn, at any level, and `/allscreen … last` does the same.
- **Name a window.** Rename window… gives a window a name that its caption, taskbar preview and Alt+Tab entry show, and that comes back when the window reopens.
- **Release notes in the update prompt.** The update prompt is now a window of its own that shows the notes of every release on your channel, with your version marked and the updates in green.
- **Show Transcript draws tables.** Markdown tables in Claude's replies render as real grids, an (i) guide compares the five levels, and Compact gives each tool call the reason Claude stated for it.
- **Deactivate All Waiting-for-you.** The last item of every Deactivate ▸ menu parks every tab whose session is waiting for you, in this window or in all of them.
- **The reopen question can remember.** "Reopen your N previous Agentmaster windows?" gets a Remember my choice box, and Settings → Behavior → STARTUP answers it for good.
- **File links through your own address.** The hosted file-link mode no longer points at the author's site, and Copy AI Impl Instructions (Settings → Messaging → FILE LINKS) hands an AI agent on your PC the steps to set up yours.

## What's fixed

- **Window switches no longer freeze big workspaces.** Every activation re-colored every tab of both windows, which took seconds with hundreds of tabs; a tab now redoes its colors only when they change.
- **A background Agent Manager page no longer freezes every window.** A minimized or covered page now waits and catches up when its window is activated, instead of rebuilding for 1–2.5 s at every session change.
- **Show Transcript no longer freezes every window.** A screen reader or another accessibility tool searching a very long transcript kept the UI busy for 40 minutes and more; the search now stays quick.
- **Long prompts are delivered again.** A long one-line prompt, such as a long Telegram message or a voice note's transcript, no longer fails as "not delivered" or gets typed into the box twice.
- **Telegram no longer floods itself.** A refused topic creation now pauses for 15 minutes instead of retrying every second, and a card older than 48 hours stays where it is instead of failing to move.
- **New sessions from Telegram for Android.** A thread opened from "All" on Telegram for Android now starts its new-session wizard, and `/newsession` also works in the main view and the Search topic.
- **A dim suggestion is no longer a draft.** A dim guess in an empty input box no longer shows the 3 dots of an unsent draft, travels into forks or gets typed back after a restart.
- **Closing a Waiting-for-you tab leaves it Idle.** A later resume no longer brings it back as Waiting, and its Telegram topic leaves as closed.
- **The AI search no longer fails at "thinking off".** Every request at off failed on Claude Code 2.1.310, so the thinking level now starts at low, and a stored off runs as low.
- **The Sessions search box keeps its text.** Turning AI on or off, or starting over with ⊘, no longer empties it.
- **`--help` and `--version` answer in the console.** Typed in a console or run by a script, they print there instead of opening the app's message box.
- **The Agent Manager page's descriptions act like tooltips.** The description panel shows while the pointer rests on a control and hides when it leaves, instead of staying up for good.
- **The side panel's unread ring is centered.** It sits evenly on a card's status dot instead of a pixel off.

## Worth knowing when you upgrade

- ⚠️ **Two mods load into every Claude tab by default.** Turn either off under **Settings → Claude → MODS IN CLAUDE TABS**; a tab already open keeps what it started with. Your own installed copy of a mod always wins.
- ⚠️ **Two environment variables join your Global environment, once:** `CLAUDE_CODE_ADDITIONAL_DIRECTORIES_CLAUDE_MD=1` (Claude Code reads the CLAUDE.md of every folder a session adds) and `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1` (a resumed session continues a turn that a restart, a crash or an app close cut off). Edit or delete them under **Settings → Sessions → ENVIRONMENT VARIABLES → Global**: a deleted one never comes back, and your own value for either name always wins.
- ⚠️ **A session restarts by itself** after a cut-off response (at once) or a temporary API failure (after 1 to 15 minutes), and the turn goes on. Turn it off under **Settings → Sessions → "Restart a session by itself after a cut-off response or a temporary API failure"**.
- ⚠️ **Telegram cards no longer show the terminal by themselves.** Tap 🖼️ for a picture.
- ⚠️ **Telegram's queue is kept on disk** (`%USERPROFILE%\.agentmaster\messaging\telegram\outbox\`). After a crash, a message may arrive twice.
- ⚠️ **The hosted file-link mode needs your own public address.** Until you enter one, hosted links point at the placeholder and reach nothing.
- ⚠️ **The AI search has no "thinking off" any more.** A stored off runs as low.
- **The side panel's header is four buttons** (⋯ · Sum · the funnel · +): docking, scrolling to the selected tab, the card scope and order, and Collapse live in the ⋯ menu. A collapsed panel's strip hides itself: move the pointer to the panel's edge to bring it back.

## Install

**One command (PowerShell)** — installs 0.9.0 (administrator rights not needed; it asks where Agentmaster should live: just for you, for all users of this PC, or as the app package):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.9.0
```
Add `-Scope User` (or `Machine` / `Package`) to skip the question, and/or `-Launch` to start it right after. 0.9.0 is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable (no cert):** download `Agentmaster_0.9.0.0_x64.zip` (or `_arm64`), unzip anywhere, run `Agentmaster.exe`. Its first launch offers a per-user install, or stays portable.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.9.0.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.9.0.0.msixbundle`.

**Debugging symbols:** the portable zips carry a *public* PDB beside each binary (every function named in a stack trace); `Agentmaster_0.9.0.0_x64_symbols.zip` (or `_arm64`) holds the *full* PDBs, Source-Linked to this release's commit. Extract it into the folder that holds `Agentmaster.exe`, or onto your debugger's symbol path (an MSIX install's folder is read-only).

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
