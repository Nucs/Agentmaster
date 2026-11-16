<!-- ARCHIVED AS PUBLISHED 2026-11-16: the body below this header is now the release's body exactly as
     GitHub published it, saved before the one-row rewrite of every release replaced it there. The
     body this file held until then differed from the published one (a draft revised after, or never
     applied as written); it is this file at the commit before this note.
     Agentmaster 0.4.1 release notes. Cumulative since v0.4.0. A fixes-and-polish point release.
     Format matches prior notes: flat "- **Title.** desc." bullets, New + Fixes sections.
     Drafted from a one-by-one read of all 15 commits in v0.4.0..HEAD (docs-only commits omitted).
     Asset names assume tag v0.4.1 -> 0.4.1.0. -->
## Agentmaster 0.4.1

**0.4.1** is a fixes-and-polish release on top of 0.4.0: **Restart session** now resumes the conversation, tab-navigation keys work from the Manager tab, the Triage Board keeps its scroll position across refreshes, and the per-tab summary panel gains a truncate toggle — plus a **one-command installer** and a build-identity line in Settings.

## New in 0.4.1

- **One-command install / upgrade.** A single PowerShell line installs or upgrades to a specific version — `& ([scriptblock]::Create((irm <raw-url>))) -Version 0.4.1` — fetching the signed bundle, verifying its SHA-256, and trusting the self-signed cert behind a single UAC prompt (skipped when the cert is already trusted, so upgrades are usually prompt-free). `-Portable` is a cert-free, no-admin path (downloads the zip and preserves in-folder state); `-Launch` starts it after; a missing VCLibs dependency is auto-fetched.
- **Build identity in Settings.** The Settings overlay now shows `v‹version› · ‹commit› · ‹channel› · ‹config›` directly under the title (hover for the git branch + full package family), so you always know exactly which build you're running — version read live from the package, commit build-stamped.
- **Summary panel — truncate toggle.** A new `…` toggle in the times bar caps long messages (6 lines when wrapping, else 500 chars), **on by default**, beside the existing wrap (`↵`) toggle; both are global + persisted. Turn it off to show every message in full.
- **Summary box — each file listed once.** A file that was both read and written now appears only under Files Edited / Created (Files Read keeps only files that were *only* read) — applied at the data source, so it's consistent across the Sessions-page detail, the per-tab panel, and the copied Summary. The Sessions header (search + scope toggles) is also left-aligned next to the title.

## Fixes

- **"Restart session" did nothing for an already-used session.** Restart replayed the *original launch* commandline — for a normally-launched session that's `--session-id <A>`, but A's transcript now exists, so Claude refused with "session id already in use" and the relaunch exited instantly (a fork re-forked from its parent; only a resume happened to work). Restart now **resumes the current conversation** (transcript-gated — fresh if none; Codex via `codex resume`), tears the old process down first (no orphan), and restores the full session environment — and it follows a conversation that diverged via `/clear` · `/compact` · `/resume`.
- **Alt+Left / Alt+Right didn't navigate tabs from the Manager tab.** The Manager's focused search box swallowed the arrow keys; a tunneling handler now dispatches the tab-switch chords first (leaving typing / caret keys untouched), so Alt+arrows reach the tab strip from the Manager too.
- **Triage Board snapped to the top on every refresh.** A board rebuild (a selection, a state/title change, an observer enrichment) reset each column's scroll to 0 — losing the card you'd clicked near the bottom of a tall column (e.g. a large External census). Columns now preserve their scroll position across rebuilds.
- **Sessions From/To range popup mis-anchored.** After the header cluster moved next to the title, the date-range popup opened over the window buttons; it's re-anchored to open under its own button.
- **Selection pill looked like a tight box.** The hovered/selected tab pill now has padding and rounded corners — a proper pill — without shifting the tab's icon or title.

## Install

**One command (PowerShell)** — install or upgrade to 0.4.1 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.4.1
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.4.1.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.4.1.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.4.1.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data — pick **Production** in the first-launch profile picker (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
