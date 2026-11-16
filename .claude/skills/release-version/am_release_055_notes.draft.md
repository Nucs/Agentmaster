<!-- ONE-ROW NOTES since 2026-11-16, at the owner's request ("Change the releases on gh too, finish",
     after v0.8.0-v0.9.0 were rewritten that way the same day): the body is a short intro, "## What's
     new" and "## What's fixed" (one row per change: a bold lead and about a sentence), then the
     kept sections (Worth knowing / Thanks / Install) unchanged. The body GitHub published until
     2026-11-16 is this file at 3da02fdbc; none of it is on GitHub any more.
     Agentmaster 0.5.5 release notes - archived 2026-11-16 from the published release (no draft was
     kept when it was released). The body below is the release's body exactly as GitHub published
     it, saved before the one-row rewrite of every release replaced it there (release id 343446862,
     published 2026-06-23T11:13:19Z). -->
## Agentmaster 0.5.5

**0.5.5 is a stable release** focused on reliability, and it brings the 0.5.4 pre-release's features to everyone. Upgrading keeps your data.

## What's new

- **Autopilot defaults to Full.** New and reopened sessions run on Autopilot, so queued prompts send themselves when a turn completes.
- **Multi-line prompts send as one message.** A multi-line prompt goes as a single bracketed paste instead of splitting at its first line break.
- **Smarter stuck-submit recovery.** A submit that does not land is retried quickly, then marked Failed with that session's Autopilot paused, instead of looping.
- **The idle recap shows for external sessions too.** Claude's "what we did / what's next" recap is read from the transcript of sessions Agentmaster only observes.
- **Prompt navigation stays in sync.** Alt+↑ / Alt+↓ re-read the conversation before each jump, and ↓ past the last prompt drops to the live tail.
- **Rich tab-strip tooltips.** Hovering a tab shows a session card: its state and why it needs you, model, folder and branch, the queue and the last exchange.
- **Claude's idle recap everywhere.** The recap shows on the board card, the tab tooltip, the summary panel, the Sessions detail, the copied Summary and `agentmaster show`.
- **Durable session titles.** A session's name persists across windows and restarts.
- **Waiting-for-you is an unread inbox.** A finished turn stays there until you look at its tab or a timeout (1 hour by default) lapses, and Mark Unread pins it.
- **An updater overhaul and one-click uninstall.** An embedded installer self-heals the 0x80073CFB conflict, and Settings gets an Uninstall Agentmaster button.
- **Prompt navigation on the badge.** ↑ / ↓ buttons scroll to your previous or next sent prompt, and a third row previews the next queued prompt.
- **Deleted sessions hide from Sessions.** The transcript stays on disk, and a Hidden checkbox shows them again with Unhide.
- **Autopilot drives adopted sessions.** Turning Autopilot on for an adopted session sends its queued prompts.
- **"Claude not found" on every launch path.** Every launch prompts when no claude.exe exists, and recovers once you install Claude.

## What's fixed

- **Mid-conversation switches are tracked.** After /clear, /compact or /resume into another conversation, the tab follows the live conversation instead of the launch id.
- **Resume and restore land where you left off.** They follow the continuation chain to its newest session, and the same session is never resumed twice.

## Install

**One command (PowerShell)** — install 0.5.5 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.5.5
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.5.5.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.5.5.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.5.5.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
