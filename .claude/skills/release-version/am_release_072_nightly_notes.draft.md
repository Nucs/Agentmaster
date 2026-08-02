<!-- Agentmaster 0.7.2-prerelease-nightly release notes — NIGHTLY (the tier BELOW pre-release).
     Range: v0.7.1-prerelease-nightly..2446db123 = 12 commits (7 docs/skill, 5 user-facing). Baseline = the
     previous release of ANY kind = the 0.7.1 nightly (release-notes §2 — a nightly's own notes may baseline
     off the previous release of any kind). RELEASING from 2446db123 (= origin/agentmaster tip).
     Engine gate at that tree: 3035 checks, 0 failures.
     NIGHTLY mechanics: the tag CONTAINS "nightly" -> release.yml strips the suffix to digits for the MSIX
     Identity Version (ver4 stays numeric 0.7.2.0, so ASSET NAMES carry no suffix) and auto-marks the release
     prerelease. The in-app updater offers it ONLY to users who accepted the cog's warning-gated "Allow
     updating to nightly builds (unstable)" switch — pre-release-opted users skip it. The one-command
     installer needs the FULL suffixed version (-Version maps to the TAG).
     Publish: gh release edit v0.7.2-prerelease-nightly -R Nucs/Agentmaster --draft=false --prerelease --latest=false
       (NEVER --latest; the stable 0.7.0 stays Latest).
     Assets: Agentmaster_0.7.2.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.7.2-prerelease-nightly (nightly)

> ⚠️ **NIGHTLY build** — an unstable development version. It may have memory leaks, CPU issues, and crashes. Install it only to help test, and be willing to have your work interrupted.

**The tab-drag crash is finally dead.** The headline of this nightly is the elimination of the WinUI tab-strip drag crash — a null-dereference deep inside Microsoft's tab control that could take the whole app down when you reordered or tore out a tab on a busy strip, dump-proven four separate times and unguarded even upstream. Native tab-drag is now permanently off and reorder / tear-out are re-implemented as a pointer-owned gesture, so dragging a tab still works but can no longer reach the broken code path. Nightlies are **not** offered to pre-release users — enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** (a warning confirm gates it) or install from the assets below. The stable **0.7.0** remains Latest for everyone else.

## New

- **The MAIL button now MOVES the draft.** Queuing a tab's unsent draft into the Tests Autorunner from the overlay used to leave a copy behind in Claude's input box, so you had a duplicate to delete by hand. A plain click now **moves** it — queue, then clear the box (the clear is the draft-swap's verified, keystroke-safe clear) — while **Shift+Click keeps** the draft in the box. The clear only runs after the queue append succeeds, so a failed or empty queue never touches your box.
- **Briefings written to the scratchpad are never deleted.** The shipped default hand-off location is the session scratchpad — a temp folder already outside your repo that the OS reclaims — so "delete the briefing after hand-off" is now hard-disabled while the location is the scratchpad, not merely nudged off. Even a hand-edited `settings.json` with the toggle on can't arm a delete there, and the cog greys the toggle out with a note explaining why.

## Fixes

- **The WinUI tab-drag crash class, killed.** MUX's tab-view drag lookup null-dereferences on the first virtualized-out tab on *both* the drag-start and the drop path (`0xC0000005` inside `Microsoft.UI.Xaml.dll`), a class that no check-then-allow gate could close (the drop cannot be vetoed) and that could also leave the app live-but-wedged. Native drag is now off by construction and the reorder / tear-out gesture is pointer-owned, so neither crash site can run. A side effect gone with it: a ~30 Hz log storm during a large window restore.
- **Turning delete-after off actually disarms it.** A briefing already queued for deletion while the setting was on stayed armed even if you turned delete-after off before the successor started — far more reachable now that the wait is 24 hours. The sweep re-reads the setting: off ⇒ every pending deletion is dropped and its file kept. (Turning it back on never retro-arms an already-spawned successor.)
- **The MAIL button no longer dead-clicks.** It is enabled only while an unsent draft exists — greyed on an empty box — so it lights exactly when the "3 dots" pulse, instead of being an always-present button that silently does nothing on an empty box.
- **A selected board tag chip reads cleaner.** A picked tag-filter chip on the Triage Board is now a 1px accent **border** rather than a filled accent background, so it matches the plain buttons beside it instead of standing out as a translucent block.

## Install (nightly)

> ⚠️ **This is a nightly.** It is offered as an automatic update **only** if you enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** — enabling "Allow pre-release versions" is *not* enough. The stable **0.7.0** stays the default Latest.

**One command (PowerShell)** — installs this nightly (admin not required: as admin it trusts the self-signed cert silently; otherwise it asks — elevate once (UAC), a no-admin per-user portable install, or cancel):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.7.2-prerelease-nightly
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.7.2.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.7.2.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.7.2.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
