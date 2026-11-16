<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request: "I want you to create a more one-row changes in the
     release notes, like we do in the head of all release notes ... sentences that start with a bold and then,
     essentially a sentence, maybe a sentence and a half, or a bit more if you really have to", and the layout the
     owner picked from three previews rendered by GitHub: "Head only". The body is a 1-2 sentence intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the unchanged Worth
     knowing and Install sections. The long body published until 2026-11-16 is this file at 89cbb9e26; none of it is
     on GitHub any more. Besides the head: the long-form Highlights became the rows (plus three rows from the per-version changelog: unsent drafts, the AI search's missed results, the tab strip); Worth knowing is its own section; the per-version changelog since 0.7.0 (0.8.0, 0.7.10, 0.7.9, 0.7.7) left the page.
     Agentmaster 0.8.0 release notes — STABLE (becomes Latest; the 0.7.x pre-release line graduates).
     STABLE = the comprehensive changelog since the last stable (0.7.0, 2026-07-25): a curated Highlights reel + the
     0.8.0-only batch + the full per-version fold of every intervening pre-release (0.7.10, 0.7.9, 0.7.7: their PUBLISHED
     bodies minus their intro paragraph and Install, headers demoted; release-notes skill §6). The nightlies are invisible
     to baselines: 0.7.7 re-shipped the 0.7.1–0.7.6 nightlies (it baselined off 0.7.0) and 0.7.9 re-shipped 0.7.8.
     0.8.0's own batch: v0.7.10-prerelease..77a7c7db1 = 122 commits (74 code: 49 feat, 23 fix, 1 perf, 1 build; 48 docs,
     design and deploy notes, which change nothing a user sees). Since 0.7.0: 348 commits.
     RELEASING from the branch tip (the user, 2026-10-22: "Kindly release stable version with latest commit"):
     77a7c7db1 plus the commit of this draft. That includes 6224e14e7 (Telegram FILE LINKS, off by default; its Settings
     texts name the owner's relay example.com and K:\source\proxmox paths) and the install rework (aefacf2cc, 0792de95d,
     64cbd92aa, 77a7c7db1) plus the console guard (697b37c90).
     Builds behind it: production 0.6.8.23 (purged Release) and dev 0.0.1.32 run 5f70cfda1; dev 0.0.1.33 adds
     6224e14e7 + aefacf2cc; the install QA built x64 Release of 77a7c7db1's sources (Q:\agentmaster-layouts\QA).
     Version: tag v0.8.0 -> ver4 0.8.0.0, rawver 0.8.0. The updater compares major.minor.patch numerically, so 0.8.0 is
     offered over 0.7.10 (pre-release channel) and 0.7.0 (stable channel).
     ⚠ GitHub has seen nothing past e2a8e5570: the session database service (agentmaster-sessiondb.vcxproj) and the
     workflows' "Fetch libduckdb" step (e8cb7c4ad) get their FIRST CI run with this push, and arm64 has never been
     built anywhere (release.yml builds it; ci.yml is x64 only). The push also publishes the reworked
     tools/Install-Agentmaster.ps1, which the one-command install fetches from the branch.
     Push first (fast-forward): env -u GH_TOKEN -u GITHUB_TOKEN git push origin agentmaster
     Tag:     git tag -a v0.8.0 -m "Agentmaster 0.8.0" <tip> && env -u GH_TOKEN -u GITHUB_TOKEN git push origin v0.8.0
     Notes:   strip this comment (awk 'f{print} /^## Agentmaster 0.8.0/{f=1; print}' <draft> > body.md), then
              gh release edit v0.8.0 -R Nucs/Agentmaster --notes-file body.md
     Publish: gh release edit v0.8.0 -R Nucs/Agentmaster --draft=false --latest --prerelease=false (REPLACES 0.7.0 as Latest).
     Assets: Agentmaster_0.8.0.0.msixbundle, Agentmaster_0.8.0.0_x64.zip, Agentmaster_0.8.0.0_arm64.zip, Agentmaster.cer.
     DONE 2026-10-22 (times UTC):
       - pushed e2a8e5570..0468fecf6; tag v0.8.0 -> 0468fecf6 (this draft's commit);
       - ci.yml run 39800299648 (x64, from scratch): success;
       - release.yml run 39800356681: success (queued ~7 min, built 15:04-15:28, x64 + arm64; the first arm64 build
         of the session database service and the first "Fetch libduckdb" step passed);
       - assets: bundle 51,911,334 B, x64 zip 28,494,890 B, arm64 zip 29,945,712 B, cer 768 B. The x64 zip was
         downloaded and listed: Agentmaster.exe, WindowsTerminal.exe, agentmaster-cli.exe, agentmaster-sessiondb.exe,
         duckdb.dll, .portable, .am-version (0.8.0.0), .am-manifest;
       - notes applied (equal to this body), published 15:29:24Z as stable + Latest; /releases/latest = v0.8.0. -->
## Agentmaster 0.8.0

**0.8.0 is a stable release: the whole 0.7.x pre-release line plus a large final batch, now the default for everyone.** This build becomes the new default **Latest**; below are the highlights since the last stable (0.7.0).

## What's new

- **Your fleet in Telegram.** With your own bot (Settings → Messaging), every Claude tab that is not idle becomes a topic holding its whole conversation, with its state as the icon and a status card with buttons.
- **Drive sessions from your phone.** A message in a topic is queued as that session's next prompt, `/steer` sends one into the running turn, and the card's buttons fork, restart or start sessions.
- **Answer Claude's questions from Telegram.** A multiple-choice question arrives as one card you answer with a tap or in your own words, while the PC's own dialog stays usable.
- **Search and start sessions from Telegram.** The 🔎 Search topic finds and reopens old sessions, a new thread starts a session in the folder and window you pick, and a pinned message sums up the fleet.
- **File links from Telegram.** Turned on under Settings → Messaging → FILE LINKS, Claude's `file:///` links become download links your phone can open, served by a small file server inside Agentmaster.
- **The Agent Manager is a page, beside a fleet side panel.** The Manager opens from the tab strip's Home button, and a side panel docked beside the terminal shows the session cards over the summary of the session you are in.
- **The Prompt Queue Runner, in every build.** Prompts you queue for a session go out one per finished turn, Auto or Paused per session, each filled, read back and only then submitted.
- **AI search for your sessions.** Ask for a session in plain words on the Sessions page: a hosted Sonnet 5.5 reads your sessions, shows the ones that fit with the evidence, and can resume or fork one.
- **Presets: Claude Code on other models and providers.** OpenRouter, Xiaomi MiMo, NanoGPT, your own RunPod vLLM endpoint or any Anthropic-compatible URL join every launch and fork menu once added under Settings → Presets.
- **Read any conversation.** Show Transcript renders a session as Markdown, Copy ▸ Transcript copies it at five detail levels, and the summary spans the whole conversation.
- **Park, dock and batch-close tabs.** Deactivate ▸ stops a session's claude while its tab keeps its place, a tab dragged onto another window docks there, and Close ▸ Of Same Folder closes a folder's sessions at once.
- **Unsent drafts are kept.** The tab overlay's mail button moves a tab's unsent prompt into its queue, and a reopened session gets its remembered draft typed back into its box.
- **Interrupted work goes on.** A turn cut off by a crash reads Error, a turn that died on an API error continues when resumed, and busy tabs start by themselves after a restart.
- **Install your way.** The installer asks where Agentmaster should live: just for you with no administrator rights, for all users of the PC, or as the app package.
- **Unread work on the taskbar.** A red count on the taskbar button and a framed window preview show where unread sessions wait.
- **Software rendering and lighter hidden tabs.** Software rendering (Settings → Behavior → RENDERING) gets around the NVIDIA driver's ~62-terminal limit, and hidden tabs stop drawing and give back their GPU memory.
- **Tab size.** Settings → Tabs & Overlay → TAB SIZE sets the tabs' width mode, minimum and maximum width, font and height, live in every window.
- **A folder filter on the Sessions page.** 📂 Folders lists the folders your sessions worked in, most recent and most used, and shows the sessions of any you pick.
- **Unsent prompts travel with a fork.** A fork types the source's unsent prompt into its own box, pasted blocks included, without sending it.
- **Tag filter chips on the Triage Board.** The board's header filters its cards by tag, showing a card that carries any picked tag, with an Untagged chip for the rest.
- **A MOST RECENT sort.** The board, side panel and tree can sort by turn starts and ends, so a session busy with tool calls no longer floats above the one whose turn just ended.
- **A session database (preview, off by default).** Settings → Sessions → SESSION DATABASE keeps every Claude Code session in one DuckDB file that powers the Sessions search and `agentmaster search` / `agentmaster sql`.

## What's fixed

- **Five UI freezes are gone.** Among them a Remote Desktop theme storm, an 8-hour freeze on a tab switch, and a session that hopped between two tabs 175,000 times.
- **Crashes and a hung launch, fixed.** The WinUI tab-drag crash, a crash on a mouse move over a just-opened tab, and a launch that hung with no window no longer happen.
- **Large fleets cost far less.** Animations no longer redraw the whole window, the side panel draws only the cards in view, the scanner follows one file-system watch, and a memory leak in every terminal is fixed.
- **Prompts arrive as typed.** Prompts Claude Code 2.1.277+ refused for an "invisible" character are cleaned first, non-ASCII prompts no longer arrive garbled, and a queued `/add-dir` or `/model` no longer fails the queue.
- **Sessions show the right state.** No more Running for hours after a turn ended, a Running → Waiting → Running flicker, an Error that outlived a retry, or a compaction dropping a running session to Idle.
- **The AI search finds what it missed.** It waits for a complete index instead of answering "still being indexed", searches every session instead of the last 90 days, and its deep search takes 7 s instead of 79 s.
- **Hooks no longer time out.** On Claude Code 2.1.139+ hooks run PowerShell directly instead of Git Bash, and Claude Code's Ctrl+G editor works again with an over-long PATH.
- **Tabs behave.** Orphaned console sessions are reaped, closing a tab no longer starts a dormant neighbor's claude, Deactivate keeps parked tabs as Claude tabs, and a killed fullscreen claude no longer breaks its tab.
- **The tab strip scrolls and trims.** Selecting a far-off tab scrolls the strip to it, and a long title ends in "…" inside its tab instead of running under the next one.
- **Safer installs, updates and uninstalls.** An uninstall removes only what a release shipped, a failed upgrade puts the old version back, and a failed update starts the installed version again.

## Worth knowing when you upgrade from 0.7.0
- ⚠️ **The Prompt Queue Runner is on in every build.** New sessions start in **Auto**, so a prompt still Pending from an earlier Debug Mode run, or a `/handover` briefing that was never delivered, is sent once its session is open and settled. The default is under **Settings → Prompt Queue Runner**.
- ⚠️ **Alt+Left and Ctrl+Home are Windows Terminal's again.** The tab strip's **Home** button is the way into the Agent Manager.
- ⚠️ **The executable is `Agentmaster.exe`.** A `WindowsTerminal.exe` forwarder ships beside it, so old shortcuts, pins and scripts still work.
- ⚠️ **The one-command installer asks where to install.** Script it with `-Scope User|Machine|Package` (`-Portable` still means just for you), and add PowerShell's `-NonInteractive` to an unattended run.
- ⚠️ **One line appended to a transcript.** To let a turn that died on an API error continue, Agentmaster appends Claude Code's own rewind line before the resume (never a deletion, never while a claude runs the conversation). It is the one change Agentmaster makes to a transcript; turn it off under **Settings → Sessions**.
- ⚠️ **Telegram carries what it shows.** The bot is off until you set it up. Once on, your sessions' text, and the terminal's screen when you ask for it, travel through Telegram, whose bot chats are not end-to-end encrypted.

## Install

**One command (PowerShell)** — installs 0.8.0 (administrator rights not needed). It asks where Agentmaster should live: **[1] Just for you** (the default — no administrator rights, now or for any update), **[2] For all users of this PC** (one administrator approval, only for that step), or **[3] As the app package** (MSIX — an administrator trusts this release's certificate once):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.8.0
```
Add `-Scope User` (or `Machine` / `Package`) to skip the question, and/or `-Launch` to start it right after. Run by an administrator from another account, or as SYSTEM, it installs for the user signed in to Windows and starts Agentmaster as them (`-ForCurrentAccount` installs for the running account instead); unattended runs add PowerShell's `-NonInteractive`. 0.8.0 is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable zip (no certificate):** download `Agentmaster_0.8.0.0_x64.zip` (or `_arm64`), unzip anywhere, run `Agentmaster.exe`. Its first launch asks the same question — just for you (recommended), for all users, for the user signed in to Windows when another account runs it, a folder you pick, here, keep it portable, or later — and it self-updates in place from then on.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.8.0.0.msixbundle`, trust this release's certificate (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin; every release has its own), then `Add-AppxPackage .\Agentmaster_0.8.0.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
