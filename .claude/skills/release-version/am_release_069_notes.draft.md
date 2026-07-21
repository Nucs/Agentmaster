<!-- Agentmaster 0.6.9 release notes — PRE-RELEASE (NOT Latest; v0.6.1 stays Latest).
     PRERELEASE NOTES RULE: show ONLY this prerelease's OWN explicit delta vs the previous release (0.6.8).
     24 commits v0.6.8..213e8426e, themed. Mainline tip = 213e8426e (origin was 14 behind; pushed at release).
     Headliners: the /handover command family (3ded15111 foundation + 9e66208de content injection +
     f63f8b108 /handover-here + 1bb4b0c52 fan-out + edb81eb0d Commands tab + c26e6da8f successor shaping),
     the 0.6.8 UI-FREEZE fix (a74e6e348 + afe8cec03), the updater envelope-schema fix (8e954ae3c +
     93b59f717 + 1d51f7e73), the sub-2s toast guard (a86570e08).
     Omitted (internal): exception-forensics sweep/docs, SHA-256 definition history, test/harden commits,
     the agentcli build-script link fix. PRERELEASE mechanics: --draft=false --prerelease --latest=false.
     Assets: Agentmaster_0.6.9.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.9 (pre-release)

**0.6.9 introduces `/handover`** — hand a session off to a fresh successor without losing the thread — and fixes **the 0.6.8 UI freeze** plus a long-standing bug that stopped 0.6.x installs from ever prompting for updates. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.6.1** remains the default for everyone else.

## New in 0.6.9

- **`/handover` — hand your session off to a fresh successor.** Type **`/handover <what to carry over>`** in any managed Claude session: Claude writes a self-contained briefing (`HANDOVER-<topic>.md`) addressed to its successor, and Agentmaster automatically opens a **new tab** with a fresh session in the same working directory — seeded with that briefing **verbatim and in full as its first message**. You get a clean context window that already knows everything, without copy-pasting. It's repeatable (every `/handover` spawns its own successor), and it **fans out**: if Claude writes **several** briefing files in one turn, you get **several parallel successors**, one per file.
- **`/handover-here` — the in-place twin.** Same hand-off, except the successor **replaces the current tab** instead of opening beside it. The original session is archived (and stays resumable from the **Sessions** browser), so a long-running tab can start fresh without you losing anything or re-arranging your workspace.
- **Customize the hand-off — a new Settings "Commands" tab.** **Rename** either command (the word you type is the command's name) or **disable** it entirely, and **shape what a hand-off produces**: pick the successor's **model**, **rewrite its title** with a find/replace regex, control **which markdown files count** as a briefing (a file-match regex, defaulting to the `HANDOVER-…` naming), and optionally **delete the briefing file** once the successor has actually started — the answer to leftover `HANDOVER-*.md` clutter. Each tab also gains a **Reset** button to restore its defaults.

## Fixes

- **The 0.6.8 UI freeze.** On 0.6.8 the window could stop repainting and stop responding to input while everything else kept running normally — sessions still advanced, hooks still arrived. The cause: the summary panel re-resolved every linked session's prompt-jump targets every few seconds, on the UI thread, for **all** sessions rather than the visible one; with a busy fleet and long conversations that pegged the thread at ~98% of a core and it stopped pumping input entirely. That refresh is now **cached** per buffer change, **limited to the focused tab**, and **floored at one pass per 2.5 minutes** — with a ~5× faster resolver behind it.
- **Automatic updates actually work now.** The updater stored its three preferences at the wrong level of `settings.json`, so the startup and hourly checks **never saw "Allow pre-release versions"** — they always queried the stable channel, which (with every release since 0.6.4 marked pre-release) meant a 0.6.x install **never prompted for an update at all**, no matter what the checkbox said; Skip/Postpone choices were also wiped by the next settings save. All three now round-trip correctly, writes are atomic, **"Not now"** silences the updater until the next launch, and the whole surface is hardened against errors. ⚠️ Because the *old* build is what does the checking, **install 0.6.9 once by hand** — after that, pre-release updates arrive on their own.
- **No more premature "completed" toast.** A session that reported a completion after a sub-2-second Running span — while it was in fact still working — no longer fires a misleading "waiting for you" notification; the toast is held for re-light confirmation first.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.6.1** stays the default Latest.

**One command (PowerShell)** — installs 0.6.9 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.9
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.6.9.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.9.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.9.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
