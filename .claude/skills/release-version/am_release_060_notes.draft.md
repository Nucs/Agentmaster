<!-- Agentmaster 0.6.0 release notes — STABLE (becomes Latest; NOT a pre-release).
     Style: matches the 0.5.x drafts (tight intro, "## New in X.Y.Z" + "## Fixes" + "## Install").
     Range: v0.5.9..HEAD = 74 commits of NEW work; v0.5.5..HEAD = 222 (the 0.5.6-0.5.9 pre-release line
     is promoted to stable here). RELEASING from HEAD = 2f6e1d3fa. Tag v0.6.0 -> 0.6.0.0.
     STABLE mechanics: publish --draft=false --latest (this REPLACES 0.5.5 as Latest; no --prerelease).
     Asset names: Agentmaster_0.6.0.0.msixbundle, Agentmaster_0.6.0.0_x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.0

**0.6.0 is a stable release** — the first stable since 0.5.5, and it brings the entire **0.5.6 → 0.5.9 pre-release line** to everyone plus a large batch of new work. Headlines: a full **bookmark Tags** system, **three tab-coloring modes**, **"Activate All Tabs"** + Shift-click eager-init, a **launch splash** that covers session restore, a rewritten **rich tab tooltip**, and broad **crash- and log-flood hardening** (including a fix for a runaway 320 MB log). It also folds in everything the pre-releases added — the unsent-draft **"3 dots"** indicator, **API-error → Error** detection, a **6-tab Settings** cog, **cross-file conversation lineage**, **git worktrees** in the Launch picker, and in-place **session-title editing**. This build becomes the new **default (Latest)** — no "allow pre-release" toggle needed. The project is dual-licensed **AGPL-3.0-or-later + commercial**.

## New in 0.6.0

- **Bookmark Tags — color-coded labels on any session.** Attach durable, named **tags** to a session and they show everywhere it appears: as little **bookmark ribbons** hanging off the bottom edge of its tab, on its Triage-Board card, and in a dedicated **Tags column** in the Sessions browser. Each tag has a **user-pickable color** (durable), a rich **clickable hover panel** listing every session that carries it (click a row to jump to that tab), and the tab tooltip shows a colored tag-chip row. Manage them from a **"Tags"** panel on the tab / card / Sessions-row right-click menu; filter the Sessions browser by **tag filter chips**. Tag removal is explicit (a tag stays listed until you delete its row), and the whole set persists across restarts.
- **Three tab-coloring modes.** Settings now offers how managed tabs get their color: **Shared per working directory** (the classic — a folder keeps one permanent color), **Individual per tab** (each session keeps its own), or **Inferred working directory** (color by the directory a session actually *works in*, detected by occurrence-ranking the paths its tools touch — git-root-aware by default, with machine-`%TEMP%` scratch paths excluded so a scratchpad file can't hijack the color). The color picker itself is now a true occurrence-ranking picker.
- **Activate All Tabs + Shift-click eager-init.** Managed tabs restore **lazily** (a background tab's `claude` doesn't start until first shown). You can now **Shift-click** a dormant tab to activate it **in place** (start it without switching to it), or use **"Activate All Tabs"** to wake the whole fleet — paced (drip-fed, a few at a time) with an **"Activating N tabs…"** busy state so a large workspace doesn't stampede.
- **Launch splash.** A lightweight **splash window** (on its own thread) now covers the ~7 s workspace restore at startup, so launch no longer looks hung. It's a normal taskbar window (minimize / close), never topmost, and dismisses only once the main window has actually settled.
- **Rewritten rich tab tooltip.** The tab hover is now a **modern summary-style card** (anchored under the tab) instead of a flat wall of text — and it was hardened against a long string of XAML fail-fasts (reused-tooltip use-after-free, rapid tab-swap stowed fail-fast, unrooted-owner and layout-cycle crashes) so hovering the tab strip is stable.
- **Auto Testing / Tests Autorunner (renamed; release-clean).** The prompt-queue automation formerly called *Flight Plan / Autopilot* is renamed **Auto Testing / Tests Autorunner** and is now a **development-only testing tool**. In this **release** build the Manager's bottom-right pane is a clean, read-only **session Summary** — the auto-send queue and its controls are hidden and inert. (Your data and every other Manager capability are unchanged.)
- **Stability & log hygiene.** Fixed a runaway logging path that could grow `hooks.log` to **~320 MB** with ~300 K forced disk flushes (a `[Unknown]` notify flood + redundant `sessions.json` writes) — now quiet. Numerous background lanes were wrapped so a thrown exception on a worker thread can never `std::terminate` the process, and the app ships with a new in-repo crash-analysis toolkit behind the scenes.

## Also in 0.6.0 — the 0.5.6–0.5.9 line, now stable

- **Unsent-draft "3 dots" indicator** — see at a glance which tabs hold a prompt you typed but never sent (animated on the tab strip and Triage cards, cross-window, with configurable contrast-picked colors).
- **API errors become a real "Error" state** — a rate-limited / overloaded / 5xx turn is classified Error with its message + HTTP status on the card, and recovers on the next activity (active-leaf-aware, and detected even after Claude writes its post-error bookkeeping).
- **Settings reorganized into 6 tabs**, plus a **Waiting-for-you timeout** duration field (default 3d, up to 7d).
- **Summary panel: cross-file conversation lineage** — follow a session across `/clear` · `/compact` · plan-restart, with previous-session context; auto-refreshes as the transcript grows and on tab focus.
- **Git worktrees in the Launch path-picker** (with each one's branch), **in-place session-title editing** + a **"Ctx"** context-tokens column in the Sessions browser, configurable **status flash-ring** (color + opacity), **per-tab overlay opacity**, a selectable **Favorite marker** (Crown / Star), and **Shift+Home** to toggle the Agent Manager tab.
- **Revert-aware transcript display** (double-ESC'd branches excluded from summary / title / copy) and a launch-timing **`[startup]`** trace.

## Fixes

- **"Dev won't launch" / a stalled startup.** The multi-window "reopen your N windows?" decide-prompt could render as an **invisible modal**, hanging launch forever — fixed (it's a real, visible prompt now). Startup timing and the splash dismiss were also corrected.
- **Copy actually copies.** "Copy Summary" (and every overlay / board copy) could silently fail from a **focused** tab — replaced with a robust retry-looped Win32 clipboard writer.
- **Restore fidelity across a crash.** A crash-reopen now **preserves** a session's Running / Waiting-for-you triage state, persists the clarifying-question guard (so the autorunner can't auto-answer a pending question on reopen), and won't auto-send into a not-yet-started re-homed tab.
- **Close confirmations** are never permanently suppressible (no more "Don't ask again" foot-gun), and the splash / loading window no longer floats **topmost**.
- Plus the accumulated pre-release fixes: **Resume / Fork / restore open the exact session you picked**, forks keep their content, app-wide crash-hardening of every background transcript reader, and broad tooltip / hover polish.

## Install

**One command (PowerShell)** — installs 0.6.0 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.0
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). 0.6.0 is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable (no cert):** download `Agentmaster_0.6.0.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.0.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.0.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
