<!-- Agentmaster 0.5.9 release notes — PRE-RELEASE.
     Style: matches 0.5.7 (tight intro, no "fork of Windows Terminal" preamble, no "## Capabilities",
     "## New in X.Y.Z" + "## Fixes" + "## Install (pre-release)").
     Range: v0.5.8..HEAD = 89 commits (13 docs/refactor/chore/test omitted from user notes).
     RELEASING from HEAD = c42442a89. Tag v0.5.9 -> 0.5.9.0.
     PRERELEASE mechanics: publish --prerelease --latest=false (the stable 0.5.5 stays Latest).
     Asset names: Agentmaster_0.5.9.0.msixbundle, Agentmaster_0.5.9.0_x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.5.9 (pre-release)

**0.5.9 is a pre-release** building on 0.5.8 — a large one. Highlights: an **unsent-draft "3 dots" indicator** (see at a glance which tabs have a prompt you typed but never sent), **API-error detection** that turns a rate-limited / overloaded turn into a real **Error** state with the message and status code on the card, a **Settings cog reorganized into 6 tabs**, **cross-file conversation lineage** in the summary panel (follow a session across `/clear` · `/compact` · plan-restart), **git worktrees in the Launch path-picker**, in-place **session-title editing** in the Sessions browser, configurable **flash-ring / overlay-opacity / Favorite-marker**, and broad **crash-hardening**. The project is now **dual-licensed AGPL-3.0-or-later + commercial**. Enable **Settings → Updates → "Allow pre-release versions"** to receive it automatically, or install from the assets below. The stable **0.5.5** remains the default for everyone else.

## New in 0.5.9

- **Unsent-draft "pending input" indicator.** Agentmaster now notices when you've **typed a prompt but not sent it** in a Claude tab and shows an animated **3-dot pulse** — on both the tab strip (under the status dot) and the Triage-Board cards, **cross-window** (a draft in one window shows on another's board). The dots' color is a configurable **light/dark pair** that is auto-contrast-picked against the tab's background (by WCAG luminance) so they're never invisible, and they get a reserved band so they're never clipped — even when the Star favorite marker is showing.
- **API errors become a real "Error" state.** When a turn fails on a transient API error (rate-limit, overload, 5xx), the observer now classifies the session as **Error**, **captures the message and HTTP status code**, and shows them on the **Error** board card — then recovers automatically on the next activity. It's **active-leaf-aware**, so a double-ESC rewind *past* the error leaves the Error standing correctly instead of clearing it prematurely.
- **Settings reorganized into 6 tabs.** The Agent Manager **cog** is now grouped into six top tabs instead of one long scroll, so launch / autopilot / tabs / behavior / environment / updates settings are each a click away.
- **Summary panel: cross-file conversation lineage.** The per-tab summary now follows a conversation **across `/clear`, `/compact`, and plan-restart joins** — surfacing the **previous session(s)** that led to the current one, with a toggle to show or hide them. It also **auto-refreshes as the transcript grows** (not only on a registry notify), with a manual **"Refresh Summary"** in the right-click menu and a refresh button on both summary surfaces.
- **Git worktrees in the Launch path-picker.** The Launch path-picker dropdown now **lists your git worktrees** with each one's **branch** shown, appends the branch to recent path rows too, and flags the **current** worktree when you're launching from inside a linked one.
- **Edit a session's title in place.** In the **Sessions** browser you can now rename a session's title directly — right-click **"Edit Title"** or a slow double-click on the Title cell — persisted durably to the session store. The browser also gains a compact **"Ctx"** context-tokens column, a row-menu **"Copy Selected Text"** with Truncate / Wrap toggles, and now **honors the column sort while a search is active**.
- **Triage board: dormant dot + Activate All Tabs + move-to-state.** A new half-hollow **"dormant"** dot distinguishes a not-yet-initialized session; **"Activate Tab" / "Activate All Tabs"** eagerly start a session's terminal; and a status-adaptive **"Move to Idle/Done" / "Move to Waiting-for-you"** is available on the card and tab right-click menus.
- **Configurable status flash-ring (color + opacity).** The tab status-dot **"unread" flash ring** is now fully configurable from a Settings color picker — both hue and opacity (default red at 80%) — housed in a compact flyout.
- **Selectable Favorite marker — Crown or Star.** Choose whether a favorited session's tab wears a **Crown** (default) or a **Star**, in Settings.
- **Configurable per-tab overlay opacity.** A dual-thumb gradient slider sets the per-tab overlay's **rest and hover** opacity independently.
- **Shift+Home toggles the Agent Manager tab.** A **Shift+Home** hotkey jumps to the Manager tab and back, mirroring the tab-strip Home / Jump-Back buttons, and now registers regardless of where focus sits.
- **Persistence-aware close-window confirm.** Closing a window now shows a persistence-aware confirm with a **"Close All Windows"** escalation; the "Don't ask me again" checkbox was removed.
- **Revert-aware transcript display.** Branches you **rewound away** with double-ESC are now excluded from the summary, the title derivation, and the **Transcript** copy — so a conversation you backed out of no longer pollutes the displayed history.
- **Launch-timing timeline.** A lightweight **`[startup]`** phase-timing trace (greppable in `hooks.log`) makes a slow launch diagnosable — every phase from exe prelude through per-window workspace restore is stamped on one clock.
- **More accurate prompt jumps.** Alt+↑ / Alt+↓ (jump to the previous / next sent prompt) now validates a match against Claude Code's prompt-marker glyph for fewer mis-jumps.
- **Per-window Manager tab color, menu polish, and a dev aid.** The Manager tab's right-click color is now remembered **per window**; the tab context menu is grouped with separators (with "Change tab color" / Copy repositioned); and **AgentmasterDev** builds prepend each element's unique id as the first tooltip row to aid UI work.

