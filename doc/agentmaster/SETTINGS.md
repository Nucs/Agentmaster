# Settings

Every setting is a field of `AppSettings` (`AgentMaster/SessionModels.h`), stored inside the engine's
`{version, settings:{…}}` envelope in `<profile>\settings.json`. Every default reproduces the behavior
the app has without the setting, so a missing file or field changes nothing. The settings are loaded at
engine init (`SetSettings`) and saved by `SetSettingsHandler`.

The ⚙ on the Agent Manager toolbar opens the editor: an in-content modal overlay over the page (a
dimmed `Grid` over `_root`; a `ContentDialog`'s text boxes get no keys under XAML Islands). While it is
open, the hovered control's tooltip renders in a fixed panel left of the card (`AgentLocalTooltip`).

## The cog's tabs

| Tab | What it holds (the tab's own description) |
|---|---|
| Sessions | How new Claude sessions launch: permissions, model, environment variables, the Launch box's directory history. |
| Presets | Named ways to start a Claude session on another provider, its models, limits and arguments ([HARNESSES.md](HARNESSES.md)). |
| Prompt Queue Runner | The runner defaults stamped onto every new session, and how a sent prompt treats a draft already in the input box. |
| Behavior | Close confirms, cleaning up orphaned sessions, the Waiting-for-you "unread" timeout, GPU or CPU rendering. |
| Commands | The /handover family ([COMMANDS.md](COMMANDS.md) §6). A **Reset** restores this tab's shipped defaults (form only: Save commits, Cancel discards). |
| Notifications | Which state changes notify, the focused-tab skip, the sound. |
| Messaging | The Telegram bot ([MESSAGING.md](MESSAGING.md) §21). |
| Tabs & Overlay | Close affordances, tab size, the rename key and title naming, the favorite marker, tab colors, the status-flash color, overlay opacity. |
| Claude | Which native `claude.exe`, how long Claude keeps session history. |
| About | Version and build, updates, the active profile folder, Debug Mode, uninstall. |

- GLOBAL settings apply live in every window on Save through the settings broadcast; per-window state
  lives in the window's lens (`windows/<id>.json`), not here.
- Settings written outside the cog (below) use a freshest-disk read-modify-write of their own key, and
  both cog-Save paths preserve them from disk.

## Launching Claude sessions

- `skipPermissions` (true): the spawn's `--dangerously-skip-permissions`. Off drops the flag and pins
  `permissions.defaultMode:"default"` in the hooks-settings file.
- `trustWorkspaceOnLaunch` (true): before a launch, seed `~/.claude.json`
  `projects.<git-root-or-dir>.hasTrustDialogAccepted = true` so claude's workspace-trust modal never
  parks a tab (`EnsureClaudeWorkspaceTrusted`, called from the three launch seams through
  `PrepareManagedClaudeWorkspace`, ⚠ never from a spec builder: `BuildClaudeSpawn` /
  `BuildClaudeRestartSpec` stay pure). The write is a surgical splice (`SpliceWorkspaceTrust`): only when
  the flag is missing or false, never on a config it cannot parse, re-validated, atomic, and under
  claude's own lock (`~/.claude.json.lock`, skipped while claude holds it). Logged `[trust]`. Off never
  touches `~/.claude.json`. HOOKS.md *Workspace trust*.
- `model` (empty): the default model (`/model <v>`).
- `launchModels` (`Fable | fable` · `Opus | opus` · `Sonnet | sonnet`): `Display name | model-id` lines;
  the ids are Claude Code's latest-model aliases. Every Open New Session Here and Fork session menu is a
  submenu: **Default** (the `model` setting), **`Specify…`**, one item per model (`--model <id>` for that
  one launch; a fork's pick rides its command line), then the presets. Resume follows `model`. A stored
  list equal to a retired shipped default is upgraded on load (`LaunchModelsAreSupersededDefault`); an
  edited or empty list stays. The editor is live-lexed (`LexLaunchModelsText`: border color + a status
  line; at most 32 models, `kMaxLaunchModels`). A Codex row keeps the plain items.
- **The "Specify a model" prompt** (`AgentBuildSpecifyModelCard`, `AgentModelPrompt.h`; hosted by
  `AgentManagerContent::_PromptForModel` and `TerminalPage::_PromptForModel`, which also serves the tab
  menu): an editable ComboBox listing the configured models, then the model list `model-list.json`
  (every id specified here, newest first, cap 100; `RememberSpecifiedModel`; merged by
  `MergeModelIdGroups`), pre-filled with the last committed id (the file's `last` key,
  `LoadLastSpecifiedModel`, the same in every window). Links to Anthropic's model overview and the Codex
  `models.json`. **Fetch list** replaces the list with Anthropic's `/v1/models` (it needs
  `ANTHROPIC_API_KEY` or `ANTHROPIC_AUTH_TOKEN`; a subscription login gets 401) and the Codex catalog,
  Anthropic's ids first (`ModelCatalog.h`, `HttpsGet`); `last` survives a Fetch. Enter and Esc act on the
  key-up; Cancel, Esc and a backdrop click launch nothing; a blank commit is refused. ⚠ The ComboBox
  autocompletes its text on focus to the first id that starts with it, so the pre-filled id is listed
  ahead of longer ids (`PlaceSeedAheadOfLongerIds`).
- `modelFamilies`: the model families the `model` adornment shortens names by (`ShortModelName`); it
  replaces the built-in list; blank or garbage falls back to the built-ins.
- `includeCoAuthoredBy` (true).
- **Environment variables** ([ENV_VARS.md](ENV_VARS.md)): a Global / Per-directory editor (one
  `NAME=VALUE` per line, live-lexed by `LexEnvText`). The global set is `env`; per-folder overrides live
  in `<profile>\dir-env.json` (keyed by `NormDirKey`). They merge at spawn, the folder winning
  (`ResolveSessionEnv`), for Claude and Codex alike; `CCMGR_*` names are dropped. `envDefaultsVersion`
  records which shipped defaults were seeded into `env` once (v1: `CLAUDE_CODE_MAX_RETRIES=50000`); a
  user's edit or deletion sticks.
- `defaultLaunchDir` (empty = `%USERPROFILE%`): seeds the Launch box.
- `recentDirsLimit` (10): the folder picker's RECENT rows.
- `carryDraftOnFork` (true): a fork carries the source's unsent draft into its own box (PENDING_INPUT.md
  §11).
- `resumeApiErrorTurns` (true): append the one-line rewind so a resumed session continues a turn that an
  API error ended (HOOKS.md *API-error turns*).
- **Hidden sessions:** Reset hidden sessions clears `hiddenSessionIds` (the Sessions page's Hide from
  list).

## The session database

- `sessionDbMode` (Off / Shadow / On), `sessionDbRefreshSeconds` (5, 1–60), `sessionDbThreads` (4),
  `sessionDbMemoryMb` (1024): the SESSION DATABASE section ([SESSIONS_DB.md](SESSIONS_DB.md)). Shadow
  builds the database and runs the Sessions page's content search on it beside the regular search,
  logging `[sessdb-diff]`; On makes the page use it and gives the AI search its tools from the next app
  start.

## Prompt Queue Runner and delivery

- Stamped onto every new session: `defaultAutorunnerMode` (`Full` = Auto), `maxAutoSends` (100),
  `stopOnError` (true), `pauseOnHumanInput` (true; ⚠ it has no feeder, so it never pauses anything).
- The four switches every send path reads (DELIVERY.md, PENDING_INPUT.md):
  - `preserveDraftOnSend` (true): the draft swap — an unsent draft is taken out of the box around a
    send and put back (§9).
  - `draftSwapUseCtrlS` (true): the swap stashes with Claude's Ctrl+S (a whole-box toggle) instead of
    the Ctrl+U kill ring.
  - `verifySendBeforeSubmit` (true): verified placement — the box is read back before the submitting CR,
    and a foreign read-back refuses the CR even with the swap off; off restores the one-write paste + CR.
  - `restoreDraftOnResume` (true): a reopened session's remembered draft is typed back into its new box
    (fill, never send; verified, §10).

## Behavior and session state

- `confirmBeforeKill` (true): the Close confirm ("Confirm before closing").
- `reapOrphanedSessions` (true): the Fleet Observer ends a session of ours whose ConPTY host died without
  its graceful shutdown (OBSERVER.md §13a); off = recognized as an orphan, left running.
- `uiStallAutoRestart` (true): when every window's UI lane is dead for 3+ minutes while the Win32 pump is
  provably alive and repeated rescues failed, the app restarts itself (the workspace persists); off = the
  watchdog only detects, logs and attempts rescues.
- `waitingForYouTimeoutMinutes` (4320 = 3 days; a 1 min – 7 d slider, a free-text box such as `12h5m`
  that may exceed 7 d, and "Never"): a WaitingForInput session demotes to Idle only once the timeout
  elapsed **and** its tab was visited since the last turn (`ShouldDecayWaitingToIdle`); an unread or
  Mark-Unread session keeps waiting.
- `serverCacheMinutes` (5): Claude's server-side prompt-cache lifetime, for the board's ⚡ glyph and the
  tab's spark crown. Both read `ServerCacheStillWarm`: the session is at rest (never Running) and real
  API-turn evidence of this conversation is younger than the lifetime — the transcript's parent-line API
  activity (`convApiActivityUnixMs`) and the hook stamp `lastTurnUnixMs` (`IsApiTurnEvidence`: never
  `SessionStart` / `SessionEnd`, a synthesized quiescent Stop, `SubagentStop` or the external-work
  synth). Claude only.
- **Rendering** ([RENDERING.md](RENDERING.md)):
  - `softwareRendering` (false): every terminal of every window draws on WARP (the CPU), so no per-tab
    GPU device exists. Applied live by `_ApplySoftwareRenderingSetting` →
    `TermControl::SetSoftwareRenderingOverride` + `ReapplySoftwareRendering()` on every pane (the other
    windows through the broadcast; new terminals read it in `ControlCore::Initialize`); OR'ed with
    Terminal's `rendering.software`. A frame costs ~1–6 ms of CPU; each shown terminal ~19 MB. Logged
    `[render]`.
  - `pauseHiddenTerminals` (true): a terminal out of its window's visual tree, or in a minimized window,
    draws no frames and repaints once when shown.
  - `releaseHiddenTerminalDevices` (true) after `releaseHiddenTerminalDevicesAfterSeconds` (60; 0 = at
    once; blank = 60): a hidden terminal releases its device, swap chain and glyph cache, rebuilt on
    return (~0.1 s blank). Applied live by `_ApplyHiddenRenderingSetting` →
    `TermControl::ConfigureHiddenRenderingPolicy` + `ReapplyHiddenRenderingPolicy`. Logged
    `[render] hidden tabs …`.

## Commands ([COMMANDS.md](COMMANDS.md) §6)

- Per command (`commandHandover*`, `commandHandoverHere*`, `commandHandoverStandby*`): `…Name`
  (`handover` / `handover-here` / `handover-standby`; normalized, never colliding,
  `ResolveCommandNameTriple`) and `…Enabled` (true) — applied at the next start; `…SuccessorModel`
  (empty = Default; a list of Default + the launch models, an unlisted id shown `(custom) <id>`; a
  leading `[model]` or `[title]` in the typed command overrides it for one handover).
- Family-wide, applied at the next handover: `commandHandoverTitleFindRegex` /
  `commandHandoverTitleReplace` (rewrite the successor title from the origin's; seeded with the rule that
  reproduces `<origin> (handover)` and its numbering), `commandHandoverWritePath` (where briefings are
  written: the session scratchpad by default, or `./`, `./docs`, `./handovers`, any folder; re-rendered
  into the live definition files on Save), `commandHandoverDeleteFileAfterLaunch` (false; forced off while
  the location is the scratchpad, `CommandHandoverDeleteEffective`) with
  `commandHandoverDeleteDeadlineMinutes` (1440; 0..30 days; 0 = only if the successor already started;
  blank = 1440).
- `commandHandoverFileMatchRegex` (`HANDOVER\-`; blank or invalid = the built-in "name contains handover"
  hint) — applied at the next start.
- Engine-owned markers (never a preference): `command*MaterializedName`.
- The tab states each definition file (up to date / managed / edited by you — left alone / not
  installed) and offers **Reinstall definition files…** (confirmed). Every pattern goes through
  `RegexUtil.h`; a live status line flags an invalid one.

## Notifications ([NOTIFICATIONS.md](NOTIFICATIONS.md))

- `notificationsEnabled` (true) and `notifyOnWaiting` / `notifyOnNeedsApproval` / `notifyOnIdle` /
  `notifyOnDone` / `notifyOnError` (true each): a Windows toast when a managed session leaves Running for
  that state. `notifySuppressFocused` (true): no toast for the focused tab of the active window.
  `notifySound` (true; off adds `<audio silent>`). The per-state boxes grey out while the master is off.
- `taskbarUnreadBadge` (true): the taskbar button's unread count (§8).
- `taskbarUnreadThumbnailFrame` (true): the flash-colored frame on a window's taskbar / Alt+Tab preview
  while it holds an unread tab (§9).

## Messaging ([MESSAGING.md](MESSAGING.md) §21)

- `messagingEnabled` (false) runs the Telegram bot; the token lives in Windows Credential Manager
  (`Agentmaster:messaging:telegram:<profile leaf>`), never in a file; `messagingTelegramOwnerId` is the
  user's id.
- `messagingIdleGraceMinutes` (10): an idle, unpinned session's topic is deleted after this.
- `messagingSearchThinking` (`high`): the Search topic agent's thinking level.
- `messagingVerbose` (false): new topics show the whole session (true) or the conversation only; a topic
  overrides it (`/verbose`).
- `messagingRichMessages` (true): Claude's Markdown goes as Telegram rich messages.
- `messagingAllScreenLevel` (`full`): 📜 All Screen's default transcript level (`minimal` / `compact` / `default` / `full` /
  `max`): what a bare `/allscreen` draws and the level the card's level picker marks with ⭐; an empty or unknown word reads
  as `full`. Applied live (MESSAGING.md §21.39).
