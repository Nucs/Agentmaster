@echo off
REM Agentmaster M5 standalone test runner. Sets up MSVC, compiles the themed test TUs + the
REM engine TUs (no PCH/WinRT), and runs the suite. Exit code 0 == all pass.
REM  /utf-8 - matches the msbuild app build (common.build.pre.props adds it globally): every
REM           engine source is BOM-less UTF-8, so WITHOUT the flag cl reads them in the ANSI
REM           codepage and wide literals holding non-ASCII (the SessionSearch snippet ellipsis,
REM           its scope emoji prefixes, ...) compile to DIFFERENT constants than the shipped
REM           binary carries (the C4066 "characters beyond first" warning was the tell).
REM  /MP    - parallel compile across the ~20 TUs (measured ~3x on this suite).
REM  /DUNICODE /D_UNICODE - app-build parity too: msbuild defines both globally, and Updater.h's
REM           resource lookup (RT_RCDATA -> MAKEINTRESOURCE) requires the W flavor.
REM Where this runner sits decides the rest (2026-11-13): the build server's build VM compiles it in a worktree on D:
REM (buildserver\steps\build\tests.ps1, `--compile-only`; `amb test` then runs the zipped result), this PC from K:.
REM The repository's root is five folders up (tests, AgentMaster, TerminalApp, cascadia, src), or AM_REPO_ROOT when set:
REM a copy of the engine folder elsewhere (tools\interrupt-probe builds from one on Q:) takes cmark and WebView2 from the
REM checkout it names. AM_ROOT is this file's own result: never read as an input, since a call from an interactive cmd
REM leaves it set (no setlocal here) and would carry one checkout's root into another's run.
REM MSVC: AM_VCVARS when set; else the newest Visual Studio with the C++ tools, by vswhere (Community here, the Build
REM Tools in the build VM); else Community's own path. One-line IFs only, no labels: this file has LF line endings,
REM with which cmd's GOTO can miss a label.
for %%I in ("%~dp0..\..\..\..\..") do set "AM_ROOT=%%~fI"
if defined AM_REPO_ROOT set "AM_ROOT=%AM_REPO_ROOT%"
set "AM_VS="
set "AM_VCVARS_BAT=%AM_VCVARS%"
set "AM_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined AM_VCVARS_BAT if exist "%AM_VSWHERE%" for /f "usebackq delims=" %%I in (`"%AM_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "AM_VS=%%I"
if not defined AM_VCVARS_BAT if defined AM_VS if exist "%AM_VS%\VC\Auxiliary\Build\vcvars64.bat" set "AM_VCVARS_BAT=%AM_VS%\VC\Auxiliary\Build\vcvars64.bat"
if not defined AM_VCVARS_BAT set "AM_VCVARS_BAT=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call "%AM_VCVARS_BAT%" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
REM vcvars may move the current folder (to %USERPROFILE%\source when that exists): come back to this runner's own.
cd /d "%~dp0"
REM user32 + oleaut32: ProcessInspect's BringClaudeWindowToFront (EnumWindows/ShowWindow/... + UIA BSTR names)
REM icu.lib: ClaudeInvisibleChars.cpp (DELIVERY.md 15 - claude's invisible-character rule: Unicode properties, NFC, grapheme breaks)
REM TranscriptWatcher.cpp: SessionScanner's ReadDirectoryChangesW change feed (HOOKS.md "Transcript watcher") - a LINK dependency of SessionScanner.cpp.
REM version.lib (ClaudeSpawn's claude.exe version read) needs no entry here: ClaudeSpawn.cpp links it via #pragma comment.
REM tests_sidepanel.cpp: the side panel card list's order planner (SIDE_PANEL.md 2c - KeyedListDiff.h, header-only, so no engine .cpp joins)
REM and the cards' and rows' "created 4d1h, active 2m4s ago" timing line (SessionTimingText.h, header-only too),
REM where the header's "+" launcher card opens (SIDE_PANEL.md 2e - PointerCardPlacement.h, header-only too),
REM and which rows of the virtualized card list get cards (SIDE_PANEL.md 2f - VirtualListWindow.h, header-only too),
REM and when the AI view's list changes (SIDE_PANEL.md 2h - SidePanelAiList.h, header-only; it reads the types of
REM SessionSearchAgent.h, whose .cpp stays out: no agent function is called).
REM tests_taskbar.cpp: the taskbar unread badge (NOTIFICATIONS.md section 8) and the unread frame on a window's taskbar preview
REM (section 9, TaskbarThumbnail.h); gdi32.lib comes in through both headers' #pragma comment.
REM tests_turns.cpp + TranscriptTurns.cpp: the Copy > Transcript levels; the fixtures + goldens it reads are in turns\ (beside the exe).
REM tests_presets.cpp: PRESETS (HARNESSES.md - LaunchPresets.h, header-only and pure, so no engine .cpp joins; the OS half,
REM LaunchPresetsOs.h, is TerminalApp-only and stays out of the harness).
REM tests_harness.cpp + AgentHarnessCore.cpp + SessionSearchTools.cpp: the agent harness's model-free half and the Sessions
REM page's AI search tools (SDK_HARNESS.md, SESSIONS.md section 9). Neither needs the claude-agent-sdk-cpp submodule or
REM nlohmann/json: AgentHarness.cpp, the SDK unity file and its process backend stay out of this suite (their test is the
REM LIVE one, run-harness-live.bat, which starts the real claude.exe).
REM SessionSearchLog.cpp: the AI search's conversation log (SESSIONS.md section 9.17, debug mode only) - its lines, files and
REM prune over a scratch folder with a test clock (TestSessionSearchLog, tests_harness.cpp). It reads AgentHarness.h for the
REM event TYPES only, so it needs no SDK either.
REM SessionDbRefresher.cpp + SessionDbClient.cpp: the session database's refresh tick, which Engine.cpp creates (a LINK
REM dependency, SESSIONS_DB.md 7.6), and the pipe client it uses; tests_sessiondb.cpp covers their pure parts (the
REM protocol names, the tool definitions, the change set, the facts epoch, the AI search's database mode). The service
REM itself (sessiondb\, DuckDB) is not in this suite: it builds with sessiondb\_compile.bat.
REM tests_messaging.cpp: MESSAGING.md (the fleet in Telegram) — the pure core (TelegramFormat.h + MessagingModels.h) and the
REM two engine rules it leans on. Messaging.cpp + Messaging.Inbound.cpp + TelegramApi.cpp are LINK dependencies of Engine.cpp
REM (the service is created at engine init); the suite never calls the network (winhttp.lib comes in through TelegramApi.cpp's
REM pragma, advapi32 through Messaging.cpp's). The Search topic's agent (MessagingSearchBridge.cpp, TerminalApp-only) stays out:
REM it wraps SessionSearchAgent, which needs the SDK unity file this suite does not build.
REM tests_messaging_search.cpp: MESSAGING.md 5.4 / 21.7 - the Search topic's pure rules in MessagingModels.h (its verbs:
REM /clear, the refused session commands; the stateless pick buttons; where a picked session stands). Header-only: no .cpp joins.
REM tests_messaging_directory.cpp: MESSAGING.md 21.16 - the pinned directory, MessagingDirectory.h (header-only: the links, the
REM stats rows, the tags x tabs join, both renders, the hash, the post / edit decision) + the tag palette in SessionStore.cpp.
REM tests_messaging_newthread.cpp: MESSAGING.md 21.17 - the title links, the You lines' title row and the new-thread folder
REM picker (MessagingNewThread.h, header-only) + SessionRegistry::PlaceSamples (SessionRegistry.cpp, already linked).
REM tests_messaging_card.cpp + ScreenImage.cpp: MESSAGING.md 21.24 - the card's two pages of buttons (MessagingModels.h), the
REM styled screen snapshot's format (ScreenSnapshot.h, header-only) and the Screen Image renderer, which draws real PNGs with
REM Direct2D + DirectWrite + WIC (d2d1 / dwrite / windowscodecs / ole32 come in through ScreenImage.cpp's pragmas) and the
REM test decodes them again. ScreenImage.cpp is also a LINK dependency of Messaging.Inbound.cpp (the card's Screen Image).
REM tests_messaging_taps.cpp: MESSAGING.md 21.28 - a tapped button's answer held while its work runs (MessagingTaps.h, header-only
REM and std-only: the holds, the scopes, the texts), driven with a fake sink; no .cpp joins.
REM tests_messaging_steer.cpp: MESSAGING.md 21.33 - /steer, the words sent NOW (MessagingSteer.h, header-only and pure: the
REM decision, the texts, the prompt and its place, the watch's verdict) + the command's reading (MessagingModels.h); no .cpp joins.
REM tests_messaging_enter.cpp: MESSAGING.md 21.47 - /enter, ONE Enter pressed on the prompt in the box (MessagingEnter.h,
REM header-only and pure: the reply's format, the window's decision, the menu's highlighted option, the read-back's verdict, the
REM texts) + the command's reading (MessagingModels.h); no .cpp joins.
REM tests_messaging_outbox.cpp + MessagingOutboxJournal.cpp: MESSAGING.md 21.50 - the DURABLE OUTBOX: the pure core (MessagingOutbox.h,
REM header-only: the journal's lines and fold, the answer table, the classes, back-off, breaker, lanes, the topic probe) and the
REM journal's files on a scratch folder (the lock, the rewrite, appends, dead.jsonl, concurrent appends). MessagingOutboxJournal.cpp is
REM also a LINK dependency of Messaging.cpp (the service opens the journal at its start); no test calls Telegram.
REM tests_messaging_files.cpp + Messaging.Files.cpp + MessagingFileServer.cpp: MESSAGING.md 21.32.4 - the Telegram FILE LINKS: the
REM pure rules (MessagingFileLinks.h, header-only) and the server end to end on 127.0.0.1 (MessagingFileServer.cpp: Winsock,
REM a loopback-only bind, so this workstation's firewall never asks). Both .cpp are LINK dependencies of Messaging.cpp (the
REM service's file links); ws2_32, iphlpapi, bcrypt and advapi32 come in through MessagingFileServer.cpp's #pragma comment.
REM tests_tuimenus.cpp + Messaging.Menus.cpp: MESSAGING.md 21.34 - /add-dir's dialog answered from Telegram (TuiMenus.h,
REM header-only and pure: the screen parser against the probe's captures, the window's press decision, the transcript's result,
REM the card) and the delivery fixes in Scheduler.h (the command echo, the lost-send hold). Messaging.Menus.cpp is a LINK
REM dependency of Messaging.cpp and Messaging.Inbound.cpp (the service's menu watches and cards).
REM tests_claude_status.cpp + ClaudeStatus.cpp + Messaging.Status.cpp: MESSAGING.md 21.40 - /status shows what Claude Code's own
REM /status shows: the pure readers (ClaudeStatus.h: the headless /usage gate, the account lines, the /usage answer and its limit
REM lines, the token wording, the transcript fold) and the report's layout (MessagingStatus.h), plus the transcript FILE read and
REM the hidden child runner on this PC (ClaudeStatus.cpp; the child is cmd.exe, never claude). Messaging.Status.cpp is a LINK
REM dependency of Messaging.Inbound.cpp (the /status command), ClaudeStatus.cpp of Messaging.Status.cpp.
REM tests_ask.cpp + Messaging.Questions.cpp: MESSAGING.md 21.30 - Claude Code's AskUserQuestion answered from Telegram: the
REM answers composed as the PC composes them, the preview rules (ClaudeInvisibleChars.cpp, already linked), the card (AskQuestion.h,
REM header-only) and the ask pipe end to end (HooksBridge.cpp, already linked). Messaging.Questions.cpp is a LINK dependency of
REM Messaging.cpp (the service's question cards).
REM tests_rewind.cpp + ApiErrorRewind.cpp: HOOKS.md "API-error turns" - the one-line rewind that lets a resumed session
REM continue a turn an API error ended (the planner, the marker line, the append on temp files under the scratch profile).
REM tests_bundled_mods.cpp + BundledMods.cpp: BUNDLED_MODS.md - the Claude Code mods Agentmaster ships (the embedded catalog in
REM the generated BundledModsData.h, the launch decisions, the folders in a scratch profile, the spawn builders' provider seam).
REM Only this suite and TerminalAppLib link BundledMods.cpp: the app sets the provider, so every other program that compiles
REM ClaudeSpawn.cpp (the CLI, the session database, the probes) builds and runs without it.
REM tests_restorewake.cpp: PERSISTENCE.md 14 - the RESTORE WAKE (RestoreWake.h, header-only and pure: the session predicate,
REM the per-window foreground gate, the idle-UI wake decision, the cross-window barrier, the process-wide pacer) + the registry
REM it reads (already linked).
REM tests_tabtools.cpp: SESSIONS.md 9.14 - the AI search's window and tab tools (SessionSearchTools.cpp, already linked)
REM and the engine's answered window fan-out (Engine.cpp, already linked); no new engine .cpp joins.
REM tests_markdown.cpp + MarkdownBlocks.cpp: the transcript overlay's Markdown flattener, which parses with cmark. The
REM harness takes cmark from the vcpkg tree a full msbuild installs (obj\x64\vcpkg, static CRT like this /MT build);
REM without that tree MarkdownBlocks.cpp is left out and AM_HAVE_CMARK stays undefined, so TestMarkdownBlocks
REM reports a skip instead of the whole suite failing to build.
REM tests_conversation_image.cpp + ConversationImage.cpp: MESSAGING.md 21.39 - the card's All Screen, the whole conversation
REM drawn as tall pictures (the part planner and the tall renderer in ScreenImage.cpp, the Claude Code-like layout and the
REM pipeline in ConversationImage.cpp). ConversationImage.cpp parses with MarkdownBlocks, so it joins only WITH cmark; without
REM it, tests\conversation_image_stub.cpp stands in (Messaging.Inbound.cpp calls RenderConversationImages, so something must
REM define it) and the test's layout and end-to-end cases report a skip.
REM tests_voice.cpp: MESSAGING.md 21.43 / VOICE.md 5.5 - the voice notes' session vocabulary (VoiceVocabulary.h, header-only and
REM pure) against the POC's goldens in voice\ (written by voice\make_goldens.py from tools\voice-poc\vocab.py and llm_fix.py),
REM the voice worker protocol (VoiceProtocol.h, pure) and the correction layer's prompt, arguments, guard and answer parser
REM (VoiceCorrect.h, pure; its claude call, VoiceCorrect.cpp, is not linked: no test runs a model). Messaging.Voice.cpp joins
REM as a LINK dependency of Messaging.cpp (the service's voice lanes and thread); the transcriber behind its seam
REM (VoiceTranscriber.cpp, VoiceWorker.cpp, VoiceHttp.cpp, VoiceCorrect.cpp) is TerminalApp-only, like the Search agent's
REM bridge: with no factory set, the harness's service answers a voice note "not available in this build".
set "AM_VCPKG=%AM_ROOT%\obj\x64\vcpkg\x64-windows-static"
set "AM_MD_SRC=conversation_image_stub.cpp"
set "AM_MD_FLAGS="
if exist "%AM_VCPKG%\include\cmark.h" if exist "%AM_VCPKG%\lib\cmark.lib" (
  set "AM_MD_SRC=..\MarkdownBlocks.cpp ..\ConversationImage.cpp %AM_VCPKG%\lib\cmark.lib"
  set "AM_MD_FLAGS=/DAM_HAVE_CMARK /I%AM_VCPKG%\include"
)
REM tests_update_notes.cpp: UPDATER.md "The prompt window" - the update prompt window's release notes (UpdaterReleaseNotes.h: the list, the
REM timeline, the Install cut, cmark's HTML, the page and its model, the plain text) and its layout (UpdaterPromptWindow.h).
REM Updater.h includes both parts, so EVERY unit that includes it (tests_spawn_sched.cpp too) compiles the prompt window: it
REM renders the notes with cmark when the vcpkg tree above is there (the same include path), and it hosts WebView2 when the
REM WebView2 SDK the repository restores for WinUI is there - its headers on the include path and its static loader linked
REM (advapi32 for the loader's registry and event-log calls). Without the package the window compiles its plain-text path.
REM Keep the version in step with WebView2PackageRoot in src\common.nugetversions.targets.
REM tests_reopen.cpp: PERSISTENCE.md 18 - several windows at start: the reopen question's "Remember my choice" and the Behavior
REM setting (ReopenWindowsAtStart.h, pure; ReopenWindowsPrompt.h, header-only Win32 like Updater.h, which it includes for the
REM settings.json helpers - so this unit compiles the update prompt window too, with the same flags as tests_spawn_sched.cpp).
REM No engine .cpp joins; no dialog is ever shown (the asking path is modal: an e2e / manual check).
set "AM_WV2=%AM_ROOT%\packages\Microsoft.Web.WebView2.1.0.1661.34\build\native"
set "AM_WV2_FLAGS="
set "AM_WV2_LIB="
if exist "%AM_WV2%\include\WebView2.h" if exist "%AM_WV2%\x64\WebView2LoaderStatic.lib" (
  set "AM_WV2_FLAGS=/I%AM_WV2%\include"
  set "AM_WV2_LIB=%AM_WV2%\x64\WebView2LoaderStatic.lib advapi32.lib"
)
cl /std:c++20 /EHsc /nologo /W3 /utf-8 /MP /DUNICODE /D_UNICODE %AM_MD_FLAGS% %AM_WV2_FLAGS% /Fe:m5_tests.exe m5_tests.cpp tests_state.cpp tests_spawn_sched.cpp tests_persistence.cpp tests_transcript.cpp tests_summary_anchor.cpp tests_commands.cpp tests_tabdrag.cpp tests_invisible.cpp tests_taskbar.cpp tests_turns.cpp tests_markdown.cpp tests_sidepanel.cpp tests_presets.cpp tests_harness.cpp tests_sessiondb.cpp tests_messaging.cpp tests_messaging_search.cpp tests_messaging_directory.cpp tests_messaging_newthread.cpp tests_messaging_card.cpp tests_conversation_image.cpp tests_messaging_taps.cpp tests_messaging_steer.cpp tests_messaging_enter.cpp tests_messaging_outbox.cpp tests_messaging_files.cpp tests_tuimenus.cpp tests_claude_status.cpp tests_ask.cpp tests_rewind.cpp tests_restorewake.cpp tests_tabtools.cpp tests_voice.cpp tests_update_notes.cpp tests_bundled_mods.cpp tests_reopen.cpp %AM_MD_SRC% ..\ApiErrorRewind.cpp ..\BundledMods.cpp ..\TranscriptTurns.cpp ..\SessionRegistry.cpp ..\HooksBridge.cpp ..\ClaudeSpawn.cpp ..\Persistence.cpp ..\SessionScanner.cpp ..\TranscriptWatcher.cpp ..\CommandWatch.cpp ..\ProcessInspect.cpp ..\ProcessInspect.Transcript.cpp ..\ProcessInspect.Content.cpp ..\ProcessInspect.Window.cpp ..\ProcessInspect.Summary.cpp ..\TranscriptStore.cpp ..\SessionSearch.cpp ..\SessionStore.cpp ..\Scheduler.cpp ..\Engine.cpp ..\SessionDbRefresher.cpp ..\SessionDbClient.cpp ..\Messaging.cpp ..\Messaging.Inbound.cpp ..\Messaging.Questions.cpp ..\Messaging.Menus.cpp ..\Messaging.Status.cpp ..\ClaudeStatus.cpp ..\Messaging.Files.cpp ..\Messaging.Voice.cpp ..\MessagingFileServer.cpp ..\TelegramApi.cpp ..\MessagingOutboxJournal.cpp ..\ScreenImage.cpp ..\ProcessObserver.cpp ..\ClaudeInvisibleChars.cpp ..\AgentHarnessCore.cpp ..\SessionSearchTools.cpp ..\SessionSearchLog.cpp %AM_WV2_LIB% ole32.lib user32.lib oleaut32.lib icu.lib
if errorlevel 1 (
  echo [error] compile failed
  exit /b 1
)
REM --compile-only: the build server's build step makes the test artifact here, and `amb test` runs it elsewhere.
if /i "%~1"=="--compile-only" (
  echo compiled m5_tests.exe; not run: --compile-only
  exit /b 0
)
echo.
REM State isolation: run against a WIPED %TEMP% scratch profile so the engine traces the linked
REM units emit ([fork-echo]/[send]/[recon-*] -> hooks/autorunner/scanner logs) and the window-record
REM disk round-trips (windows\<id>.json + open-windows.json) never touch the LIVE ~/.agentmaster —
REM the RELEASE install's active profile. m5_tests.exe self-defaults to this same scratch dir when
REM the var is absent (a direct exe run); only the bat wipes it fresh.
set "AGENTMASTER_PROFILE=%TEMP%\agentmaster-m5-tests"
rd /s /q "%TEMP%\agentmaster-m5-tests" >nul 2>&1
REM Explicit ".\" so it launches even when NoDefaultCurrentDirectoryInExePath is set.
.\m5_tests.exe
exit /b %errorlevel%
