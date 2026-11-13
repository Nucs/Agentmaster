<!-- RELEASED 2026-11-13 20:02:45Z as STABLE, the new Latest, from 158d52016:
     https://github.com/Nucs/Agentmaster/releases/tag/v0.9.0 (release id 444658786; 0.8.1 is no longer Latest;
     /releases/latest answers v0.9.0). The owner: "Lets release! stable release", then "Pause releasing" while the draft
     sat checked and unpublished (nothing was published or changed meanwhile), then "Release, green light".
     release.yml run 43489333252: prep, build x64 (23 min), build arm64 (30 min), bundle, release all green (~31 min);
     ci.yml run 43489329606 on the same push (the from-scratch x64 Release build + the symbols check): success. Assets
     checked: the 6 names, each the expected size (bundle 54,786,365 B; x64 zip 41,015,110 B; arm64 zip 42,567,204 B;
     x64 symbols 183,149,259 B; arm64 symbols 187,333,956 B; cer 768 B); the x64 zip (downloaded by asset id) holds 16
     PDBs, its agentmaster-0.9.0.0/.am-version reads 0.9.0.0 and its TerminalApp.dll bakes 158d52016. The notes below
     were applied unchanged (read back identical but for the API read's trailing newline).
     Agentmaster 0.9.0 release notes - STABLE (replaces 0.8.1 as Latest).
     Range: v0.8.1..158d52016 = 103 commits. Baseline = the last stable 0.8.1 (no nightly since); the one pre-release
     between, 0.8.2-prerelease (29b683148, 73 commits), is folded in below from its PUBLISHED body (release-notes §6).
     v0.8.2-prerelease..158d52016 = 30 commits: 14 user-facing (10 feat + 4 fix: 6d14f793c, 82a626ecf, d3e617f84,
     f5e12b5e3, fe5303802, e98ff0253, 34df0a309, ee8b44cba, b4321c3dc, f03433c9d, 67defd929, 0f014a873, 2e04d18de,
     772c34d3b) and 16 internal (the build server's second site and `amb test`, the durable outbox's design and its
     benchmark tools, the README's screenshots, the Builds table, the skills, the 0.8.2 release record).
     RELEASING from 158d52016 (= origin/agentmaster at the tag). Its code is b8d5224ae's (the commits after it are docs),
     which production runs as 0.8.0.13 since 2026-11-13 13:57 local: the build server's PURGED Release build
     w20261113-6, installed by `amb install`.
     Gate (the owner's pick, 2026-11-13: "Publish when CI is green"): ci.yml's from-scratch x64 Release build of
     cda97ae71 (the same code) succeeded (run 43401098408); the purged Release build w20261113-6; the m5 harness on
     907bde046 (w20261113-4: 12,907 checks, only the 3 known TaskbarBadge failures; the engine code is the same).
     ⚠ NO e2e since 2026-11-03 (190/205 on 89cbb9e26; SP-K2 still open: with Sum off, hovering a side-panel card opens
     the rich session card). ⚠ Production's first Telegram census windows after the update: 559 and 438 calls per 10 min
     (531 + 378 card edits; later 10-99, mean 76; no 429): MESSAGING.md §21.49 check 1 says the 🖼️ rollout must not
     burst; cause not established.
     Version: tag v0.9.0 -> ver4 0.9.0.0, ver3 0.9.0 (the owner's pick over 0.8.3). The updater compares
     major.minor.patch, so 0.9.0 is offered over 0.8.1 AND over 0.8.2-prerelease (a stable 0.8.2 would not have been).
     The fork also carries the old upstream Windows Terminal tag v0.9.433.0 (no release attached): no clash.
     Tag:     git tag -a v0.9.0 -m "Agentmaster 0.9.0" 158d52016 ; env -u GH_TOKEN -u GITHUB_TOKEN git push origin v0.9.0
              (pushed 2026-11-13 19:08Z after a fast-forward push of agentmaster cda97ae71..158d52016 and a private-data
              scan of the 4 outgoing commits: 0 findings) -> release.yml run 43489333252, ci.yml run 43489329606.
     Notes:   strip this comment (awk 'f{print} /^## Agentmaster 0.9.0/{f=1; print}' <draft> > body.md), then
              gh release edit v0.9.0 -R Nucs/Agentmaster --notes-file body.md
     Publish: gh release edit v0.9.0 -R Nucs/Agentmaster --draft=false --latest --prerelease=false
     Assets: Agentmaster_0.9.0.0.msixbundle, _x64.zip, _arm64.zip, _x64_symbols.zip, _arm64_symbols.zip, Agentmaster.cer. -->
## Agentmaster 0.9.0

**0.9.0 is a stable release** and the new default **Latest**, replacing 0.8.1. It carries everything since 0.8.1: the 0.8.2 pre-release in full (folded in below), and what came after it. Since 0.8.1, two Claude Code mods ship inside Agentmaster (**file-links** and **better-verbose-bash**). The side panel gets **filters**, now with a By Folder submenu, **Pause filter** and a **Filter** menu on every card, plus **Pin to Top**, a **⋯ menu** and the **AI search**. **Restart and Fork ask** whether a cut-off turn should go on, and a session whose response was cut off or hit a temporary API failure **restarts by itself**. **Telegram keeps every message** through errors, updates and crashes, and takes a screenshot only when you ask. Three fixes keep **big workspaces responsive** when you switch windows. Agentmaster 0.8.x offers it as an update by itself, or install it from the assets below.

## New in 0.9.0

- **More errors restart a session by itself.** 0.8.2 restarted only a turn cut by "Connection lost mid-response". Now:
  - **Every response Claude Code cut off** restarts at once, as Restart uninterrupted: "The response stopped arriving", "Server error mid-response", "Part of the response never arrived" and the rest of the family that ends "The response above may be incomplete.", and the "… Try again." errors raised before any output.
  - **A temporary API failure that outlived Claude Code's own retries** (a 5xx, "Repeated 529 Overloaded errors", "Connection error.", "Request timed out", …) restarts after a wait of 1, then 5, then 15 minutes, at most 3 times an hour.
  - **An error that needs you never restarts:** a usage or rate limit, credits, /login, /model, a refusal, a prompt that is too long, a certificate.
  - Errors are recognized by their words and their HTTP status, not by one exact message. A tab with an unsent draft or work still running in the background waits, and the restart touches the session's own pane, never your shell in a split. **Settings → Sessions → "Restart a session by itself after a cut-off response or a temporary API failure"** turns it off.
- **The side panel's filters grow.**
  - **By Folder's submenu.** By Folder… keeps its click (the folder picker). Resting on it, or pressing →, opens a submenu: **Pick…**, then the last ten folders you picked, newest first (one list for the whole app). Each line switches its folder on or off for this window's filter, with a ✓ while it filters, and the submenu stays open while you pick.
  - **Pause filter / Resume filter**, beside Clear, switches the whole row off (every chip, and the text typed in the box) and brings back exactly what applied. A click on a chip switches that one filter off and on again; its ✕ still removes it. Whatever is off stays in the row, dimmed. Adding a filter, or typing in the box, resumes.
  - **Filter ▸ on a card's right-click menu** (under Tags): the card's folder, or any folder above it, or the folder "(exactly)" without its subfolders; its state; one of its tags, or Untagged; its **git branch**; its **fork family**. Branch and fork family are new filters, each with a chip. A pick starts the filter over with that one thing; the line that is the whole filter shows a ✓, and its click drops it.
  - **The header:** "+" and the funnel are now icons of one size, and "+" is the innermost button again (docked left: **⋯ Sum ⏷ +**).
- **A collapsed side panel takes no room.** Its strip no longer keeps a column: it floats over the terminal's edge, hidden, and shows while the pointer is within 40 px of it (and for 1.5 s right after you collapse the panel). The terminal keeps its whole width and never resizes. A press beside the chevron goes to the terminal.
- **The side panel's AI search gives your request back.** ⊘ (a new conversation) puts the newest request back into an empty request box, whether it was still running or already answered. A new **copy button** under it copies the whole conversation (every request with Claude's reply) and the listed sessions as Markdown.
- **Telegram takes a screenshot only when you tap 🖼️.** A card no longer reads the terminal by itself, which was the main source of its flood holds. A new **🖼️** beside ⚙️ Session on the card's main page puts the terminal's current screen on the card; it stays there until the next tap. A parked tab's card has no 🖼️.
- **Telegram keeps every message (the durable outbox).**
  - A 502, a timeout or no answer no longer loses a message: it is sent again after a wait that grows from 1 s to 60 s, while the topic's later posts wait behind it. Eight failures in a row pause sending for a while.
  - The conversation's posts are written to a journal in your profile and sent after a restart, an update or a crash. A message cut off in flight is sent again, so one may arrive twice.
  - A topic deleted in Telegram is checked twice and made again with its whole history, instead of being posted into every minute. A flood hold now survives a restart.

## Fixes

- **Switching windows froze big workspaces.** Every window activation and deactivation re-colored every tab of both windows, because Windows Terminal's default dark theme gives an unfocused tab row a slightly different color. With hundreds of tabs, tab switches and new tabs waited seconds. A tab now redoes its colors only when the row's color changes what it draws.
- **A minimized or covered Agent Manager page froze every window.** A window left on its Agent Manager page rebuilt the whole board at every session change, even minimized or behind another window, for 1–2.5 s at a time on the UI thread that every window shares. Such a page now waits, and catches up once when its window is activated, so a board in a visible but inactive window updates when you click into it.
- **The Agent Manager page's description panel stayed up for good.** It now behaves like a tooltip: it shows once the pointer rests on a control, and hides when the pointer leaves it, on a click, after 10 seconds and when the page hides.
- **Telegram no longer floods itself.**
  - A topic creation Telegram refused was tried again on every tick, up to once a second. Now "BOT_TOPIC_WRONG_HOME" pauses every topic creation of the chat for 15 minutes, kept across restarts, and any other refusal backs that one session off from 15 seconds to 10 minutes.
  - Telegram lets a bot delete its own message only within 48 hours, so moving an older card down failed. That card now stays where it is, its buttons still working.
  - The quiet re-sort puts only the top 24 topics in most recent order, with the fewest calls.

## Worth knowing when you upgrade

- ⚠️ **More errors restart a session by itself:** a cut-off response at once, a temporary API failure after 1 to 15 minutes. Turn it off under **Settings → Sessions** (see above).
- ⚠️ **Telegram cards no longer show the terminal by themselves.** Tap 🖼️ for a picture.
- ⚠️ **Telegram's queue is kept on disk** (`%USERPROFILE%\.agentmaster\messaging\telegram\outbox\`). After a crash, a message may arrive twice.
- **The side panel's header changed:** "+" is the innermost button again, and the funnel sits between it and Sum.
- **A collapsed side panel's strip hides itself.** Move the pointer to the panel's edge to bring it back.
- Upgrading from **0.8.1**? Also read 0.8.2's list below.

---

# Full changelog since the last stable (0.8.1)

## 0.8.2-prerelease

**0.8.2 is a pre-release** building on 0.8.1. Two Claude Code mods now ship inside Agentmaster and load into every Claude tab it starts: **file-links** (file paths in Claude's replies become clickable links) and **better-verbose-bash** (a shell call's reason heads its row in the verbose views). The side panel gets **filters**, **Pin to Top**, a **⋯ menu** and the **AI search**. **Restart and Fork ask** whether a cut-off turn should go on, and a turn cut by a lost connection **restarts by itself**. You can **name a window**, the update prompt **shows the release notes**, and Telegram gains **/enter**, **voice searches** and **more folders for a new session**. Long prompts are **delivered again**, and Show Transcript no longer **freezes every window** when a screen reader or another accessibility tool searches it.

### New

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

### Fixes

- **Long prompts are delivered again.** Claude Code shows a paste of over 800 characters, or one with more than two line breaks, as a placeholder: `[Pasted text #N +M lines]`, or a bare `[Pasted text #N]` when it has no line break. The verified send took the bare form for foreign text, so every long one-line queued prompt (a long Telegram message, every voice note's transcript) failed as "not delivered" and left the placeholder in the input box; and a long prompt's send could lose track of its own placeholder after the Enter, then type the prompt again or leave it in the box. The send now recognizes the placeholder Claude Code drew for it, presses Enter into it, and gives a slow redraw 3 seconds before filling the box again. Telegram's 👎 reply on a failed prompt also states the runner's real mode, instead of always "the runner paused".
- **Show Transcript froze every window.** With a very long transcript level open (tens of thousands of blocks), a search by an accessibility client (a screen reader, or a clipboard or assistant tool that reads apps) kept Agentmaster's UI busy for 40 minutes and more, while the terminals went on drawing. The view now hands such a client one item per chunk, and the search stays quick.
- **A new Telegram session from anywhere.** On Telegram for Android, the message that opens a new thread from "All" arrives without its thread, so the thread's wizard never started; it starts now. `/newsession` also works in the main view, the Search topic and every new thread.
- **A dim suggestion counted as a draft.** A guess that Claude Code draws dim in an empty input box (its own, or a mod's such as next-prompt) showed the 3 dots of an unsent draft. It was remembered, carried into forks and typed back after a restart. Dim text no longer counts as a draft.
- **Closing a "Waiting for you" tab leaves the session Idle.** Every close you make (the tab's ✕, the batch closes, a pane close) moves a Waiting-for-you session to Idle and clears its Mark Unread. A later resume no longer brings it back as Waiting, and its Telegram topic leaves as closed. A window close or a quit keeps the state.
- **The AI search failed at "thinking off".** On Claude Code 2.1.310 every request at off fails. The thinking toggle now rotates low → medium → high → xhigh → max, the default is low, and a stored off runs as low.
- **The Sessions search box keeps its text.** Turning AI on or off, and the ⊘ reset, no longer empty the box. With AI on, the typed text waits as the unsent request; with AI off, it filters the list.
- **No message box from the command line.** `agentmaster --help` (`-h`, `-?`, `/?`) and `--version` (`-v`), typed in a console or run by a script that reads the output, print there instead of opening the app's message box. `--version` adds the app's version.
- **The side panel's unread ring** sits centered on a card's status dot, 2 px wide on every side, instead of a pixel off.

### Worth knowing when you upgrade from 0.8.1

- ⚠️ **Two mods load into every Claude tab by default.** Turn either off under **Settings → Claude → MODS IN CLAUDE TABS**; a tab already open keeps what it started with. Your own installed copy of a mod always wins.
- ⚠️ **Two environment variables join your Global environment, once:** `CLAUDE_CODE_ADDITIONAL_DIRECTORIES_CLAUDE_MD=1` (Claude Code reads the CLAUDE.md of every folder a session adds) and `CLAUDE_CODE_RESUME_INTERRUPTED_TURN=1` (a resumed session continues a turn that a restart, a crash or an app close cut off). Edit or delete them under **Settings → Sessions → ENVIRONMENT VARIABLES → Global**: a deleted one never comes back, and your own value for either name always wins.
- ⚠️ **A turn cut by "Connection lost mid-response" restarts its session by itself**, and the turn goes on.
- ⚠️ **The hosted file-link mode needs your own public address.** Until you enter one, hosted links point at the placeholder and reach nothing.
- ⚠️ **The AI search has no "thinking off" any more.** A stored off runs as low.

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
