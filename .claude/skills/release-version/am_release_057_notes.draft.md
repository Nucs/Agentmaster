<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.5.7 release notes — PRE-RELEASE. DRAFT (held for the user's "go").
     Adapted to the 0.5.5/0.5.6 style: tight intro (NO "A fork of Windows Terminal..." preamble),
     NO "## Capabilities" section, optional "## Thanks" for community PRs (NONE in this range).
     Range: v0.5.6..HEAD — 26 commits, 4 docs-only omitted (8f87f9839 /scratch+README, 7c77ffae0 +
     0932a8cc0 the README lean-rewrite, d52bc303e prompt-nav contract doc).
     RELEASING 2026-06-24 from HEAD = 652500794 (origin tip 0932a8cc0 + 4 local crash-hardening commits:
     the prompt-nav AV guard f71bd6722, its contract doc d52bc303e, the coroutine exception-proofing
     ecd63015a, and the engine-wide transcript-reader hardening 652500794). Push origin/agentmaster,
     tag v0.5.7 -> 652500794, publish --prerelease --latest=false (stable 0.5.5 stays Latest).
     PRERELEASE mechanics: publish --prerelease --latest=false (the stable 0.5.5 stays Latest).
     Asset names: tag v0.5.7 -> 0.5.7.0. -->
## Agentmaster 0.5.7 (pre-release)

**0.5.7 is a pre-release** building on 0.5.6. Enable **Settings → Updates → Allow pre-release versions** to receive it automatically, or install from the assets below. The stable **0.5.5** remains the default for everyone else.

## What's new

- **A Summary tab beside the Flight Plan.** The pane's header switches between the selected session's summary, newest first, and its Flight Plan.
- **Environment variables, global and per folder.** Settings merges them into every Claude and Codex session you launch, a folder's entry overriding a global one.
- **Two reliability defaults.** `CLAUDE_CODE_MAX_RETRIES=50000` and `cleanupPeriodDays=36500` are seeded once, and one you delete never comes back.
- **Copy a prompt from the Flight Plan.** Any row's right-click menu copies its prompt.
- **Bulk tab closes.** Close › gets Close all tabs and ★ Favorite & close all tabs.
- **Slash-insensitive folder search.** `k:/source` finds a session under `k:\source`.

## What's fixed

- **"Last active" is accurate.** It comes from the last real conversation line, so a resume or a `/model` no longer makes an idle session read "just now".
- **Autopilot no longer stalls on a question.** A queued prompt stays queued for the next turn instead of moving to a Held state that could strand the queue.
- **Forks track their own conversation.** A new fork's tab no longer follows the session it branched from.
- **No crash on Alt+↓ / Alt+↑.** Jumping between prompts on a tab whose terminal had not started no longer crashes the app.
- **Hover and tooltip polish.** Every Manager button hovers properly, and the splitter keeps its highlight while you drag it.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.5.5** stays the default Latest.

**One command (PowerShell)** — install 0.5.7 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.5.7
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.5.7.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.5.7.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.5.7.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
