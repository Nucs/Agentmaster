<!-- Agentmaster 0.6.0 release notes — STABLE (becomes Latest; NOT a pre-release).
     COMPREHENSIVE: contains the full changelog since the last stable (0.5.5) — the 0.6.0 new work
     up top, then each 0.5.6-0.5.9 pre-release's complete New/Fixes/Thanks (Install sections stripped).
     Range: v0.5.9..HEAD = 74 new commits; v0.5.5..HEAD = 222. RELEASING from HEAD = 69ae35f6e (code 2f6e1d3fa).
     Tag v0.6.0 -> 0.6.0.0. STABLE mechanics: publish --draft=false --latest (REPLACES 0.5.5 as Latest; no --prerelease).
     Assets: Agentmaster_0.6.0.0.msixbundle, _x64.zip, _arm64.zip, Agentmaster.cer. -->
## Agentmaster 0.6.0

**0.6.0 is a stable release** — the first stable since 0.5.5, and it brings the entire **0.5.6 → 0.5.9 pre-release line** to everyone plus a large batch of new work. Headlines: a full **bookmark Tags** system, **three tab-coloring modes**, **"Activate All Tabs"** + Shift-click eager-init, a **launch splash** that covers session restore, a rewritten **rich tab tooltip**, and broad **crash- and log-flood hardening** (including a fix for a runaway 320 MB log). It also folds in everything the pre-releases added — the unsent-draft **"3 dots"** indicator, **API-error → Error** detection, a **6-tab Settings** cog, **cross-file conversation lineage**, **git worktrees** in the Launch picker, and in-place **session-title editing**. This build becomes the new **default (Latest)** — no "allow pre-release" toggle needed. The project is dual-licensed **AGPL-3.0-or-later + commercial**.

_The complete changelog for each 0.5.6–0.5.9 pre-release folded into this stable release is included in full below._

## New in 0.6.0

