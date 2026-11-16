<!-- ARCHIVED AS PUBLISHED 2026-11-16: the body below this header is now the release's body exactly as
     GitHub published it, saved before the one-row rewrite of every release replaced it there. The
     body this file held until then differed from the published one (a draft revised after, or never
     applied as written); it is this file at the commit before this note.
     Agentmaster 0.3.0 release notes. Cumulative since the last PUBLIC release (v0.1.1):
     v0.2.0 was built but never published, so its line is folded into "New in 0.3.0".
     Format matches the v0.2.0 / v0.1.1 notes: flat "- **Title.** desc." changelog bullets.
     Asset names assume tag v0.3.0 -> 0.3.0.0. -->
## Agentmaster 0.3.0


## New in 0.3.0

- **Codex, managed alongside Claude.** Agentmaster now runs **Codex** sessions as first-class agents, not just Claude. Launch one from the primary launch bar via a **Claude ⇄ Codex** toggle; it restores with its window like any session, wears a teal **codex** pill on its Triage-Board card and Explorer-tree row, and the Fleet Observer derives its turn-state from the rollout transcript (observe-only, nothing injected).
- **Command-line introspection — `agentmaster <verb>`.** Query the whole fleet from any shell, whether the app is running *or* not. `agentmaster show <ref>` gives a session's derived state + presence, the conversation tail + last assistant reply, activity (messages / tools / **files touched**), the last prompt, and the Flight-Plan queue + Autopilot; plus `list` / `sessions` / `tabs` / `windows` / `external`, `--self` (the calling tab), and `--json`. Read-only — it reads persisted + OS-observable state, nothing injected.
- **Shell tabs restore at their real working directory.** Reopening a window brings pwsh/cmd tabs back where you left them (the OSC-reported cwd, with an out-of-band fallback), fully passively — not at the profile default.
- **Sharper external census.** External claudes are labeled by **host identity** (real Windows Terminal vs Agentmaster vs Agentmaster Dev); orphaned claudes are dropped (no phantom rows after deploy cycles), and the Claude **desktop app** is excluded (no spurious "system32" sessions).
- **Launch-box & manager niceties.** Paste a session id in the launch box and it validates live (green/red) with **Resume** + **Fork**; **Copy Session Id** is on every context menu; session titles can be **multi-line** (Return inserts a newline) and the rename box auto-sizes.
- **Engine state-machine robustness.** The scanner no longer strands blocked / interrupted / approved sessions in the wrong state; a resumed mid-turn session no longer falsely lights up **Running** (run-repair ignores the restore-time history replay); and Claude tabs no longer auto-close when `claude.exe` exits gracefully.
- **Tooltip & tab-strip polish.** Tooltips dismiss correctly everywhere — the Manager tab, through ScrollViewers, and on keyboard-nav scroll; the tab-strip status dot is repositioned and no longer clips at the tab's left edge.
- **Per-install profiles + clean release/dev coexistence** *(first public release of the 0.2.0 line)*. On first launch Agentmaster asks for a **profile folder** — Production (`%USERPROFILE%\.agentmaster`), Development (`%USERPROFILE%\.agentmaster-dev`), or **Browse…** — holding *everything* it persists, including Terminal's own settings. The released **`Agentmaster`** and a from-source **`AgentmasterDev`** install side by side with separate launchers, single-instance scopes, and profiles; hooks route to each install's own engine. Portable zips are truly self-contained (a `.portable` marker keeps all state inside the unzip folder).
- **Ordered hook state machine** *(0.2.0 line)*. Hook events carry a fire-time `ts` + per-turn identity, so a slow `Stop` landing after the next prompt can't paint a stale state — fixes the "second turn never shows Running" bug; type-ahead prompts keep the session correctly Running.
- **Sessions search grammar + cross-window actions** *(0.2.0 line)*. `"quoted phrases"` match exactly and a bare session-id GUID finds that session *and its forks*; the 📁/📄 directory/file scopes are on by default and answered from the index. Triage-Board cards gain the full right-click menu and a LOCAL/GLOBAL scope toggle shared with the tree; **Activate** and **Rename** work on a session hosted in *any* window.

## Install

Paste into PowerShell — installs **v0.3.0** (one UAC prompt, only to trust the signing cert):

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.3.0
```

Add `-Portable` for a cert-free, no-admin install, and/or `-Launch` to start it right after:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.3.0 -Portable -Launch
```

**Portable (recommended, no cert):** download `Agentmaster_0.3.0.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):**
1. Download `Agentmaster.cer` + `Agentmaster_0.3.0.0.msixbundle`.
2. Trust the cert once (admin PowerShell):
   `Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`
3. `Add-AppxPackage .\Agentmaster_0.3.0.0.msixbundle` (or double-click it).

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading from v0.1.x keeps your data — pick **Production** in the first-launch profile picker (your existing `%USERPROFILE%\.agentmaster` carries over; Terminal settings are seeded in automatically). Portable-zip users: state now lives inside the unzip folder — copy `%USERPROFILE%\.agentmaster` into `<unzip>\profile` to bring sessions along.

Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally [Codex](https://openai.com/codex/) — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT).
