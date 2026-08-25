<!-- Agentmaster 0.7.6-prerelease-nightly release notes — NIGHTLY (the tier BELOW pre-release).
     Range: v0.7.5-prerelease-nightly..5bf3d08e6 = 5 commits (1 the 0.7.5 notes archive; 4 user-facing).
     Baseline = the previous release of ANY kind = the 0.7.5 nightly (release-notes §2). RELEASING from
     5bf3d08e6 (= origin/agentmaster tip after push).
     Engine gate at that tree: 3368 checks, 0 failures.
     NIGHTLY mechanics: the tag CONTAINS "nightly" -> release.yml strips the suffix to digits for the MSIX
     Identity Version (ver4 stays numeric 0.7.6.0, so ASSET NAMES carry no suffix) and auto-marks the release
     prerelease. The in-app updater offers it ONLY to users who accepted the cog's warning-gated "Allow
     updating to nightly builds (unstable)" switch — pre-release-opted users skip it. The one-command
     installer needs the FULL suffixed version (-Version maps to the TAG).
     Publish: gh release edit v0.7.6-prerelease-nightly -R Nucs/Agentmaster --draft=false --prerelease --latest=false
       (NEVER --latest; the stable 0.7.0 stays Latest).
     Assets: Agentmaster_0.7.6.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.7.6-prerelease-nightly (nightly)

> ⚠️ **NIGHTLY build** — an unstable development version. It may have memory leaks, CPU issues, and crashes. Install it only to help test, and be willing to have your work interrupted.

**The "window stops accepting clicks" freeze now heals itself.** A rare, system-pressure-induced failure — diagnosed live off a frozen instance under machine-wide memory exhaustion — could silently kill the UI framework's event delivery: the window kept painting and every engine lane kept running, but clicks and queued UI work were accepted and never delivered, indefinitely. No crash, no exception, nothing any public API could recover — until now: the UI-stall watchdog (shipped in 0.7.5) gained a **rescue tier** that force-pumps the wedged dispatcher and, if that fails, a **gated self-restart** that relaunches the app cleanly — the workspace comes back via the normal durability path. Nightlies are **not** offered to pre-release users — enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** (a warning confirm gates it) or install from the assets below. The stable **0.7.0** remains Latest for everyone else.

## New

- **A locked terminal shows the "unavailable" cursor.** While a pane is read-only — during the draft swap's brief lock as a queued prompt is delivered, or the stock read-only toggle — the mouse pointer over it becomes the Windows circle-with-a-slash cursor, so the pane reads as *intentionally locked*. Previously, typing into a locked tab just made the pointer vanish (Windows hides it while typing) with no signal at all of why nothing was happening.
- **The session summary lists the skills a session loaded.** A new **Skills Loaded** section appears in the summary box on every surface that renders it (the per-tab summary panel, its Copy Summary, the Sessions detail pane, the Manager's Summary pane), and the artifacts block is reordered to **Skills Loaded → Files Created → Files Edited → Files Read**.

## Fixes

- **The dispatch-wedge freeze self-heals.** Under machine-wide commit exhaustion, XAML's event delivery could latch dead for the process's one UI thread — clicks dead, queued UI work never delivered (measured 18+ minutes, ~550 stranded heartbeats), while the message pump answered probes and the engine kept working, so nothing looked hung from outside. The `[ui-stall]` watchdog now escalates: a sent-message rescue that force-processes the stalled queue, then — only if the UI is still dead — a self-restart that brings the app back with its workspace restored.
- **Clearing a multi-line draft leaves no empty lines behind.** The automatic input-box clear (used by the draft swap and the MAIL button's move) stopped too early on multi-line drafts: Ctrl+U kills a line's *content*, but removing the now-empty row takes another press — so blank rows were left in the box. The clear now tracks the box's visual row span and keeps going until a single empty row remains.

## Install (nightly)

> ⚠️ **This is a nightly.** It is offered as an automatic update **only** if you enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** — enabling "Allow pre-release versions" is *not* enough. The stable **0.7.0** stays the default Latest.

**One command (PowerShell)** — installs this nightly (admin not required: as admin it trusts the self-signed cert silently; otherwise it asks — elevate once (UAC), a no-admin per-user portable install, or cancel):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.7.6-prerelease-nightly
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.7.6.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.7.6.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.7.6.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
