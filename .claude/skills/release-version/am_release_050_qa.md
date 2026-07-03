<!-- Manual-QA checklist for Agentmaster 0.5.0 (everything in v0.4.1..HEAD).
     Built from a one-by-one read of all 84 non-merge commit messages.
     Baseline: v0.4.1 (14777bd83, 2026-06-18). HEAD: 6689a04f0 (2026-06-20). -->
# Agentmaster 0.5.0 — Manual QA Checklist

**Range:** `v0.4.1..HEAD` — **92** non-merge commits (last refreshed at HEAD `6689a04f0`). ~22 are docs/README (listed at the bottom, no QA). The rest are grouped below by area. Each item: **what** it does, the **commit(s)**, and **how to QA it by hand** with the expected result.

## 0. Before you start (read this)

- **You must deploy a fresh full build first.** The overwhelming majority of these commits were committed **unbuilt** (lib-compile-check only, or "not built per request"). Do the close → build → relaunch inner loop and do a **full `Terminal\CascadiaPackage` build**, *not* just `TerminalAppLib` — several features change WinRT projections (`TermControl.idl`, `TerminalTabStatus.idl`) and XAML `x:Bind`, which a lib-only compile can't validate. Items needing that are tagged **[full-build]** below.
- **Working tree had uncommitted edits at QA time** (`ProcessInspect.*`, `TranscriptStore.cpp`, `AgentTabOverlay.cpp`, `TabManagement.cpp`, `TabRowControl.xaml`, `TerminalPage.*`, `m5_tests.cpp`). A build from the tree includes them — decide whether to commit/stash first so you know exactly what you're testing.
- **Some intermediate commits were superseded inside this range** — the steps below describe only the **final** behavior: board sort default = MOST ACTIVE (not "least active"); the dot flash is a red **ring** (now 13px-thin, not a stroke recolor); compose **Shift+Enter does *not* send** (that chord was reverted) — Enter is a newline, the `!` button sends; the Triage-Board card's `model·effort·hb` adornment line was **removed entirely**.
- **Tags:** **[full-build]** = needs a full package build to even appear · **[release-only]** = path is gated to a packaged Release install · **[install]** = exercised via the installer/updater, not the running app.
- Recommended: have **2 working dirs** with **2+ live Claude sessions** and at least **1 Codex** session, **1 plain pwsh tab**, and a couple of **archived / on-disk** sessions, so board/tree/Sessions all have content.

---

## 1. In-app updater  `69648b1c2` `2f23e1212` `c2b1982f1` **[release-only]**

**What:** Self-updater that checks `Nucs/Agentmaster` GitHub Releases — at startup (release builds) and on demand from Settings — offering Update now / Postpone (3·7·30 days) / Skip this version, with a prerelease opt-in and a baked-in installer.

