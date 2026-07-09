<!-- Agentmaster 0.6.1 release notes — STABLE patch (becomes Latest; NOT a pre-release).
     Range: v0.6.0..8ea00ecc = 8 commits (3 user-facing + docs). Last stable is 0.6.0, so this IS
     the full changelog since last stable. RELEASING from the pinned commit 8ea00ecc5d88ac01449214c5c261b569b04f45f1.
     Tag v0.6.1 -> 0.6.1.0. STABLE mechanics: publish --draft=false --latest (REPLACES 0.6.0 as Latest; no --prerelease).
     Assets: Agentmaster_0.6.1.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.1

**0.6.1 is a stability & correctness patch on 0.6.0** — recommended for everyone. The headline is a fix for a **memory leak that could eventually freeze the app** during long, multi-window sessions. It also lands two correctness fixes: the **Inferred working directory** now drives where a session lives across the whole UI (not just its tab color), and the Triage-Board **⚡ "still cached"** hint no longer shows false positives. This build becomes the new default **Latest**.

## Fixes

- **The long-session freeze (memory leak) — the big one.** On a machine that kept Agentmaster open for a long time (especially with more than one window), committed memory could climb steadily — measured at roughly **1.6 GB/hour**, reaching tens of GB after a day or two — until Windows ran out of commit and the app **froze**: the window stayed up and everything non-graphical kept running (hooks, the engine, the autorunner), but the view stopped repainting and stopped responding to input. Root cause: under XAML Islands, a per-element tooltip's framework-side registration was **never torn down**, so every rebuild of the Manager's board/tree/Sessions surfaces left its ~100 tooltips (and their whole element trees) pinned in memory forever. Tooltips now route through **one shared, per-thread tooltip host** that attaches only for the duration of an actual hover, so nothing accumulates — plus a round of follow-up hardening (a strong open-owner reference so a rebuilt-away element can't strand an attachment, a deliberately-leaked host to avoid a shutdown-teardown crash, live re-tipping of an open tooltip, and cancelling a pending tooltip when its page hides).
- **Triage-Board ⚡ "still cached" false positives.** The card's **⚡** "server prompt-cache still warm" hint keyed on a fallback timestamp (the Waiting-for-you decay anchor) that gets stamped "now" by events with **zero API traffic** — so it lit up right after you **adopt** a session, use **Move to Waiting-for-you**, freshly **launch** or **resume**, on a never-prompted session, or on a managed **Codex** (which has no prompt cache at all). It now keys strictly on **real API-turn evidence**, so ⚡ only appears when a request actually happened inside the cache window.

## Changes

- **The Inferred working directory is now the session's working directory everywhere.** When you use the **Inferred working directory** tab-coloring mode (0.6.0), Agentmaster detects the directory a session *actually works in* (by the paths its tools touch). Previously only the **color** used that; every other surface still read the launch folder — so a session that inferred into another repo wore that repo's color but was still **grouped, scoped, displayed, "Open New Session Here"-targeted, and copied** under its launch folder. Now a single resolver (`EffectiveWorkingDir`) feeds all of those, so a card/row can never sit in one directory group while its tab wears another's color. The terminal's real working directory still owns all the *mechanics* (spawn / resume / fork / restart, hook + transcript correlation, per-directory environment) — so launching and correlation are unchanged; only *where the UI files the session* follows the inference.

## Also in this release

- Documentation: the contributor **CLA** is finalized (governing law set to Israel; DRAFT framing removed), README **licensing** details clarified, and the 0.6.0 release notes archived in full.

## Install

**One command (PowerShell)** — installs 0.6.1 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.1
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). 0.6.1 is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable (no cert):** download `Agentmaster_0.6.1.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.1.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.1.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
