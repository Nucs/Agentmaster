<!-- Agentmaster 0.8.1 release notes — STABLE (becomes Latest, replacing 0.8.0).
     Range: v0.8.0..89cc2766e = 84 commits. 37 user-facing: 30 feat + 17 fix, minus 11 dev-tooling ones, plus the
     45cbf8d15 refactor that changes a Telegram menu. 47 internal: 21 docs, 14 test, 1 tools, 9 build-server feats,
     1 swap-script feat, 1 e2e-host feat. No pre-release since 0.8.0, so there is nothing to fold (release-notes §6):
     the notes are 0.8.1's own batch.
     RELEASING the PINNED commit 89cc2766ee2c3c096013b996a6beb9a454185220 (the user, 2026-10-29: "Pin current commit and
     lets do 0.8.1 no prerelease"). The tag goes on it even if the branch moves on; this draft's own commit comes after
     it and changes nothing that is built.
     Builds behind it: production 0.8.0.1 runs 1e0076bc6; dev 0.0.1.39 runs 364e18f23 (build server b20261028-3).
     21f0482d4 (the profile-overwrite audit) and 89cc2766e (state files across versions) are in NO build yet. This
     release's ci.yml and release.yml runs are their first compile and link; before them, only the m5 harness and
     cl /Zs had checked them.
     GitHub had built up to 68f44f0de (ci.yml run 40312689347: success). That run was the first of d496ccc6e's symbols
     steps (x64). arm64's symbols steps run for the first time in release.yml.
     Version: tag v0.8.1 -> ver4 0.8.1.0, rawver 0.8.1. The updater compares major.minor.patch, so 0.8.1 is offered
     over 0.8.0.
     Push first (fast-forward from 68f44f0de): env -u GH_TOKEN -u GITHUB_TOKEN git push origin <draft commit>:agentmaster
     Tag:     git tag -a v0.8.1 -m "Agentmaster 0.8.1" 89cc2766ee2c3c096013b996a6beb9a454185220
              env -u GH_TOKEN -u GITHUB_TOKEN git push origin v0.8.1
     Notes:   strip this comment (awk 'f{print} /^## Agentmaster 0.8.1/{f=1; print}' <draft> > body.md), then
              gh release edit v0.8.1 -R Nucs/Agentmaster --notes-file body.md
     Publish: gh release edit v0.8.1 -R Nucs/Agentmaster --draft=false --latest --prerelease=false (REPLACES 0.8.0 as Latest).
     Assets: Agentmaster_0.8.1.0.msixbundle, _x64.zip, _arm64.zip, _x64_symbols.zip, _arm64_symbols.zip, Agentmaster.cer. -->
## Agentmaster 0.8.1

**0.8.1 is a stable release building on 0.8.0. Its theme: your workspace survives everything.** No install, update or uninstall deletes Agentmaster's state any more. A state file that cannot be read is kept instead of saved over, and a window holds on to the tabs it could not bring back. Every save keeps what another version of Agentmaster wrote, and each new version first copies your state files into `backups\`.

Telegram gains:
- 🎤 **voice notes**, read as prompts;
- 📜 **All Screen**, the whole conversation as tall pictures;
- the terminal's **screen on every session's card**;
- ⎋ **Esc** and 🌙 **Deactivate** buttons;
- a `/status` with Claude Code's own facts and your plan usage.

On the PC, each session keeps its **Prompt Queue Runner mode** across a restart. Restart and Fork ask before continuing an **interrupted turn**, and **Deactivate All Idle Tabs** parks every idle tab. Eight defects that the new end-to-end suite found in 0.8.0 are fixed, among them a window close that brought its Claude tabs back as plain shells. Releases now ship **debugging symbols**. This build becomes the new default **Latest**.

## New in 0.8.1

- **State files that survive every version.** Every save now keeps what another version of Agentmaster wrote: settings, fields and records this version does not know stay in the file, and a file in a newer format is never written. Each state file names the version that wrote it. If you run an older version on the same profile and come back, 0.8.1's next start puts back what the older one dropped. The first start of each new version copies every state file into `<profile>\backups\`, with a README (the newest 12 copies are kept).
- **🎤 Voice notes in Telegram.** A voice note in a session's topic becomes that session's next prompt.
  - It is transcribed with the session's own vocabulary: its folder's name, the names in its CLAUDE.md, a developer glossary and your own dictionary.
  - Claude then corrects it, weighing what each speech engine heard (Sonnet 5.5 by default, through the PC's Claude Code login).
  - The text is echoed under the note, and a note sent in a new thread starts a session like a typed message.

  It needs a speech engine you choose under **Settings → Messaging → VOICE NOTES**: an OpenAI-compatible transcription endpoint (OpenAI's `whisper-1` or `gpt-4o-transcribe`, Groq's `whisper-large-v3-turbo`), or a local voice worker such as the repository's `tools/voice-poc` (Whisper with Parakeet). Until one is set, a note is answered with where to set it.
- **📜 All Screen: the whole conversation as pictures.** A fullscreen Claude Code keeps no scrollback, so **⚙️ Session → 📜 All Screen ▸** on a session's card draws the conversation from its transcript, in Claude Code's own look.
  - The pictures are tall, come in up to 10 parts, and are sent as documents so Telegram keeps them sharp.
  - You pick a level: Minimal, Compact, Default, Full or Max. Full is the default; **Settings → Messaging** sets another.
  - The **File diffs** box adds every file change as Claude Code's numbered diff, on green and red bands, at any level.

  `/allscreen [level] [diffs]` does the same.
- **The terminal's screen on every card.** A session's status card now ends with a picture of its terminal, taken again whenever the card updates; a parked tab keeps its last picture. It costs no extra Telegram calls. **Settings → Messaging → "Show the screen in each session's card"** turns it off.
- **⎋ Esc and 🌙 Deactivate from your phone.** While the session's claude runs, the card's ⚙️ Session page has two new buttons:
  - **⎋ Esc** presses Esc once in the session, as at the PC: a running turn stops, and a waiting permission or question is turned down.
  - **🌙 Deactivate** parks the tab, asking first while a turn runs.

  The window that hosts the tab checks each request again and answers with what happened. An Esc never lands while a delivery holds the input box, and never twice within 3 s (two quick Escs open Claude Code's Rewind menu). `/interrupt` and `/deactivate` work the same way.
- **`/status` like Claude Code's own.** In a session's topic, `/status` now sends what Claude Code's `/status` dialog shows:
  - the version, model, effort, permission mode, context, MCP servers (with why one failed) and memory files;
  - your account and plan usage, with bars;
  - this conversation's tokens.

  Nothing is typed into the session. The facts come from the transcript, Claude Code's heartbeat, `claude auth status`, and `claude -p /usage` (a local command with no model call; it needs Claude Code 2.1.310+). In the 🔎 Search topic and the main view, `/status` gives the account and plan usage.
- **A queued message shows where its turn starts.** A message you sent while the session was busy is posted again when the queue sends it, where its turn really begins: "👤 You · … · 📱 from the phone · 📤 sent from the queue". Your own message gets 🫡.
- **The Search topic's pick menu follows the session's state.** It offers ▶ Activate for a deactivated tab, ⏰ Wake for an idle one, and 💬 Go to topic; ▶ Resume shows only for a closed session.
- **Continue or keep an interrupted turn.** Restarting a session whose last turn you interrupted asks **"Continue the interrupted turn?"** (Keep it interrupted is the default). Forking a session in the middle of a turn asks whether the fork runs that turn on its own or waits for you; for a fork that waits, Agentmaster presses Esc in it the moment it starts running. Telegram asks the same in the topic.
- **Deactivate All Idle Tabs.** The last item of every **Deactivate ▸** menu parks every tab whose session is Idle and leaves the rest running. It is grayed out while no tab is idle. When other windows hold idle tabs too, it asks **This window** or **All windows**.
- **Card and tree menus match the tab's menu.** A side-panel card, a Triage Board card and an Explorer Tree row now have **Deactivate ▸** above **Close ▸**. Both submenus list the tab menu's scopes, row for row: this tab, left, right, others, same folder, other of same folder and all, plus ★ Favorite & Close All Tabs under Close. Each item runs on the session's own tab, in the window that hosts it.
- **Side-panel cards wear the tab's status icon.** A card's dot is now the tab strip's, with its three indicators:
  - the 3 dots of an unsent prompt;
  - the red unread ring, blinking in step with the tab's;
  - the spark crown while the server cache is warm.
- **The AI search sees your tabs.** Two new tools let it answer "what runs in my tabs" and "what needs me":
  - **overview** lists every window's tabs on one page, grouped by working folder, with status, age, turns, context, model and queue;
  - **status** says what up to 32 sessions are doing now, and why a queued prompt waits.

  Every search row now gives its session's status and age.
- **Startup dialogs show up in the command that started Agentmaster.** Each dialog Agentmaster.exe opens at startup is also written to that command's console or pipe, with its choices and then its answer, so a script or a Claude session sees a dialog it caused. That covers:
  - the profile and install questions, and "profile in use";
  - the update prompt and "Reopen your windows?";
  - `--help`.
- **Screen readers and UI Automation.** The quit confirmation, About, and the paste and link warnings can now be read by UI Automation. Buttons that show only a glyph have names. Tabs, board cards, tree rows and side-panel cards report their state, their selection, unread, and an unsent draft.
- **Debugging symbols in every release.** The portable zips carry a public PDB beside each binary. New `_symbols.zip` assets hold the full PDBs, Source-Linked to the release's commit (see Install).
- **"Delete the downloaded copy?"** A portable copy that installed itself somewhere else now offers to move the folder you unzipped into the Recycle Bin. It asks only when that folder holds nothing but the download's own files.

## Fixes

- **Closing a window could turn its Claude tabs into plain shells.** An autosave still pending at **Close Window** ran after the close and saved every Claude tab as a PowerShell tab, so the next start (or **Reopen Windows**) opened shells. The close is now the window's last save.
- **A start without Claude Code wiped every window's tabs.** When Claude Code was not found, or a preset's key was missing, a reopened window dropped the tabs it could not open at its next save. A window left with no tab deleted its record, and the Telegram bot then deleted those sessions' topics. Such tabs are now held in the window's record and come back at the next start, and their topics stay.
- **An unreadable state file was saved over.** A `sessions.json` could load as an empty fleet, and the next save then wrote that emptiness over it. That happened when the file started with a byte-order mark, held NUL bytes, ended torn, or was locked at the wrong moment. Such files are now read correctly, retried, or kept as `<name>.could-not-read-<time>` before anything replaces them. A start that cannot use `sessions.json` first copies the workspace into `<profile>\recovery\`, with a README. The same guard now covers:
  - the tag files;
  - the Telegram bot's `state.json`;
  - the update prompt's Skip and Postpone;
  - the profile map, `~\.agentmaster.profiles`.
- **A restored tab could lose its session record.** A restore that did not find the transcript (an unmounted drive, a working folder past MAX_PATH) gave the session a new id and dropped the old record, with its tags, favorite and Telegram topic. It now keeps the id, and transcripts past MAX_PATH are found.
- **Installs never delete your data.** Every delete that the installer, the updater or an uninstall makes now refuses a folder that is or holds Agentmaster state. A destination folder that holds someone else's files is renamed aside (`<folder>.before-install-<time>`) instead of being mixed into. Removing the app package first copies the package's own files into your profile.
- **The runner mode reset at every restart.** A session left Paused came back on Auto after a window reopen or a **Resume here**, and its queue then went by itself. Each session now keeps its own mode across a restart, including an Esc's interrupt hold.
- **Tags from the tab menu typed into Claude.** The Tags panel lost the keyboard to the closing menu, so a tag name landed in Claude's input box and Esc interrupted the running turn. The panel now keeps the keyboard, and Esc closes only the panel. The "Specify a model" prompt got the same fix.
- **Shift+Click on a tab switched to it.** It now starts a parked tab's session in place and keeps you where you are, as the tab menu says. Also, a middle-click released off a tab no longer makes the next ✕ click count as a middle click.
- **Launches from the Agent Manager page left the page.** A launch there now stays on the page and selects the new session, and Home jumps to it. In the side panel's **+** card, Launch works with one click while its presets menu is open.
- **The side panel jumped to the top.** Closing a card's right-click menu, or picking an item on it such as Move to Idle/Done, selected the focused tab's card and scrolled away from the card you clicked. The selection now stays.
- **`agentmaster <verb>` could start the app.** In a per-user, all-users or portable install, the `agentmaster` command found Agentmaster.exe itself. A CLI call from a script or a Claude session could therefore start the app and put a "profile in use" dialog on the screen. A CLI call now goes to agentmaster-cli.exe before anything else runs, and the release CLI no longer reads the dev install's profile.
- **The one-command installer, hardened.**
  - Typed into a tab of the Agentmaster it upgrades, it goes on in a window of its own (closing the app used to end it halfway).
  - `irm | iex` no longer changes the PowerShell you ran it in.
  - It asks before closing a running Agentmaster, and starts it again afterwards. It also closes the app package's own instances itself, since `Add-AppxPackage` cannot.
  - `-Version 0.7.10` finds a pre-release tag, and `-ZipPath` installs a zip you downloaded.
  - A refused GitHub token is dropped and the call retried.
  - Ctrl+C in the middle of an upgrade puts the old version back.

## Worth knowing when you upgrade from 0.8.0

- ⚠️ **Each session keeps its Prompt Queue Runner mode.** 0.8.0 put every reopened session on the default mode; 0.8.1 brings each back as it was. A session that the runner paused by itself (stop-on-error, a send that never started its turn, a lost send) stays Paused until you turn it back on. New sessions still start on the default under **Settings → Prompt Queue Runner**.
- ⚠️ **New folders in your profile.**
  - `backups\` holds a copy of every state file from the first start of each new version; the first start of 0.8.1 makes one.
  - `shadow\` keeps each state file as this version last wrote it, to repair what an older version drops.
  - `recovery\` appears only when `sessions.json` could not be used at a start.
  - `package-data\` appears only when a package removal kept the package's own files.

  A state file now begins with a `"writer"` stamp.
- ⚠️ **Telegram cards carry the screen.** It is on by default. With several sessions running it uploads roughly 100–250 MB an hour, and the screen travels through Telegram, whose bot chats are not end-to-end encrypted.
- ⚠️ **A voice note's words reach Anthropic.** The AI correction sends the note's text, never its audio, through the PC's Claude Code login; **AI correction → Off** keeps the engine's words instead. The audio goes only to the engine you chose: a local worker keeps it on the PC, and an endpoint receives it.
- ⚠️ **Shift+Click on a tab starts it in place** instead of switching to it.
- ⚠️ **A card's Close Tab now closes a session in another window.** In 0.8.0 that item did nothing for such a session; now that window closes it, after asking.

## Install

**One command (PowerShell)** installs 0.8.1, with no administrator rights needed. It asks where Agentmaster should live:
- **[1] Just for you**, the default: no administrator rights, now or for any update.
- **[2] For all users of this PC**: one administrator approval, only for that step.
- **[3] As the app package** (MSIX): an administrator trusts this release's certificate once.

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.8.1
```
- Add `-Scope User` (or `Machine` / `Package`) to skip the question, and `-Launch` to start Agentmaster right after.
- Run by an administrator from another account, or as SYSTEM, it installs for the user signed in to Windows and starts Agentmaster as them. `-ForCurrentAccount` installs for the running account instead.
- An unattended run adds PowerShell's `-NonInteractive`.

0.8.1 is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable zip (no certificate):** download `Agentmaster_0.8.1.0_x64.zip` (or `_arm64`), unzip anywhere, and run `Agentmaster.exe`. Its first launch asks where Agentmaster should live:
- just for you (recommended);
- for all users;
- for the user signed in to Windows, when another account runs it;
- a folder you pick, or here;
- keep it portable, or decide later.

After an install elsewhere, it offers to move the folder you unzipped into the Recycle Bin. It self-updates in place from then on.

**MSIX bundle (self-signed):** download `Agentmaster.cer` and `Agentmaster_0.8.1.0.msixbundle`. Trust this release's certificate as an administrator (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`; every release has its own), then run `Add-AppxPackage .\Agentmaster_0.8.1.0.msixbundle`.

**Debugging symbols:** the portable zips carry a *public* PDB beside each binary, so every function in a stack trace has its name. `Agentmaster_0.8.1.0_x64_symbols.zip` (or `_arm64`) holds the *full* PDBs, Source-Linked to this release's commit. Extract it into the folder that holds `Agentmaster.exe`, or onto your debugger's symbol path (an MSIX install's folder is read-only).

It runs as **`agentmaster`**, and as **Agentmaster** in the Start menu. Upgrading keeps your data: your existing `%USERPROFILE%\.agentmaster` carries over.

Agentmaster needs Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) on `PATH`, and optionally Codex. It is built on Windows Terminal v1.24.2372 (MIT), and dual-licensed **AGPL-3.0-or-later** + commercial.
