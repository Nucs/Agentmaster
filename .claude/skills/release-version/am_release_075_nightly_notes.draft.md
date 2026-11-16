<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     ARCHIVED AS PUBLISHED 2026-11-16: the body below this header is now the release's body exactly as
     GitHub published it, saved before the one-row rewrite of every release replaced it there. The
     body this file held until then differed from the published one (a draft revised after, or never
     applied as written); it is this file at the commit before this note.
     Agentmaster 0.7.5-prerelease-nightly release notes — NIGHTLY (the tier BELOW pre-release).
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

Nightlies are **not** offered to pre-release users — enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** (a warning confirm gates it) or install from the assets below. The stable **0.7.0** remains Latest for everyone else.

## What's new

- **The app is now `Agentmaster.exe`.** Task Manager, the portable zip and the COM servers say Agentmaster, and a `WindowsTerminal.exe` forwarder keeps old shortcuts working.
- **Portable copies choose a profile.** A portable copy's first launch shows the profile picker, led by a self-contained portable profile, and remembers the answer next to the exe.
- **Portable copies update themselves in place.** A portable copy downloads the new zip, swaps its binaries while keeping your settings and profile, and relaunches.
- **Pop the last queued prompt back for editing.** Ctrl+Shift+Up on an empty compose box pulls the last queued prompt back before the autorunner sends it.
- **A sensible `EDITOR` default.** Sessions get `EDITOR=edit` when Microsoft's edit.exe is on PATH and nothing else claimed `EDITOR`.

## What's fixed

- **No more Remote Desktop theme-storm freeze.** An RDP connect or disconnect no longer makes the settings reload re-theme every tab and grind the UI thread for 20+ minutes.
- **A huge transcript line no longer strands a session on Running.** The scanner's read now grows past a line over 1 MiB, so the turn's end is seen.
- **No second prompt into a running turn.** A duplicate Stop hook without a timestamp no longer fires another queued prompt, and automatic sends keep at least 20 s apart.

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
