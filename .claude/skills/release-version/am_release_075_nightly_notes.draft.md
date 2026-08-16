<!-- Agentmaster 0.7.5-prerelease-nightly release notes — NIGHTLY (the tier BELOW pre-release).
     Range: v0.7.4-prerelease-nightly..23b9f02f1 = 11 commits (1 the 0.7.4 notes archive; 10 user-facing).
     Baseline = the previous release of ANY kind = the 0.7.4 nightly (release-notes §2). RELEASING from
     23b9f02f1 (= origin/agentmaster tip after push).
     Engine gate at that tree: 3328 checks, 0 failures.
     ⚠ IDENTITY NOTE: this build renames the GUI binary WindowsTerminal.exe -> Agentmaster.exe (a
     WindowsTerminal.exe forwarding shim ships beside it for old shortcuts). The package identity/alias
     (agentmaster.exe) is unchanged; the appxmanifest changed, so a LOOSE-LAYOUT dev deploy needs a
     re-register — but the RELEASE MSIX/portable this workflow builds is self-contained, no user action.
     NIGHTLY mechanics: the tag CONTAINS "nightly" -> release.yml strips the suffix to digits for the MSIX
     Identity Version (ver4 stays numeric 0.7.5.0, so ASSET NAMES carry no suffix) and auto-marks the release
     prerelease. The in-app updater offers it ONLY to users who accepted the cog's warning-gated "Allow
     updating to nightly builds (unstable)" switch — pre-release-opted users skip it. The one-command
     installer needs the FULL suffixed version (-Version maps to the TAG).
     Publish: gh release edit v0.7.5-prerelease-nightly -R Nucs/Agentmaster --draft=false --prerelease --latest=false
       (NEVER --latest; the stable 0.7.0 stays Latest).
     Assets: Agentmaster_0.7.5.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.7.5-prerelease-nightly (nightly)

> ⚠️ **NIGHTLY build** — an unstable development version. It may have memory leaks, CPU issues, and crashes. Install it only to help test, and be willing to have your work interrupted.

**The RDP freeze is fixed, the app binary is now `Agentmaster.exe`, and portable copies gained real profile support.** The headline is a stability fix: connecting or disconnecting over Remote Desktop flips the system theme, which on a busy window (42 tabs, measured) sent the UI thread into a settings-reload × tabs theme storm that pegged a core at 100% for 20+ minutes with the window frozen — now killed, with staggered theme application, per-tab fail-open guards, and a UI-stall watchdog behind it. This build also renames the shipped GUI binary from `WindowsTerminal.exe` to `Agentmaster.exe` (a forwarding shim keeps old shortcuts working) and lets a portable copy choose its profile folder and self-update in place. Nightlies are **not** offered to pre-release users — enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** (a warning confirm gates it) or install from the assets below. The stable **0.7.0** remains Latest for everyone else.

## New & changed

- **The app binary is now `Agentmaster.exe`.** The shipped executable is renamed from `WindowsTerminal.exe` to `Agentmaster.exe` project-wide — so Task Manager, the portable zip's launch target, and the defterm/toast COM servers all say Agentmaster. A **`WindowsTerminal.exe` forwarding shim** ships beside it (same icon), so an old Start-menu shortcut, taskbar pin, script, or muscle-memory launch of the old name still lands in the one real app. The `agentmaster` command / package identity is unchanged.
- **Portable copies choose a profile.** A portable (unzipped) copy's first interactive launch now shows the same profile picker the Settings cog uses — led by a self-contained *"Portable profile"* option, then Default / Development / Browse… — and remembers the answer in a one-line `profile.path` pointer next to the exe (relative when the folder sits inside the unzip, so a moved copy keeps working). Each picker option marks *"— existing data"* when that folder already holds a profile, so adopt-vs-fresh is an informed choice. Installed copies are unchanged (still silent).
- **Portable self-update, in place.** A portable copy can now update itself: it downloads the new zip, swaps its binaries while preserving your settings + profile, and relaunches — no reinstall. (The picker's former "Production profile" option is also renamed **Default profile**, since `~/.agentmaster` is the installed default.)
- **Pop the last queued prompt back for editing.** **Ctrl+Shift+Up** on an *empty* Auto-Testing compose box now pulls the last queued prompt back into the box — the inverse of the queue envelope — so you can take back and edit what you just queued before the autorunner sends it.
- **A sensible `EDITOR` default.** Spawned sessions get `EDITOR=edit` when Microsoft's `edit.exe` is on PATH and nothing has already claimed `EDITOR`, so tools that shell out to an editor have a working one out of the box (set your own `EDITOR` in the cog's env to override).

## Fixes

- **The RDP theme-storm freeze.** An RDP connect/disconnect transiently flips the system dark/light theme, and the resulting settings reload re-applied tab theming for every pane × every tab, re-entrantly — grinding the single UI thread at 100% for 20+ minutes on a large window while everything else kept running. The reload no longer storms: background brushes stop raising redundant change events, theme application is staggered, each tab is guarded fail-open, and a **UI-stall watchdog** logs a `[ui-stall]` line if the thread ever hangs again.
- **A session stuck "Running" forever behind a huge transcript line.** A session could sit Running for 17+ minutes with Claude provably idle because a single transcript line larger than 1 MiB fell outside the scanner's delta read, so the turn-end it needed was never seen. The delta read now grows past an oversized line, and the state reconciles.
- **Another double-send into a running turn.** A duplicate Stop hook that arrived without a timestamp (37 ms after the echo) was accepted as a fresh turn-complete and fired a second queued prompt into the still-running turn. Two guards close it: a timestamp-less hook is stamped with its arrival time, and a **20-second minimum spacing** between automatic sends to one session turns any residual status race from a double-send into a merely delayed one.

## Install (nightly)

> ⚠️ **This is a nightly.** It is offered as an automatic update **only** if you enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** — enabling "Allow pre-release versions" is *not* enough. The stable **0.7.0** stays the default Latest.

**One command (PowerShell)** — installs this nightly (admin not required: as admin it trusts the self-signed cert silently; otherwise it asks — elevate once (UAC), a no-admin per-user portable install, or cancel):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.7.5-prerelease-nightly
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.7.5.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.7.5.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.7.5.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