- `messagingCardScreenshots` (true): every session's sticky card carries a screenshot of its terminal, read again at each card
  update (a changed screen is uploaded with the card's own edit, an unchanged one reused by `file_id`, a parked tab keeps its
  last picture); off = text-only cards. Needs rich messages. Costs upload bandwidth (~0.1–0.6 MB a new picture; a running card
  updates every ~20 s). Applied live (MESSAGING.md §21.42).
- File links: `messagingFileLinksMode` (`off` / home network / through `cc.sysd.cc`),
  `messagingFileLinkStation`, `messagingFileLinkPort`, `messagingFileLinkLanBase`,
  `messagingFileLinkHostedBase` (empty = `https://cc.sysd.cc`), `messagingFileLinksExclude` (folders
  never linked; secret-looking files never are).
- Voice notes (MESSAGING.md §21.43, [VOICE.md](VOICE.md) §9): `messagingVoiceEnabled` (true: a voice note in a session's
  topic is transcribed and read like the text it says), `voiceEngine` (`worker` — a local voice worker process — or `openai`
  — an OpenAI-compatible endpoint, which receives the audio — or `off`), `voiceWorkerCommand` (empty = the bundled worker
  when the install carries one), `voiceEndpoint` / `voiceModel` (empty = OpenAI's URL / `whisper-1`; the key in Credential
  Manager, `Agentmaster:voice:api-key:<profile leaf>`), `voiceLanguage` (`en`; empty = the engine detects it),
  `voiceUseSessionContext` (true: the folder's name and CLAUDE.md join the vocabulary), `voiceDictionary` (the owner's
  dictionary: `Word` or `spoken, forms => Word` per line), `voiceWorkerIdleMinutes` (10, 0–1440), `voiceEchoTranscript`
  (true: the transcript is posted under the note first), `voiceCorrectionModel` (`claude-sonnet-5-5`: one hidden `claude -p`
  per note corrects what the engines misheard, with the glossary and every engine's hearing, VOICE.md §5.5; the cog's AI
  correction offers `claude-haiku-5-5` and `off`; another model id is passed to `--model` as is; each note is one model call
  on the PC's Claude Code login, and its text reaches Anthropic). Applied at the next note.

## Tabs & Overlay

- **The strip:** `showTabCloseButton` (true), `closeTabOnMiddleClick` (true), `showTabIcon` (false: tab
  icons hidden, `IconStyle::Hidden`), the tab size (`tabWidthMode`, `tabMinWidth`, `tabMaxWidth`,
  `tabTitleFontSize`, `tabTitleFontWeight`, `tabMinHeight`: [TAB_STRIP.md](TAB_STRIP.md) *Size*).
- `tabRenameCommitMode` (`ClickAwayOrShiftEnter`, or `Enter`): the rename box's commit key.
- **Title naming** (how an untitled session's default title derives from its folder; applied by
  `DeriveSessionTitle` at the next launch, adopt or fork-from-disk of an untitled session; an existing or
  renamed title never re-derives, Rule #11):
  - `tabTitleNaming`: Last word in folder name (default; `Potato.Tomato.SlangGang` → `SlangGang`) ·
    Folder name as is · Two folder names (`repos/Foo`) · Capital letters (`PotaTo.Tomato.Slang` → `PTTS`)
    · Branch name · Branch name / folder name · Branch name / two folder names (the folder's live git
    branch, `ReadGitBranchForDir`; no branch drops the component).
  - `tabTitleCase` (Default / Lowercase / Uppercase), `tabTitleSpacesToUnderscores` (false).
  - Every mode walks past generic `bin/obj/Debug/…` segments, turns `\` into `/`, and trims only a title
    over 255 chars (to 252 + `...`); a live preview derives four sample paths (`_UpdateTitleNamingPreview`).
- `favoriteIcon` (Crown / Star) and `maxTags` (20, at most 40): [TAGS.md](TAGS.md).
- `tooltipTagsOpacity` (0.9, 10–100%): the tab tooltip's tag row.
- `flashRingColor` (`#CCFF0000`): the unread ring's color and opacity.
- `pendingDotsLightColor` (`#FFE0A92B`) / `pendingDotsDarkColor` (`#FF5A3E00`): the draft dots on a dark
  / light background, picked by WCAG luminance (`PendingDotsColorFor`; the board's dark cards use the
  light color).
- **The overlay:** `showTabOverlay` (true), `tabOverlayRestOpacity` (0.50), `tabOverlayHoverOpacity` (1.0).
- `alwaysShowHomeButton`: unused (Home always shows).

## Tab coloring

`tabColorMode` (GLOBAL; applied live by `_ReapplyManagedTabColors`) picks a tab's color key (Rule #12).
Every color surface — tabs, board bands, Sessions chips, the pending dots — resolves through
`ResolveSessionColorHex(mode, s)`.

- **Shared per working directory** (`WorkingDirectory`, the default): one permanent color per folder
  (`dir-colors.json`), shared by every tab of the folder.
- **Individual per tab:** each session keeps its own color (`SessionInfo::tabColorHex`); a pick recolors
  only that session.
- **Inferred working directory** (`InferredWorkingDirectory`): per-folder colors keyed by the folder the
  session actually works in (`SessionInfo::inferredWorkingDir`, stored empty when it equals the cwd).
  - **The inference** (`InferWorkingDirectory`): every tool-touched path (`TranscriptStats::pathsAccessed`:
    the Read / Edit / Write / Grep / Glob path fields plus absolute paths mined from shell commands,
    `ExtractPathsFromText`) votes for its ancestor chain and belongs to one cluster: its nearest git root
    (with `inferGitRoot`, default true; a worktree keys that worktree) or its top-level folder. The
    cluster with the most votes wins; a tie keeps the launch folder. A git cluster answers its repo root;
    a non-git cluster descends while a child holds more than half of its parent's paths. Drive / UNC
    roots and relative paths never vote; machine-temp paths never vote (`CollectMachineTempRoots`).
  - **The loop:** re-detected off the scanner tick (mtime-gated, ~15 s throttle, `_ScanInferredTabColors`,
    from the Sessions sidecar), cached on the record (so a reopened session wears its color at once),
    logged `[infer-dir]`; a mode or `inferGitRoot` change re-infers every hosted session.
  - **Lineage:** a fork inherits its source's inference and infers from the source's transcript until it
    has its own; a restore-fresh keeps the old conversation's inference until the new one says otherwise;
    `/clear` starts uninferred; Codex never infers.
  - **A session launched in `%USERPROFILE%` infers in every mode** (`SessionInfersWorkingDir`), since a
    claude started in the home folder almost never works there.
- **Remove colors** (`NoColor`): no tab is colored, strip-wide. Managed tabs reset; every other tab's
  color is parked on the tab (`Tab::SetTabColorSuspended`) and restored on leaving the mode; Change tab
  color and `openTabColorPicker` are disabled on every tab; colors landing meanwhile are parked by
  `_OnClaudeTabColorChanged`; nothing persisted is read or written.
- **The effective working directory** (`EffectiveWorkingDir(mode, s)`): the inferred folder while the
  session infers, else the cwd. It drives grouping and scope in the tree, the card's folder line, the
  Prompt Queue header and apply-to-folder, the Launch box pre-aim, Open New Session Here, the overlay's
  subline, Open Path and Copy Path, and the tab tooltip. The cwd keeps the mechanics: spawn / resume /
  fork / restart cwd, hook and transcript correlation, per-folder env, recent folders, persistence, the
  summary's `Dir:`. For color only, `SessionColorKeyDir` maps a git worktree to its main repo root, so a
  repo and its worktrees share one color.

## Claude

- `claudeExePath` (empty): an `.exe`-only override of the detected `claude.exe` (shown read-only beside
  it; Browse…); re-resolved live on Save (`RefreshClaudeExe`).
- `claudeCleanupDaysSeeded`: whether `cleanupPeriodDays=36500` was written once into the global
  `~/.claude/settings.json` (only when the user had not set it), so Claude never purges session history.

## About

- Version and build. **Updates** ([UPDATER.md](UPDATER.md)): Check for updates, `allowUpdatePrerelease`
  (false) and `allowUpdateNightly` (false; turning it on asks first), each applied at once.
- **Profile:** the active profile folder (read-only) and Change profile folder… (applied at the next
  start; PROFILES.md). The profile is not an `AppSettings` field: it is the pointer to `settings.json`.
- `debugMode` (false): Enable Debug Mode, applied at the next start; it makes `Profiles::IsDevOrDebugPackage()`
  true (as the `AgentmasterDev` package, the `--debug` flag or `AGENTMASTER_DEBUG` do). No feature reads it
  today. ⚠ Its startup apply (`ApplyPersistedDebugMode`) exports `AGENTMASTER_DEBUG=1` into the process
  env, because the `DebugForced` latch is module-local.
- Uninstall (`--uninstall-portable`; PROFILES.md §2b).

## Written outside the cog

- `treeSort` (Newest) and `boardSort` (MostActive): the tree's and the board's / side panel's sort.
- `autoTestingShowsSummary` (true): the Prompt Queue pane opens on its Summary tab.
- The summary panel: `showSummaryPanel` (true), `summaryPanelWrapNewlines` (false),
  `summaryPanelTruncate` (true), `summaryPanelShowHistory` (true), `summaryPanelWidthFraction` /
  `summaryPanelHeightFraction` (0 = the default size).
- The transcript view: `transcriptOverlayLevel` (`default`), `transcriptOverlayWrapLines` (true).
- `sessionsAiThinkingLevel` (`off`): the AI search's thinking level.
- The side panel: `sidePanelOnLeft` (true), `sidePanelScrollToSelectedTab` (true), and the seeds for new
  windows `sidePanelWidthFraction` (0.22), `sidePanelSplitFraction` (0.5), `sidePanelCollapsed` (false),
  `sidePanelSummaryHidden` (false).
- The tab color flyout: `tabColorPickerCustomOpen`, `tabColorPickerAdvancedOpen` (true).
- The updater: `updateSkippedVersion`, `updatePostponedUntilUnixMs`.
- `hiddenSessionIds`: the Sessions page's hidden rows.
- `archiveSplitFraction`: unused; kept so an older file round-trips unchanged.
