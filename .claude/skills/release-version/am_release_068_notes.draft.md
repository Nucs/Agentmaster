<!-- Agentmaster 0.6.8 release notes — PRE-RELEASE (NOT Latest; v0.6.1 stays Latest).
     PRERELEASE NOTES RULE: show ONLY this prerelease's OWN explicit delta vs the previous release (0.6.7).
     ~46 commits v0.6.7..c236f9012, themed. Mainline tip = c236f9012 (origin in sync).
     Headliners: tab-title naming conventions (659ea2207+5615b8dbd), in-app Debug Mode (c236f9012+ddba06a1a),
     one-click Claude install (2c292ac8f), worktree-shared tab color (829db210a), toast click->foreground+jump
     (c04f9cf9a+4e6da3cc6), LocalTooltip panels, home-dir infer; + a big stability/crash + perf batch.
     Omitted (internal/dev/no-op): logging/nav, exception-forensics, the non-virtualizing swap+revert (net-zero),
     dev-only splash/brand text, docs. PRERELEASE mechanics: --draft=false --prerelease --latest=false (0.6.1 stays Latest).
     Assets: Agentmaster_0.6.8.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.8 (pre-release)

**0.6.8 is a large pre-release** — configurable **tab-title naming**, an in-app **Debug Mode**, **one-click Claude install**, and a big batch of **stability** (window-close / tab-drag crash fixes) and UI polish. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.6.1** remains the default for everyone else.

## New in 0.6.8

- **Tab-title naming conventions.** How an untitled session's tab title derives from its working directory is now a **Settings ⚙ → Tabs → "Tab title naming"** choice, with a **live preview**. Pick from **Last word** of the folder (the new default — `Potato.Tomato.SlangGang` → `SlangGang`), **Folder name**, **Two folders** (`repos/Agentmaster`), **Capital letters** (`PotaTo.Tomato.Slang` → `PTTS`), or the **git-branch trio** — **Branch name**, **Branch / folder**, **Branch / two folders** (`feature/ui/Agentmaster`) — plus a **Title-case** option and **spaces → underscores**. Replaces the old fixed heuristic that named things `PTS` with no way to change it.
- **Debug Mode — unlock Auto Testing in a release build.** A new **Settings ⚙ → About → Enable Debug Mode** toggle (persisted) turns on the developer-only **Auto Testing / Tests Autorunner** subsystem in a normal release install — the durable, in-app twin of launching with `--debug` — so queued prompts auto-send on turn-complete, Send-now works, and every hidden autorunner surface appears.
- **One-click Claude install.** When no native `claude.exe` is detected, the "Claude Code not found" prompt now offers a **one-click install/migrate** tailored to your machine — `claude install` (npm) or the official claude.ai native bootstrap — instead of only linking the docs.
- **A git repo and all its worktrees share one tab color.** Worktrees of the same repository now wear the repository's single per-directory color, so a repo and its worktrees read as one at a glance (grouping and mechanics stay worktree-granular).
- **Click a notification to jump to the session.** Clicking a session's Windows toast now **brings its window to the front and switches to its tab** (restoring it if minimized, across windows) — and a toast click no longer opens a stray empty window.
- **Home-directory sessions infer their working directory.** A session you launch in your home folder (where a bare `claude` almost never actually works) now infers the directory its tool calls concentrate in — in **every** tab-color mode — so its color, grouping, and "Open New Session Here" target follow the real work, not the empty launch folder.
- **Designated-area tooltips.** The Settings cog, the Sessions page, and the Manager tab now render their hover help in a **fixed side panel** instead of floating tooltips that chase the pointer — every control is titled, and the panel is sized to the window.

## Fixes

- **Window-close & tab-drag crashes.** A cluster of teardown crashes is fixed: every UI-thread destructor path is guarded, six unguarded fire-and-forget lanes are contained, and the MUX tab-strip drag-start null-dereference is guarded — so closing a window or dragging a tab no longer risks terminating the app.
- **Runaway terminal auto-scroll.** Selecting text and releasing outside the terminal could leave it **scrolling as if the mouse were held above the pane**; auto-scroll now stops on pointer-capture loss, pointer-cancel, and focus loss.
- **Truer notifications.** A "completed / waiting for you" toast is now **held** while a session's background work (a shell, a `run_in_background` agent, a teammate) is still live — so a session that's actually still working doesn't fire a premature "waiting for you" — plus a 20-second per-session double-toast guard.
- **`/model` no longer flips a session to "Running."** Running a local slash command like `/model` on an idle or waiting-for-you session no longer bounces it to **Running**; turn-less user lines and the `/compact` bridge are handled too.
- **Long tab titles show in full.** The tab-title length limit is now a **255-character safety net**, not a 30-character display cap — a long name is no longer clipped.
- **"Copy Launch CLI" pastes and runs.** The copied command is prefixed with the PowerShell call operator, so pasting it into a shell runs instead of just printing the path.
- **UI performance.** Tab tooltip cards are built **on hover** instead of every tick (killing a measured ~65%-of-a-core UI-thread burn), the Manager's per-notify refresh is coalesced and throttled, the sessions autosave is debounced to one write per quiet second, and the pending-input poll is a single integer compare on a quiet tab.
- **Splash.** Closing the loading window now **aborts the launch** (shuts the starting instance down) instead of continuing to start up in the background.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.6.1** stays the default Latest.

**One command (PowerShell)** — installs 0.6.8 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.8
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.6.8.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.8.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.8.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
