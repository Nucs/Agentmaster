<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request: "I want you to create a more one-row changes in the
     release notes, like we do in the head of all release notes ... sentences that start with a bold and then,
     essentially a sentence, maybe a sentence and a half, or a bit more if you really have to", and the layout the
     owner picked from three previews rendered by GitHub: "Head only". The body is a 1-2 sentence intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the unchanged Worth
     knowing and Install sections. The long body published until 2026-11-16 is this file at 89cbb9e26; none of it is
     on GitHub any more. Besides the head: the intro's "Telegram gains" list became rows; the card-screen row names its off switch.
     Agentmaster 0.8.1 release notes — STABLE (becomes Latest, replacing 0.8.0).
     Range: v0.8.0..605e314dc = 84 commits. 37 user-facing: 30 feat + 17 fix, minus 11 dev-tooling ones, plus the
     71a585d81 refactor that changes a Telegram menu. 47 internal: 21 docs, 14 test, 1 tools, 9 build-server feats,
     1 swap-script feat, 1 e2e-host feat. No pre-release since 0.8.0, so there is nothing to fold (release-notes §6):
     the notes are 0.8.1's own batch.
     RELEASING the PINNED commit 605e314dca26cae0606863996b5321e2e7939f1d (the user, 2026-10-29: "Pin current commit and
     lets do 0.8.1 no prerelease"). The tag goes on it even if the branch moves on; this draft's own commit comes after
     it and changes nothing that is built.
     Builds behind it: production 0.8.0.1 runs fe47d239f; dev 0.0.1.39 runs 64a3a4846 (build server b20261028-3).
     672e3bc67 (the profile-overwrite audit) and 605e314dc (state files across versions) are in NO build yet. This
     release's ci.yml and release.yml runs are their first compile and link; before them, only the m5 harness and
     cl /Zs had checked them.
     GitHub had built up to 2a255bc75 (ci.yml run 40312689347: success). That run was the first of a753de9b9's symbols
     steps (x64). arm64's symbols steps run for the first time in release.yml.
     Version: tag v0.8.1 -> ver4 0.8.1.0, rawver 0.8.1. The updater compares major.minor.patch, so 0.8.1 is offered
     over 0.8.0.
     Push first (fast-forward from 2a255bc75): env -u GH_TOKEN -u GITHUB_TOKEN git push origin <draft commit>:agentmaster
     Tag:     git tag -a v0.8.1 -m "Agentmaster 0.8.1" 605e314dca26cae0606863996b5321e2e7939f1d
              env -u GH_TOKEN -u GITHUB_TOKEN git push origin v0.8.1
     Notes:   strip this comment (awk 'f{print} /^## Agentmaster 0.8.1/{f=1; print}' <draft> > body.md), then
              gh release edit v0.8.1 -R Nucs/Agentmaster --notes-file body.md
     Publish: gh release edit v0.8.1 -R Nucs/Agentmaster --draft=false --latest --prerelease=false (REPLACES 0.8.0 as Latest).
     Assets: Agentmaster_0.8.1.0.msixbundle, _x64.zip, _arm64.zip, _x64_symbols.zip, _arm64_symbols.zip, Agentmaster.cer.
     DONE 2026-10-29 (times UTC):
       - pushed 2a255bc75..7145bbce5 (the pin + this draft's first commit); annotated tag v0.8.1 (c0cecc0a2) -> 605e314dc.
       - ci.yml run 40820849372 (x64 Release from scratch, on 7145bbce5): success, done by 05:29. That was the first
         compile and link of 672e3bc67 and 605e314dc anywhere.
       - release.yml run 40820853815: success, with no queue wait:
         - prep 05:00:18-05:00:21;
         - x64 05:00:25-05:19:02 (18.6 min);
         - arm64 05:00:24-05:33:27 (33 min; the first arm64 run of the symbols steps passed);
         - bundle 05:33:29-05:33:57, then release (the draft) at 05:33:59.
       - The assets:
         - bundle 53,163,449 B;
         - x64 zip 39,784,271 B, arm64 zip 41,268,761 B;
         - x64 symbols 179,230,289 B, arm64 symbols 183,104,875 B;
         - cer 768 B.
       - Both x64 zips were downloaded into an empty scratch folder and listed:
         - the portable zip: 83 files under agentmaster-0.8.1.0\, .am-version 0.8.1.0, and 16 public PDBs (83.4 MB
           unpacked), each named in .am-manifest's 46 lines;
         - the symbols zip: the same 16 names as full PDBs (893 MB unpacked; TerminalApp.pdb 222 MB).
         - A UTF-16 probe of the zip's TerminalApp.dll finds "605e314dc" and strings of 605e314dc (the snapshot
           marker), 672e3bc67 ("HELD in the record"), 5a9ba9f79 ("late save refused") and the voice notes.
       - Notes applied; the read-back equals this body apart from its final newline. Published 05:37:58 as stable +
         Latest: `gh release list` shows "Agentmaster 0.8.1 Latest", and /releases/latest answers v0.8.1 with 6
         assets. -->
## Agentmaster 0.8.1

**0.8.1 is a stable release building on 0.8.0. Its theme: your workspace survives everything.** This build becomes the new default **Latest**.

## What's new

- **State files that survive every version.** Every save keeps what another version of Agentmaster wrote, and each new version first copies your state files into `backups\`.
- **🎤 Voice notes in Telegram.** A voice note in a session's topic becomes its next prompt, transcribed with the session's vocabulary by the engine you set under Settings → Messaging → VOICE NOTES, then corrected by Claude.
- **📜 All Screen.** A session's card (or `/allscreen`) sends the whole conversation as tall pictures in Claude Code's own look, at a level you pick, since a fullscreen Claude Code keeps no scrollback.
- **The terminal's screen on every card.** A session's Telegram card ends with a picture of its terminal, taken again whenever the card updates; Settings → Messaging → "Show the screen in each session's card" turns it off.
- **⎋ Esc and 🌙 Deactivate from your phone.** The card's ⚙️ Session page presses Esc in the session or parks its tab, each checked again by the window that hosts it.
- **`/status` like Claude Code's own.** In a session's topic it shows what Claude Code's `/status` dialog shows, plus your account and plan usage, without typing anything into the session.
- **A queued message shows where its turn starts.** A message sent while the session was busy is posted again where its turn really begins, and your own message gets 🫡.
- **The Search topic's pick menu follows the session.** It offers Activate for a parked tab, Wake for an idle one and Go to topic, and Resume only for a closed session.
- **Continue or keep an interrupted turn.** Restarting a session whose last turn you interrupted asks whether to continue it, and a fork made mid-turn asks whether it runs that turn or waits for you.
- **Deactivate All Idle Tabs.** The last item of every Deactivate ▸ menu parks every idle tab, in this window or in all of them.
- **Card and tree menus match the tab's menu.** Side panel cards, board cards and tree rows get the tab menu's Deactivate ▸ and Close ▸ scopes, each run in the window that hosts the tab.
- **Side panel cards wear the tab's status icon.** A card's dot shows the unsent-draft dots, the blinking unread ring and the spark crown, like its tab's.
- **The AI search sees your tabs.** Two new tools list every window's tabs by folder and say what up to 32 sessions are doing now, and why a queued prompt waits.
- **Startup dialogs show in the command that started Agentmaster.** Each dialog at startup is also written to that command's console or pipe, with its choices and its answer.
- **Screen readers and UI Automation.** More dialogs can be read, glyph-only buttons have names, and tabs, cards and rows report their state, unread and unsent draft.
- **Debugging symbols in every release.** The portable zips carry public PDBs, and new `_symbols.zip` assets hold the full PDBs, Source-Linked to the release's commit.
- **"Delete the downloaded copy?"** A portable copy that installed itself elsewhere offers to move the folder you unzipped into the Recycle Bin.

## What's fixed

- **A window close no longer turns its Claude tabs into shells.** An autosave still pending at Close Window saved every Claude tab as a PowerShell tab; the close is now the window's last save.
- **A start without Claude Code no longer wipes a window's tabs.** Tabs a reopened window could not open now stay in its record and come back at the next start, and their Telegram topics stay.
- **An unreadable state file is never saved over.** A `sessions.json` that read as empty (a byte-order mark, NUL bytes, a torn end, a lock) is now read correctly, retried or kept aside before anything replaces it.
- **A restored tab keeps its session record.** A restore that missed the transcript no longer gives the session a new id and drops its tags, favorite and Telegram topic.
- **Installs never delete your data.** Every delete an install, update or uninstall makes refuses a folder that holds Agentmaster state, and a destination holding someone else's files is renamed aside.
- **Each session keeps its runner mode.** A session left Paused no longer comes back on Auto after a window reopen or Resume here.
- **Tags from the tab menu no longer type into Claude.** The Tags panel keeps the keyboard, and Esc closes only the panel instead of interrupting the running turn.
- **Shift+Click on a tab starts it in place.** It starts a parked tab's session and keeps you where you are, as the tab menu says.
- **Launches from the Agent Manager page stay on the page.** A launch there selects the new session on the page, and the side panel's + card launches with one click even while its presets menu is open.
- **The side panel no longer jumps to the top.** Closing or using a card's right-click menu keeps the card you clicked selected.
- **`agentmaster <verb>` no longer starts the app.** A CLI call goes to agentmaster-cli.exe before anything else runs, so a script can no longer open the app or a "profile in use" dialog.
- **The one-command installer, hardened.** It goes on in its own window when run inside the Agentmaster it upgrades, asks before closing the app, and puts the old version back on Ctrl+C.

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
