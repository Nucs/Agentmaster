<!-- Agentmaster 0.7.0 release notes — STABLE (becomes Latest; the 0.6.x line graduates).
     STABLE = the comprehensive changelog since the last stable (0.6.1): a curated Highlights reel +
     the full per-version fold of every intervening pre-release (0.6.3..0.6.9). release-notes skill §6.
     Assembled from the (now-terse) per-prerelease drafts; per-version boilerplate stripped, New/Fixes kept.
     ⚠ NOT YET RELEASED — the user triggers it officially later. When releasing:
       git tag v0.7.0 <commit> && git push origin v0.7.0  -> release.yml -> apply this body ->
       publish: gh release edit v0.7.0 -R Nucs/Agentmaster --draft=false --latest --prerelease=false
       (REPLACES 0.6.1 as Latest). Assets: Agentmaster_0.7.0.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.7.0

**0.7.0 is a stable release — the entire 0.6.x pre-release line, now the default for everyone.** Since the last stable (0.6.1) Agentmaster gained a lot — **`/handover`** to pass a session to a fresh successor, **Windows notifications** when an agent needs you, **configurable tab-title naming**, a **launch-model picker**, a live **current-model** readout, and an in-app **Debug Mode** — and it fixed the things that made the 0.6.x line worth graduating: the **23 GB memory leak**, the **resume crash-loop**, the **0.6.8 UI freeze**, and an **auto-updater that never prompted**. This build becomes the new default **Latest**; the highlights are below, followed by the complete per-version changelog since 0.6.1.

## Highlights

### New
- **`/handover` & `/handover-here`.** Hand a session off to a fresh successor — a new tab beside it, or one that replaces it in place — seeded with a full self-written briefing as its first message. Fans out: several briefing files in one turn open several parallel successors. A Settings **Commands** tab renames/disables the commands and shapes the successor (model · title · which files count · delete-after).
- **Windows notifications.** Every managed session (Claude & Codex) raises a toast when it leaves **Running** for another state; clicking it brings the window to the front and jumps to the tab. A **Notifications** cog tab controls the states, focus-skip, and sound.
- **Configurable tab-title naming.** Choose how an untitled tab's title derives from its working directory — folder-word / folder / two-folders / capitals, or the **git-branch** trio — plus case and spaces→underscores, with a live preview.
- **Launch-model picker.** Every "Open New Session Here" **and** "Fork session / Fork here" is a picker — Default plus your configured models — so a launch or fork can start on a chosen Claude model. The list is a cog setting that validates as you type.
- **Live current-model readout.** Each session shows the model it's *actually* on (read from the transcript, so a mid-session `/model` updates it) on the board card, tab tooltip, and per-tab overlay.
- **In-app Debug Mode** (Settings → About) unlocks the developer-only Auto Testing / Tests Autorunner in a release install, and **one-click Claude install** installs or migrates Claude straight from the "not found" prompt.
- **Tab display options.** A **"Remove colors"** coloring mode (your saved colors kept), an option to **hide the profile icon** on tabs, and the tab strip now **scrolls to reveal a selected session's tab**.
- **Smarter state & tab color.** A still-running shell / background agent / teammate keeps a session **Running**; a git repo and **all its worktrees share one tab color**; a session launched in your **home directory infers** where it actually works. Designated-area tooltip panels replace floating tooltips across the cog, Sessions, and Manager.

### Fixes
- **The 23 GB Manager memory leak** — a reference-cycle leak that grew ~85 MB/min under a busy fleet until the UI froze.
- **The resume crash-loop** — heavy sessions dying in a self-sustaining loop seconds after launch; every summary/analyze lane is now contained.
- **The 0.6.8 UI freeze** — the summary panel re-resolving every session on the UI thread until it stopped responding; now cached, focus-gated, and floored.
- **Auto-updates that never fired** — 0.6.x installs never prompted for updates (the preferences were stored at the wrong level of `settings.json`); fixed end-to-end, with atomic writes and a working "Not now."
- **Window-close & tab-drag crashes** — guarded UI-thread teardown + the MUX tab-drag null-deref, so closing a window or dragging a tab no longer risks a terminate. Plus a runaway terminal auto-scroll fix and truer, non-premature notifications.
- **"Restart session" actually restarts** — no `259` banner, no false archive, and a fork keeps its branch. `/model` (and other local slash commands) no longer flip an idle session to Running.

---

**Full changelog since 0.6.1** — every change, grouped by the pre-release it first shipped in:

## 0.6.9
### New