- **Bookmark Tags — color-coded labels on any session.** Attach durable, named **tags** to a session and they show everywhere it appears: as little **bookmark ribbons** hanging off the bottom edge of its tab, on its Triage-Board card, and in a dedicated **Tags column** in the Sessions browser. Each tag has a **user-pickable color** (durable), a rich **clickable hover panel** listing every session that carries it (click a row to jump to that tab), and the tab tooltip shows a colored tag-chip row. Manage them from a **"Tags"** panel on the tab / card / Sessions-row right-click menu; filter the Sessions browser by **tag filter chips**. Tag removal is explicit (a tag stays listed until you delete its row), and the whole set persists across restarts.
- **Three tab-coloring modes.** Settings now offers how managed tabs get their color: **Shared per working directory** (the classic — a folder keeps one permanent color), **Individual per tab** (each session keeps its own), or **Inferred working directory** (color by the directory a session actually *works in*, detected by occurrence-ranking the paths its tools touch — git-root-aware by default, with machine-`%TEMP%` scratch paths excluded so a scratchpad file can't hijack the color). The color picker itself is now a true occurrence-ranking picker.
- **Activate All Tabs + Shift-click eager-init.** Managed tabs restore **lazily** (a background tab's `claude` doesn't start until first shown). You can now **Shift-click** a dormant tab to activate it **in place** (start it without switching to it), or use **"Activate All Tabs"** to wake the whole fleet — paced (drip-fed, a few at a time) with an **"Activating N tabs…"** busy state so a large workspace doesn't stampede.
- **Launch splash.** A lightweight **splash window** (on its own thread) now covers the ~7 s workspace restore at startup, so launch no longer looks hung. It's a normal taskbar window (minimize / close), never topmost, and dismisses only once the main window has actually settled.
- **Rewritten rich tab tooltip.** The tab hover is now a **modern summary-style card** (anchored under the tab) instead of a flat wall of text — and it was hardened against a long string of XAML fail-fasts (reused-tooltip use-after-free, rapid tab-swap stowed fail-fast, unrooted-owner and layout-cycle crashes) so hovering the tab strip is stable.
- **Auto Testing / Tests Autorunner (renamed; release-clean).** The prompt-queue automation formerly called *Flight Plan / Autopilot* is renamed **Auto Testing / Tests Autorunner** and is now a **development-only testing tool**. In this **release** build the Manager's bottom-right pane is a clean, read-only **session Summary** — the auto-send queue and its controls are hidden and inert. (Your data and every other Manager capability are unchanged.)
- **Stability & log hygiene.** Fixed a runaway logging path that could grow `hooks.log` to **~320 MB** with ~300 K forced disk flushes (a `[Unknown]` notify flood + redundant `sessions.json` writes) — now quiet. Numerous background lanes were wrapped so a thrown exception on a worker thread can never `std::terminate` the process, and the app ships with a new in-repo crash-analysis toolkit behind the scenes.

## Fixes

- **"Dev won't launch" / a stalled startup.** The multi-window "reopen your N windows?" decide-prompt could render as an **invisible modal**, hanging launch forever — fixed (it's a real, visible prompt now). Startup timing and the splash dismiss were also corrected.
- **Copy actually copies.** "Copy Summary" (and every overlay / board copy) could silently fail from a **focused** tab — replaced with a robust retry-looped Win32 clipboard writer.
- **Restore fidelity across a crash.** A crash-reopen now **preserves** a session's Running / Waiting-for-you triage state, persists the clarifying-question guard (so the autorunner can't auto-answer a pending question on reopen), and won't auto-send into a not-yet-started re-homed tab.
- **Close confirmations** are never permanently suppressible (no more "Don't ask again" foot-gun), and the splash / loading window no longer floats **topmost**.
- Plus the accumulated pre-release fixes: **Resume / Fork / restore open the exact session you picked**, forks keep their content, app-wide crash-hardening of every background transcript reader, and broad tooltip / hover polish.

---

# Full changelog since the last stable (0.5.5)

Everything below shipped across the **0.5.6 → 0.5.9** pre-releases and is now part of stable **0.6.0** (newest first).

## 0.5.9 (pre-release)

**0.5.9 is a pre-release** building on 0.5.8 — a large one. Highlights: an **unsent-draft "3 dots" indicator** (see at a glance which tabs have a prompt you typed but never sent), **API-error detection** that turns a rate-limited / overloaded turn into a real **Error** state with the message and status code on the card, a **Settings cog reorganized into 6 tabs**, **cross-file conversation lineage** in the summary panel (follow a session across `/clear` · `/compact` · plan-restart), **git worktrees in the Launch path-picker**, in-place **session-title editing** in the Sessions browser, configurable **flash-ring / overlay-opacity / Favorite-marker**, and broad **crash-hardening**. The project is now **dual-licensed AGPL-3.0-or-later + commercial**.

### New

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

### Fixes

- **Resume / Fork / restore now open the EXACT session you picked.** Every open path (Resume, Fork, double-click, window-restore) previously ran the clicked id through a timing-based "continuation" heuristic that could **jump to an unrelated live tab** or silently **merge** a restored tab into another session. That unsound redirect is removed — you now always land on the conversation you chose. (The solid plan-restart-parent lineage that powers the summary's "previous sessions" is kept.)
- **App-wide crash hardening.** A hover tooltip whose open-timer fired on an already-detached element could crash the app — fixed. The earlier crash-proofing of every background transcript reader (prompt-nav, summary panel, Sessions browser, state scanner) is extended, so a malformed/partial transcript or a teardown race can never escape its worker thread and take a window down.
- **Forks keep their content.** Restoring a **never-messaged fork** now **re-forks from its source** instead of starting fresh (so the forked context isn't lost), and the summary panel shows the fork **parent's** transcript for a never-messaged fork (fixing the empty panel / "the toggle does nothing").
- **Triage board behaves in the background.** A backgrounded Triage Board no longer **steals OS foreground** on every refresh; double-click-to-Activate works even when the card isn't already selected; and a click no longer resets a column's scroll position to the top.
- **Tooltip & hover polish.** Stopped the global hover-tip flicker (no more close-and-reopen on every pointer move), tips hide the instant the pointer moves and are click-through so they never eat a click, and the per-tab overlay's "100%" opacity is now truly opaque.
- **Summary accuracy.** Teammate **protocol** notifications (idle-notification chatter) are dropped while real teammate reports are kept; embedded tables render un-framed in wrap-on mode; and cwd advances correctly across a plan-parent hop in the lineage walk.


## 0.5.8 (pre-release)

**0.5.8 is a pre-release** building on 0.5.7: a stability-and-diagnostics pass — a greppable **navigation audit trail** and **timestamped logs**, two notable **freeze fixes** (switching keyboard layout · interrupting a subagent turn), **smarter Sessions search ranking**, a **~95% quieter log**, and a small **favorite/close** refinement.

### New

- **Navigation audit trail (`[nav]` log).** Every UI navigation and action — clicks, selections, launch/fork/resume/close, and a tab's conversation **swaps** (`/clear`, `/resume`, `/compact`) — now emits one greppable line in `hooks.log`. `grep '[nav]' hooks.log` reconstructs exactly what you clicked, and the lines reference the same ids the engine already tags (`[fork]`/`[resume]`/`[rehome]`/`[spawn]`/`[archive]`), so *intent* and *mechanism* finally join up. This is what turns a vague "it opened the wrong session" into a followable chain.
- **Timestamps on every log line.** A local-time `[HH:MM:SS.mmm]` stamp is now prefixed at the single logging chokepoint, so every layer — hook events, engine tags, the `[nav]` trail, the observer census, `autopilot.log`, `scanner.log` — is time-ordered at once. Turn duration, Enter-retry gaps, and observer lag read straight off the log.
- **Silent failures are now surfaced.** A launch that produced no tab (`[launch-fail]`) and a failed state write (`[persist-fail]` — `sessions.json`, per-window records, templates, dir-colors, …) used to vanish without a trace; both now log the id/path and reason, so a lost session or a "the launch did nothing" is diagnosable.
- **~95% quieter logs.** The observer census (which had grown to roughly a third of a multi-hundred-MB `hooks.log`) now re-logs only when **your** fleet actually changes — unrelated `claude`/`codex` sessions you're running elsewhere no longer trigger it — and its keep-alive heartbeat went from every 15 s to every 5 min. The full live counts still print as context; they just no longer spam.

### Fixes

- **No more multi-second freeze when you switch keyboard layout.** Changing the input language (Alt+Shift / Win+Space) while a Claude session was running froze the window for several seconds, every time — an inherited Windows Terminal issue ([microsoft/terminal#11522](https://github.com/microsoft/terminal/issues/11522)) where a layout change triggered a full settings reload re-applied to every pane. A layout change now only re-resolves the keybinding map (the one thing it can legitimately affect), so it's instant even with many live panes.
- **No more freeze / runaway log on an interrupted subagent turn.** Interrupting (Esc) a **Full-autopilot** session that was running Task subagents could pin the whole window unresponsive (observed: ~13 minutes, with `hooks.log` ballooning past a quarter-gigabyte) as the state scanner flip-flopped the session **Running ↔ Waiting** on every tick — each flip rebuilding the board, advancing Autopilot, and rewriting `sessions.json`. The interrupt is now correctly treated as a turn-ender, so the oscillation can't start.
- **Sessions search ranks names and titles above incidental content.** Searching the browser for a session by name now surfaces the **name/title** match at the top, instead of letting a more-recently-active session that merely *mentions* the term somewhere in its conversation float above it — the subtle trap behind resuming or forking the wrong row.
- **Closing an already-favorite session offers "Unfavorite & Close."** The close confirm's keep-this button flips sense on the session's current star: an already-favorite session now offers **☆ Unfavorite & Close** (the hollow anti-star) instead of a no-op re-favorite.


## 0.5.7 (pre-release)

**0.5.7 is a pre-release** building on 0.5.6: a Flight-Plan **Summary tab**, a **Global + per-directory environment-variable** editor, two shipped **reliability defaults** (far more API retries · keep Claude history effectively forever), bulk **tab-close** actions, more accurate **"last active"** timing, an **Autopilot** question-handling fix, and **crash/hover** fixes.

### New

- **Flight Plan ⇄ Summary tab.** The Flight-Plan pane's header is now a two-state **[Summary | Flight Plan]** pill. The new **Summary** tab renders the selected Claude session's summary box (recap · tasks · messages · files) **newest-first** and scrollable, right inside the pane — so you can read where a session stands without leaving the Manager. The prompt queue + compose box live under **Flight Plan** (with the Autopilot toggle moved to a strip atop it). The choice is global and syncs across windows.
- **Per-session environment variables (Global + Per-directory).** Settings' **Environment variables** area is now a two-tab **Global / Per-directory** editor whose values are merged into every **Claude *and* Codex** session you launch — a per-directory entry overriding a same-named global one. Per-directory sets persist (like per-dir tab colors), and the editor live-validates each line (green / amber / red, with a status line and reserved-name warnings).
- **Two shipped reliability defaults — editable, and they stay deleted.** Every install (new *and* updated) now seeds two sensible defaults you can freely change or remove (a deleted one never silently returns): **`CLAUDE_CODE_MAX_RETRIES=50000`** in the global env (far more resilient to transient API errors), and **`cleanupPeriodDays=36500`** written to your `~/.claude/settings.json` so Claude effectively **never purges session history** — what Resume and the Sessions browser rely on. A new **"Keep Claude history (days)"** field in Settings exposes the latter (blank reverts to Claude's 30-day default).
- **Flight Plan: copy a prompt.** The prompt right-click menu gains **Copy** on any row (sent or queued); the misplaced whole-session "Close" was removed from it (session-level verbs already live on the card / tab menus).
- **Bulk tab-close actions.** The tab right-click **Close ›** submenu gains **Close all tabs** (the whole-window twin of "Close other tabs") and **★ Favorite & close all tabs** (stars every managed session, then closes everything — shown only when the window hosts a managed session). Both route through the same path as a normal tab close, so they keep the single aggregate confirm and the always-archive (never-delete) bookkeeping, and never touch the pinned Manager tab; the **Move** and **Close** submenus also pick up header icons.
- **Sessions: slash-insensitive directory search.** A directory search term now matches regardless of `/` vs `\` — so `k:/source` finds a session under `k:\source`.

### Fixes

- **Accurate "last active."** A session's last-activity is now derived from its last **real conversation line** instead of the transcript file's modified-time — so a `claude --resume` (or a `/model`, permission-mode, or shell-cwd change) that appends untimestamped trailer lines no longer bumps an idle session to "just now." This corrects the Triage-Board cards, the Explorer tree, the per-tab overlay, the tab tooltip, the **MOST ACTIVE** sort, the **⚡ "still cached"** hint, and the `agentmaster` CLI.
- **Autopilot no longer stalls on a question.** When an agent ends a turn with a clarifying question (a "needs-you" state), the next queued prompt now simply **stays queued** and waits for the following turn — exactly as it does mid-run — instead of being moved to a separate "Held" state that could strand the whole queue on resume. (Prompts stranded as Held by an older build are rehabilitated automatically.)
- **Forked sessions track the right conversation.** Forking a session — from the Sessions page, a duplicate-tab fork, or **Adopt → fork-a-copy** — no longer leaves the new tab following the *source* it branched from. A `--fork-session` fork's very first startup hook briefly reports the source id (Claude is mid-resume at that instant, before it forks), and the app mistook that echo for an in-session `/resume` and re-homed the fork's tab onto the inactive source — so its overlay, state dot, and "copy launch CLI" all tracked the wrong conversation while "the observer never caught any changes." That source-id echo is now recognized by tab token and ignored (one-shot), so the tab stays bound to the live fork (a later, deliberate `/resume` still re-homes normally).
- **No crash on Alt+↓ / Alt+↑ — plus app-wide crash hardening.** Pressing **Alt+↓ / Alt+↑** (jump to the next / previous sent prompt) on a tab whose terminal hadn't initialized yet — a lazily-restored or never-focused tab — could crash the app: the prompt-nav path read the viewport before confirming the prompt had actually resolved, dereferencing an uninitialized buffer. It now bails cleanly when nothing resolves. The same pass hardened **every background path that reads a transcript** (the prompt navigator, the summary panel, the Sessions browser, the state scanner): a thrown error on a malformed or partial transcript — or a teardown race — can no longer escape its background thread and take the **entire window** down, killing every managed session; the error is contained and the operation simply no-ops. Genuine off-screen prompt navigation is unaffected.
- **Hover & tooltip polish.** Every Manager toolbar / header / compose button is now hit-testable with a real hover lift; the splitter grip keeps its highlight while you drag the divider; the scope-toggle accent and the per-tab badge/summary hover are no longer disrupted by inner labels; the per-tab badge/summary tooltips route through the shared **dark, stuck-proof** tooltip helper; and the default tab-title tooltip is dark-pinned (was light over the dark strip). The tab-strip **Favorite crown** is also a touch larger.


## 0.5.6 (pre-release)

**0.5.6 is a pre-release** building on 0.5.5: a new **Keep Awake** toolbar control, **Favorites replace the Archive** (the Sessions page is now the single history view), **context-window usage** on Triage-Board cards, a row-level **Filter** menu in Sessions, and a refreshed Manager toolbar.

### New

- **Keep Awake (tri-mode).** A new **Keep Awake** toggle in the Manager toolbar stops the PC and display from sleeping during long unattended runs — the in-app equivalent of a stay-awake script. It cycles three modes: **Off** (sleep normally) · **Always** (always held) · **While Running** (held *only* while a session is actively running, then lets the machine sleep once every agent is at rest). The button is green while actually holding the machine awake and amber when *While Running* is armed but nothing is running, so you can tell at a glance. Per-window and not persisted across restarts (a deliberate safety default).
- **Favorites replace Archive.** The separate "Archive" concept is gone — a managed tab's lifecycle is now **Close** and **Favorite**, and the **Sessions** page is the single browser for your history and every other session on the machine. **★ Favorite** any session to keep and find it: a leftmost ★ column and a `[ ] Favorite` filter in Sessions, a row/tab right-click **Favorite/Unfavorite**, and a **gold crown** on a favorited session's tab-strip dot while you work. **★ Favorite & Close** keeps + closes in one gesture (the close confirm — single and batch — offers Close · ★ Favorite & Close · Cancel). Favorites persist in the durable per-session store, survive Close, and work for never-managed on-disk sessions; closing never deletes a transcript, so any session stays resumable in Sessions.
- **Context-window usage on Triage-Board cards.** Each managed session card now shows how much context the conversation is carrying as a raw token count (e.g. `ctx 182K`, with the exact comma-grouped count in the tooltip), so a session nearing auto-compact stands out. The number is the API's own token accounting (input + cache-creation + cache-read + output of the newest turn), not an estimate — verified to track the context carried into the next request within ~1–2%.
- **Sessions: a row-level Filter menu.** Right-click any Sessions row → **Filter ▸** to narrow the list to sessions *like* that row: **By Same Directory / Same Branch / Same Day / Same Week / Same Month / Fork Family** — composing (**AND**) with the search box and the scope / Open / Hidden / Favorite toggles. A dismissible **✕ filter** chip beside the search box summarizes the active facets and clears them. (Directory match is filesystem-aware, branch is exact, the day/week/month buckets are DST-safe local time, and "Fork Family" is the connected fork lineage of the row.)
- **Manager toolbar refresh.** The action buttons now sit in a **compact row under the title** — **Settings · Sessions · Keep Awake · Pause Autopilot**. Opening the Manager no longer auto-focuses the Launch path box (no surprise caret or path-picker popup — clicking any control still focuses it). The Sessions page search bar moved to its own row with the filters beneath it, stretched to the table edge.

### Fixes

- **Rich tab tooltip no longer gets stuck open.** The session-aware tab-strip tooltip relied on a single `PointerExited` to close, which XAML Islands routinely drops (window deactivate, a fast exit, a click/drag stealing pointer capture) — leaving it stranded open. It now closes defensively: an 8-second auto-dismiss backstop (re-armed while you read) plus close-on-`PointerExited`/`PointerCanceled`/`PointerCaptureLost`.
- **Sessions Filter hover crash.** Fixed a stowed-exception crash (`0xC000027B`) when hovering the filter chip / submenu — the shared tooltip helper wasn't idempotent and re-tipping a persistent element orphaned a placement-less ToolTip. The fix also hardens every tooltip surface in the app against the same foot-gun.

### Thanks

- **@galiani1** for the community PR (#1) that introduced **Keep Awake** and the **context-window usage** indicator (both refined here — Keep Awake promoted to tri-mode, context shown as a reliable raw token count).


## Install

**One command (PowerShell)** — installs 0.6.0 (trusts the self-signed cert with a single UAC prompt):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Nucs/Agentmaster/agentmaster/tools/Install-Agentmaster.ps1))) -Version 0.6.0
```
Add `-Portable` (cert-free, no admin) or `-Launch` (start after install). 0.6.0 is now the default **Latest**, so a plain run (no `-Version`) also picks it up.

**Portable (no cert):** download `Agentmaster_0.6.0.0_x64.zip` (or `_arm64`), unzip anywhere, run `agentmaster.exe`. Fully self-contained.

**MSIX bundle (self-signed):** download `Agentmaster.cer` + `Agentmaster_0.6.0.0.msixbundle`, trust the cert once (`Import-Certificate -FilePath Agentmaster.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`, admin), then `Add-AppxPackage .\Agentmaster_0.6.0.0.msixbundle`.

Runs as **`agentmaster`** / Start menu **Agentmaster**. Upgrading keeps your data (your existing `%USERPROFILE%\.agentmaster` carries over). Requires Windows 10 2004+ (19041), x64 or arm64, with [Claude Code](https://www.anthropic.com/claude-code) (`claude`) — and optionally Codex — on `PATH`. Built on Windows Terminal v1.24.2372 (MIT). Agentmaster is dual-licensed **AGPL-3.0-or-later** + commercial.
