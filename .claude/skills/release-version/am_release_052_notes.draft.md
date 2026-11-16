<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     ARCHIVED AS PUBLISHED 2026-11-16: the body below this header is now the release's body exactly as
     GitHub published it, saved before the one-row rewrite of every release replaced it there. The
     body this file held until then differed from the published one (a draft revised after, or never
     applied as written); it is this file at the commit before this note.
     Agentmaster 0.5.2 release notes. Tag v0.5.2 -> d63cf610b (the user-specified release commit).
     Range v0.5.1..d63cf610b — 13 commits (12 feat/fix below + 1 docs-only: 777746b13 summary-jump v2 plan, omitted).
     DEFERRED to a later release (NOT in 0.5.2, committed/WIP after d63cf610b):
       * 17f3ed30f fix(autopilot): drive ADOPTED sessions on toggle/idle-start.
       * uncommitted WIP — (a) Waiting-for-you "unread" model (read-gated decay + sticky Mark-Unread + 60m
         decay split from a cosmetic ⚡ serverCache indicator); (b) auto-recovering "Claude not found" gate.
     Asset names: tag v0.5.2 -> 0.5.2.0. -->
## Agentmaster 0.5.2

**0.5.2 is a polish-and-control release.**

## What's new

- **A dark Manager on any theme.** The Manager and its tooltips render dark whatever your Windows or Terminal theme.
- **Alt+↑ / Alt+↓ step between your sent prompts.** On a Claude tab they center the nearest sent prompt above or below the view.
- **A summary-jump highlight.** Jumping to a prompt highlights its row in the summary panel.
- **A Title scope in Sessions search.** A 🏷 Title toggle gates title matching, and an open session's renamed tab title is searchable.
- **Browse… in the Launch box.** A folder dialog sits at the top of the path picker, and its pick joins your recent folders.
- **"Always display Home button".** A setting, on by default, keeps the Home button visible on every other tab.
- **The Triage Board tidies itself.** The Error column collapses while nothing errs, and a window left with only the Manager tab closes itself.

## What's fixed

- **Triage cards no longer shift on hover.** The hover ring is drawn over the card, so nothing reflows.
- **Calmer card tooltips.** Board card tooltips open after 4 s, so moving across the board no longer flashes a tip on every card.

## Install

**One command (PowerShell)** — install or upgrade to 0.5.2 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.5.2
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). *Already on 0.5.0+? Just use **Settings → Check for updates** (or the startup prompt).*

**Portable (no cert):** download `Agentmaster_0.5.2.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.5.2.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.5.2.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