- **`/handover` — hand your session off to a fresh successor.** Type **`/handover <what to carry over>`** in any managed Claude session: Claude writes a self-contained briefing (`HANDOVER-<topic>.md`) addressed to its successor, and Agentmaster automatically opens a **new tab** with a fresh session in the same working directory — seeded with that briefing **verbatim and in full as its first message**. You get a clean context window that already knows everything, without copy-pasting. It's repeatable (every `/handover` spawns its own successor), and it **fans out**: if Claude writes **several** briefing files in one turn, you get **several parallel successors**, one per file.
- **`/handover-here` — the in-place twin.** Same hand-off, except the successor **replaces the current tab** instead of opening beside it. The original session is archived (and stays resumable from the **Sessions** browser), so a long-running tab can start fresh without you losing anything or re-arranging your workspace.
- **Customize the hand-off — a new Settings "Commands" tab.** **Rename** either command (the word you type is the command's name) or **disable** it entirely, and **shape what a hand-off produces**: pick the successor's **model**, **rewrite its title** with a find/replace regex, control **which markdown files count** as a briefing (a file-match regex, defaulting to the `HANDOVER-…` naming), and optionally **delete the briefing file** once the successor has actually started — the answer to leftover `HANDOVER-*.md` clutter. Each tab also gains a **Reset** button to restore its defaults.

### Fixes

- **The 0.6.8 UI freeze.** On 0.6.8 the window could stop repainting and stop responding to input while everything else kept running normally — sessions still advanced, hooks still arrived. The cause: the summary panel re-resolved every linked session's prompt-jump targets every few seconds, on the UI thread, for **all** sessions rather than the visible one; with a busy fleet and long conversations that pegged the thread at ~98% of a core and it stopped pumping input entirely. That refresh is now **cached** per buffer change, **limited to the focused tab**, and **floored at one pass per 2.5 minutes** — with a ~5× faster resolver behind it.
- **Automatic updates actually work now.** The updater stored its three preferences at the wrong level of `settings.json`, so the startup and hourly checks **never saw "Allow pre-release versions"** — they always queried the stable channel, which (with every release since 0.6.4 marked pre-release) meant a 0.6.x install **never prompted for an update at all**, no matter what the checkbox said; Skip/Postpone choices were also wiped by the next settings save. All three now round-trip correctly, writes are atomic, **"Not now"** silences the updater until the next launch, and the whole surface is hardened against errors — so updates flow again from here on.
- **No more premature "completed" toast.** A session that reported a completion after a sub-2-second Running span — while it was in fact still working — no longer fires a misleading "waiting for you" notification; the toast is held for re-light confirmation first.


## 0.6.8
### New

- **Tab-title naming conventions.** How an untitled session's tab title derives from its working directory is now a **Settings ⚙ → Tabs → "Tab title naming"** choice, with a **live preview**. Pick from **Last word** of the folder (the new default — `Potato.Tomato.SlangGang` → `SlangGang`), **Folder name**, **Two folders** (`repos/Agentmaster`), **Capital letters** (`PotaTo.Tomato.Slang` → `PTTS`), or the **git-branch trio** — **Branch name**, **Branch / folder**, **Branch / two folders** (`feature/ui/Agentmaster`) — plus a **Title-case** option and **spaces → underscores**. Replaces the old fixed heuristic that named things `PTS` with no way to change it.
- **Debug Mode — unlock Auto Testing in a release build.** A new **Settings ⚙ → About → Enable Debug Mode** toggle (persisted) turns on the developer-only **Auto Testing / Tests Autorunner** subsystem in a normal release install — the durable, in-app twin of launching with `--debug` — so queued prompts auto-send on turn-complete, Send-now works, and every hidden autorunner surface appears.
- **One-click Claude install.** When no native `claude.exe` is detected, the "Claude Code not found" prompt now offers a **one-click install/migrate** tailored to your machine — `claude install` (npm) or the official claude.ai native bootstrap — instead of only linking the docs.
- **A git repo and all its worktrees share one tab color.** Worktrees of the same repository now wear the repository's single per-directory color, so a repo and its worktrees read as one at a glance (grouping and mechanics stay worktree-granular).
- **Click a notification to jump to the session.** Clicking a session's Windows toast now **brings its window to the front and switches to its tab** (restoring it if minimized, across windows) — and a toast click no longer opens a stray empty window.
- **Home-directory sessions infer their working directory.** A session you launch in your home folder (where a bare `claude` almost never actually works) now infers the directory its tool calls concentrate in — in **every** tab-color mode — so its color, grouping, and "Open New Session Here" target follow the real work, not the empty launch folder.
- **Designated-area tooltips.** The Settings cog, the Sessions page, and the Manager tab now render their hover help in a **fixed side panel** instead of floating tooltips that chase the pointer — every control is titled, and the panel is sized to the window.

### Fixes

- **Window-close & tab-drag crashes.** A cluster of teardown crashes is fixed: every UI-thread destructor path is guarded, six unguarded fire-and-forget lanes are contained, and the MUX tab-strip drag-start null-dereference is guarded — so closing a window or dragging a tab no longer risks terminating the app.
- **Runaway terminal auto-scroll.** Selecting text and releasing outside the terminal could leave it **scrolling as if the mouse were held above the pane**; auto-scroll now stops on pointer-capture loss, pointer-cancel, and focus loss.
- **Truer notifications.** A "completed / waiting for you" toast is now **held** while a session's background work (a shell, a `run_in_background` agent, a teammate) is still live — so a session that's actually still working doesn't fire a premature "waiting for you" — plus a 20-second per-session double-toast guard.
- **`/model` no longer flips a session to "Running."** Running a local slash command like `/model` on an idle or waiting-for-you session no longer bounces it to **Running**; turn-less user lines and the `/compact` bridge are handled too.
- **Long tab titles show in full.** The tab-title length limit is now a **255-character safety net**, not a 30-character display cap — a long name is no longer clipped.
- **"Copy Launch CLI" pastes and runs.** The copied command is prefixed with the PowerShell call operator, so pasting it into a shell runs instead of just printing the path.
- **UI performance.** Tab tooltip cards are built **on hover** instead of every tick (killing a measured ~65%-of-a-core UI-thread burn), the Manager's per-notify refresh is coalesced and throttled, the sessions autosave is debounced to one write per quiet second, and the pending-input poll is a single integer compare on a quiet tab.
- **Splash.** Closing the loading window now **aborts the launch** (shuts the starting instance down) instead of continuing to start up in the background.


## 0.6.7
### New

- **Windows notifications when a session needs you.** Every managed session — Claude *and* Codex — now raises a **Windows toast** the moment its status leaves **Running** for anything else, so a tab or window you aren't looking at can still get your attention: *"‹session title› — Has completed after 2h30m and is ‹status›."* A new **Settings ⚙ → Notifications** tab controls it: a master on/off, a per-state checkbox for each target state (**Waiting for you / Needs approval / Idle / Done / Error** — all on by default), **"Skip when the tab is focused"** (on), and **"Play the notification sound"** (on).
- **See each session's current model at a glance.** A session's **live model** now shows in short form — on the **Triage-Board card**, the **tab tooltip**, and the **per-tab overlay** (left of the status dot). It reflects the model the session is *actually on*, read from the transcript, so a mid-session **`/model`** switch updates it (the launch `--model` / env is only the initial request and goes stale). The family list that shortens bare `--model` aliases is now a **cog setting** (Settings → Sessions), so a newly-shipped Claude family is recognized without waiting for an app update.
- **The "Launch models" editor validates as you type.** The cog's **Settings ⚙ → Sessions → Launch models** box (the launch-model picker's list, from 0.6.6) now lints live like the environment-variable editors — a colored border plus a status line ("✓ N models in the pickers" / a warning with the first issue / an error), so a typo in a `Display name | model-id` entry is obvious before it reaches the menus.


