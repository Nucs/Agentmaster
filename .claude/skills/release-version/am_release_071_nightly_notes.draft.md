<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.7.1-prerelease-nightly release notes — NIGHTLY (the tier BELOW pre-release).
     Range: v0.7.0..6695ac853 = 22 commits. RELEASING from 6695ac853 (= origin/agentmaster tip).
     Engine gate at that tree: 2985 checks, 0 failures.
     NIGHTLY mechanics: the tag CONTAINS "nightly" -> release.yml strips the suffix to digits for the
     MSIX Identity Version (ver4 stays numeric 0.7.1.0, so ASSET NAMES carry no suffix) and auto-marks
     the release prerelease. The in-app updater offers it ONLY to users who accepted the cog's
     warning-gated "Allow updating to nightly builds (unstable)" switch — pre-release-opted users skip it.
     The one-command installer needs the FULL suffixed version (-Version maps to the TAG).
     Publish: gh release edit v0.7.1-prerelease-nightly -R Nucs/Agentmaster --draft=false --prerelease --latest=false
       (NEVER --latest; the stable 0.7.0 stays Latest).
     Assets: Agentmaster_0.7.1.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.7.1-prerelease-nightly (nightly)

> ⚠️ **NIGHTLY build** — an unstable development version. It may have memory leaks, CPU issues, and crashes. Install it only to help test, and be willing to have your work interrupted.

Nightlies are **not** offered to pre-release users — enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** (a warning confirm gates it) or install from the assets below. The stable **0.7.0** remains Latest for everyone else.

## What's new

- **The tab hover card scrolls.** The rich tab tooltip's body takes the mouse wheel, so a long summary is readable instead of clipped.
- **A mail button on the tab overlay.** It queues the tab's unsent draft into the Tests Autorunner as a copy (dev and debug builds, Claude tabs only).
- **Tag filter chips on the Triage Board.** The board's header filters its cards by tag, any of the picked ones, with an Untagged chip that is on by default.
- **The tab color picker opens ready.** Custom and its RGB / HSV / Hex inputs start expanded, and Use Tab Color seeds it with the tab's current color.
- **Completion toasts say which request finished.** A third line shows the prompt whose turn just completed, and the toast stays about 25 s.
- **The Manager's compose box follows your tab draft.** It pulls the tab's unsent prompt in, extends it as you type in the terminal, and offers a replace only when the texts differ.
- **A reopened session gets its unsent draft typed back.** The remembered draft is filled into the new input box, never sent, and checked by reading the box back.
- **Copy a single prompt from the summary.** A right-click on a numbered prompt copies that exact prompt.
- **The updater's "Not now" is a real choice.** Remind me next restart is the default, beside tomorrow at 08:00 local time, 3, 7 or 30 days, or skipping the version.
- **Briefings are kept by default.** `/handover`'s delete-after-hand-off is off on every install, and its wait became a 24-hour setting.

## What's fixed

- **No more UI freeze when arming SemiAuto.** The autorunner no longer writes to the registry on every pass, which spun a 3 ms loop.
- **Tab tooltips open reliably.** A tooltip attached mid-hover no longer stays dark, and a recycled tab keeps its tooltip.
- **A two-file /handover keeps its second successor.** The seal holds while Claude is still writing the second briefing.
- **The draft swap restores your text properly.** It stashes the whole box with Claude's own Ctrl+S, so the text comes back without the cursor stranded mid-string.
- **Board chips stay put.** The tag chips are sorted alphabetically, so they no longer reshuffle under your pointer.
- **The summary panel's menus can no longer crash the app.** All three of its context-menu handlers are guarded.

## Install (nightly)

> ⚠️ **This is a nightly.** It is offered as an automatic update **only** if you enable **Settings → Updates → "Allow updating to nightly builds (unstable)"** — enabling "Allow pre-release versions" is *not* enough. The stable **0.7.0** stays the default Latest.

**One command (PowerShell)** — installs this nightly (admin not required: as admin it trusts the self-signed cert silently; otherwise it asks — elevate once (UAC), a no-admin per-user portable install, or cancel):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.7.1-prerelease-nightly
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.7.1.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.7.1.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.7.1.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
