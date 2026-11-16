<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.7.10-prerelease release notes — PRE-RELEASE (the stable 0.7.0 stays Latest).
     Range: v0.7.9-prerelease..b8c33adb7 = 19 commits (6 user-facing; 13 docs: the 0.7.9 notes archive,
     the deploy/swap references, status corrections, and DESIGN-only documents — the DuckDB session
     database (SESSIONS_DB.md) and the session-agent tool redesign (SESSIONS_AGENT.md) — which build nothing).
     Baseline = the previous NON-nightly release = the 0.7.9 pre-release (no nightly since).
     RELEASING from b8c33adb7 (= origin/agentmaster tip after pushing the 18 unpushed commits).
     NOT included: another session's uncommitted harness work (AgentHarness*.{h,cpp}) that was in the
     working tree while this was cut.
     Tag v0.7.10-prerelease -> ver4 0.7.10.0, rawver 0.7.10-prerelease. 0.7.10 = the patch after 0.7.9
     (the updater compares major.minor.patch numerically: 10 > 9; ParseVersion reads each part as a number).
     nightly=false, so release.yml does NOT auto-mark prerelease — mark it at publish. The installer's
     -Version maps to the TAG (full suffix 0.7.10-prerelease); asset names stay numeric ver4.
     Pre-validation: no local build (project rule). Dev 0.0.1.6 (Debug, built 2026-10-13 from d860ebdf1)
     already holds every code change except 9aa6e0d85 (the AI-search prompt rule); ci.yml run 38523114488
     (x64 Release) and release.yml run 38523127576 (x64 + arm64), both on b8c33adb7, are the clean builds.
     Publish: gh release edit v0.7.10-prerelease -R Nucs/Agentmaster --draft=false --prerelease --latest=false
       (NEVER --latest; the stable 0.7.0 stays Latest).
     Assets: Agentmaster_0.7.10.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.7.10-prerelease

**0.7.10 is a pre-release** building on 0.7.9 — a small batch. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.7.0** remains the default Latest for everyone else.

## What's new

- **The Home button is also the way back.** On the Agent Manager page, Home turns into a ↶ arrow while a session this window hosts is selected, and a click returns to that tab.
- **A folder filter on the Sessions page.** 📂 Folders lists the folders your sessions worked in, recent and most used, and shows the sessions of every folder you pick.
- **A fork's summary says it is a fork.** The separator after a fork's parent reads "fork 358db618", in the summary and in a copied Summary.

## What's fixed

- **The side panel's + launcher no longer covers its path box.** Its folder list opens only on your click or first key, placed after the card has settled.
- **The AI search's hidden process runs leaner.** Its model sees only its own two tools, it runs on a permission mode it checks, and its reply is one short sentence.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.7.0** stays the default Latest.

**One command (PowerShell)** — installs 0.7.10-prerelease (admin not required: as admin it trusts the self-signed cert silently; otherwise it asks — elevate once (UAC), a no-admin per-user portable install, or cancel):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.7.10-prerelease
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.7.10.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained — its first launch offers a per-user install or stays portable.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.7.10.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.7.10.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