## Fixes

- **Resume / Fork / restore now open the EXACT session you picked.** Every open path (Resume, Fork, double-click, window-restore) previously ran the clicked id through a timing-based "continuation" heuristic that could **jump to an unrelated live tab** or silently **merge** a restored tab into another session. That unsound redirect is removed — you now always land on the conversation you chose. (The solid plan-restart-parent lineage that powers the summary's "previous sessions" is kept.)
- **App-wide crash hardening.** A hover tooltip whose open-timer fired on an already-detached element could crash the app — fixed. The earlier crash-proofing of every background transcript reader (prompt-nav, summary panel, Sessions browser, state scanner) is extended, so a malformed/partial transcript or a teardown race can never escape its worker thread and take a window down.
- **Forks keep their content.** Restoring a **never-messaged fork** now **re-forks from its source** instead of starting fresh (so the forked context isn't lost), and the summary panel shows the fork **parent's** transcript for a never-messaged fork (fixing the empty panel / "the toggle does nothing").
- **Triage board behaves in the background.** A backgrounded Triage Board no longer **steals OS foreground** on every refresh; double-click-to-Activate works even when the card isn't already selected; and a click no longer resets a column's scroll position to the top.
- **Tooltip & hover polish.** Stopped the global hover-tip flicker (no more close-and-reopen on every pointer move), tips hide the instant the pointer moves and are click-through so they never eat a click, and the per-tab overlay's "100%" opacity is now truly opaque.
- **Summary accuracy.** Teammate **protocol** notifications (idle-notification chatter) are dropped while real teammate reports are kept; embedded tables render un-framed in wrap-on mode; and cwd advances correctly across a plan-parent hop in the lineage walk.

## Install (pre-release)

> **This is a pre-release.** It is **not** offered as an automatic update unless you enable **Settings → Updates → "Allow pre-release versions."** The stable **0.5.5** stays the default Latest.

**One command (PowerShell)** — install 0.5.9 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.5.9
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install).

**Portable (no cert):** download `Agentmaster_0.5.9.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.5.9.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.5.9.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
