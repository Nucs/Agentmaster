<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.7.9-prerelease release notes — PRE-RELEASE (the stable 0.7.0 stays Latest).
     Range: v0.7.7-prerelease..b842fcf5a = 118 commits (~89 user-facing; 29 docs/chore/refactor/test/style,
     incl. the 0.7.8 nightly notes archive; also omitted: the hot-deploy tooling dce0d5503 / 0965bd0af,
     which only affect local slot deploys, and the debug-dumps skill daa27dfb7).
     Baseline = the last NON-nightly release = the 0.7.7 pre-release. The 0.7.8 nightly
     (v0.7.8-prerelease-nightly @ f5d1784ad) is INVISIBLE to baselining (release-notes §2), so this
     pre-release RE-LISTS its 25 user-facing commits + the 84 commits past it: presets, the agent harness
     + the Sessions AI search, Show Transcript + the Copy ▸ Transcript levels, the Prompt Queue Runner in
     every build, the rendering + tab-size settings, the taskbar preview frame, the Deactivate scopes, the
     startup deadlock, the cut-off-turn Error, and the UI-thread frame-loop work.
     RELEASING from b842fcf5a (= origin/agentmaster tip; nothing was unpushed).
     Tag v0.7.9-prerelease -> ver4 0.7.9.0, rawver 0.7.9-prerelease. 0.7.9, not 0.7.8: the 0.7.8 nightly
     holds 0.7.8 (the updater compares major.minor.patch numerically, and ver4 0.7.8.0 would collide).
     nightly=false, so release.yml does NOT auto-mark prerelease — mark it at publish. The installer's
     -Version maps to the TAG (full suffix 0.7.9-prerelease); asset names stay numeric ver4.
     Pre-validation: no local build (project rule: a build needs the user's permission). Most of the
     post-nightly batch was committed "(NOT BUILT)"; ci.yml run 38114840284 (x64 Release, same commit) and
     release.yml run 38114984308 (x64 + arm64) are its first clean builds.
     Publish: gh release edit v0.7.9-prerelease -R Nucs/Agentmaster --draft=false --prerelease --latest=false
       (NEVER --latest; the stable 0.7.0 stays Latest).
     Assets: Agentmaster_0.7.9.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.7.9-prerelease

**0.7.9 is a pre-release** consolidating everything since the 0.7.7 pre-release — the 0.7.8 nightly plus a large final batch — on top of the 0.7.0 stable. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.7.0** remains the default Latest for everyone else.

## What's new

- **The Agent Manager is a page behind the Home button.** The pinned Manager tab is gone: the board, tree and Prompt Queue open from the tab strip's Home button, and a window can now hold zero tabs.
- ⚠️ **Alt+Left and Ctrl+Home are Windows Terminal's again.** Both reach your shell and Claude as in stock Windows Terminal; Home and Jump Back are the way in and out of the Manager.
- **A fleet side panel beside the terminal.** It shows this window's (or every window's) session cards over the summary of the session you are in, with a + that launches a session at the click.
- ⚠️ **The Prompt Queue Runner ships in every build.** The prompt queue leaves Debug Mode: a session is Paused or Auto, an automatic send waits for 40 s of steady readiness, and new sessions start in Auto.
- **Presets: Claude Code on other models and providers.** OpenRouter, Xiaomi MiMo, NanoGPT, your own RunPod vLLM endpoint or any Anthropic-compatible endpoint join every launch and fork menu once added under Settings → Presets → TEMPLATES.
- **AI search on the Sessions page.** An AI toggle in the search box takes a request in plain words, and a hosted Sonnet 5.5 fills the table with the sessions that fit, best match first.
- **Show Transcript.** Any conversation opens as Markdown in a reading view over the window, one tab per detail level, with each turn's tool calls folded.
- **Copy ▸ Transcript at five detail levels.** From Minimal (your messages and the final replies) to Max (every written file), with the sessions it was forked or restarted from.
- **The summary shows the whole conversation.** It lists every earlier session the conversation continues (a fork's parent, a plan restart, a /clear or /handover predecessor), numbered straight through.
- **Unread work on the taskbar.** A red count on the taskbar button and a framed preview show the sessions waiting for you, across every window.
- **Software rendering, and hidden tabs that give back the GPU.** Settings → Behavior → RENDERING draws every terminal on the CPU, past the NVIDIA driver's ~62-terminal limit, and hidden tabs stop drawing and free their GPU memory.
- **Tab size.** Settings → Tabs & Overlay → TAB SIZE sets the tabs' width mode, minimum and maximum width, font and height, live in every window.
- **Deactivate ▸ gets Close's scopes.** Tabs to the Left, Tabs to the Right, Of Same Folder and Other of Same Folder join Deactivate Tab, Other Tabs and All Tabs.
- **Unsent prompts travel with you.** A fork types the source's unsent prompt into its own box, pasted blocks included, and Copy Current Prompt hands over the pasted text instead of its label.
- **A MOST RECENT sort, and new sessions on top.** The board, side panel and tree can sort by turn starts and ends, and a new session or fork starts at the top.
- **"Specify a model…" remembers your last pick.** It opens with the model you last committed, in every window and after a restart.

## What's fixed

- **A launch no longer hangs with no window.** The GPU driver could deadlock against the first folder lookup at startup; the folders are now resolved first, and a watchdog ends a hung launch after 25 s.
- **A mouse move over a just-opened tab no longer crashes the app.** A terminal now starts in its first sizing pass and ignores the mouse until it has started.
- **No more 8-hour freeze on a tab switch.** The summary jump could spin forever on a conversation that repeated a prompt's opening words; that input now resolves in 3 ms.
- **A large fleet costs far less UI time.** The animations run on the Composition API, the side panel edits its cards in place, the Tags panel opens at once, and one file-system watch replaces the scanner's folder walks.
- **Sessions show the right state.** No more Running for hours after a turn ended, a Running → Waiting → Running flicker, a cut-off turn reading a ready Idle, or a compaction dropping a session to Idle.
- **Hooks no longer time out at launch and exit.** On Claude Code 2.1.139+ the forwarder runs PowerShell directly (median 0.36 s), every hook has a timeout, and SessionEnd gets 5 s.
- **Claude Code's Ctrl+G editor works again.** Agentmaster trims the PATH it hands each session when it passes cmd.exe's 8,191-character limit.
- **Prompts are no longer refused or garbled.** Everything Agentmaster types is cleaned with Claude Code 2.1.277+'s own invisible-character rule, and non-ASCII prompts such as Hebrew arrive intact.
- **Deactivate and Restart keep their tabs.** A parked tab no longer turns into a plain PowerShell tab, and a restarted session is no longer archived seconds later.
- **The tab strip scrolls and trims.** Selecting a far-off tab scrolls the strip to it, and a long title ends in "…" inside its tab.
- **Closing a tab no longer starts another Claude.** Focus goes to the nearest tab that starts nothing, or to the Agent Manager page.
- **/compact, /clear and resume behave.** The context size updates when a compaction ends, summaries span the whole session, and a /clear keeps its tab managed.
- **Summaries show mid-turn messages and a new fork's source.** A message typed while Claude works appears in the summary, and a fork with no messages yet shows the conversation it came from.
- **Smaller fixes.** Unsent drafts are read under Claude Code's fullscreen view, and Send now on a dormant tab keeps the prompt queued.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.7.0** stays the default Latest.

**One command (PowerShell)** — installs 0.7.9-prerelease (admin not required: as admin it trusts the self-signed cert silently; otherwise it asks — elevate once (UAC), a no-admin per-user portable install, or cancel):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.7.9-prerelease
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.7.9.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained — its first launch offers a per-user install or stays portable.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.7.9.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.7.9.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