## 0.6.6
### New

- **Launch-model picker — start or fork a session on the model you choose.** Every **"Open New Session Here"** *and* every **"Fork session" / "Fork here"** affordance — on the Triage Board, the Explorer tree, an **External** row, the **Sessions** page (its detail buttons are now split buttons), and the tab right-click menu — is now a **picker**: **Default** (your global model, or Claude's own default when that's blank) plus one item per configured model. Picking a model launches (or forks) *that one session* with `--model <id>` — only that launch is affected (resume / restart still follow your settings model, and it's deliberately not persisted). The model list is a **new cog setting** (**Settings ⚙ → Sessions → Launch models**), a simple **"Display name | model-id"** list shipped with **Fable 5**, **Opus 4.8**, and **Sonnet 5** (edit it to add your own). The Triage-Board card / per-tab overlay `model·effort` adornment reflects the pick automatically; Codex sessions keep their plain New/Fork items (the list is Claude models).


## 0.6.5
### New

- **"Remove colors" tab-coloring mode.** A fourth option under **Settings → Tabs → Tab coloring**: no tab is colored at all — but your saved per-directory / per-session colors are **kept** (not erased), so switching back to any colored mode restores exactly the colors your fleet had. It's strip-wide (every non-managed tab's color is *suspended*, never voided) and "Change tab color" is disabled while the mode is active.
- **Hide the profile icon on tabs.** A new global **"Show icons on tabs"** setting (default **off** — an Agentmaster change from stock Windows Terminal) drops the profile icon from every tab, so a tab reads on its status dot + title. Toggles live.
- **The tab strip reveals a selected session's tab.** Selecting a session on the Triage Board or Explorer tree now scrolls the tab strip so that session's terminal tab is visible — completing the tab-strip half of the Linked-Lenses selection sync (previously the highlight pill could sit scrolled off-screen with many tabs). It's virtualization-aware and lands the tab clear of the overlay's scroll / **+** buttons.
- **A still-running shell, background agent, or teammate keeps the session "Running."** Work that *outlives the turn* — a live shell job, a `run_in_background` agent, or a Claude teammate writing to the lead's side files for minutes after the lead's turn ends — now correctly reads **Running** instead of dropping to Idle / Done / Waiting-for-you, and settles back to Waiting only once that background work actually quiets.
- **Dismiss an Error card by hand.** The triage **Move to Idle** / **Move to Done** actions now extend to the **Error** state on both the Triage Board and the Explorer tree, so you can clear an errored session you've already dealt with.
- **Ctrl+Home** toggles the Agent Manager (home / back), rebound from Shift+Home.

