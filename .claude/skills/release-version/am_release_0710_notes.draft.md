<!-- Agentmaster 0.7.10-prerelease release notes — PRE-RELEASE (the stable 0.7.0 stays Latest).
     Range: v0.7.9-prerelease..07e7a295b = 19 commits (6 user-facing; 13 docs: the 0.7.9 notes archive,
     the deploy/swap references, status corrections, and DESIGN-only documents — the DuckDB session
     database (SESSIONS_DB.md) and the session-agent tool redesign (SESSIONS_AGENT.md) — which build nothing).
     Baseline = the previous NON-nightly release = the 0.7.9 pre-release (no nightly since).
     RELEASING from 07e7a295b (= origin/agentmaster tip after pushing the 18 unpushed commits).
     NOT included: another session's uncommitted harness work (AgentHarness*.{h,cpp}) that was in the
     working tree while this was cut.
     Tag v0.7.10-prerelease -> ver4 0.7.10.0, rawver 0.7.10-prerelease. 0.7.10 = the patch after 0.7.9
     (the updater compares major.minor.patch numerically: 10 > 9; ParseVersion reads each part as a number).
     nightly=false, so release.yml does NOT auto-mark prerelease — mark it at publish. The installer's
     -Version maps to the TAG (full suffix 0.7.10-prerelease); asset names stay numeric ver4.
     Pre-validation: no local build (project rule). Dev 0.0.1.6 (Debug, built 2026-10-13 from 2ad186327)
     already holds every code change except 70990582f (the AI-search prompt rule); ci.yml run 38523114488
     (x64 Release) and release.yml run 38523127576 (x64 + arm64), both on 07e7a295b, are the clean builds.
     Publish: gh release edit v0.7.10-prerelease -R Nucs/Agentmaster --draft=false --prerelease --latest=false
       (NEVER --latest; the stable 0.7.0 stays Latest).
     Assets: Agentmaster_0.7.10.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.7.10-prerelease

**0.7.10 is a pre-release** building on 0.7.9 — a small batch. On the Agent Manager page the tab strip's **Home** button now turns into the **way back**: a jump-back arrow to the tab you came from, replacing the separate Jump Back button. The Sessions page gains a **folder filter** (recent and most-used folders, combined with OR), a fork's summary now says it is a fork, the side panel's **+** launcher no longer covers its own path box with the folder list, and the AI search's hidden Claude process runs leaner and on a verified permission mode. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.7.0** remains the default Latest for everyone else.

## New

- **The Home button is also the way back.** On the Agent Manager page, the tab strip's **Home** button turns into a jump-back arrow (↶) while a session this window hosts is selected — usually the tab you came from — and a click returns to that tab, after which it is Home again. The separate Jump Back button that appeared beside Home is gone; with nothing to go back to, the button keeps its Home glyph. Its tooltip names the session it will return to, and a double-click no longer opens the page and leaves it at once.
- **A folder filter in the Sessions page.** **📂 Folders ▾** (after Favorite) opens a dropdown of the folders the listed sessions worked in — each session's launch folder and the folder its tool calls point to — split half and half: **RECENT** (the folder with the newest activity first) over **MOST USED** (the folder more sessions worked in first). Pick as many as you like: they combine with **OR**, while the folder filter as a whole combines with the search and the other filters. A find box (Enter adds the top match, or a typed absolute path), the picks as removable chips, **Include subfolders** and **Clear** sit in the dropdown, and rows and the detail pane show where a session actually worked ("Works in → …").
- **A fork's summary says it is a fork.** In the summary's whole-conversation view, the separator after a fork's parent now reads `fork 358db618` instead of a bare `358db618` (a plan restart, `/clear` or `/handover` link keeps the bare id), and the copied Summary names the fork too.

## Fixes

- **The side panel's + launcher covered its own path box.** Clicking **+** could open the folder list by itself, anchored over the path box and the **Launch Claude** button. The list now opens only on your click or first key in the box, and an open list is placed after the card has settled.
- **The AI search's hidden process, tightened.** The Sessions page's AI search drives a hidden `claude.exe`: its model now sees exactly its two tools — three stray MCP-resource tools that leaked into its list are gone (~770 tokens off every request) — and it runs with bypass permissions (auto mode when your Claude Code policy forbids bypass, then don't-ask), checking the mode it actually got, since Claude Code lowers a disallowed mode silently; a windowless process must never wait for an answer. Its reply is now held to one short sentence.

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
