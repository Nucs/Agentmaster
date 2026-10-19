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
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [error] could not initialize MSVC environment
  exit /b 2
)
cd /d "K:\source\Agentmaster\src\cascadia\TerminalApp\AgentMaster\tests"
REM user32 + oleaut32: ProcessInspect's BringClaudeWindowToFront (EnumWindows/ShowWindow/... + UIA BSTR names)
REM icu.lib: ClaudeInvisibleChars.cpp (DELIVERY.md 15 - claude's invisible-character rule: Unicode properties, NFC, grapheme breaks)
REM TranscriptWatcher.cpp: SessionScanner's ReadDirectoryChangesW change feed (HOOKS.md "Transcript watcher") - a LINK dependency of SessionScanner.cpp.
REM version.lib (ClaudeSpawn's claude.exe version read) needs no entry here: ClaudeSpawn.cpp links it via #pragma comment.
REM tests_sidepanel.cpp: the side panel card list's order planner (SIDE_PANEL.md 2c - KeyedListDiff.h, header-only, so no engine .cpp joins)
REM and the cards' and rows' "created 4d1h, active 2m4s ago" timing line (SessionTimingText.h, header-only too),
REM and where the header's "+" launcher card opens (SIDE_PANEL.md 2e - PointerCardPlacement.h, header-only too).
REM tests_taskbar.cpp: the taskbar unread badge (NOTIFICATIONS.md section 8) and the unread frame on a window's taskbar preview
REM (section 9, TaskbarThumbnail.h); gdi32.lib comes in through both headers' #pragma comment.
REM tests_turns.cpp + TranscriptTurns.cpp: the Copy > Transcript levels; the fixtures + goldens it reads are in turns\ (beside the exe).
REM tests_presets.cpp: PRESETS (HARNESSES.md - LaunchPresets.h, header-only and pure, so no engine .cpp joins; the OS half,
REM LaunchPresetsOs.h, is TerminalApp-only and stays out of the harness).
REM tests_harness.cpp + AgentHarnessCore.cpp + SessionSearchTools.cpp: the agent harness's model-free half and the Sessions
REM page's AI search tools (SDK_HARNESS.md, SESSIONS.md section 9). Neither needs the claude-agent-sdk-cpp submodule or
REM nlohmann/json: AgentHarness.cpp, the SDK unity file and its process backend stay out of this suite (their test is the
REM LIVE one, run-harness-live.bat, which starts the real claude.exe).
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
REM tests_ask.cpp + Messaging.Questions.cpp: MESSAGING.md 21.30 - Claude Code's AskUserQuestion answered from Telegram: the
REM answers composed as the PC composes them, the preview rules (ClaudeInvisibleChars.cpp, already linked), the card (AskQuestion.h,
REM header-only) and the ask pipe end to end (HooksBridge.cpp, already linked). Messaging.Questions.cpp is a LINK dependency of
REM Messaging.cpp (the service's question cards).
REM tests_rewind.cpp + ApiErrorRewind.cpp: HOOKS.md "API-error turns" - the one-line rewind that lets a resumed session
REM continue a turn an API error ended (the planner, the marker line, the append on temp files under the scratch profile).
REM tests_restorewake.cpp: PERSISTENCE.md 14 - the RESTORE WAKE (RestoreWake.h, header-only and pure: the session predicate,
REM the per-window foreground gate, the idle-UI wake decision, the cross-window barrier, the process-wide pacer) + the registry
REM it reads (already linked).
REM tests_tabtools.cpp: SESSIONS.md 9.14 - the AI search's window and tab tools (SessionSearchTools.cpp, already linked)
REM and the engine's answered window fan-out (Engine.cpp, already linked); no new engine .cpp joins.
REM tests_markdown.cpp + MarkdownBlocks.cpp: the transcript overlay's Markdown flattener, which parses with cmark. The
REM harness takes cmark from the vcpkg tree a full msbuild installs (obj\x64\vcpkg, static CRT like this /MT build);
REM without that tree MarkdownBlocks.cpp is left out and AM_HAVE_CMARK stays undefined, so TestMarkdownBlocks
REM reports a skip instead of the whole suite failing to build.
set "AM_VCPKG=K:\source\Agentmaster\obj\x64\vcpkg\x64-windows-static"
set "AM_MD_SRC="
set "AM_MD_FLAGS="
if exist "%AM_VCPKG%\include\cmark.h" if exist "%AM_VCPKG%\lib\cmark.lib" (
  set "AM_MD_SRC=..\MarkdownBlocks.cpp %AM_VCPKG%\lib\cmark.lib"
  set "AM_MD_FLAGS=/DAM_HAVE_CMARK /I%AM_VCPKG%\include"
)
cl /std:c++20 /EHsc /nologo /W3 /utf-8 /MP /DUNICODE /D_UNICODE %AM_MD_FLAGS% /Fe:m5_tests.exe m5_tests.cpp tests_state.cpp tests_spawn_sched.cpp tests_persistence.cpp tests_transcript.cpp tests_summary_anchor.cpp tests_commands.cpp tests_tabdrag.cpp tests_invisible.cpp tests_taskbar.cpp tests_turns.cpp tests_markdown.cpp tests_sidepanel.cpp tests_presets.cpp tests_harness.cpp tests_sessiondb.cpp tests_messaging.cpp tests_messaging_search.cpp tests_messaging_directory.cpp tests_messaging_newthread.cpp tests_messaging_card.cpp tests_messaging_taps.cpp tests_messaging_steer.cpp tests_ask.cpp tests_rewind.cpp tests_restorewake.cpp tests_tabtools.cpp %AM_MD_SRC% ..\ApiErrorRewind.cpp ..\TranscriptTurns.cpp ..\SessionRegistry.cpp ..\HooksBridge.cpp ..\ClaudeSpawn.cpp ..\Persistence.cpp ..\SessionScanner.cpp ..\TranscriptWatcher.cpp ..\CommandWatch.cpp ..\ProcessInspect.cpp ..\ProcessInspect.Transcript.cpp ..\ProcessInspect.Content.cpp ..\ProcessInspect.Window.cpp ..\ProcessInspect.Summary.cpp ..\TranscriptStore.cpp ..\SessionSearch.cpp ..\SessionStore.cpp ..\Scheduler.cpp ..\Engine.cpp ..\SessionDbRefresher.cpp ..\SessionDbClient.cpp ..\Messaging.cpp ..\Messaging.Inbound.cpp ..\Messaging.Questions.cpp ..\TelegramApi.cpp ..\ScreenImage.cpp ..\ProcessObserver.cpp ..\ClaudeInvisibleChars.cpp ..\AgentHarnessCore.cpp ..\SessionSearchTools.cpp ole32.lib user32.lib oleaut32.lib icu.lib
if errorlevel 1 (
  echo [error] compile failed
  exit /b 1
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