### Fixes

- **"Restart session" actually restarts.** Restarting a managed session could print a `[process exited with code 259] … press Enter to restart` banner into the pane, then **falsely archive** the freshly-restarted session ~1.4 s later, and relaunch an **empty** conversation (a forked session silently losing its branch). Five distinct defects behind that were fixed; Restart now cleanly re-points the connection with no banner, no false archive, and a fork keeps its branch.
- **A marked triage state survives opening the tab.** Marking a background session **"Move to Waiting-for-you"** (or **Mark Unread**) and then activating it for the first time reset it straight back to **Idle** — because focusing a lazily-restored tab starts its `claude`, whose `SessionStart` hook unconditionally forced the state to Idle. A lazy-start `SessionStart` now **preserves** an at-rest "needs-you" triage (this also reinforces the crash-restore state preservation).
- **Overlay buttons register clicks on their icons.** Clicking the *glyph* of a per-tab overlay button (e.g. the summary-panel **pencil**) could miss because the icon ate the hit instead of the button — fixed, and the pencil now gives clear visual feedback (a state-colored glyph, plus an empty-panel placeholder) on every click.
- **Teammate & background-agent traffic no longer pollutes your history or fakes the cache hint.** When a Claude teammate or a background agent delivers a message to a session, that delivery fires a real prompt-submit on the receiving session — which used to record the wrapper text into that session's **Sent/Typed** history (and persist it to `sessions.json`) and light the Triage-Board **⚡ "still cached"** hint even though the teammate's turn ran in its own context. Corpus-audited across both delivery shapes, that machine traffic is now noise-gated out of the Typed record, and the ⚡ hint keys strictly on the receiving session's own API turns.
- **Consistent tab-strip height.** A tab with a multi-line title no longer changes the tab-strip height depending on scroll position.


## 0.6.4
### Fixes

- **The 0.6.x "resume crash-loop."** Resuming a large / heavy session — an image-paste-heavy, `/compact`-ed transcript — could put the app into a **self-sustaining crash loop**: it would die silently ~2–4 s after launch, durability would reopen the workspace, and it would die again. Root cause: the summary surfaces (the per-tab **overlay panel**, the Manager's **Summary** pane, and the **Sessions** detail with its neighbor prefetches) all whole-file-analyze the *same* transcript **concurrently on background threads** with no exception containment — so a transient memory spike (`std::bad_alloc`) on a huge transcript threw, and a throw on a detached thread / fire-and-forget coroutine is an **instant, log-less process death**. (The 0.5.5 line survived the same data for weeks because it did a single-pass analyze; every 0.6.x launch died in 15–35 s.) Now **every** summary/analyze lane is contained end-to-end — a failure degrades to an empty panel plus one log line instead of crashing — the analyze footprint is bounded, and the two **Copy** lanes (**Copy Transcript** / **Copy Summary**, the same unguarded class but click-triggered) are netted too.


## 0.6.3
### Fixes

- **The 23 GB Manager memory leak.** After 0.6.1 fixed the tooltip-leak freeze, a *second* leak remained: a C++/WinRT reference cycle in the Triage-Board / Explorer-tree session context menu (the menu held its host card/row strongly, and the menu's **Tags** item held the host strongly back — an uncollectable cycle under COM refcounting). Because the Manager rebuilds its board and tree on every registry update (plus a 30 s backstop), each rebuild leaked the entire discarded card/row subtree — measured at roughly **85 MB/minute** under a busy fleet, reaching ~23 GB and eventually freezing the UI. The menu is now weak-anchored to its host, animation storyboards are stopped instead of left running forever, and every insert-only cache is bounded — so a long-lived Manager no longer grows.

### New

- **Hourly update check.** With auto-update enabled, Agentmaster now re-checks for a new version **every hour while running**, not only at startup — so a fix like the one above reaches you without a restart.



## Install

**One command (PowerShell)** — installs 0.7.0 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.7.0
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). 0.7.0 is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable (no cert):** download `Agentmaster_0.7.0.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.7.0.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.7.0.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
