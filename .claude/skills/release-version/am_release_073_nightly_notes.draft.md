<!-- Agentmaster 0.7.3-prerelease-nightly release notes — NIGHTLY (the tier BELOW pre-release).
     Range: v0.7.2-prerelease-nightly..3e3654c1f = 8 commits (1 the 0.7.2 notes archive, 1 internal
     refactor; 6 user-facing). Baseline = the previous release of ANY kind = the 0.7.2 nightly
     (release-notes §2). RELEASING from 3e3654c1f (= origin/agentmaster tip after push).
     Engine gate at that tree: 3095 checks, 0 failures.
     NIGHTLY mechanics: the tag CONTAINS "nightly" -> release.yml strips the suffix to digits for the MSIX
     Identity Version (ver4 stays numeric 0.7.3.0, so ASSET NAMES carry no suffix) and auto-marks the release
     prerelease. The in-app updater offers it ONLY to users who accepted the cog's warning-gated "Allow
     updating to nightly builds (unstable)" switch — pre-release-opted users skip it. The one-command
     installer needs the FULL suffixed version (-Version maps to the TAG).
     Publish: gh release edit v0.7.3-prerelease-nightly -R Nucs/Agentmaster --draft=false --prerelease --latest=false
       (NEVER --latest; the stable 0.7.0 stays Latest).
     Assets: Agentmaster_0.7.3.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.7.3-prerelease-nightly (nightly)

> ⚠️ **NIGHTLY build** — an unstable development version. It may have memory leaks, CPU issues, and crashes. Install it only to help test, and be willing to have your work interrupted.

**Auto Testing delivery is now atomic — the queue can no longer stack, park invisibly, or double-send.** This nightly's centerpiece is a **delivery gate** that makes placing "the next prompt" one indivisible operation across the four actors that used to race over the input box (the scheduler advance, the draft swap, the Enter-retry watchdog, and the pending-input scan), closing a set of log-proven failures: an 8-cycle mark/decline/rollback storm, a prompt marked-sent twice around a decline, restart press-storms into restored boxes, and — the worst — a duplicate late-stamped Stop firing an advance that pasted a second prompt into a *running* turn (silently lost into an open question dialog). Nightlies are **not** offered to pre-release users — enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** (a warning confirm gates it) or install from the assets below. The stable **0.7.0** remains Latest for everyone else.

## New

- **Close a whole folder's sessions at once.** The Triage-Board card / Explorer-tree session right-click **Close** is now a **Close ▸** submenu: **This Session** (the plain close, unchanged), **Of Same Folder** (close every live session sharing this one's effective working directory), and **Other of Same Folder** (the same batch minus this one). It's cross-window and shows a single confirm listing the tabs it will close — no per-session prompt train.
- **"Open In Explorer" on the tab right-click menu.** A managed session's tab menu now opens its working directory in Explorer, reusing the exact code the per-tab overlay's folder button runs (so the two can never disagree), resolved to the session's *effective* directory — the inferred work dir when it infers, else the launch cwd.

## Fixes

- **The Auto Testing queue no longer stacks or double-sends.** A new **delivery gate** (see above) gives the scheduler, the draft swap, the Enter-retry watchdog, and the pending-input scan one shared fact that a delivery is in progress — ending the mark/decline/rollback storm, the marked-sent-twice race, the restart press-storm into restored input boxes, and a multi-line compose-box prompt that never matched its own wire echo (a phantom unacknowledged send).
- **A second prompt can't land in a running turn.** A duplicate Stop whose slow forwarder stamped its timestamp *after* the next prompt's submit read as a 15 ms "turn complete" and fired an advance — pasting a second queued prompt into Claude's already-running turn, where an open question dialog silently swallowed it. The ordered state machine now refuses to advance off a Stop whose turn never actually ran.
- **A queue could park invisibly with no way to release it.** When an agent ended its turn on a clarifying question, the question-guard latched and parked every queued prompt — with the only trace a deduped line in the autorunner log, and *no* user gesture (not even cycling the mode) releasing it. The trap is released, and the MAIL button's queued draft — your own next message, by construction — is never treated as a blind auto-answer.

## Install (nightly)

> ⚠️ **This is a nightly.** It is offered as an automatic update **only** if you enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** — enabling "Allow pre-release versions" is *not* enough. The stable **0.7.0** stays the default Latest.

**One command (PowerShell)** — installs this nightly (admin not required: as admin it trusts the self-signed cert silently; otherwise it asks — elevate once (UAC), a no-admin per-user portable install, or cancel):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.7.3-prerelease-nightly
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.7.3.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.7.3.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.7.3.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
