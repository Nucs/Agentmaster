<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     ARCHIVED AS PUBLISHED 2026-11-16: the body below this header is now the release's body exactly as
     GitHub published it, saved before the one-row rewrite of every release replaced it there. The
     body this file held until then differed from the published one (a draft revised after, or never
     applied as written); it is this file at the commit before this note.
     Agentmaster 0.5.4 release notes — PRE-RELEASE. Tag v0.5.4 -> b6c756c19 (HEAD).
     Supersedes the never-published 0.5.3 (tagged then cancelled/deleted before any draft existed).
     Cumulative since v0.5.2 (the last PUBLISHED/stable release) — 27 commits = the 0.5.3 feature set
     + 4 new: durable per-session title store (b6c756c19), Triage-Board refresh button (bff9c6eb3),
     scope-toggle emphasis (2ba424a7c), recap appended to the tab tooltip uncapped (fb7660b21).
     PRERELEASE mechanics: publish --prerelease, NOT --latest (0.5.2 stays stable Latest).
     Asset names: tag v0.5.4 -> 0.5.4.0. -->
## Agentmaster 0.5.4 (pre-release)

**0.5.4 is a pre-release** — a large feature drop for early adopters. Enable **Settings → Updates → Allow pre-release versions** to receive it automatically, or install from the assets below. The stable **0.5.2** remains the default for everyone else.

## What's new

- **Rich tab-strip tooltips.** Hovering a tab shows a session card: its state and why it needs you, model, folder and branch, the queue and the last exchange.
- **Claude's idle recap, surfaced everywhere.** The recap shows on the board card, the tab tooltip, the summary panel, the Sessions detail, the copied Summary and `agentmaster show`.
- **Durable session titles.** A session's name, set or derived, persists across windows and restarts.
- **Waiting-for-you is an unread inbox.** A finished turn stays there until you look at its tab or a timeout (1 hour by default) lapses, and Mark Unread pins it.
- **An updater overhaul and one-click uninstall.** An embedded installer self-heals the 0x80073CFB conflict, and Settings gets an Uninstall Agentmaster button.
- **Prompt navigation on the badge.** ↑ / ↓ buttons scroll to your previous or next sent prompt, and a third row previews the next queued prompt.
- **Deleted sessions hide from Sessions.** The transcript stays on disk, and a Hidden checkbox shows them again with Unhide.
- **Autopilot drives adopted sessions.** Turning Autopilot on for an adopted session sends its queued prompts.
- **"Claude not found" on every launch path.** Adopt, Open New Session Here, Resume, Restore and Fork prompt when no claude.exe exists, and recover once you install Claude.
- **Manager header polish.** The board gets a ↻ refresh, and the LOCAL / GLOBAL toggle stands out.

## What's fixed

- **No more hover flicker on cards and rows.** Board cards and Sessions rows no longer flicker their hover ring or tooltips, and a row is one clean click target.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.5.2** stays the default Latest.

**One command (PowerShell)** — install 0.5.4 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.5.4
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.5.4.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.5.4.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.5.4.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
