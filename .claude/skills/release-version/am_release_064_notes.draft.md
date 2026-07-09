<!-- Agentmaster 0.6.4 release notes — PRE-RELEASE (NOT Latest; v0.6.3 stays Latest).
     v0.6.4 = v0.6.3 (4a038e7f8) + 2 cherry-picked crash-fix commits (d606d657 -> 7eb61789), tag commit e8a1646f6.
     Last stable is 0.6.3, so this IS the full changelog since last stable.
     PRERELEASE mechanics: publish --draft=false --prerelease --latest=false (the stable 0.6.3 stays Latest).
     Assets: Agentmaster_0.6.4.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.4 (pre-release)

**0.6.4 is a pre-release** on top of 0.6.3 carrying one important stability fix: it stops a **crash loop when resuming a heavy session** that could kill the app within seconds of launch on 0.6.x. If you've seen Agentmaster die silently a few seconds after it reopens your workspace, this is for you. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.6.3** remains the default for everyone else.

## Fixes

- **The 0.6.x "resume crash-loop."** Resuming a large / heavy session — an image-paste-heavy, `/compact`-ed transcript — could put the app into a **self-sustaining crash loop**: it would die silently ~2–4 s after launch, durability would reopen the workspace, and it would die again. Root cause: the summary surfaces (the per-tab **overlay panel**, the Manager's **Summary** pane, and the **Sessions** detail with its neighbor prefetches) all whole-file-analyze the *same* transcript **concurrently on background threads** with no exception containment — so a transient memory spike (`std::bad_alloc`) on a huge transcript threw, and a throw on a detached thread / fire-and-forget coroutine is an **instant, log-less process death**. (The 0.5.5 line survived the same data for weeks because it did a single-pass analyze; every 0.6.x launch died in 15–35 s.) Now **every** summary/analyze lane is contained end-to-end — a failure degrades to an empty panel plus one log line instead of crashing — the analyze footprint is bounded, and the two **Copy** lanes (**Copy Transcript** / **Copy Summary**, the same unguarded class but click-triggered) are netted too.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.6.3** stays the default Latest.

**One command (PowerShell)** — installs 0.6.4 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.4
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.6.4.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.4.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.4.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