**QA — Settings (works on dev too):**
1. Open the Manager tab → **⚙ Settings** → scroll to the new **UPDATES** section.
2. Confirm an **"Allow updating to pre-release versions"** toggle (default **OFF**) and a **"Check for updates"** button.
3. When the cog opens, a silent check runs; if a newer release exists a **"vX.Y.Z available!"** label appears in dark green next to the button. (If you're already on the newest, no label — expected.)
4. Click **Check for updates** → a **TaskDialog** appears with **Update now / Postpone / Not now** and a radio group **3 / 7 / 30 days / Skip this version**. Cancel/X = Not now.
5. Flip the prerelease toggle ON, Save, reopen the cog, re-check → it now also considers GitHub pre-releases.
6. **Skip this version** → reopen cog/re-check → that exact version is never offered again (persisted as `updateSkippedVersion`). **Postpone 3 days** → persists `updatePostponedUntilUnixMs`; no nag until then.
✅ Network check must run **off the UI thread** — the cog stays responsive while it queries.

**QA — startup prompt (dev needs the env gate):**
7. The startup check is gated to packaged **Release** installs. To exercise it on **dev**, set `AGENTMASTER_UPDATE_STARTUP=1` in the environment, then launch. It must run **after** the profile resolves and **before** the "Reopen your N windows?" prompt, and be bounded (~6 s) so a slow network can't wedge launch.
8. **Update now** (only worth a real end-to-end test from an *older* installed version with a newer release published): the app writes the baked-in `am-update.cmd`/`am-update.ps1` into the active profile, downloads the `.msixbundle` + `.cer`, trusts the cert (one UAC prompt, skipped if already trusted), upgrades, relaunches, and the old process exits so the package isn't in use.
✅ A dev install that updates **graduates to the release identity**.

---

## 2. Install over a conflicting registration  `c2b1982f1` **[install]**

**What:** `tools/Install-Agentmaster.ps1` now recovers from `0x80073CFB` ("a packaged version cannot replace an unpackaged one") instead of aborting.

**QA:**
1. Have a registered **loose-layout** dev install of the same identity present, then run the installer for the packaged MSIX of that identity.
2. It should detect the conflict, **show** what's blocking (packaged vs registered/unpackaged + version), ask to uninstall & continue (`-Force` auto-confirms), `Remove-AppxPackage` it per-user (no admin), and **retry**.
✅ Your **profile data survives** (it lives in `~/.agentmaster*`, outside the package) — sessions/settings still there afterward. Also confirm the separate VCLibs-dependency (`0x80073CF3`) path still works and the two branches don't collide.

---

## 3. Tab strip

### 3a. "Home" button — jump back to the Manager when it scrolls off  `f05813f31` **[full-build]**
1. Open enough tabs that the strip overflows; scroll right until the pinned **Manager** tab (tab 0) slides off the left edge.
2. A small **Home** button appears **immediately left of the `<` scroll arrow** — only in that exact situation (real overflow AND tab 0 fully off-screen).
3. Click it → jumps to + focuses the Manager tab and scrolls the strip fully left. The button then hides again.
✅ With no overflow, or while the Manager is visible, the button is hidden.

### 3b. "Jump Back" button — the inverse of Home  `608d76791` **[full-build]**
1. From a session tab, switch to the **Manager** tab (the lens auto-selects the card for the session you came from).
2. A **Jump Back** button (undo/revert glyph) appears in the same slot — **only while on the Manager tab** and only when the selected card is a managed session **this window** hosts as a live tab.
3. Click it → returns to that session's tab. Home and Jump Back are **mutually exclusive** (Home only off-Manager, Jump Back only on-Manager).
✅ Select an **external** card or **Clear** the selection → Jump Back hides (no dead jump offered).

### 3c. Red attention-flash **ring** around the status dot  `33479fda3` `d21d68640` `54470b13f` `6c8026f8c` `109980767` **[full-build]**
1. Focus tab A. In an **unfocused** session tab B, drive a state change **Running → {Idle | WaitingForInput | NeedsApproval}** (e.g. let a turn finish, or hit a permission/question).
2. Tab B's status dot grows a **full red ring** (drawn behind the dot: red ring · black stroke · status fill) that **blinks every 600 ms**. Confirm it's a complete **circle**, not a clipped square, and a **thin** rim (~1.5px — the ring is 13px inside a 14px wrap, per `109980767`).
3. Multiple flashing tabs blink **in lockstep**. Switch to tab B → the ring **clears** immediately (visiting = clearing).
4. **Negative cases:** Running → **Done** and Running → **Error** must **not** flash. A change on the **currently-focused** tab never flashes. Archiving/closing a session forgets its last state (a later background restore won't spuriously flash).
✅ Pairs with §11a fixes — a long tool / API-retry must keep the dot **blue (Running)** and **not** flash red.

### 3d. Minimum tab width raised to 130px  `466b09bc7`
1. Open many tabs in Equal-width mode until the strip fills.
✅ Tabs stop shrinking at **130px** (legible) and the strip **scrolls** instead of collapsing to ~48px.

### 3e. Show-X-button + middle-click-close settings honored  `e8050440f`
1. ⚙ Settings → toggle **"Show close (x) button on tabs"** OFF → Save. Every tab's X disappears (Manager already X-less).
2. With X hidden, **middle-click** a normal tab → it closes (manual hook).
3. ⚙ → toggle **"Close tab with middle-mouse click"** OFF → Save. Middle-click no longer closes, whether the X is shown or hidden.
4. Middle-click the pinned **Manager** tab with X hidden → it must **not** close (guarded).
✅ Both default ON (no change for an existing `settings.json`).

### 3f. "Mark Unread" — force the flash ring until visited  `bb4870478` **[full-build]**
**What:** A right-click tab item that *manually* flashes the red ring — **sticky**, and it ignores the automatic "active tab never flashes" rule.
1. Right-click a managed **Claude/Codex** tab → **"Mark Unread"** (flag glyph). It appears only on a managed tab (resolved at flyout-open, like the Copy submenu) — **not** on a shell or the Manager tab.
2. Mark a **background** tab → its red ring flashes (in lockstep with any auto-flashing tabs). It keeps flashing through state changes / re-running / even if it becomes focused — the automatic flash logic **never clears a manual mark**. Switch **to** it → the mark clears.
3. Mark the tab you are **currently on** → it flashes immediately *even though it's focused*; it clears only on a **leave-then-return** (switch away, then back).
✅ Archiving/closing the session also clears it. Marks are session-keyed, so they follow a re-home.

### 3g. Fast tab drag-reorder no longer loses the tab  `a61afb11b` **[full-build]**
1. Grab a tab and **drag-reorder it with a quick flick** (fast drag). Repeat several times, including on a tab that **hosts a live Claude/Codex session**.
✅ The tab and its content must **stay visible** — no blank/vanished tab. (The dragged container's delete/re-insert race is fixed by dropping the AddDelete/Content transitions; only the sibling Reorder slide remains.) The session is never lost.
2. Confirm the acceptable trade-off: opening a new tab / closing a tab now **snaps** instead of fade-sliding — reorder itself still animates smoothly.

---

## 4. Per-tab badge & summary panel (`AgentTabOverlay`)

### 4a. Badge redesign + clickable Autopilot  `2f23e1212` `aa3ceb576` **[full-build]**
1. On a **linked** Claude/Codex tab, hover the top-right badge. Row 1 is now **discrete, individually-tooltipped parts** (status · the folder/copy/pencil action cluster · Autopilot · queued count · link). Hover each part → its own tooltip explains it.
2. The folder / copy / pencil **action buttons are always visible** (no longer hover-only); hover only **dims/brightens** the whole badge (~0.55 ↔ 1.0).
3. **Link** indicator shows **only when *not* linked** — a linked badge shows no `⛓ linked` mark.
4. Row 2 is just `<workdir folder>/<branch>`; with no dir/branch it **collapses** to a single compact row.
5. **Autopilot is a clickable button** — click it to cycle **Off → Semi → Full → Off**, colored gray/amber/green to match the Triage Board. Queue a prompt on an Idle/Waiting session, then click Autopilot to **Full** → it should arm and the queued plan starts (no separate send path; it nudges the scheduler). Confirm clicking it **doesn't** steal keyboard focus from the terminal.

### 4b. Pencil glyph = pencil-with-a-line  `9918a2232` **[full-build]**
✅ The summary-panel toggle button renders as a **pencil with a line** (edit/notes look), not a plain pencil. Folder (📁) and copy icons unchanged.

### 4c. Title row at top of the summary panel  `2f77982bb` **[full-build]**
1. Toggle the summary panel on (pencil). The **very top row** is now the session **title** (bold, brighter), above the times line and the numbered messages.
2. **Rename** the session → the title row updates **live**.
✅ A never-prompted session (no times/content) still shows **no** panel (the title doesn't open a title-only box).

### 4d. "Copy Summary" right-click on the summary panel  `d0bc87d79` **[full-build]**
1. Right-click anywhere on the summary panel (title, times bar, a message run, padding).
2. A **"Copy Summary"** context menu appears → click it → clipboard holds the **FULL** session-end box (id + resume CLI + dir + folder + branch + duration + tasks + messages + files), matching the badge copy-menu's "Summary".
✅ Text selection + Ctrl+C still work (only the right-click menu is overridden). Same content as the copy-menu Summary item (one shared path).

### 4e. File bullets flush-left  `481b4ca27` **[full-build]**
✅ In the summary box, **Files Read / Created / Edited** bullets are `* name` flush-left (no 3-space indent) — aligned with their section headers and the numbered Messages. Same in the Sessions-page detail box.

### 4f. Summary panel drags past its own content, snaps on release (no crash)  `595c5b7cc` `f3c6f68da` **[full-build]**
1. On a tab whose transcript is **short**, drag the panel's resize grip outward.
2. Mid-drag it should visibly **grow leftward/downward past its own content**, reaching the max bands (≤50% pane width / ≤75% height) instead of feeling stuck at fit-content.
3. **Release** → it snaps back to **fit content**, but the dragged **fraction is preserved** — switch to a fuller tab and confirm that tab fills out to the new shared size (size is a GLOBAL setting).
4. **Crash check (`f3c6f68da`):** drag the grip **vigorously** over content that wraps/scrolls — the app must **not** fail-fast with **"Layout cycle detected"** (`0xc000027b` in `Windows.UI.Xaml.dll`). The forced size now pins only the outer panel box (one-way layout), so the measure pass can't cycle.

### 4g. Summary panel no longer empty when the first human message starts with a newline  `338edb774`
1. Find/create a session whose **only** human message is a multi-line paste that **begins with a leading newline** (e.g. a pasted terminal capture).
2. Toggle the summary panel.
✅ The **Messages** list now shows that message (leading newline trimmed) and the times populate — previously the panel was empty until the first tool call. Genuine noise (`\nCaveat:`, whitespace-only) is still filtered.

---

## 5. Terminal search box

### 5a. Search box restyled to the HUD + seated left of & top-aligned with the badge  `956341997` `f7be57990` **[full-build]**
1. Press **Ctrl+Shift+F** on a session tab.
2. The search box adopts the badge look (dark translucent `#CC202020` fill, hairline border, rounded, compact, no drop shadow) and renders **dark-themed** regardless of app theme.
3. It sits **left of the per-tab badge**, reading as one HUD; the slide-in animation, match counter (n/m), prev/next, and regex/case toggles all still work.
4. It **top-aligns** with the badge — both 4px from the top of the pane. Confirm their top edges line up (the box was nudged up 4px, `f7be57990`).
✅ On a non-agent / shell tab the box keeps its default top-right edge. *(Requires a full CascadiaPackage build — adds `TermControl.idl SetSearchBoxRightInset`.)*

### 5b. Ctrl+F opens find AND passes ^F through to the app  `8ad119bc2` **[full-build]**
1. Focus a **shell** tab, press **Ctrl+F** → the search box opens **and** the shell receives `^F` (readline forward-char moves the cursor). On a Claude tab, its TUI also sees `^F`.
2. With the **search box focused**, Ctrl+F routes to the box (no passthrough).
3. **Ctrl+Shift+F** unchanged (opens search, no passthrough). Read-only connection: find opens, nothing sent.
✅ Requires `defaults.json` + the rebuilt projection — a full deploy.

---

## 6. Triage Board & Manager tab

### 6a. Card title band painted the working-directory color  `c976f8e7e`
1. On the board, each managed card's **title sits in a colored band** = the session's **per-dir color** (the same color its tab wears). Top corners rounded (4px), bottom a straight square edge; body below stays neutral gray.
2. Title text flips **black on light bands / white on dark** for contrast (e.g. `#528BFF` → black text, `#BE5046` → white).
3. Two sessions in the **same dir** share the band color and visually cluster across state columns.
✅ External-census cards stay **plain** (no band).

### 6b. Full-title tooltip on the title band; `model·effort·hb` line removed  `c4fdb9636` `6689a04f0`
1. **Removed (`6689a04f0`):** the board card **no longer renders** the `model · effort · bg · hb:<status>` adornment line — those facts already read on the per-tab badge / other rows, and the line was ellipsis-trimmed on a narrow card anyway. Confirm a managed card's body is now just **working dir · timing · autopilot** (no model/effort/hb row). The presence heartbeat is still an observer fact, just not shown here.
2. Make a state column narrow so the card **title** ellipsizes → hover the **title band** → tooltip shows the **full** title (`c4fdb9636`).

### 6c. Sortable Triage Board + live cross-window sync  `7a89937e6` `b77887f37`
1. The board header has a **sort toggle** next to the scope toggle. Click it to cycle **MOST ACTIVE (default) → NEWEST → OLDEST → A–Z** — it reorders cards **within each state column** (and the External column).
2. It's a **separate global** from the tree's sort (`boardSort` vs `treeSort`) — changing one doesn't move the other.
3. **Cross-window live sync:** open **two windows**. Change the board sort (or any cog global) in window A → window B updates **immediately** (not just on next launch). Same for the tree sort.
✅ Default is **MOST ACTIVE** (the old "least active" mode was removed). Persists across restart.

### 6d. Hover "⋯" more-button + "Jump to Tab"; tree defaults to LOCAL  `d26ef2ca3`
1. Hover any board card (managed or External) → a three-dot **⋯ More** button fades in top-right (140 ms). Click it → opens the **same menu** as right-click.
2. Right-click a managed card / tree row → **"Jump to Tab"** is the first item → switches to the live tab (cross-window).
3. Leave the Explorer Tree in **EXTERNAL**, close & reopen the window → it reopens in **LOCAL**, never EXTERNAL (LOCAL/GLOBAL still persist).

### 6e. Board-card click syncs the Explorer Tree scope  `64b4b8bd5`
1. In the tree's **GLOBAL** scope, click a managed card hosted by **another** window → tree stays GLOBAL (card's home lens). Click a card hosted by **this** window → tree swaps to **LOCAL**. Click an **External** card → tree swaps to **EXTERNAL**.
✅ All three Linked-Lenses regions agree on the lens after the click (previously a managed-card click left the scope untouched).

### 6f. Scroll the selected card into view on returning to the Manager  `802201bfb`
1. Switch among session tabs (the per-tab→Manager sync keeps re-selecting the matching card while the Manager is hidden), enough that the selected card would be scrolled off — a tall column, or a state change moved it to another column.
2. Switch **to** the Manager tab → the selected board card **and** its tree row are scrolled into view.
✅ It does **not** yank the view on every refresh — only on entering the Manager tab (it won't fight your own scrolling while you sit there).

### 6g. Manager pane no longer drags the window on gap-clicks  `551806fbd` (+ window fix §12)
1. On the Manager tab, **left-drag** on an empty gap (between cards, the tip of a text run, a thin border).
✅ The window must **not** move and **right-click** must **not** open the system/caption menu — the pane is now opaque so clicks land inert instead of falling through to the titlebar. (Clicks on an actual card still open that card's menu.)

### 6h. "Create & Launch" a missing working dir  `76e587033`
1. In the Launch box type an **absolute** path that doesn't exist yet (e.g. `K:\source\brand-new-folder`).
2. The underline turns **amber** and the button reads **"Create & Launch Claude"** (or **"…Codex"** in Codex mode).
3. Click it → the folder (and missing parents) is created and the session launches there.
4. **Negative:** a bare relative token (`agent`) or a malformed path stays **red + disabled** (we never create a folder at a surprising cwd). A non-creatable path (perm denied / bad drive) shows a "Couldn't create folder" dialog and aborts (dir not pushed to recent-dirs).

---

## 7. Launch box & path-picker

### 7a. Subfolder filtering by typed leaf, exact match first  `7752b7700`
1. Type `K:\source\Agent` → the picker lists `K:\source\` folders whose name **starts with "Agent"**, narrowing as you type; an **exact** name match is hoisted to the **top**.
2. `K:\source\` (trailing separator) → lists **all** of `K:\source` unfiltered.
3. Click a folder → it appends a separator and re-lists that folder's children (drill-in).

### 7b. An exact dir also lists its own contents below its siblings  `b39239285`
1. Type `K:\source` (existing dir, **no** trailing separator) → **two** sections: **MATCHES FOR "source" IN K:\** (the `source*` siblings) above, and **SUBFOLDERS OF K:\source** (everything inside) below.
✅ The single-section cases (trailing separator; partial non-dir leaf) are unchanged.

### 7c. Fuzzy-match recent dirs when typing a bare token  `f9a20fbb0`
1. Type a bare token with **no path separator / drive / session-id** (e.g. `agent`).
2. The **RECENT** section becomes **RECENT MATCHES FOR "agent"** — a fuzzy (Levenshtein) search over recent/known dirs, closest first, non-matches dropped (matches mid-path segments too, e.g. `...\Agentmaster`). No useless "(path not found)" subfolder section.
3. A ≤2-char token requires an exact substring (so it doesn't match everything). No matches → "No recent directory matches" hint.

### 7d. Validation clears on a programmatic cwd set  `d7fc2bf0a` `95721d120`
1. Type a non-existent folder → red underline, **Launch disabled**.
2. **Switch terminal tabs** (the selection sync drops the focused session's working dir into the box).
✅ The underline + button **repaint correctly** for the new path (Launch enabled) — they no longer stay frozen red/disabled. Repeat after a **resume/fork** (box clears) and after a **settings re-seed** (cog Save with a `defaultLaunchDir`) — all re-validate.

---

## 8. Flight Plan (compose row)

### 8a. Focus returns to the compose box after queue/send  `cefd381fa`
1. Queue a prompt (✉) or **Send now** (`!`, includes the confirm dialog).
✅ Keyboard focus lands back in the **compose textarea** so you can fire several in a row without re-clicking. The **eye** (jump-to-tab) button intentionally does **not** pull focus back.

### 8b. Up/Down prompt history  `cefd381fa` `a25a8e8f8` `ca4e4d124` `3fe962b24`
1. Send a few prompts on a session. Put the caret on the **first visual row** of the compose box and press **Up** → recalls the previous sent prompt (newest-first, consecutive dups collapsed; both queued+injected and typed-into-terminal count).
2. While browsing, **Up/Down navigate freely**; **Down** off the newest restores your **pre-history draft**.
3. The trigger is the first **visual** row (with word-wrap, Up first moves the caret up wrapped rows, then recalls at the true top).
4. **Esc** while browsing history cancels back to the draft.
5. **Modified arrows pass through:** Shift+Up extends selection, Ctrl/Alt+Arrow are editor motions — **not** hijacked as history.
6. History is **per-session** — resets on selecting a different session / clearing / lens reseed.
✅ **Plain Enter is a newline; Shift+Enter does NOT send** (that chord was reverted — the `!` button sends). Template **Apply / Apply-to-dir** also return focus to the compose box.

---

## 9. Tab right-click context menu

### 9a. "New Session Here" + "Fork session" rename  `a6e7b3fc0`
1. Right-click a tab → a **"New Session Here"** item sits **directly above "Restart session"** → spawns a managed session in **this tab's working dir** (a managed Claude/Codex tab uses its recorded cwd; a shell tab uses its live OSC cwd; else `%USERPROFILE%`), of the **same agent kind** the tab hosts (Codex for a codex tab, else Claude).
2. The old **"Duplicate session"** item now reads **"Fork session"** (label-only; behavior unchanged), consistent in both the tab menu and the pane command-bar.
3. Tail order: Find → New Session Here → Restart session → Fork session → — → Close. New Session Here is **disabled** on the Manager/Settings tabs.

### 9b. "New Session Here" / "Fork session" land next to the clicked tab  `74682e218`
1. Right-click a tab that is **not** the last one → **New Session Here** (and **Fork session**) → the new tab opens **immediately to the right of the clicked tab**, not at the end of the strip.
2. **Latent-bug check:** invoke **Fork session** from a **non-focused** tab's menu → it forks **that** tab (not the focused one).
✅ Keyboard/command-palette DuplicateTab (null sender) still forks the **focused** tab at the **end** (default placement).

### 9c. "Copy >" submenu on the tab menu  `451061a13` **[full-build]**
1. Right-click a **linked** Claude/Codex tab → a **"Copy >"** submenu sits **directly below "Rename Tab (F2)"** with: Session Id · Copy Path · Copy Branch Name · Claude Launch CLI · Codex Launch CLI · Summary · Transcript.
2. Each copies the right thing (it routes through the **same** `CopySessionField` action as the badge copy-menu and the board/tree Copy submenu — verify a couple match the badge's output byte-for-byte, e.g. Summary).
✅ The submenu appears only on a linked tab (resolved at flyout-open) — **not** on a plain shell or the pinned Manager tab.

### 9d. "Close tabs to the left" + Manager-tab protection  `f022c1f87` `751ce6922`
1. Right-click a tab → **Close >** submenu now reads **Close tabs to the left · to the right · other tabs · this pane**.
2. **Close other tabs** must **never** close the pinned Manager tab (it's filtered from every bulk close).
3. From the **first movable tab** (index 1), **Close tabs to the left** is a harmless no-op (only the Manager is to the left, and it's skipped).
4. **Enablement:** when the only tab that would be affected is the Manager, **Close tabs to the left** and **Close other tabs** are **disabled** (greyed), not lit-but-dead.

### 9e. Batch close dialog — Delete All / Archive All / Cancel All  `87f3e01b3`
1. With ≥1 **managed session** in the affected set, trigger a bulk close (Close tabs to the right/left/other, or close the window).
2. **One** consolidated dialog appears: **🗑 Delete All · Archive All · Cancel All** (not a train of per-tab prompts).
3. **Archive All** (safe default-ish, Secondary) → every session shut down & kept restorable from Archived. **Delete All** (Primary) → drops the records but **keeps the `.jsonl`** (still in the Sessions browser). **Cancel All** (Close) → aborts the whole close.
✅ A batch of **only shell tabs** skips the agent dialog (upstream's generic confirm, gated on ConfirmOnClose).

### 9f. Rename disabled on the pinned Manager tab  `c12539e78`
1. On the **Manager** tab try all three rename gestures: header **double-tap**, context-menu **"Rename Tab"**, and the **openTabRenamer** action/keybind.
✅ None open the rename editor; the **"Rename Tab"** menu item is **greyed** (like Close/Move). Regular tabs rename normally.

---

## 10. Sessions browser (full-window page)

### 10a. Double-click an **open** session jumps to its tab  `5b18a5962`
1. Double-click a row that is **live** (open in a tab in any window).
✅ It **jumps straight to that tab** (cross-window — brings the hosting window forward), instead of resolving the continuation chain and possibly landing on a different id / a fresh tab.

### 10b. Double-click a **closed** session → Resume / Fork / Cancel  `4a699a5a3`
1. Double-click a **not-live** row.
✅ A 3-button dialog appears — **Resume** (primary/default) / **Fork** (secondary) / **Cancel**. Resume → `_ResumeSessionFromDisk`; Fork → `_ForkSessionFromDisk`; Cancel → nothing. (Previously it silently resumed.) A never-prompted row's Fork uses a smart name, not "(no prompt yet) (fork)".

### 10c. Bulk background-open from the row right-click menu  `2f23e1212`
1. Right-click a row → **Jump / Resume here / Fork here / Open New Session Here** open the tab in the **background** and **keep the Sessions list up**, so you can open several rows in a row without focus jumping away each time. (Each background tab's claude starts lazily when first focused.)
✅ The **detail-pane** buttons still open in the **foreground** (single open lands on its tab).

### 10d. Title + Directory cells underlined in the working-dir color  `fb9258f25` `87fe537df`
✅ Each row's **Title** and **Directory** cells carry a colored bottom underline = the session's per-dir color (same as its tab/chip). Left-aligned so it hugs the text; archived rows are dimmer (alpha baked into the brush, not double-dimmed).

### 10e. Prefetch adjacent summaries + loading spinner  `bd5fcf837`
1. Open the Sessions page, select a row, then Up/Down/click through neighbors.
✅ Navigation feels **instant** (neighbors are warmed ahead). On a **cold** summary a **ProgressRing + "Loading summary…"** shows, replaced by the box when it lands (re-renders only if that row is still selected).

### 10f. Time-window range popup dismisses on button-leave  `373b66bb3`
1. Hover the **[1mo]** time-window button → the From/To range popup opens.
2. Move the pointer **away** from the button **without** entering the popup card → after ~250 ms the popup **dismisses** (hover-intent close).
3. Slide button → card → back → it stays open across the gap.

---

## 11. Lifecycle & state

### 11a. State no longer flaps idle↔running on a long tool / API-retry  `7cfa86a0b` `faadf3ee3`
1. Run a session through a **long tool** (a multi-minute `Bash`/build) and, if you can reproduce one, a **"No response from API · Retrying"** backoff.
✅ The tab dot stays **blue (Running)** throughout — it must **not** oscillate Running↔Waiting (gray) every ~5 s, and must **not** trigger the §3c red flash on those false edges. (A genuinely finished no-op turn — no writes, no "busy" heartbeat for ~30 s — still releases to Waiting.) If you can read `scanner.log`, confirm you don't see alternating `[recon-stop-idle]` / `[recon-run]` for one session.

### 11b. Trash / permanent-remove (record-only) beside Archive, everywhere  `4ff0b2d6e`
**What:** A new **trash** action (placed left of Archive) that **drops** a session's Agentmaster record (registry + `sessions.json` + strips it from saved window records) but **keeps the conversation `.jsonl` on disk** — so the session still shows in the Sessions browser, resumable.
1. **Close-session dialog:** close a session's tab → the dialog is now **3 buttons** — **🗑 Delete** (leftmost) · **Archive** · **Cancel** (default). Delete drops the record + closes the tab; Archive = prior behavior.
2. **Manager session menu** and **Flight-Plan message menu:** a **"Delete permanently…"** item (trash icon) beside Archive → confirm dialog (copy spells out "the conversation file on disk is kept; still in Sessions").
3. **Archive page:** an icon-only **trash** left of "Restore here" (session detail) and left of "Reopen its window" (saved-window detail), plus a **"Delete selected"** bulk action left of "Restore selected" (acts on checked ∩ visible, in table order). Each confirms first.
4. **Verify non-destructive:** after Delete, the session is gone from Board/Archive but **still appears in the Sessions browser** and can be resumed there. The `.jsonl` is never deleted.
5. **Negative:** Delete on a session **still running but hosted in another window** is **refused** (the observer would re-create it).
✅ The Sessions browser itself has **no** trash (it lists on-disk transcripts this path never deletes).

### 11c. Managed agents run inside an interactive pwsh — quitting drops to a live prompt  `6bac08723` **[full-build]**
**What:** Every managed Claude/Codex session's ConPTY **root is now an interactive PowerShell** that execs the agent with `-NoExit`. When the agent quits, you land at a live `PS <cwd>>` prompt at the session's working dir instead of a dead pane.
1. Launch a managed **Claude** session; confirm it runs and binds normally — board card + per-tab badge **linked** + correct state (the observer correlates `claude.exe` as a *child* of pwsh, so nothing changes there).
2. Inside it, **quit the agent** — `/exit`, Ctrl+C twice, or let it crash.
✅ Instead of the old **"[process exited] — press Enter to restart"** dead pane (which used to mis-replay the launch commandline at the wrong cwd), the tab stays **alive** at a usable **`PS <session-dir>>`** prompt. Run a shell command there to confirm it's live and rooted at the right directory.
3. Repeat for a managed **Codex** session (quit → prompt).
4. **Restore/restart** a session (Archive → Restore here, or restart-connection) → it keeps the quit-to-prompt behavior (both restart branches are wired), not a dead pane.
5. Closing the **tab** still archives/closes normally — close releases the connection → pwsh exits (the close-on-exit suppression now keys on **pwsh** exiting, not the agent quitting).
6. Optional: `hooks.log` (in the active profile) shows `[engine] pwsh host: ...` at engine init; the host resolves to PowerShell 7 (`pwsh.exe`) on PATH, else Windows PowerShell, by **full path**.

---

## 12. Window — caption/drag hit-test restricted to the titlebar  `f018a6a96`
1. On **any** content area (Manager gaps, the 6px margins between triage cards, a TextBlock's glyph gaps, the body of a page) **left-drag**.
✅ The window does **not** move and **right-click** does **not** open the system menu — only the actual **titlebar band** drags. Real titlebar dragging and resize borders are unaffected. This is the global counterpart to §6g.

---

## 13. Profiles — silent auto-select on first launch  `7f414c8c1`
1. On an install's **first launch** (a profile not yet chosen).
✅ It **does not** show the Production/Development/Browse… TaskDialog any more — it **silently** picks and persists the per-identity default (release → Production `~/.agentmaster`; dev → Development `~/.agentmaster-dev`), seeds Terminal settings, and continues. A `-Embedding` (defterm) activation takes the same silent path.
2. The picker still exists, reachable only from **⚙ Settings → Change profile folder…** (applies on restart). `AGENTMASTER_PROFILE` override still wins.

---

## 14. Build speed (process, not a UI feature)  `dbfda4524`
1. Run `tools/Build-Agentmaster.ps1 -NoRestore` for a 1-TU change.
✅ Build drops to **~20–50 s** (was ~204 s); a perf summary shows **`GenerateAppxSymbolPackage` absent**. The loose layout still registers and runs; PDBs still emit next to the binaries (local debugging unaffected). Opt back in with `-WithSymbolPackage`. *(If a build sits ~2.5 min on one node after the `.msix`, you forgot the flag on a raw `msbuild` call.)*

---

## 15. Tooltip & copy polish (verify wording reads clearly on hover)
`e6c0cc687` (Manager top-bar) · `9fb273674` (Sessions page) · `dac0b16c8` `91a075d4b` (tab-menu rename / move) · `ba5775b79` (agent tooltips & menu text)

Spot-check that each hover tooltip now leads with the action and is accurate, and that the two controls that had **none** now have one:
- **Manager top bar:** Claude/Codex toggle, **workdir box**, **Launch button (new)**, Fork, Reopen Windows, **Settings cog**, **Pause Autopilot (new)**, Archived, Sessions.
- **Sessions page:** search box ("every word must match…"), 👤/🤖 (your prompts vs Claude's replies+tools, triggers the slower scan), 📁/📄 (instant, no scan), **(F)** ("agmst" matches "agentmaster"), time-window, column headers ("click to sort"), color chip (color = working-dir color + open/archived/on-disk + live heartbeat + fork lineage), Branch, Created/Active (absolute datetime), Resume/Fork, **Jump to tab** + **Open New Session Here** (had none).
- **Tab menu:** **Rename Tab** ("Shift+Enter commits… Enter inserts a line break (swap in Settings → Tab rename: commit with)"), **Move to new window** ("Move this tab to a new window"). The Manager Explorer/board **Rename…** and the cog's **Tab rename: commit with** tooltips read consistently.
- **General:** Claude heartbeat wording; Codex state tooltips note they're **derived from rollout transcripts**; PID-underline tooltip; consistent **"Codex"/"Claude"** capitalization in Adopt / Copy-Id; Archive tooltips on the archive actions.

---

## Excluded — docs/README only (no QA)
Doc fact-check sweep & README/CLAUDE.md edits, no behavior change:
`960b1ffeb` `ad7d49a7d` `9e26fef41` `0502baf34` `0e953657a` `bbabd67da` `d97373274` `16ede20dc` `6a4e64fad` `567b4cccc` `0d4fefbc9` `cb379533f` `e9eaae004` `dce2398ed` `7cfbce5b4` `41912d5f8` `8f1c33992` `0122c3d4f` `1137500a5` `fe8661057` `bec0c4d2a` `15b04a0bf`.

---

### One-paragraph QA order suggestion
Deploy a **full** build → smoke-test the **regression floor**: Manager opens + a session launches + binds + Autopilot sends, and **quitting the agent drops to a `PS>` prompt** (§11c — it touches every launch) → then walk **§3–§10** with two windows and a Claude+Codex+pwsh mix open (don't miss the new §3f Mark-Unread, §3g fast-drag, and §4f drag-crash check) → exercise **§11 lifecycle** (archive/delete round-trips, verify the `.jsonl` survives) → finish with the **§1 updater** (Settings path on dev; full upgrade only if you can stage an older→newer release) and **§13 first-launch profile** (needs a clean profile state).
