// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing). The
// process-singleton wiring is exactly what TerminalPage::_InitAgentmasterEngine did inline
// before M9; hoisted here so it runs once for the whole WindowEmperor process.
#include "Engine.h"

#include "ClaudeSpawn.h"
#include "CommandWatch.h" // COMMANDS.md — slash-command bindings (the /handover integration)
#include "HookWire.h"
#include "HooksBridge.h"
#include "Persistence.h"
#include "ProcessObserver.h"
#include "ProfileBootstrap.h" // Profiles::ApplyPersistedDebugMode — the Debug Mode belt for this module (no feature reads the gate today: the Prompt Queue Runner left it on 2026-10-09)
#include "RegexUtil.h" // COMMANDS.md §6b — validate the configured file-match pattern once at bind time
#include "Scheduler.h"
#include "SessionRegistry.h"
#include "SessionScanner.h"
#include "SessionDbRefresher.h" // SESSIONS_DB.md §7.6 — the session database refresh tick
#include "Messaging.h" // MESSAGING.md — the Telegram bot inside this process (D1)
#include "SessionStore.h"
#include "StartupTiming.h" // [startup] phase timing — find where launch spends its time

#include <windows.h>
#include <shellapi.h> // Agentmaster: ShellExecuteW (LaunchClaudeInstall opens a visible PowerShell install window)

#include <algorithm> // Agentmaster: std::min (the autosaver's debounce deadline)
#include <atomic>
#include <condition_variable> // Agentmaster: the debounced sessions.json autosaver worker
#include <cstdio>
#include <mutex> // Agentmaster: guards the log observer's per-id dedup map (notify fires from several threads)
#include <random>
#include <chrono> // Agentmaster: the UI-stall watchdog's sleep cadence
#include <string>
#include <thread> // Agentmaster: the debounced sessions.json autosaver worker + the UI-stall watchdog
#include <unordered_map> // Agentmaster: per-id last-logged [Unknown] line, to dedup the pull/transient notify flood
#include <vector> // Agentmaster (MESSAGING.md §21.18): the package-id buffer of AppVersionWordForMessaging

namespace Agentmaster
{
    namespace
    {
        // Agentmaster (perf — the CPU-hotspot fix): the DEBOUNCED sessions.json autosaver. The M8
        // autosave observer used to run SaveSessions(reg->Snapshot()) INLINE on EVERY registry notify —
        // a deep copy of ALL records + a ~3 MB JSON serialize + an atomic file replace, on whichever
        // engine thread notified (scanner / S-lane / bridge). With 19 active sessions the notify stream
        // is continuous, so the fleet was being re-serialized to disk many times a second (measured:
        // sessions.json = 3.0 MB, rewritten per notify on the live 70-tab release instance).
        //
        // This worker coalesces bursts: a Poke() marks dirty + wakes the thread; the write runs once
        // the stream is QUIET for kQuietMs, or at the kMaxLatencyMs cap while it never goes quiet — so
        // steady-state pressure costs ~1 write/second instead of one per notify, and the on-disk state
        // is never more than ~1s behind. DURABILITY (Rule 16 / Rule 4): the loss window is bounded by
        // the cap; lifecycle-critical transitions do NOT ride the debounce — the engine observer below
        // write-through-saves synchronously for a !live notify (archive/close/teardown, the moments
        // that precede an exit), and the teardown/bind seams keep their own direct SaveSessions calls.
        // SaveSessions itself is thread-safe (per-thread temp + atomic replace, Persistence.cpp), so
        // this worker racing a direct seam call is the same class of interleave the per-notify saves
        // already had — just far rarer.
        class SessionAutosaver
        {
        public:
            explicit SessionAutosaver(std::shared_ptr<SessionRegistry> reg) :
                _reg(std::move(reg)),
                _thread([this] { _run(); })
            {
            }

            // Never runs in practice (the Engine singleton is deliberately never deleted) — but keep
            // teardown correct for any future owner: stop, join, final-flush.
            ~SessionAutosaver()
            {
                {
                    std::lock_guard lk{ _mtx };
                    _stop = true;
                }
                _cv.notify_all();
                if (_thread.joinable())
                {
                    _thread.join();
                }
                if (_dirty)
                {
                    SaveSessions(_reg->Snapshot());
                }
            }

            SessionAutosaver(const SessionAutosaver&) = delete;
            SessionAutosaver& operator=(const SessionAutosaver&) = delete;

            void Poke()
            {
                const int64_t now = static_cast<int64_t>(::GetTickCount64());
                {
                    std::lock_guard lk{ _mtx };
                    _lastPokeMs = now;
                    if (_firstPokeMs == 0)
                    {
                        _firstPokeMs = now;
                    }
                    _dirty = true;
                }
                _cv.notify_all();
            }

        private:
            static constexpr int64_t kQuietMs = 300; // write once the notify stream pauses this long
            static constexpr int64_t kMaxLatencyMs = 1000; // ... but never lag the disk more than this

            void _run()
            {
                std::unique_lock lk{ _mtx };
                while (!_stop)
                {
                    _cv.wait(lk, [this] { return _stop || _dirty; });
                    if (_stop)
                    {
                        break; // the dtor final-flushes after the join
                    }
                    for (;;)
                    {
                        const int64_t now = static_cast<int64_t>(::GetTickCount64());
                        const int64_t deadline = (std::min)(_lastPokeMs + kQuietMs, _firstPokeMs + kMaxLatencyMs); // (std::min): dodge the windows.h min macro (no NOMINMAX in the pch-less engine TUs)
                        if (now >= deadline)
                        {
                            break;
                        }
                        _cv.wait_for(lk, std::chrono::milliseconds(deadline - now));
                        if (_stop)
                        {
                            break;
                        }
                    }
                    if (_stop)
                    {
                        break;
                    }
                    _dirty = false;
                    _firstPokeMs = 0;
                    lk.unlock(); // the Snapshot+serialize+write runs OUTSIDE the lock — Poke() never blocks on disk
                    SaveSessions(_reg->Snapshot());
                    lk.lock();
                }
            }

            std::shared_ptr<SessionRegistry> _reg;
            std::mutex _mtx;
            std::condition_variable _cv;
            bool _stop = false;
            bool _dirty = false;
            int64_t _firstPokeMs = 0; // first un-flushed Poke (drives the max-latency cap)
            int64_t _lastPokeMs = 0; // newest Poke (drives the quiet window)
            std::thread _thread; // last member: starts after everything above is initialized
        };

        // MESSAGING.md §21.18: the app's version word for the Telegram bot's start note and run marker — the package
        // identity's version for a packaged install ("0.6.8.21"; the dev package's as "dev 0.0.1.28"), a portable copy's
        // .am-version stamp ("portable 0.7.9.0"), else "" (a local unpackaged build, the test harness: the note then names no
        // version). It is what tells the user, from the phone, that a restart was an update (the note's "was …") and which
        // build came up after a hot swap. Plain Win32 (Engine.cpp stays WinRT-free); asked once per start of the bot.
        // Never throws: an identity or stamp that cannot be read gives "".
        std::wstring AppVersionWordForMessaging() noexcept
        {
            try
            {
                std::wstring version;
                const bool packaged = !Profiles::PackageFamilyName().empty();
                if (packaged)
                {
                    // PACKAGE_ID carries the manifest's Major.Minor.Build.Revision — the version the hot-swap script stamps
                    // (0.0.1.28) and the release pipeline writes (0.6.8.21), so the word matches what the user deployed.
                    UINT32 len = 0;
                    if (::GetCurrentPackageId(&len, nullptr) == ERROR_INSUFFICIENT_BUFFER && len > 0)
                    {
                        std::vector<BYTE> buf(len); // heap memory: aligned for the PACKAGE_ID at its start
                        if (::GetCurrentPackageId(&len, buf.data()) == ERROR_SUCCESS)
                        {
                            const auto* id = reinterpret_cast<const PACKAGE_ID*>(buf.data());
                            version = std::to_wstring(id->version.Major) + L"." + std::to_wstring(id->version.Minor) + L"." + std::to_wstring(id->version.Build) +
                                      L"." + std::to_wstring(id->version.Revision);
                        }
                    }
                }
                else if (Profiles::IsPortableInstall())
                {
                    // The stamp the release zip ships beside the exe (the updater's PortableVersionFromDir reads the same
                    // file): its first run of digits and dots, so a BOM, a CRLF or a trailing note never reaches the word.
                    const std::wstring stamp = Profiles::detail::ReadUtf8File(Profiles::detail::ExeDirPath() + L"\\.am-version");
                    for (const wchar_t c : stamp)
                    {
                        if ((c >= L'0' && c <= L'9') || c == L'.')
                        {
                            version.push_back(c);
                        }
                        else if (!version.empty())
                        {
                            break;
                        }
                    }
                }
                return Msg::FormatAppVersionWord(Profiles::IsDevPackage(), !packaged && Profiles::IsPortableInstall(), version);
            }
            catch (...)
            {
                LogSwallowedException(L"AppVersionWordForMessaging"); // Rule #18; recovery: no version word in the note
                return {};
            }
        }
    }

    Engine& SharedEngine()
    {
        // Function-local static: C++ guarantees this initializer runs exactly once even if two
        // window threads call SharedEngine() concurrently. Heap-allocated and never deleted on
        // purpose (process lifetime) — see Engine.h.
        static Engine* const g = []() -> Engine* {
            // [startup] timing: this process-once wiring runs on the FIRST window's _InitAgentmaster-
            // Engine, so its cost lands inside that window's "engine-init" phase. Break it down here —
            // the Resolve* PATH scans + the file materializations are the usual launch-time hogs.
            Startup::ScopedPhase _wiring{ L"shared-engine wiring (process-once)" };
            auto* e = new Engine{};
            e->registry = std::make_shared<SessionRegistry>();

            // Per-startup tab-color randomization + one-time dir-colors migration (Agentmaster). A
            // fresh random seed each launch re-rolls the auto palette assignment (so the fleet looks
            // different every run, and two open dirs never share a color — collision-avoided in
            // AssignDirAutoColor), and the migration upgrades a v1 dir-colors.json (which mixed user
            // picks with the old colliding auto colors) to v2 = user picks only. Process-global state.
            {
                Startup::ScopedPhase _t{ L"  seed+migrate dir-colors" };
                std::random_device rd;
                SeedDirColors((static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd()));
                MigrateDirColorsToV2IfNeeded();
            }

            // Agentmaster (ENV_VARS.md §8): one-time shipped defaults, for NEW installs AND updaters
            // alike. CLAUDE_CODE_MAX_RETRIES=50000 is appended to the global env, and cleanupPeriodDays=
            // 36500 is written into the user's ~/.claude/settings.json (so Claude never purges global
            // history). Both are marker-gated, so a user who edits or deletes either keeps it gone.
            // Process-once, before any window seeds the cog or a session spawns.
            {
                Startup::ScopedPhase _t{ L"  seed session/claude defaults" };
                SeedSessionEnvDefaults();
                SeedClaudeCleanupPeriodDaysIfNeeded();
            }

            // Observer: record every state change to a log file (and the debugger). Runs on a
            // bridge thread, so it must touch no XAML.
            e->registry->AddObserver([](const SessionInfo& s, HookEvent ev) {
                wchar_t line[600];
                ::swprintf(line,
                           600,
                           L"[%s] %s state=%d question=%d dir=%s\n",
                           HookEventName(ev),
                           s.id.c_str(),
                           static_cast<int>(s.state),
                           s.lastMessageWasQuestion ? 1 : 0,
                           s.workingDir.c_str());
                ::OutputDebugStringW(line);
                // Agentmaster: the PUSH event stream (named hooks — SessionStart / UserPromptSubmit /
                // Stop / Notification / ...) is ALWAYS logged; it is the state machine's audit trail.
                // But a HookEvent::Unknown notify carries no event semantics — it is "some fact changed"
                // from a PULL / transient path (a pending-input draft flip, a rename, an enrichment
                // refresh, a triage move), and those paths already log their OWN contextual tag
                // ([pending] / [nav] / ...). Logging an [Unknown] line here TOO produced a flood: ~98%
                // of [Unknown] lines were byte-identical consecutive repeats of the same id's same
                // (state,question,dir) — a pending-draft appear/clear alone re-emitted the identical
                // line — which grew hooks.log by hundreds of MB. So DEDUPE Unknown: skip it when this
                // id's line is unchanged since the last [Unknown] we logged for it (the first [Unknown]
                // after any real state/dir change still lands, so pull-driven transitions are recorded
                // once). Thread-safe — notify is raised from several engine threads.
                if (ev == HookEvent::Unknown)
                {
                    static std::mutex s_unknownMtx;
                    static std::unordered_map<std::wstring, std::wstring> s_lastUnknownLine; // id -> last [Unknown] body
                    std::lock_guard lk{ s_unknownMtx };
                    // Agentmaster (bounded dedup): the map held one line per session id for the
                    // process LIFETIME (insert-only — unbounded across a long run's many ids). Past a
                    // generous cap, reset it wholesale before admitting a NEW id: the cost is one
                    // extra [Unknown] line per still-active id while the dedup re-primes — nothing
                    // next to the flood the dedup exists to stop.
                    if (s_lastUnknownLine.size() > 512 && s_lastUnknownLine.find(s.id) == s_lastUnknownLine.end())
                    {
                        s_lastUnknownLine.clear();
                    }
                    auto& prev = s_lastUnknownLine[s.id];
                    if (prev == line)
                    {
                        return; // identical to the last [Unknown] for this id -> nothing new to record
                    }
                    prev = line;
                }
                AppendStateLog(L"hooks.log", line);
            });

            // Prompt Queue Runner scheduler (M7; the code and the logs still say "autorunner", its name
            // until 2026-10-09): owns its own worker thread and drives every session's prompt queue. A
            // clean turn-complete (Stop -> WaitingForInput) lands on the advance seam and is forwarded
            // to the scheduler; a separate observer feeds the stopOnError backstop.
            //
            // Agentmaster (IN EVERY BUILD since 2026-10-09): the runner used to be a DEV-OR-DEBUG
            // feature. An ordinary release switched the scheduler's auto-advance off
            // (SetAutoAdvanceEnabled(false)), left the advance seam unwired and hid the whole UI, so no
            // session there could queue or auto-send anything. It is an ordinary feature now: the
            // release package, the dev package and an unpackaged build all start the scheduler with
            // BOTH of its jobs — the auto-advance (DecideAdvance) and the pickup watch that re-presses
            // a lone Enter when the TUI ate a send's submit Enter (DecideEnterRetry, DELIVERY.md §17).
            // ⚠ What that changes for an EXISTING release install, on its first start with this code:
            // a session whose queue still holds a Pending prompt (a /handover paste that was never
            // delivered, a queue left by a Debug Mode run) AND whose stored mode is Auto gets that
            // prompt sent, once its tab is open, its claude is started and it has held a sendable
            // status for the settle window (kStateSettleMs). A stored mode is never rewritten here: a
            // record minted Paused stays Paused until the user changes it.
            //
            // Agentmaster (Enable Debug Mode — the cross-module BELT): Debug Mode itself is KEPT — the
            // About-tab toggle, `--debug` and AGENTMASTER_DEBUG still drive Profiles::IsDevOrDebugPackage,
            // which is reserved for FUTURE debug-only features and which NOTHING below reads. The call
            // re-applies the persisted toggle IN THIS MODULE, so that such a feature gets the right
            // verdict here. The EXE prelude already applied it (WindowEmperor::HandleCommandlineArgs),
            // but the DebugForced latch is MODULE-local — one private copy per linked binary
            // (ProfileBootstrap.h) — so the EXE's apply reaches this DLL only via its AGENTMASTER_DEBUG
            // env export. This belt makes the DLL derive the verdict from settings.json ITSELF, covering
            // any host/ordering that skipped the EXE prelude. One-way + idempotent (double application
            // is harmless); inert in the engine test harness, which never calls SharedEngine().
            ::Agentmaster::Profiles::ApplyPersistedDebugMode();

            e->scheduler = std::make_shared<Scheduler>(e->registry);
            // Both jobs run. The auto-advance is on by default (Scheduler::_autoAdvance) and no build
            // turns it off any more — SetAutoAdvanceEnabled stays for the test harness and as the place
            // a future kill switch would go. It is still set explicitly, BEFORE the worker starts and
            // before the observer is registered, so the engine's intent is stated here and no advance
            // could be requested (or run) in a gap if that default ever changed.
            e->scheduler->SetAutoAdvanceEnabled(true);
            e->scheduler->Start();
            {
                // The observer arms the pickup watch for any send that awaits pickup, applies the
                // stopOnError backstop, and requests the idle-start advance for a ready session with
                // queued work (Scheduler::OnObserved).
                auto sched = e->scheduler;
                e->registry->AddObserver([sched](const SessionInfo& s, HookEvent) {
                    sched->OnObserved(s);
                });
            }
            // The mode stamped on a session the registry mints by itself (see below). Read once from the
            // persisted settings; also named in the start line, so hooks.log shows what a new session gets.
            const AutorunnerMode defaultRunnerMode = LoadAppSettings().defaultAutorunnerMode;
            {
                // The Stop seam: a clean turn-complete asks the scheduler to decide that session's next
                // queued prompt. Wired in every build; without it a turn that completes with queued work
                // only leaves the registry's [advance-dropped] breadcrumb.
                auto sched = e->scheduler;
                e->registry->SetAdvanceHandler([sched](const std::wstring& id) {
                    sched->RequestAdvance(id);
                });
                // Adopt/re-home autorunner default (SetDefaultAutorunnerMode): the MODE the registry
                // stamps onto records it mints ITSELF — the hook SessionStart adopt create + the Fleet
                // Observer's first-sight create (a hand-typed adopted claude; the NEW conversation id
                // an in-session /clear or /resume mints on a re-homed tab). The launch/restore seams
                // stamp the cog default in TerminalPage, but these two creations happen inside the
                // engine and used to default Off — silently exempting exactly those sessions from the
                // autorunner ("queued prompts stuck Pending"). Seeded here from the persisted settings;
                // the cog Save re-pushes it live (TerminalPage's settings handler), in every build.
                e->registry->SetDefaultAutorunnerMode(defaultRunnerMode);
            }
            // One line per process start: proof in hooks.log that this build runs the runner, and with
            // which default (the machine words Off / Full — AutorunnerModeLabel — so the line can be
            // matched against sessions.json). It replaced the old "release build (no --debug): … disabled".
            AppendStateLog(L"hooks.log", std::wstring{ L"[engine] prompt queue runner started (auto-advance + pickup watch, every build); default mode for new sessions: " } + AutorunnerModeLabel(defaultRunnerMode) + L"\n");

            // Persistence (M8): autosave the registry (queue + autorunner + metadata) to
            // sessions.json on change, so an in-progress plan survives a crash. Restore never
            // replays Sent prompts (statuses are preserved).
            //
            // Agentmaster (perf — the CPU-hotspot fix): DEBOUNCED, not per-notify. The old inline
            // SaveSessions(reg->Snapshot()) re-serialized the whole ~3 MB fleet to disk on EVERY
            // registry notify (presence flips, hook events, recon synths — continuous with an active
            // fleet), on whichever engine thread happened to notify. High-frequency LIVE churn now
            // folds into the SessionAutosaver (quiet 300ms / cap 1s — the disk is never more than ~1s
            // behind); a !live transition (archive/close/teardown — the writes that precede an exit,
            // Rule 16) still saves SYNCHRONOUSLY right here, exactly like before, and the teardown/
            // bind seams keep their own direct SaveSessions calls.
            {
                auto reg = e->registry;
                auto saver = std::make_shared<SessionAutosaver>(e->registry);
                e->registry->AddObserver([reg, saver](const SessionInfo& s, HookEvent) {
                    if (!s.live)
                    {
                        SaveSessions(reg->Snapshot()); // lifecycle write-through (durability unchanged)
                    }
                    else
                    {
                        saver->Poke();
                    }
                });
            }

            // Per-session DURABLE store (SessionStore): mirror the authoritative TITLE — the ONE
            // value Explorer/tab/persistence share (Rule #11) — to <profile>/session-store/<sid>.json
            // on every change, so a title known in ANY window persists across windows and OUTLIVES
            // the live session. The Sessions browser reads it (O(1) by id) for closed/historical rows,
            // and it is the generalized layer for any future per-session datum. SPARSE by design:
            // ObserveClaude never sets a title (Rule #13), so only sessions we actually launch / adopt
            // / rename / restore get a file; the write is deduped (skipped when the stored title is
            // already equal), so a steady-state enrichment re-notify costs one small read, not a write.
            // Loading the fleet at startup (each Upsert notifies) backfills the store for free.
            e->registry->AddObserver([](const SessionInfo& s, HookEvent) {
                if (!s.id.empty() && !s.title.empty())
                {
                    SetStoredSessionTitle(s.id, s.title);
                }
            });

            // Interval reconciler (M11; the PULL half — the bridge is PUSH). A low-priority
            // worker tails each live session's transcript to recover what a dropped hook missed
            // (a missed Stop strands a session in Running; assistant text is hook-invisible) and
            // fans out a periodic liveness sweep so a dead claude.exe (crash / exit with no
            // SessionEnd) is archived. Adaptive cadence + idle-sleep keep it ~free when nothing is
            // live; the observer below Wake()s it the instant a session goes live. The liveness
            // CHECK is WinRT (walks tabs), so it is delegated to per-window probes the scanner ticks.
            e->scanner = std::make_shared<SessionScanner>(e->registry);
            // Waiting-for-you "unread" model: seed the WaitingForInput -> Idle timeout from settings.json
            // BEFORE the worker starts (the Settings cog re-pushes it on save). Default 60 minutes (1h);
            // 0 disables (the cog's "Never"). (A second tiny LoadAppSettings read happens below for the
            // hook files — both are one small-file read at process init.)
            e->scanner->SetWaitingDecayMinutes(LoadAppSettings().waitingForYouTimeoutMinutes);
            // Agentmaster (COMMANDS.md): the slash-command binding/await engine, attached BEFORE the
            // scanner worker starts (the feed pointer is read unsynchronized on the worker; thread
            // creation is the happens-before). The one v1 binding is /handover: when a managed
            // session's user types `/handover <context-or-filepath>`, await the handover markdown
            // the command instructs Claude to Write, then fan the resolved (sessionId, mdPath) out
            // to the per-window command-action sinks — the window hosting the origin tab spawns the
            // "<title> (handover)" successor session pointed at the file. The leaf preference
            // "handover" makes the command's own HANDOVER-*.md outrank an incidental doc edit in
            // the same message. Fires on the scanner thread; the sinks marshal to their own UI
            // dispatchers (the activate-sink idiom).
            e->commandWatch = std::make_shared<CommandWatch>();
            // Durable per-session command progress (COMMANDS.md §3a — restart resilience): the
            // fired watermark + armed markers ride the SessionStore KV (session-store/<sid>.json,
            // one small key), so a history replay after a restart/resume can neither re-fire an
            // already-processed /handover(-here) NOR lose one that was mid-await at shutdown.
            e->commandWatch->SetProgressStore(
                [](const std::wstring& sid) { return GetSessionStoreField(sid, kSessionStoreCommandProgressKey); },
                [](const std::wstring& sid, const std::wstring& enc) { SetSessionStoreField(sid, kSessionStoreCommandProgressKey, enc); });
            // The /handover family is USER-CUSTOMIZABLE (COMMANDS.md §6a — the cog's "Commands"
            // tab): each command's NAME and ENABLED come from settings.json (normalized +
            // collision-healed at load) and are applied HERE, once per process — a cog change
            // deliberately binds at the NEXT start only (the scanner worker reads the binding set
            // unsynchronized; engine-init registration before Start() is the happens-before).
            // The fan-out ACTION names stay the canonical L"handover"/L"handover-here"/
            // L"handover-standby" whatever the typed names are — the per-window sinks dispatch
            // on them (TerminalPage.AgentEngine.cpp), so a rename never touches the UI layer.
            const AppSettings cmdSettings = LoadAppSettings();
            std::wstring handoverCmdName = cmdSettings.commandHandoverName;
            std::wstring handoverHereCmdName = cmdSettings.commandHandoverHereName;
            std::wstring handoverStandbyCmdName = cmdSettings.commandHandoverStandbyName;
            ResolveCommandNameTriple(handoverCmdName, handoverHereCmdName, handoverStandbyCmdName); // belt — the load already heals
            // §6b file-match pattern: ONE regex shared by BOTH bindings (they are one await
            // family — a per-command pattern would split the §3 supersede family). Validated
            // ONCE here so a broken pattern is a logged fact, not a silent kill: invalid ⇒ pass
            // "" (the shipped "handover" leaf hint applies; the watch has the same fallback as
            // a second belt). Restart-applied by construction — bindings register once.
            std::wstring leafMatchRegex = cmdSettings.commandHandoverFileMatchRegex;
            if (!leafMatchRegex.empty() && !RegexIsValid(leafMatchRegex))
            {
                AppendStateLog(L"hooks.log", L"[engine] handover file-match regex INVALID - using the built-in 'handover' leaf hint: " + leafMatchRegex + L"\n");
                leafMatchRegex.clear();
            }
            // The legacy first-markdown tolerance (a mis-named single briefing file) rides ONLY
            // the SHIPPED rule: the default pattern (or a cleared/invalid one, both of which fall
            // back to the contains-hint) keeps it, while a CUSTOMIZED pattern turns it off — the
            // user has said exactly which files count, so an unrelated first `notes.md` must never
            // become the briefing. Only this layer can make that call: the default is now a real
            // setting VALUE, so the watch itself cannot tell default from customized.
            const bool allowFirstMarkdownFallback = leafMatchRegex.empty() || leafMatchRegex == kDefaultCommandFileMatchRegex;
            // A fire may carry SEVERAL markdown files (one command splitting its briefing); the
            // sink payload is one string, so the paths ride '|'-joined (JoinWatchPaths — '|' is
            // illegal in a real Windows path and IsSaneWatchPath rejects it per-path).
            if (cmdSettings.commandHandoverEnabled)
            {
                // `args` (the typed command's <command-args>) rides the fan-out verbatim — the
                // hosting window parses the §6b per-message model + title hints out of its leading
                // words at action time (ParseHandoverArgsHints), where the live settings are at hand.
                e->commandWatch->BindMarkdownAwait(handoverCmdName, L"handover", [](const std::wstring& sessionId, const std::vector<std::wstring>& mdPaths, const std::wstring& args) {
                    RaiseCommandActionInWindows(sessionId, L"handover", JoinWatchPaths(mdPaths), args);
                }, leafMatchRegex, allowFirstMarkdownFallback);
            }
            // /handover-here — the IN-PLACE twin: the SAME markdown await (same "handover" leaf
            // preference — its definition instructs the same `HANDOVER-<topic>.md` name), a
            // different fan-out action: the hosting window REPLACES the origin tab in place (the
            // Restart-session swap into a fresh "New Session Here -> Default" conversation)
            // instead of opening a successor tab beside it. The watch's binding lookup is
            // name-EXACT, so the two commands can never cross-fire (whatever they are named).
            if (cmdSettings.commandHandoverHereEnabled)
            {
                e->commandWatch->BindMarkdownAwait(handoverHereCmdName, L"handover", [](const std::wstring& sessionId, const std::vector<std::wstring>& mdPaths, const std::wstring& args) {
                    RaiseCommandActionInWindows(sessionId, L"handover-here", JoinWatchPaths(mdPaths), args);
                }, leafMatchRegex, allowFirstMarkdownFallback);
            }
            // /handover-standby — the FILL-NOT-SEND member (COMMANDS.md §5b): the SAME markdown
            // await + the SAME "handover" leaf hint (so all three commands stay ONE §3 supersede
            // family — a pivot between them re-aims the unsatisfied await), a third fan-out
            // action: the hosting window opens each file's successor like /handover but TYPES
            // the document into its input box WITHOUT submitting (one Enter away).
            if (cmdSettings.commandHandoverStandbyEnabled)
            {
                e->commandWatch->BindMarkdownAwait(handoverStandbyCmdName, L"handover", [](const std::wstring& sessionId, const std::vector<std::wstring>& mdPaths, const std::wstring& args) {
                    RaiseCommandActionInWindows(sessionId, L"handover-standby", JoinWatchPaths(mdPaths), args);
                }, leafMatchRegex, allowFirstMarkdownFallback);
            }
            e->scanner->SetCommandWatch(e->commandWatch);
            // The COMMAND DEFINITIONS (COMMANDS.md §6/§6a): without a definition under
            // <claude-config>/commands, a typed /command is rejected client-side and never reaches
            // the transcript. Reconcile BOTH files against the configured names/enables —
            // create-if-absent + the version-aware upgrade, PLUS the rename/disable migration: the
            // previously-materialized file is deleted when (and only when) it is byte-identical to
            // something we shipped (under whatever name — the digest is taken after normalizing
            // the custom name back), so the user's edited copy always sticks. Still the ONE write
            // Agentmaster makes outside its profile (additive, inert until typed). The
            // MATERIALIZED-NAME markers then RMW back into settings.json so the NEXT init knows
            // what to migrate (the reconcile logs the per-command outcome lines).
            {
                const auto [materializedHandover, materializedHandoverHere, materializedHandoverStandby] = ReconcileHandoverCommandFiles(cmdSettings);
                if (materializedHandover != cmdSettings.commandHandoverMaterializedName ||
                    materializedHandoverHere != cmdSettings.commandHandoverHereMaterializedName ||
                    materializedHandoverStandby != cmdSettings.commandHandoverStandbyMaterializedName)
                {
                    auto rmw = LoadAppSettings(); // freshest-disk RMW (the markers are the only fields we own here)
                    rmw.commandHandoverMaterializedName = materializedHandover;
                    rmw.commandHandoverHereMaterializedName = materializedHandoverHere;
                    rmw.commandHandoverStandbyMaterializedName = materializedHandoverStandby;
                    SaveAppSettings(rmw);
                }
            }
            e->scanner->Start();
            // SESSIONS_DB.md §7.6 (D16): the session database's refresh tick drains the scanner's transcript
            // watcher; it starts only when the setting asks for the database (Shadow / On), after the scanner (its
            // watcher is up), and Apply never throws (a failure is logged and leaves the app as it was).
            e->sessionDb = std::make_shared<SessionDbRefresher>(e->scanner, e->registry, AgentmasterStateDir());
            e->sessionDb->Apply(LoadAppSettings());
            // MESSAGING.md (D1): the Telegram bot, inside this process. Its window-hosted actions ride the per-window
            // sinks above (RequestSessionActionInWindows / RequestLaunchInWindow); restart and park reuse the existing
            // cross-window seams with an EMPTY source window, so the hosting window — whichever it is — acts. Off until
            // the settings say otherwise (Apply starts nothing while messagingEnabled is false, the token is missing
            // or nobody is paired); the Search factory is set by the app layer at its init (TerminalPage::
            // _InitAgentmasterEngine), since it needs app-only code. The version word (§21.18) is plain Win32 and is set
            // here — until 2026-10-18 this comment said the app layer set it too, and nothing did, so the start note never
            // named a version.
            {
                Msg::ServiceHooks hooks;
                hooks.sessionAction = [](const std::wstring& sid, const std::wstring& action, const std::wstring& arg, std::function<void(std::wstring)> reply) {
                    RequestSessionActionInWindows(sid, action, arg, std::move(reply));
                };
                hooks.launch = [](const RemoteLaunchRequest& req) { return RequestLaunchInWindow(req); };
                hooks.restartSession = [](const std::wstring& sid) { RestartSessionInOtherWindows(sid, L""); };
                hooks.deactivateSession = [](const std::wstring& sid) { DeactivateSessionInOtherWindows(sid, L""); };
                hooks.windowsRestoring = []() { return WindowsStillRestoring(); };
                hooks.versionWord = []() { return AppVersionWordForMessaging(); };
                e->messaging = std::make_shared<Msg::MessagingService>(e->registry, e->scheduler, AgentmasterStateDir(), std::move(hooks));
                e->messaging->Apply(LoadAppSettings());
            }
            // Keep the scanner ticking even with nothing live, so each window's liveness probe — which
            // also drives the Fleet Observer's per-window roster publish — keeps running (a hand-typed
            // `claude` in a fresh tab is then correlated out-of-band by the observer). The transcript-
            // discovery ENUMERATION this used to also start is retired (O7); the observer subsumes it.
            e->scanner->ArmDiscovery();
            {
                auto scan = e->scanner;
                e->registry->AddObserver([scan](const SessionInfo&, HookEvent) { scan->Wake(); });
            }

            const auto pipeName = HookPipeName(::GetCurrentProcessId());
            {
                auto reg = e->registry; // shared, captured by the sink
                e->bridge = std::make_shared<HooksBridge>(
                    pipeName,
                    [reg](const HookMessage& m) { reg->OnHookEvent(m); },
                    4);
            }
            e->bridge->Start();

            // (B+D+C — observe & control sessions we did NOT Launch): make a hand-typed `claude`
            // in any `+` tab self-wire for hooks and get adopted.
            //  * publish the live pipe to bridge.json (a forwarder that didn't inherit
            //    CCMGR_HOOK_PIPE can still find it);
            //  * export CCMGR_HOOK_PIPE on OUR process and prepend a transparent `claude` PATH
            //    shim — every `+` tab inherits our live process env block, so a bare `claude` there
            //    runs the shim, which adds `--settings <ourHooks>`. This inheritance ONLY holds
            //    because we force the profiles.defaults `reloadEnvironmentVariables` OFF (see
            //    CascadiaSettingsSerialization FixupUserSettings — [Agentmaster]): with WT's default
            //    env-reload ON, ConptyConnection rebuilds a child's env from the REGISTRY
            //    (til::env::regenerate), which DROPS these runtime-only vars and the shim is never
            //    hit. Launch/Restore does NOT go through the shim: it spawns the NATIVE claude.exe BY
            //    FULL PATH (resolved into e->claudeExePath below, before this PATH prepend, so a
            //    claude.cmd on PATH is followed to its real binary, not our shim). exe-only policy: a
            //    pure-Node `claude` resolves to empty and GATES the Manager. (CreateProcessW needs the
            //    full path — it appends only ".exe" and ignores PATHEXT, the 0x80070002 bug.)
            try
            {
                // Fleet Observer (OBSERVER.md §7): mint the per-process ownership stamp and export
                // it as AM_SESSION FIRST — before the best-effort shim/discovery setup below (which
                // can throw) — so EVERY ConPTY child is stamped even if the shim author fails. Every
                // tab (Launched or a hand-typed `+`) inherits our process env block (the same
                // mechanism that delivers CCMGR_HOOK_PIPE to a hand-typed claude — reloadEnviron-
                // mentVariables forced OFF), so its claude.exe carries AM_SESSION and the observer
                // classifies it RunningApp::Agentmaster. NewSessionId() is just a plain lowercase
                // hyphenated GUID generator (CoCreateGuid).
                e->amSession = NewSessionId();
                ::SetEnvironmentVariableW(L"AM_SESSION", e->amSession.c_str());
                AppendStateLog(L"hooks.log", L"[engine] AM_SESSION " + e->amSession + L"\n");

                WriteBridgeDiscovery(pipeName);
                const auto stateDir = AgentmasterStateDir();
                // Resolve the NATIVE claude.exe NOW (native-exe-only policy), while PATH is still
                // un-mutated so a claude.cmd on PATH is followed to its REAL binary, not the adoption
                // shim we author next. Honors the Settings override; ALWAYS a real claude.exe (or
                // empty). Empty => "Claude not detected" and the Manager gates every claude
                // interaction. Launched by full path (CreateProcessW appends only ".exe", ignores
                // PATHEXT — a bare `claude` would miss the npm install, the 0x80070002 bug).
                uint64_t _ts = ::GetTickCount64();
                e->claudeExePath = ResolveClaudeExe(LoadAppSettings().claudeExePath);
                Startup::Phase(L"  ResolveClaudeExe (PATH scan)", ::GetTickCount64() - _ts);
                AppendStateLog(L"hooks.log", L"[engine] claude.exe: " + (e->claudeExePath.empty() ? std::wstring{ L"<not detected>" } : e->claudeExePath) + L"\n");
                // The shared hook files come AFTER the claude resolve (HOOKS.md "Hook invocation"): the
                // claude.exe version picks the invocation, and exec form (PowerShell spawned directly, no Git
                // Bash / BASH_ENV) needs claude >= 2.1.139. The PATH shim below points every hand-typed claude
                // at this same file, so this first write must already carry the right form. It used to be
                // written first, with no launcher known, which always meant shell form.
                _ts = ::GetTickCount64();
                const auto hookFiles = MaterializeSharedHookFiles(stateDir, LoadAppSettings(), e->claudeExePath);
                Startup::Phase(L"  materialize hook files", ::GetTickCount64() - _ts);
                // Resolve the codex launcher too (managed-Codex support), while PATH is still pristine.
                // Same CreateProcessW/PATHEXT hazard as claude (a bare `codex` misses an npm codex.cmd ->
                // 0x80070002), but Codex is NOT native-exe-only — the observer finds codex.exe as a
                // descendant — so a .cmd/.bat launcher is accepted (BuildCodexCommandline runs it via
                // `cmd /c`). Empty => not found; the spawn falls back to the bare token and surfaces the error.
                _ts = ::GetTickCount64();
                e->codexExePath = ResolveCodexLauncher();
                Startup::Phase(L"  ResolveCodexLauncher (PATH scan)", ::GetTickCount64() - _ts);
                AppendStateLog(L"hooks.log", L"[engine] codex: " + (e->codexExePath.empty() ? std::wstring{ L"<not detected>" } : e->codexExePath) + L"\n");
                // Resolve the PowerShell host that wraps every managed agent session (so quitting the
                // agent drops to a live pwsh prompt at the cwd — BuildPwshHostedCommandline). pwsh.exe
                // (PS7) on PATH, else Windows PowerShell; empty => the spawn falls back to the bare token.
                _ts = ::GetTickCount64();
                e->pwshExePath = ResolvePwshLauncher();
                Startup::Phase(L"  ResolvePwshLauncher (PATH scan)", ::GetTickCount64() - _ts);
                AppendStateLog(L"hooks.log", L"[engine] pwsh host: " + (e->pwshExePath.empty() ? std::wstring{ L"<not detected>" } : e->pwshExePath) + L"\n");
                // Author the adoption shim BEFORE touching PATH (so ResolveRealClaude inside it never
                // finds our own shim), then prepend the shim dir for hand-typed `+`-tab self-wiring.
                _ts = ::GetTickCount64();
                const auto shimDir = MaterializeClaudeShim(stateDir, hookFiles.first);
                Startup::Phase(L"  materialize claude shim", ::GetTickCount64() - _ts);
                ::SetEnvironmentVariableW(L"CCMGR_HOOK_PIPE", pipeName.c_str());
                if (!shimDir.empty())
                {
                    std::wstring path;
                    const DWORD need = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
                    if (need > 1)
                    {
                        path.resize(need);
                        const DWORD got = ::GetEnvironmentVariableW(L"PATH", path.data(), need);
                        path.resize(got);
                    }
                    const std::wstring newPath = shimDir + L";" + path;
                    ::SetEnvironmentVariableW(L"PATH", newPath.c_str());
                    AppendStateLog(L"hooks.log", L"[engine] claude shim on PATH: " + shimDir + L"\n");
                }
            }
            catch (...)
            {
                // Forensics: this net spans the whole one-time engine bring-up (hook files, the
                // claude/codex/pwsh launcher resolution, the adoption shim, the CCMGR_HOOK_PIPE +
                // PATH exports). A throw part-way leaves the engine SILENTLY half-initialized for
                // the entire run — the [engine] lines above just stop, with no reason recorded.
                LogSwallowedException(L"Engine init (hook files / launchers / shim / PATH)");
            }

            // Fleet Observer S-lane (OBSERVER.md §8): the process-wide PULL census/correlation
            // worker, next to the scanner. Reads each claude.exe's PEB out-of-band, classifies
            // ownership via the AM_SESSION minted above, and (once a window publishes its tab roster)
            // correlates + feeds the registry — the always-correct floor beneath the lossy hook push.
            // Constructed AFTER the try block so it gets the real e->amSession (set at the top of the
            // try, before the throwing shim I/O). Never torn down (process lifetime), like the rest.
            {
                Startup::ScopedPhase _t{ L"  observer start (S-lane)" };
                e->observer = std::make_shared<ProcessObserver>(e->registry, e->amSession);
                // §13a orphan reaper: seed the on/off from disk before the first survey (the cog's Save
                // pushes a change live via SetReapOrphanedSessions). Default ON — a stamped session whose
                // console host is dead is unreachable by construction, so ending it loses nothing.
                e->observer->SetReapOrphanedSessions(LoadAppSettings().reapOrphanedSessions);
                e->observer->Start();
            }

            // Agentmaster (tags perf, 2026-10-10): read the whole session store ONCE, in the background.
            // Every whole-store read (the Tags panel's tag universe, the tag hover panel's carrier list,
            // the Sessions browser's titles / favorites / tags) goes through the store's scan cache: after
            // one complete pass, a read lists the directory and opens only the records that changed. But
            // that FIRST pass opens every record file — 2,181 on the release profile — and until now the Tags
            // panel made it on the UI thread the first time it opened (the user reported a freeze of more
            // than 5 s there). This thread makes it here instead, so the first panel of a run normally finds the
            // store warm. A panel opened before the pass is done does not wait for this thread and does
            // not scan on the UI thread either: it shows "Loading tags..." and runs its own pass on a pool
            // thread (TerminalPage::_WarmTagStoreThenRefresh).
            // NORMAL priority, like the other engine threads, and on purpose: the pass takes the store's
            // cache lock once per record, and the UI thread takes the same lock for every per-card read.
            // A low-priority thread that is preempted while it holds that lock, on a machine whose cores
            // are all busy, would make the UI thread wait until the scheduler runs it again — a stall at
            // start-up, to save a pass that needs ~0.15 s of CPU (measured: 141 ms for 2,181 records with
            // a warm disk cache) and otherwise waits for the disk. It should also finish soon: the sooner
            // the store is warm, the smaller the window in which a panel has to load.
            // Detached, like the UI-stall watchdog: it touches only process-lifetime state (the store's
            // caches, the log), and the engine is never torn down. Inert in the test harness, which never
            // calls SharedEngine().
            // A thread that cannot start (std::system_error) or a pass that throws costs only the head
            // start: both are logged (Rule #18), and the first panel then loads the store itself.
            try
            {
                std::thread([]() noexcept {
                    try
                    {
                        const uint64_t startedAt = ::GetTickCount64();
                        const uint64_t readsBefore = SessionStoreRecordFileReads();
                        const size_t records = WarmSessionStore();
                        // "file read(s)" is the process-wide counter's move during the pass: a per-session
                        // read by another thread in those moments is counted too, so on a run that
                        // restores tagged tabs it can exceed the record count by a few.
                        AppendStateLog(L"hooks.log",
                                       L"[engine] session store warmed: " + std::to_wstring(records) + L" record(s), " +
                                           std::to_wstring(SessionStoreRecordFileReads() - readsBefore) + L" file read(s), " +
                                           std::to_wstring(::GetTickCount64() - startedAt) + L" ms\n");
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"session-store warm-up (the pass)");
                    }
                }).detach();
            }
            catch (...)
            {
                LogSwallowedException(L"session-store warm-up (thread start)");
            }

            AppendStateLog(L"hooks.log", L"[engine] bridge listening on " + pipeName + L"\n");
            return e;
        }();

        return *g;
    }

    bool ClaudeAvailable()
    {
        return !SharedEngine().claudeExePath.empty();
    }

    bool EnsureClaudeAvailable()
    {
        auto& e = SharedEngine();
        if (!e.claudeExePath.empty())
        {
            return true; // already detected — cheap cache hit, no re-scan
        }
        // Cache is empty ("Claude not detected"). The user may have just run `claude install` (or put a
        // claude.exe on PATH) while the app stayed open, so re-resolve ONCE before we conclude it's
        // missing — honoring the Settings override exactly like RefreshClaudeExe. If it now resolves,
        // the caller proceeds and the install prompt never shows; the gate has self-healed. UI-thread-
        // called (every launch handler is), so this shares RefreshClaudeExe's single-writer contract.
        e.claudeExePath = ResolveClaudeExe(LoadAppSettings().claudeExePath);
        if (!e.claudeExePath.empty())
        {
            AppendStateLog(L"hooks.log", L"[engine] claude.exe found on re-check: " + e.claudeExePath + L"\n");
        }
        return !e.claudeExePath.empty();
    }

    std::wstring RefreshClaudeExe(std::wstring_view overridePath)
    {
        // Re-resolve when the Settings override changes, so a Browse/override takes effect without a
        // restart. UI-thread-called (settings save); claudeExePath has no other writer after init.
        auto& e = SharedEngine();
        e.claudeExePath = ResolveClaudeExe(overridePath);
        AppendStateLog(L"hooks.log", L"[engine] claude.exe refreshed: " + (e.claudeExePath.empty() ? std::wstring{ L"<not detected>" } : e.claudeExePath) + L"\n");
        return e.claudeExePath;
    }

    ClaudeInstallState ClaudeInstallKind()
    {
        // A native claude.exe is already resolved (the not-found UI won't show, but be honest if asked).
        if (!SharedEngine().claudeExePath.empty())
        {
            return ClaudeInstallState::Native;
        }
        // No native exe. A real npm/Node claude.cmd/.bat on PATH (excluding our shim) => LegacyNpm (offer
        // `claude install`); nothing at all => None (offer the claude.ai native bootstrap).
        return NpmClaudeLauncherOnPath().empty() ? ClaudeInstallState::None : ClaudeInstallState::LegacyNpm;
    }

    bool LaunchClaudeInstall(ClaudeInstallState state)
    {
        // Build the inner PowerShell command. Everything here is SINGLE-quoted so the whole command can be
        // wrapped once in double quotes for `-Command "..."` with no nested-quote conflict (the Windows
        // command-line quoting trap). A launcher path with a literal ' is escaped PowerShell-style ('' ).
        std::wstring inner;
        if (state == ClaudeInstallState::LegacyNpm)
        {
            std::wstring launcher = NpmClaudeLauncherOnPath();
            if (launcher.empty())
            {
                // The npm launcher vanished between detection and click — fall back to the native bootstrap.
                inner = L"irm https://claude.ai/install.ps1 | iex";
            }
            else
            {
                std::wstring escaped;
                escaped.reserve(launcher.size() + 8);
                for (const wchar_t c : launcher)
                {
                    escaped.push_back(c);
                    if (c == L'\'')
                    {
                        escaped.push_back(L'\''); // PowerShell single-quote escape: ' -> ''
                    }
                }
                // Run the REAL npm launcher by full path (bypasses our --settings shim) to migrate to native.
                inner = L"& '" + escaped + L"' install";
            }
        }
        else
        {
            // None (and any defensive default): the official claude.ai native install (the ONLY online
            // source per the setup docs). `irm` = Invoke-RestMethod, `iex` = Invoke-Expression.
            inner = L"irm https://claude.ai/install.ps1 | iex";
        }

        const std::wstring psCommand =
            L"Write-Host 'Agentmaster: installing Claude Code (native build)...' -ForegroundColor Cyan; " +
            inner +
            L"; Write-Host ''; Write-Host 'When it finishes, return to Agentmaster and click Re-check.' -ForegroundColor Green";

        // Prefer the pwsh 7 we already resolved; else Windows PowerShell (always present, honors irm/iex).
        std::wstring shell = SharedEngine().pwshExePath;
        if (shell.empty())
        {
            shell = L"powershell.exe";
        }
        // -NoProfile: fast + predictable; -ExecutionPolicy Bypass: allow the irm|iex bootstrap; -NoExit:
        // keep the window so the user can read the result before clicking Re-check.
        const std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -NoExit -Command \"" + psCommand + L"\"";

        AppendStateLog(L"hooks.log",
                       L"[claude-install] launching " + std::wstring{ state == ClaudeInstallState::LegacyNpm ? L"npm-migrate (claude install)" : L"native-bootstrap (claude.ai/install.ps1)" } + L" via " + shell + L"\n");

        const auto rc = reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", shell.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL));
        if (rc <= 32)
        {
            AppendStateLog(L"hooks.log", L"[claude-install] ShellExecute failed (rc=" + std::to_wstring(rc) + L")\n");
            return false;
        }
        return true;
    }

    std::optional<WindowRecord> ClaimWindowRecord()
    {
        return ClaimWindowRecordIn(SharedEngine());
    }

    // Agentmaster (test seam — the SessionStore `...In` idiom). The window-record / manifest /
    // reserve primitives below are parameterized on the Engine INSTANCE; the public functions
    // delegate with SharedEngine() and stay the one production entry. The standalone harness
    // (tests/run-m5-tests.bat, which links this TU) pins the PERSISTENCE.md §13.5 invariants on a
    // LOCAL Engine value instead — it must NEVER touch SharedEngine(): its first access wires and
    // STARTS the bridge/observer/scheduler, and in the test process the bridge would collide with
    // TestBridgeRoundTrip's own pipe on the same `\\.\pipe\agentmaster.<pid>` name.
    std::optional<WindowRecord> ClaimWindowRecordIn(Engine& e)
    {
        std::lock_guard<std::mutex> lk(e.windowMutex);
        if (!e.windowRecordsLoaded)
        {
            // Load every windows/*.json once. Front-to-back claim order is whatever the
            // directory iterator yields; multi-window restore (PERSISTENCE.md §13.0) will impose
            // a deterministic order when it assigns records to windows.
            e.unclaimedWindowRecords = LoadWindowRecords();
            e.windowRecordsLoaded = true;
        }
        if (e.unclaimedWindowRecords.empty())
        {
            return std::nullopt;
        }
        auto rec = std::move(e.unclaimedWindowRecords.front());
        e.unclaimedWindowRecords.erase(e.unclaimedWindowRecords.begin());
        return rec;
    }

    std::optional<WindowRecord> ClaimWindowRecord(const std::wstring& windowId)
    {
        return ClaimWindowRecordIn(SharedEngine(), windowId);
    }

    std::optional<WindowRecord> ClaimWindowRecordIn(Engine& e, const std::wstring& windowId)
    {
        if (windowId.empty())
        {
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lk(e.windowMutex);
        if (!e.windowRecordsLoaded)
        {
            e.unclaimedWindowRecords = LoadWindowRecords();
            e.windowRecordsLoaded = true;
        }
        // 1) The startup claim pool (records present at launch, not yet claimed) — the decline-at-startup
        //    recover path lands here.
        for (auto it = e.unclaimedWindowRecords.begin(); it != e.unclaimedWindowRecords.end(); ++it)
        {
            if (it->windowId == windowId)
            {
                auto rec = std::move(*it);
                e.unclaimedWindowRecords.erase(it);
                return rec;
            }
        }
        // 2) The reclaimable pool (records claimed-then-closed THIS session, returned by
        //    UnregisterLiveWindow). This is what lets the in-session "Reopen Windows" button re-claim a
        //    window it earlier closed — restoring its real id + lens — instead of minting a fresh,
        //    lens-less duplicate. find+erase is atomic under the lock, so two windows never share a
        //    record (a double-dispatch's second claim misses and mints, as intended).
        for (auto it = e.reclaimableWindowRecords.begin(); it != e.reclaimableWindowRecords.end(); ++it)
        {
            if (it->windowId == windowId)
            {
                auto rec = std::move(*it);
                e.reclaimableWindowRecords.erase(it);
                return rec;
            }
        }
        return std::nullopt;
    }

    void RegisterLiveWindow(const std::wstring& windowId)
    {
        RegisterLiveWindowIn(SharedEngine(), windowId);
    }

    void RegisterLiveWindowIn(Engine& e, const std::wstring& windowId)
    {
        if (windowId.empty())
        {
            return;
        }
        std::lock_guard<std::mutex> lk(e.windowMutex);
        e.liveWindowIds.insert(windowId);
        // The window's first-layout restore starts after this registration (TerminalPage::_InitAgentmasterEngine
        // runs before _RestoreWindowTabs in _OnFirstLayout); it leaves the set in NoteWindowRestoreDoneIn.
        e.restoringWindowIds.insert(windowId);
        // A live window always has a record on disk (the page's autosave), so writing the manifest now
        // means even a single-window session (which never triggers a remove-with-remaining) is recorded
        // — so it reopens at its geometry/lens next run (the shipped Increment-2 behavior).
        SaveOpenWindows({ e.liveWindowIds.begin(), e.liveWindowIds.end() });
    }

    void UnregisterLiveWindow(const std::wstring& windowId)
    {
        UnregisterLiveWindowIn(SharedEngine(), windowId);
    }

    void NoteWindowRestoreDone(const std::wstring& windowId)
    {
        NoteWindowRestoreDoneIn(SharedEngine(), windowId);
    }

    void NoteWindowRestoreDoneIn(Engine& e, const std::wstring& windowId)
    {
        if (windowId.empty())
        {
            return;
        }
        std::lock_guard<std::mutex> lk(e.windowMutex);
        e.restoringWindowIds.erase(windowId); // unknown id / second call: nothing to erase, by design
    }

    size_t WindowsStillRestoring()
    {
        return WindowsStillRestoringIn(SharedEngine());
    }

    size_t WindowsStillRestoringIn(Engine& e)
    {
        std::lock_guard<std::mutex> lk(e.windowMutex);
        return e.restoringWindowIds.size();
    }

    // ---- Agentmaster: UI-thread stall watchdog (hooks.log [ui-stall]) --------------------------
    // See the Engine.h declaration for the why. The state is a LEAKED function-local static, never
    // an Engine member: the detached checker thread reads it up to process exit, and process
    // teardown must not race a destructor (the AppendStateLog idiom). Lazy: nothing spawns until a
    // real UI lane beats at least once, so the test harness / CLI (which link this TU) stay clean.
    namespace
    {
        struct UiBeatState
        {
            std::mutex m;
            std::unordered_map<std::wstring, uint64_t> beats; // windowId -> last UI-lane beat (GetTickCount64 ms)
            struct Stall
            {
                uint64_t staleBeatMs = 0; // the beat the stall was declared against (for the recovery duration)
                uint64_t lastLogMs = 0; // the last [ui-stall] line for this window (re-log throttle)
                UiStallEscalation esc; // the rescue/restart ladder (the Engine.h escalation block)
                bool noRescuerLogged = false; // "no rescuer registered" logged once per episode
            };
            std::unordered_map<std::wstring, Stall> stalled;
            std::unordered_map<std::wstring, UiStallEpisodeHistory> episodes; // windowId -> drain-assisted recovery history (the LIMP guard, Engine.h)
            std::unordered_map<std::wstring, std::function<UiRescueOutcome()>> rescuers; // windowId -> app-side rescue probe
            std::function<void(const std::wstring&)> restartHandler; // process-wide, registered once by the app layer
            bool restartFired = false; // once per process — the handler terminates us, a second decision is meaningless
            uint64_t lastRestartVetoLogMs = 0; // "restart decided but vetoed" re-log throttle (covers BOTH the deep-stall and limp paths)
            bool checkerStarted = false;
        };

        UiBeatState& UiBeats()
        {
            static UiBeatState* s = new UiBeatState(); // deliberately leaked (see the block comment above)
            return *s;
        }

        void UiStallCheckerPass(UiBeatState& s)
        {
            constexpr uint64_t kRelogMs = 60 * 1000;
            const uint64_t now = ::GetTickCount64();
            std::vector<std::wstring> lines; // log OUTSIDE the lock (AppendStateLog does file I/O)
            struct RescueJob
            {
                std::wstring id;
                std::function<UiRescueOutcome()> fn;
                int attemptNo = 0;
            };
            std::vector<RescueJob> rescues; // run OUTSIDE the lock (each does a bounded SendMessageTimeout into the UI thread)
            bool restartCandidate = false;
            std::wstring restartReason;
            {
                std::lock_guard<std::mutex> lk(s.m);
                // Restart is only ever considered when EVERY window that has beaten is stalled — a
                // half-alive process (one window's lane dead, another's fine) is not the process-wide
                // dispatch wedge this ladder exists for.
                bool allStalled = !s.beats.empty();
                for (const auto& [id, beat] : s.beats)
                {
                    if (now - beat < kUiStallDeclareMs)
                    {
                        allStalled = false;
                        break;
                    }
                }
                for (const auto& [id, beat] : s.beats)
                {
                    const uint64_t quiet = now - beat;
                    const auto st = s.stalled.find(id);
                    if (quiet < kUiStallDeclareMs)
                    {
                        if (st != s.stalled.end())
                        {
                            const int episodeRescues = st->second.esc.rescueAttempts;
                            lines.push_back(L"[ui-stall] window " + id + L" RECOVERED after ~" +
                                            std::to_wstring((beat - st->second.staleBeatMs) / 1000) + L"s — the UI thread is pumping again" +
                                            (episodeRescues > 0 ? L" (drain-assisted, " + std::to_wstring(episodeRescues) + L" rescue(s) this episode)" : L"") + L"\n");
                            s.stalled.erase(st);
                            if (episodeRescues > 0)
                            {
                                // The LIMP guard (Engine.h): a drain-assisted recovery may just be
                                // the queue being force-pumped while delivery stays latched — count
                                // it, and treat repeated ones as an unrecoverable wedge.
                                if (NoteUiStallRescuedRecovery(s.episodes[id], now))
                                {
                                    restartCandidate = true;
                                    if (!restartReason.empty())
                                    {
                                        restartReason += L", ";
                                    }
                                    restartReason += id + L" LIMPING — " + std::to_wstring(s.episodes[id].rescuedRecoveries) +
                                                     L" drain-assisted recoveries in " +
                                                     std::to_wstring((now - s.episodes[id].firstMs) / 1000) + L"s (delivery latched, drains only force-pump it)";
                                }
                            }
                            else
                            {
                                s.episodes.erase(id); // an ORGANIC recovery — the dispatcher healed itself
                            }
                        }
                        continue;
                    }
                    if (st == s.stalled.end())
                    {
                        auto& fresh = s.stalled[id];
                        fresh = UiBeatState::Stall{ beat, now };
                        // Escalation clocks run on OBSERVED spans (this pass's stamp), never on the
                        // beat's age — GetTickCount64 counts through standby, so a wake-from-sleep
                        // sees a huge quiet span but zero observed span (Engine.h block comment).
                        fresh.esc.firstObservedMs = now;
                        lines.push_back(L"[ui-stall] window " + id + L" UI thread has NOT pumped for ~" +
                                        std::to_wstring(quiet / 1000) +
                                        L"s — dispatcher items are not draining (the window reads Not Responding while the engine lanes continue; sample the process's FIRST thread's CPU + stack to see what it is grinding on)\n");
                        continue; // observed span is 0 this pass — the ladder can't fire yet by construction
                    }
                    if (now - st->second.lastLogMs >= kRelogMs)
                    {
                        st->second.lastLogMs = now;
                        lines.push_back(L"[ui-stall] window " + id + L" still stalled — ~" +
                                        std::to_wstring(quiet / 1000) + L"s without a UI-lane beat\n");
                    }
                    // The escalation ladder (Engine.h). Decide with restartEnabled=true here — the
                    // SETTING is read at execution time (outside the lock, rare path), so a veto
                    // never latches restartFired and a later settings flip still takes effect.
                    auto& stall = st->second;
                    const auto action = DecideUiStallAction(now, quiet, stall.esc, allStalled, true, s.restartFired);
                    if (action == UiStallAction::Rescue)
                    {
                        const auto r = s.rescuers.find(id);
                        if (r == s.rescuers.end())
                        {
                            if (!stall.noRescuerLogged)
                            {
                                stall.noRescuerLogged = true;
                                lines.push_back(L"[ui-stall] window " + id + L" rescue skipped — no rescuer registered\n");
                            }
                        }
                        else
                        {
                            // Stamp BEFORE the (out-of-lock) run so a slow/throwing rescuer can
                            // never hot-loop the ladder.
                            stall.esc.lastRescueMs = now;
                            stall.esc.rescueAttempts += 1;
                            rescues.push_back(RescueJob{ id, r->second, stall.esc.rescueAttempts });
                        }
                    }
                    else if (action == UiStallAction::Restart)
                    {
                        restartCandidate = true;
                        if (!restartReason.empty())
                        {
                            restartReason += L", ";
                        }
                        restartReason += id + L" stalled ~" + std::to_wstring(quiet / 1000) + L"s (observed ~" +
                                         std::to_wstring((now - stall.esc.firstObservedMs) / 1000) + L"s, rescues " +
                                         std::to_wstring(stall.esc.rescueAttempts) + L")";
                    }
                }
            }
            for (const auto& l : lines)
            {
                AppendStateLog(L"hooks.log", l);
            }
            for (auto& job : rescues)
            {
                UiRescueOutcome outcome{};
                try
                {
                    outcome = job.fn();
                }
                catch (...)
                {
                    LogSwallowedException(L"UiStallRescue");
                }
                {
                    std::lock_guard<std::mutex> lk(s.m);
                    const auto st = s.stalled.find(job.id);
                    if (st != s.stalled.end())
                    {
                        st->second.esc.lastPumpAlive = outcome.pumpAlive;
                    }
                }
                AppendStateLog(L"hooks.log",
                               L"[ui-stall] rescue #" + std::to_wstring(job.attemptNo) + L" window " + job.id +
                                   L": pump=" + (outcome.pumpAlive ? L"ALIVE" : L"DEAD (probe timed out — a blocked UI thread, not the dispatch wedge; never auto-restarted)") +
                                   L" drain=" + (outcome.drainRan ? L"ran (if the dispatcher revived, the next beat clears this stall)" : L"did not run") + L"\n");
            }
            if (restartCandidate)
            {
                bool enabled = true; // an unreadable settings.json keeps the DEFAULT (restart ON) — the safe direction for a dead UI
                try
                {
                    enabled = LoadAppSettings().uiStallAutoRestart;
                }
                catch (...)
                {
                    LogSwallowedException(L"UiStallRestart settings read");
                }
                const bool debuggerAttached = ::IsDebuggerPresent() != 0;
                std::function<void(const std::wstring&)> handler;
                bool logVeto = false;
                bool logDebugger = false;
                bool latchedNow = false;
                {
                    std::lock_guard<std::mutex> lk(s.m);
                    if (s.restartFired)
                    {
                        // raced by a previous pass's in-flight handler — nothing to do
                    }
                    else if (!enabled)
                    {
                        // Throttled by time (not per-Stall state): the LIMP path decides restart at
                        // the moment its window RECOVERS — its Stall entry is already erased, so a
                        // per-Stall flag would silently swallow the veto line there.
                        if (now - s.lastRestartVetoLogMs >= 60 * 1000)
                        {
                            s.lastRestartVetoLogMs = now;
                            logVeto = true;
                        }
                    }
                    else if (debuggerAttached)
                    {
                        logDebugger = true;
                    }
                    else
                    {
                        s.restartFired = true;
                        latchedNow = true;
                        handler = s.restartHandler;
                    }
                }
                if (logVeto)
                {
                    AppendStateLog(L"hooks.log", L"[ui-stall] restart decided but uiStallAutoRestart is OFF — staying up (" + restartReason + L")\n");
                }
                if (logDebugger)
                {
                    AppendStateLog(L"hooks.log", L"[ui-stall] restart decided but a debugger is attached — staying up (" + restartReason + L")\n");
                }
                if (handler)
                {
                    AppendStateLog(L"hooks.log", L"[ui-stall] UNRECOVERABLE dispatch wedge — RESTARTING the app (" + restartReason + L"); sessions/windows/drafts are autosaved on disk and will restore\n");
                    try
                    {
                        handler(restartReason);
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"UiStallRestartHandler");
                    }
                }
                else if (latchedNow)
                {
                    AppendStateLog(L"hooks.log", L"[ui-stall] restart decided but no restart handler is registered — staying up (" + restartReason + L")\n");
                }
            }
        }
    }

    void NoteUiHeartbeat(const std::wstring& windowId)
    {
        if (windowId.empty())
        {
            return;
        }
        auto& s = UiBeats();
        bool startChecker = false;
        {
            std::lock_guard<std::mutex> lk(s.m);
            s.beats[windowId] = ::GetTickCount64();
            if (!s.checkerStarted)
            {
                s.checkerStarted = true;
                startChecker = true;
            }
        }
        if (startChecker)
        {
            try
            {
                std::thread([]() {
                    auto& st = UiBeats();
                    for (;;)
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                        try
                        {
                            UiStallCheckerPass(st);
                        }
                        catch (...)
                        {
                            LogSwallowedException(L"UiStallCheckerPass");
                        }
                    }
                }).detach();
                AppendStateLog(L"hooks.log", L"[ui-stall] watchdog armed (20s threshold, 5s cadence)\n");
            }
            catch (...)
            {
                // Thread spawn failed — un-latch so a later beat retries instead of silently
                // running with no checker forever.
                std::lock_guard<std::mutex> lk(s.m);
                s.checkerStarted = false;
                LogSwallowedException(L"NoteUiHeartbeat spawn checker");
            }
        }
    }

    void DropUiHeartbeat(const std::wstring& windowId)
    {
        auto& s = UiBeats();
        std::lock_guard<std::mutex> lk(s.m);
        s.beats.erase(windowId);
        s.stalled.erase(windowId);
        s.episodes.erase(windowId);
        s.rescuers.erase(windowId); // belt — the page unregisters explicitly, but a dropped window must never be rescued
    }

    // Agentmaster: the LIMP guard's counting rule (Engine.h). PURE mutation of the caller-owned
    // history: a recovery outside the window re-opens it at 1; inside it accumulates, and crossing
    // kUiStallMaxRescuedEpisodes says the dispatcher is limping, not healed.
    bool NoteUiStallRescuedRecovery(UiStallEpisodeHistory& h, const uint64_t nowMs) noexcept
    {
        if (h.firstMs == 0 || nowMs < h.firstMs || nowMs - h.firstMs > kUiStallEpisodeWindowMs)
        {
            h.rescuedRecoveries = 1;
            h.firstMs = nowMs;
            return false;
        }
        h.rescuedRecoveries += 1;
        return h.rescuedRecoveries >= kUiStallMaxRescuedEpisodes;
    }

    // Agentmaster: the UI-stall escalation decision (Engine.h block comment). PURE — the checker
    // pass owns every clock stamp; this only reads them, so the harness can drive the whole ladder.
    UiStallAction DecideUiStallAction(const uint64_t nowMs, const uint64_t quietMs, const UiStallEscalation& e, const bool allWindowsStalled, const bool restartEnabled, const bool restartAlreadyFired) noexcept
    {
        if (quietMs < kUiStallDeclareMs || e.firstObservedMs == 0 || nowMs < e.firstObservedMs)
        {
            return UiStallAction::None;
        }
        // The ladder runs on the OBSERVED span (stamped by a live checker pass), never on the beat's
        // age — the wake-from-sleep safety (GetTickCount64 counts through standby).
        const uint64_t observed = nowMs - e.firstObservedMs;
        if (observed >= kUiStallRestartAfterMs && e.rescueAttempts >= kUiStallRestartMinRescues &&
            e.lastPumpAlive && allWindowsStalled && restartEnabled && !restartAlreadyFired)
        {
            return UiStallAction::Restart;
        }
        if (observed >= kUiStallRescueAfterMs &&
            (e.lastRescueMs == 0 || nowMs - e.lastRescueMs >= kUiStallRescueRetryMs))
        {
            return UiStallAction::Rescue;
        }
        return UiStallAction::None;
    }

    void RegisterUiStallRescuer(const std::wstring& windowId, std::function<UiRescueOutcome()> rescuer)
    {
        if (windowId.empty() || !rescuer)
        {
            return;
        }
        auto& s = UiBeats();
        std::lock_guard<std::mutex> lk(s.m);
        s.rescuers[windowId] = std::move(rescuer);
    }

    void UnregisterUiStallRescuer(const std::wstring& windowId)
    {
        auto& s = UiBeats();
        std::lock_guard<std::mutex> lk(s.m);
        s.rescuers.erase(windowId);
    }

    void SetUiStallRestartHandler(std::function<void(const std::wstring& reason)> handler)
    {
        auto& s = UiBeats();
        std::lock_guard<std::mutex> lk(s.m);
        s.restartHandler = std::move(handler);
    }

    void UnregisterLiveWindowIn(Engine& e, const std::wstring& windowId)
    {
        if (windowId.empty())
        {
            return;
        }
        // Agentmaster: the closing window's UI thread stops beating BY DESIGN — take it off the
        // stall watchdog before anything else so teardown can never be reported as a hang.
        DropUiHeartbeat(windowId);
        // Re-read the closing window's record from disk BEFORE the lock (disk I/O off the mutex). It is
        // returned to the reclaimable pool below so the in-session recover button can re-claim it.
        // Absent on disk (a window closed before its first autosave) => nothing to re-claim, fine.
        auto reclaim = LoadWindowRecord(windowId);

        std::lock_guard<std::mutex> lk(e.windowMutex);
        e.liveWindowIds.erase(windowId);
        e.restoringWindowIds.erase(windowId); // a window closed mid-restore must not hold the bot's deletes forever
        // Agentmaster (discard Manager-only windows): clear any Manager-only self-close reservation this
        // window held, so the set never accumulates stale ids across the process lifetime.
        e.closingWindowIds.erase(windowId);
        // Skip-empty: the LAST window's teardown must NOT clear the manifest, or "open at exit" would
        // always be empty. Leaving the prior snapshot means the next run reopens what was open when the
        // app exited — for a one-by-one close that is the final window; a hard shutdown that kills the
        // window threads before they unregister leaves the full set on disk (both are correct). §13.5.
        if (!e.liveWindowIds.empty())
        {
            SaveOpenWindows({ e.liveWindowIds.begin(), e.liveWindowIds.end() });
        }
        else if (e.messaging)
        {
            // MESSAGING.md §21.18: the LAST window just closed — the app is quitting, and it ends with TerminateProcess right
            // after, so nothing later can record a clean end. The Telegram bot stamps run.closed, and its next start note says
            // this run CLOSED rather than "did not close normally". A window set that is empty only for a moment (the last tab
            // torn out into a new window) stamps too; the bot's next marker write outdates such a stamp
            // (StartNoteRunClosedCleanly). Under the window mutex like the manifest write above: NoteAppClosing takes none of
            // the engine's locks and never throws.
            e.messaging->NoteAppClosing();
        }
        // Agentmaster (discard Manager-only windows): a mid-session-closed window whose record has NO
        // tab refs held nothing but the Manager tab — reopening it would reconstruct exactly what
        // "+ new window" gives (every window auto-creates the Manager tab), so the record is pure
        // noise: DELETE it instead of pooling it, and it stops accumulating in windows/ (and polluting
        // the next launch's front-pop claim order). Mirrors the manifest's skip-empty above: the LAST
        // window out (quit / single-window close, live set now empty) KEEPS its record even when
        // empty — that record is the open-at-exit snapshot the next launch claims for geometry + lens,
        // so a pure-Manager single-window workflow still reopens at its position. (RecoverableWindows
        // filters empty records anyway, so a kept-empty final record is never OFFERED for reopen.)
        if (reclaim && reclaim->tabs.empty() && !e.liveWindowIds.empty())
        {
            DeleteWindowRecord(windowId);
            reclaim.reset();
        }
        // Return the record to the reclaimable pool so a later in-session reopen (the recover button)
        // re-claims THIS record (real id + lens) rather than minting a duplicate. Dedup the re-add so a
        // double teardown can't stack two copies. The startup pool is left alone — reclaim is by id only.
        if (reclaim)
        {
            bool present = false;
            for (const auto& r : e.reclaimableWindowRecords)
            {
                if (r.windowId == windowId)
                {
                    present = true;
                    break;
                }
            }
            if (!present)
            {
                e.reclaimableWindowRecords.push_back(std::move(*reclaim));
            }
        }
    }

    std::vector<std::wstring> LiveWindowIds()
    {
        return LiveWindowIdsIn(SharedEngine());
    }

    std::vector<std::wstring> LiveWindowIdsIn(Engine& e)
    {
        std::lock_guard<std::mutex> lk(e.windowMutex);
        return { e.liveWindowIds.begin(), e.liveWindowIds.end() };
    }

    bool ReserveManagerOnlyClose(const std::wstring& windowId)
    {
        return ReserveManagerOnlyCloseIn(SharedEngine(), windowId);
    }

    bool ReserveManagerOnlyCloseIn(Engine& e, const std::wstring& windowId)
    {
        if (windowId.empty())
        {
            return false;
        }
        std::lock_guard<std::mutex> lk(e.windowMutex);
        // Effective remaining-live = live windows MINUS those already reserved to self-close this tick.
        // Reserve THIS window's close only while >1 would remain — so concurrent Manager-only windows on
        // different threads can never all close (each reserves under the lock, and the one that finds
        // remaining==1 stays). A window that reserved then closes clears its id in UnregisterLiveWindow.
        size_t remaining = 0;
        for (const auto& id : e.liveWindowIds)
        {
            if (e.closingWindowIds.find(id) == e.closingWindowIds.end())
            {
                ++remaining;
            }
        }
        if (remaining > 1)
        {
            e.closingWindowIds.insert(windowId);
            return true;
        }
        return false;
    }

    uint64_t RegisterWindowActivateHandler(const std::wstring& windowId, std::function<void(const std::wstring& sessionId)> handler)
    {
        if (!handler)
        {
            return 0;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.activateMutex);
        const auto token = e.nextActivateToken++;
        e.activateSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    void UnregisterWindowActivateHandler(uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.activateMutex);
        for (auto it = e.activateSinks.begin(); it != e.activateSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.activateSinks.erase(it);
                return;
            }
        }
    }

    void ActivateSessionInOtherWindows(const std::wstring& sessionId, const std::wstring& sourceWindowId)
    {
        if (sessionId.empty())
        {
            return;
        }
        auto& e = SharedEngine();
        // Snapshot under the lock, invoke outside it (the registry's _notify pattern): each sink
        // hops into its own window's dispatcher, and holding the engine lock across foreign-window
        // marshaling would be a needless ordering hazard.
        std::vector<std::function<void(const std::wstring&)>> sinks;
        {
            std::lock_guard<std::mutex> lk(e.activateMutex);
            sinks.reserve(e.activateSinks.size());
            for (const auto& s : e.activateSinks)
            {
                if (s.fn && s.windowId != sourceWindowId)
                {
                    sinks.push_back(s.fn);
                }
            }
        }
        for (const auto& fn : sinks)
        {
            fn(sessionId); // fire-and-forget; the (single) hosting window selects + foregrounds, the rest miss
        }
    }

    uint64_t RegisterActivateAllDormantHandler(const std::wstring& windowId, std::function<void()> handler)
    {
        if (!handler)
        {
            return 0;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.activateAllMutex);
        const auto token = e.nextActivateAllToken++;
        e.activateAllSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    void UnregisterActivateAllDormantHandler(uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.activateAllMutex);
        for (auto it = e.activateAllSinks.begin(); it != e.activateAllSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.activateAllSinks.erase(it);
                return;
            }
        }
    }

    void ActivateAllDormantInOtherWindows(const std::wstring& sourceWindowId)
    {
        auto& e = SharedEngine();
        // Snapshot under the lock, invoke outside it (the ActivateSessionInOtherWindows pattern): each
        // sink hops into its own window's dispatcher to eager-init that window's dormant controls.
        std::vector<std::function<void()>> sinks;
        {
            std::lock_guard<std::mutex> lk(e.activateAllMutex);
            sinks.reserve(e.activateAllSinks.size());
            for (const auto& s : e.activateAllSinks)
            {
                if (s.fn && s.windowId != sourceWindowId)
                {
                    sinks.push_back(s.fn);
                }
            }
        }
        for (const auto& fn : sinks)
        {
            fn(); // fire-and-forget; each other window wakes its own dormant tabs
        }
    }

    uint64_t RegisterCommandActionHandler(const std::wstring& windowId, std::function<void(const std::wstring& sessionId, const std::wstring& command, const std::wstring& payload, const std::wstring& args)> handler)
    {
        if (!handler)
        {
            return 0;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.commandActionMutex);
        const auto token = e.nextCommandActionToken++;
        e.commandActionSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    void UnregisterCommandActionHandler(uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.commandActionMutex);
        for (auto it = e.commandActionSinks.begin(); it != e.commandActionSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.commandActionSinks.erase(it);
                return;
            }
        }
    }

    void RaiseCommandActionInWindows(const std::wstring& sessionId, const std::wstring& command, const std::wstring& payload, const std::wstring& args)
    {
        if (sessionId.empty() || command.empty())
        {
            return;
        }
        auto& e = SharedEngine();
        // Snapshot under the lock, invoke outside it (the ActivateSessionInOtherWindows pattern):
        // each sink hops into its own window's dispatcher; the fire originates on the scanner
        // thread, so EVERY window is a candidate (no source to exclude) and only the (single)
        // window hosting the session's tab acts.
        std::vector<std::function<void(const std::wstring&, const std::wstring&, const std::wstring&, const std::wstring&)>> sinks;
        {
            std::lock_guard<std::mutex> lk(e.commandActionMutex);
            sinks.reserve(e.commandActionSinks.size());
            for (const auto& s : e.commandActionSinks)
            {
                if (s.fn)
                {
                    sinks.push_back(s.fn);
                }
            }
        }
        for (const auto& fn : sinks)
        {
            // Safeguard: one window's sink failing (a tearing-down dispatcher's RunAsync throw,
            // a dead apartment) must not stop the fan-out — the HOST window's sink may be later
            // in the list, and dropping it would silently swallow the whole command action.
            try
            {
                fn(sessionId, command, payload, args);
            }
            catch (...)
            {
                LogSwallowedException(L"RaiseCommandActionInWindows sink");
            }
        }
    }

    uint64_t RegisterWindowRestartHandler(const std::wstring& windowId, std::function<void(const std::wstring& sessionId)> handler)
    {
        if (!handler)
        {
            return 0;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.restartMutex);
        const auto token = e.nextRestartToken++;
        e.restartSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    void UnregisterWindowRestartHandler(uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.restartMutex);
        for (auto it = e.restartSinks.begin(); it != e.restartSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.restartSinks.erase(it);
                return;
            }
        }
    }

    void RestartSessionInOtherWindows(const std::wstring& sessionId, const std::wstring& sourceWindowId)
    {
        if (sessionId.empty())
        {
            return;
        }
        auto& e = SharedEngine();
        // Snapshot under the lock, invoke outside it (the ActivateSessionInOtherWindows pattern): each
        // sink hops into its own window's dispatcher, so holding the engine lock across foreign-window
        // marshaling would be a needless ordering hazard.
        std::vector<std::function<void(const std::wstring&)>> sinks;
        {
            std::lock_guard<std::mutex> lk(e.restartMutex);
            sinks.reserve(e.restartSinks.size());
            for (const auto& s : e.restartSinks)
            {
                if (s.fn && s.windowId != sourceWindowId)
                {
                    sinks.push_back(s.fn);
                }
            }
        }
        for (const auto& fn : sinks)
        {
            fn(sessionId); // fire-and-forget; the (single) hosting window restarts the session, the rest miss
        }
    }

    uint64_t RegisterWindowCloseSessionHandler(const std::wstring& windowId, std::function<void(const std::wstring& sessionId)> handler)
    {
        if (!handler)
        {
            return 0;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.closeSessionMutex);
        const auto token = e.nextCloseSessionToken++;
        e.closeSessionSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    void UnregisterWindowCloseSessionHandler(uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.closeSessionMutex);
        for (auto it = e.closeSessionSinks.begin(); it != e.closeSessionSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.closeSessionSinks.erase(it);
                return;
            }
        }
    }

    void CloseSessionInOtherWindows(const std::wstring& sessionId, const std::wstring& sourceWindowId)
    {
        if (sessionId.empty())
        {
            return;
        }
        auto& e = SharedEngine();
        // Snapshot under the lock, invoke outside it (the RestartSessionInOtherWindows pattern): each
        // sink hops into its own window's dispatcher, so holding the engine lock across foreign-window
        // marshaling would be a needless ordering hazard.
        std::vector<std::function<void(const std::wstring&)>> sinks;
        {
            std::lock_guard<std::mutex> lk(e.closeSessionMutex);
            sinks.reserve(e.closeSessionSinks.size());
            for (const auto& s : e.closeSessionSinks)
            {
                if (s.fn && s.windowId != sourceWindowId)
                {
                    sinks.push_back(s.fn);
                }
            }
        }
        for (const auto& fn : sinks)
        {
            fn(sessionId); // fire-and-forget; the (single) hosting window closes the session's tab, the rest miss
        }
    }

    // Agentmaster (cross-window "Deactivate ▸ Of Same Folder"): see Engine.h. Registers one window's
    // park-session sink and hands back the token that detaches it.
    //   windowId — the registering window's id (the fan-out's "skip my own window" key).
    //   handler  — the sink; an empty one is refused, because a null sink would crash the fan-out.
    //   returns  — a non-zero token, or 0 when `handler` is empty.
    //   throws   — std::bad_alloc on allocation failure; std::system_error if the mutex cannot be locked.
    uint64_t RegisterWindowDeactivateSessionHandler(const std::wstring& windowId, std::function<void(const std::wstring& sessionId)> handler)
    {
        if (!handler)
        {
            return 0;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.deactivateSessionMutex);
        const auto token = e.nextDeactivateSessionToken++;
        e.deactivateSessionSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    // Agentmaster (cross-window "Deactivate ▸ Of Same Folder"): see Engine.h. Detaches one sink by token.
    //   token  — the token to remove; 0 (never issued) and an already-removed token are no-ops, so a
    //            double teardown is safe.
    //   throws — std::system_error if the mutex cannot be locked.
    void UnregisterWindowDeactivateSessionHandler(uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.deactivateSessionMutex);
        for (auto it = e.deactivateSessionSinks.begin(); it != e.deactivateSessionSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.deactivateSessionSinks.erase(it);
                return;
            }
        }
    }

    // Agentmaster (cross-window "Deactivate ▸ Of Same Folder"): see Engine.h. Fans `sessionId` out to
    // every registered sink except the source window's.
    //   sessionId      — the session to park; empty is a no-op.
    //   sourceWindowId — the calling window; its own sink is skipped.
    //   throws         — std::bad_alloc on allocation failure; std::system_error if the mutex cannot be
    //                    locked; anything a sink throws (a sink must not throw).
    void DeactivateSessionInOtherWindows(const std::wstring& sessionId, const std::wstring& sourceWindowId)
    {
        if (sessionId.empty())
        {
            return;
        }
        auto& e = SharedEngine();
        // Snapshot under the lock, invoke outside it (the CloseSessionInOtherWindows pattern): each sink
        // hops into its own window's dispatcher, and a sink that registered or detached another sink while
        // this lock was held would deadlock on it.
        std::vector<std::function<void(const std::wstring&)>> sinks;
        {
            std::lock_guard<std::mutex> lk(e.deactivateSessionMutex);
            sinks.reserve(e.deactivateSessionSinks.size());
            for (const auto& s : e.deactivateSessionSinks)
            {
                if (s.fn && s.windowId != sourceWindowId)
                {
                    sinks.push_back(s.fn);
                }
            }
        }
        for (const auto& fn : sinks)
        {
            fn(sessionId); // fire-and-forget; the (single) hosting window parks the session's tab, the rest miss
        }
    }

    // Agentmaster (MESSAGING.md §8.2): see Engine.h. The session-action sinks (the deactivateSessionSinks idiom).
    uint64_t RegisterWindowSessionActionHandler(const std::wstring& windowId, std::function<void(const std::wstring&, const std::wstring&, const std::wstring&, std::function<void(std::wstring)>)> handler)
    {
        if (!handler)
        {
            return 0;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.sessionActionMutex);
        const auto token = e.nextSessionActionToken++;
        e.sessionActionSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    void UnregisterWindowSessionActionHandler(uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.sessionActionMutex);
        for (auto it = e.sessionActionSinks.begin(); it != e.sessionActionSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.sessionActionSinks.erase(it);
                return;
            }
        }
    }

    void RequestSessionActionInWindows(const std::wstring& sessionId, const std::wstring& action, const std::wstring& arg, std::function<void(std::wstring)> reply)
    {
        if (sessionId.empty())
        {
            return;
        }
        auto& e = SharedEngine();
        // Snapshot under the lock, invoke outside it: each sink hops into its own window's dispatcher.
        std::vector<std::function<void(const std::wstring&, const std::wstring&, const std::wstring&, std::function<void(std::wstring)>)>> sinks;
        {
            std::lock_guard<std::mutex> lk(e.sessionActionMutex);
            sinks.reserve(e.sessionActionSinks.size());
            for (const auto& s : e.sessionActionSinks)
            {
                if (s.fn)
                {
                    sinks.push_back(s.fn);
                }
            }
        }
        for (const auto& fn : sinks)
        {
            fn(sessionId, action, arg, reply); // the (single) hosting window acts and replies; the rest miss
        }
    }

    // Agentmaster (MESSAGING.md §5.4 / §8.2 / D27): see Engine.h. The launch sinks + the last-active window.
    uint64_t RegisterWindowLaunchHandler(const std::wstring& windowId, std::function<void(const RemoteLaunchRequest&)> handler)
    {
        if (!handler)
        {
            return 0;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.launchMutex);
        const auto token = e.nextLaunchToken++;
        e.launchSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    void UnregisterWindowLaunchHandler(uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.launchMutex);
        for (auto it = e.launchSinks.begin(); it != e.launchSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.launchSinks.erase(it);
                return;
            }
        }
    }

    bool RequestLaunchInWindow(const RemoteLaunchRequest& request)
    {
        auto& e = SharedEngine();
        std::function<void(const RemoteLaunchRequest&)> fn;
        {
            std::lock_guard<std::mutex> lk(e.launchMutex);
            // The most recently activated window first; a window that closed since is no longer in the list, so
            // the fallback is the first registered (the oldest window).
            for (const auto& s : e.launchSinks)
            {
                if (s.fn && !e.lastActiveWindowId.empty() && s.windowId == e.lastActiveWindowId)
                {
                    fn = s.fn;
                    break;
                }
            }
            if (!fn)
            {
                for (const auto& s : e.launchSinks)
                {
                    if (s.fn)
                    {
                        fn = s.fn;
                        break;
                    }
                }
            }
        }
        if (!fn)
        {
            return false;
        }
        fn(request);
        return true;
    }

    void NoteWindowActivated(const std::wstring& windowId) noexcept
    {
        try
        {
            if (windowId.empty())
            {
                return;
            }
            auto& e = SharedEngine();
            std::lock_guard<std::mutex> lk(e.launchMutex);
            e.lastActiveWindowId = windowId;
        }
        catch (...)
        {
            LogSwallowedException(L"NoteWindowActivated");
        }
    }

    // Agentmaster (SESSIONS.md §9.14): see Engine.h. The AI search's window-tool sinks and their ANSWERED fan-outs.
    namespace
    {
        /// <summary>
        /// The answers of one agent-tool fan-out. Shared by the waiting caller and every reply callback through a
        /// shared_ptr, because a window may answer AFTER the caller stopped waiting (its UI thread was busy past the
        /// timeout): its reply then fills a slot nobody reads, and the state is freed with the last callback that holds it.
        /// </summary>
        struct AgentToolFanOut
        {
            std::mutex mutex; // guards everything below
            std::condition_variable answered; // raised by every reply
            std::vector<std::optional<WindowToolOutcome>> slots; // one per sink, in sink (= registration) order
            // Per sink: it is done — it answered, or it threw. Only the FIRST of the two counts, so a sink that answers
            // twice, or answers and then throws, is never counted twice (that would end the wait before another window
            // answered).
            std::vector<bool> settled;
            size_t replies{ 0 }; // settled sinks so far
            bool handledReply{ false }; // some answer has handled == true (the host of a Deactivate)
        };

        /// <summary>
        /// Send `request` to the registered agent-tool sinks (all of them, or the one of `onlyWindowId`) and wait for
        /// their answers: until every sink answered, until one answered handled == true (when `stopOnHandled`), or until
        /// `timeoutMs` passed — whichever comes first.
        /// </summary>
        /// <param name="e">The engine whose sinks to use (SharedEngine(), or a test's local Engine).</param>
        /// <param name="request">What to ask.</param>
        /// <param name="onlyWindowId">"" = every window; else only the sink of that window.</param>
        /// <param name="stopOnHandled">Stop waiting at the first handled answer (a Deactivate's host).</param>
        /// <param name="timeoutMs">The longest wait.</param>
        /// <param name="windowIds">Receives the sinks' window ids, index-aligned with the returned slots.</param>
        /// <returns>One slot per sink that was asked: its answer, or nullopt when it had not answered (or threw).</returns>
        /// <exception cref="std::bad_alloc">If the snapshot or the state cannot be allocated.</exception>
        /// <exception cref="std::system_error">If a mutex cannot be locked.</exception>
        /// <remarks>
        /// The sinks are invoked OUTSIDE agentToolMutex (each hops into its own window's dispatcher and must return at
        /// once), and the wait holds only the fan-out's own mutex — so a window that registers or unregisters during the
        /// wait never blocks on it. A sink that throws (it must not, but a dead dispatcher can) is counted as answered
        /// with nothing, so one broken window costs nobody the whole timeout.
        /// </remarks>
        std::vector<std::optional<WindowToolOutcome>> FanOutAgentTool(Engine& e, const WindowToolRequest& request, const std::wstring& onlyWindowId, bool stopOnHandled, uint32_t timeoutMs, std::vector<std::wstring>& windowIds)
        {
            std::vector<std::function<void(const WindowToolRequest&, std::function<void(WindowToolOutcome)>)>> sinks;
            windowIds.clear();
            {
                std::lock_guard<std::mutex> lk(e.agentToolMutex);
                sinks.reserve(e.agentToolSinks.size());
                for (const auto& s : e.agentToolSinks)
                {
                    if (s.fn && (onlyWindowId.empty() || s.windowId == onlyWindowId))
                    {
                        sinks.push_back(s.fn);
                        windowIds.push_back(s.windowId);
                    }
                }
            }
            auto state = std::make_shared<AgentToolFanOut>();
            state->slots.resize(sinks.size());
            state->settled.resize(sinks.size(), false);
            for (size_t i = 0; i < sinks.size(); ++i)
            {
                // The reply owns the state (not the caller's frame): it may run after this function returned.
                auto reply = [state, i](WindowToolOutcome outcome) {
                    std::lock_guard<std::mutex> lk(state->mutex);
                    if (i < state->settled.size() && !state->settled[i])
                    {
                        state->settled[i] = true;
                        state->handledReply = state->handledReply || outcome.handled;
                        state->slots[i] = std::move(outcome);
                        ++state->replies;
                    }
                    state->answered.notify_all();
                };
                try
                {
                    sinks[i](request, std::move(reply));
                }
                catch (...)
                {
                    // Rule #18. Recovery: this window is counted as answered with nothing, so the wait below ends as
                    // soon as the others answered instead of running out the timeout.
                    LogSwallowedException(L"FanOutAgentTool: a window's agent-tool sink");
                    std::lock_guard<std::mutex> lk(state->mutex);
                    if (!state->settled[i])
                    {
                        state->settled[i] = true;
                        ++state->replies;
                    }
                    state->answered.notify_all();
                }
            }
            std::unique_lock<std::mutex> lk(state->mutex);
            state->answered.wait_for(lk, std::chrono::milliseconds{ timeoutMs }, [&state, stopOnHandled] {
                return state->replies >= state->slots.size() || (stopOnHandled && state->handledReply);
            });
            return state->slots; // a copy under the lock: a late reply may still write into the shared state
        }
    }

    uint64_t RegisterWindowAgentToolHandler(const std::wstring& windowId, std::function<void(const WindowToolRequest& request, std::function<void(WindowToolOutcome)> reply)> handler)
    {
        return RegisterWindowAgentToolHandlerIn(SharedEngine(), windowId, std::move(handler));
    }

    uint64_t RegisterWindowAgentToolHandlerIn(Engine& e, const std::wstring& windowId, std::function<void(const WindowToolRequest& request, std::function<void(WindowToolOutcome)> reply)> handler)
    {
        if (!handler)
        {
            return 0;
        }
        std::lock_guard<std::mutex> lk(e.agentToolMutex);
        const auto token = e.nextAgentToolToken++;
        e.agentToolSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    void UnregisterWindowAgentToolHandler(uint64_t token)
    {
        UnregisterWindowAgentToolHandlerIn(SharedEngine(), token);
    }

    void UnregisterWindowAgentToolHandlerIn(Engine& e, uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        std::lock_guard<std::mutex> lk(e.agentToolMutex);
        for (auto it = e.agentToolSinks.begin(); it != e.agentToolSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.agentToolSinks.erase(it);
                return;
            }
        }
    }

    std::vector<WindowCensus> CollectWindowCensus(uint32_t timeoutMs)
    {
        return CollectWindowCensusIn(SharedEngine(), timeoutMs);
    }

    std::vector<WindowCensus> CollectWindowCensusIn(Engine& e, uint32_t timeoutMs)
    {
        WindowToolRequest request;
        request.kind = WindowToolRequest::Kind::Census;
        std::vector<std::wstring> windowIds;
        const auto slots = FanOutAgentTool(e, request, {}, false, timeoutMs, windowIds);
        std::wstring lastActive;
        {
            std::lock_guard<std::mutex> lk(e.launchMutex);
            lastActive = e.lastActiveWindowId;
        }
        std::vector<WindowCensus> out;
        out.reserve(slots.size());
        for (size_t i = 0; i < slots.size(); ++i)
        {
            if (!slots[i] || !slots[i]->handled)
            {
                continue; // no answer in time: the window is left out rather than reported empty
            }
            WindowCensus c = slots[i]->census;
            // The sink's own id is the truth for routing (a window's census may come back with an empty id if it
            // answered before its id was set): RunWindowTool targets exactly this string.
            c.windowId = windowIds[i];
            c.lastActive = !lastActive.empty() && c.windowId == lastActive;
            out.push_back(std::move(c));
        }
        return out;
    }

    std::optional<WindowToolOutcome> RunWindowTool(const std::wstring& windowId, const WindowToolRequest& request, uint32_t timeoutMs)
    {
        return RunWindowToolIn(SharedEngine(), windowId, request, timeoutMs);
    }

    std::optional<WindowToolOutcome> RunWindowToolIn(Engine& e, const std::wstring& windowId, const WindowToolRequest& request, uint32_t timeoutMs)
    {
        if (windowId.empty())
        {
            return std::nullopt; // "every window" is RunWindowToolOnHost's job, never a silent broadcast of an open
        }
        std::vector<std::wstring> windowIds;
        const auto slots = FanOutAgentTool(e, request, windowId, false, timeoutMs, windowIds);
        for (const auto& slot : slots)
        {
            if (slot)
            {
                return slot;
            }
        }
        return std::nullopt;
    }

    std::optional<WindowToolOutcome> RunWindowToolOnHost(const WindowToolRequest& request, uint32_t timeoutMs)
    {
        return RunWindowToolOnHostIn(SharedEngine(), request, timeoutMs);
    }

    std::optional<WindowToolOutcome> RunWindowToolOnHostIn(Engine& e, const WindowToolRequest& request, uint32_t timeoutMs)
    {
        std::vector<std::wstring> windowIds;
        const auto slots = FanOutAgentTool(e, request, {}, true, timeoutMs, windowIds);
        for (const auto& slot : slots)
        {
            if (slot && slot->handled)
            {
                return slot;
            }
        }
        return std::nullopt;
    }

    uint64_t RegisterWindowDockTarget(const std::wstring& windowId, void* topLevelHwnd, std::function<DockProbeResult(long, long, DockProbeMode)> probe)
    {
        if (!probe || !topLevelHwnd)
        {
            return 0;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.dockTargetMutex);
        const auto token = e.nextDockTargetToken++;
        e.dockTargetSinks.push_back({ token, windowId, topLevelHwnd, std::move(probe) });
        return token;
    }

    void UnregisterWindowDockTarget(uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.dockTargetMutex);
        for (auto it = e.dockTargetSinks.begin(); it != e.dockTargetSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.dockTargetSinks.erase(it);
                return;
            }
        }
    }

    std::function<DockProbeResult(long, long, DockProbeMode)> FindWindowDockProbe(void* topLevelHwnd)
    {
        if (!topLevelHwnd)
        {
            return {};
        }
        auto& e = SharedEngine();
        // By copy, invoked outside the lock (the sink-snapshot pattern) — the probe runs target-window
        // code, and holding the engine lock across it would be a needless ordering hazard.
        std::lock_guard<std::mutex> lk(e.dockTargetMutex);
        for (const auto& s : e.dockTargetSinks)
        {
            if (s.topLevelHwnd == topLevelHwnd && s.probe)
            {
                return s.probe;
            }
        }
        return {};
    }

    uint64_t RegisterSettingsChangedHandler(const std::wstring& windowId, std::function<void(const AppSettings&)> handler)
    {
        if (!handler)
        {
            return 0;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.settingsMutex);
        const auto token = e.nextSettingsToken++;
        e.settingsSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    void UnregisterSettingsChangedHandler(uint64_t token)
    {
        if (token == 0)
        {
            return;
        }
        auto& e = SharedEngine();
        std::lock_guard<std::mutex> lk(e.settingsMutex);
        for (auto it = e.settingsSinks.begin(); it != e.settingsSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.settingsSinks.erase(it);
                return;
            }
        }
    }

    void BroadcastSettingsChanged(const AppSettings& settings, const std::wstring& sourceWindowId)
    {
        auto& e = SharedEngine();
        // Snapshot under the lock, invoke outside it (the ActivateSessionInOtherWindows pattern): each
        // sink hops into its own window's dispatcher, so holding the engine lock across foreign-window
        // marshaling would be a needless ordering hazard. The source window is excluded — it already
        // applied + persisted the change (and its own toggle/cog re-rendered locally).
        std::vector<std::function<void(const AppSettings&)>> sinks;
        {
            std::lock_guard<std::mutex> lk(e.settingsMutex);
            sinks.reserve(e.settingsSinks.size());
            for (const auto& s : e.settingsSinks)
            {
                if (s.fn && s.windowId != sourceWindowId)
                {
                    sinks.push_back(s.fn);
                }
            }
        }
        for (const auto& fn : sinks)
        {
            fn(settings); // fire-and-forget; each OTHER window re-applies on its own UI thread
        }
    }

    // ---- Agentmaster (taskbar unread badge; NOTIFICATIONS.md section 8): the process-wide unread census ----

    uint32_t CountDistinctUnreadSessions(const std::map<std::wstring, std::vector<std::wstring>>& unreadByWindow)
    {
        // Flatten + sort + unique rather than a std::set: the sets are tiny (a handful of ids per window) and a
        // flat vector is one allocation instead of one per node.
        std::vector<std::wstring> all;
        for (const auto& [windowId, ids] : unreadByWindow)
        {
            (void)windowId;
            for (const auto& id : ids)
            {
                if (!id.empty())
                {
                    all.push_back(id);
                }
            }
        }
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        return static_cast<uint32_t>(all.size());
    }

    void PublishWindowUnreadSessions(const std::wstring& windowId, std::vector<std::wstring> sessionIds) noexcept
    {
        PublishWindowUnreadSessionsIn(SharedEngine(), windowId, std::move(sessionIds));
    }

    void PublishWindowUnreadSessionsIn(Engine& e, const std::wstring& windowId, std::vector<std::wstring> sessionIds) noexcept
    {
        try
        {
            // Normalize so the census stores a canonical set: the union counts by value, and an empty publish must
            // ERASE the window's entry (a closed window or a fully-read one contributes nothing, not an empty row).
            sessionIds.erase(std::remove_if(sessionIds.begin(), sessionIds.end(), [](const std::wstring& id) { return id.empty(); }), sessionIds.end());
            std::sort(sessionIds.begin(), sessionIds.end());
            sessionIds.erase(std::unique(sessionIds.begin(), sessionIds.end()), sessionIds.end());

            uint32_t before = 0;
            uint32_t after = 0;
            std::vector<std::function<void(uint32_t)>> sinks;
            {
                std::lock_guard<std::mutex> lk(e.unreadMutex);
                if (sessionIds.empty())
                {
                    e.unreadByWindow.erase(windowId);
                }
                else
                {
                    e.unreadByWindow[windowId] = std::move(sessionIds);
                }
                before = e.unreadCount;
                after = CountDistinctUnreadSessions(e.unreadByWindow);
                if (after == before)
                {
                    return; // the set moved but the COUNT didn't (e.g. one read, one new) — nothing to repaint
                }
                e.unreadCount = after;
                // Snapshot under the lock, invoke outside it (the ActivateSessionInOtherWindows pattern): each sink
                // hops into its own window's dispatcher, and holding the census lock across foreign-window
                // marshaling would be a needless ordering hazard.
                sinks.reserve(e.unreadCountSinks.size());
                for (const auto& s : e.unreadCountSinks)
                {
                    if (s.fn)
                    {
                        sinks.push_back(s.fn);
                    }
                }
            }
            AppendStateLog(L"hooks.log", L"[unread] " + std::to_wstring(before) + L" -> " + std::to_wstring(after) + L" (window " + ShortId(windowId) + L")\n");
            for (const auto& fn : sinks)
            {
                // Per-sink containment (the RaiseCommandActionInWindows rule): a tearing-down window's dispatcher
                // throwing must not keep the LATER windows' buttons on a stale number.
                try
                {
                    fn(after);
                }
                catch (...)
                {
                    LogSwallowedException(L"PublishWindowUnreadSessions sink");
                }
            }
        }
        catch (...)
        {
            // noexcept by contract (callable from ~TerminalPage on a pool thread): an allocation failure here costs
            // one census update, never the process. The next publish recomputes the union from scratch.
            LogSwallowedException(L"PublishWindowUnreadSessions");
        }
    }

    uint32_t CurrentUnreadCount() noexcept
    {
        return CurrentUnreadCountIn(SharedEngine());
    }

    uint32_t CurrentUnreadCountIn(Engine& e) noexcept
    {
        std::lock_guard<std::mutex> lk(e.unreadMutex);
        return e.unreadCount;
    }

    uint64_t RegisterUnreadCountHandler(const std::wstring& windowId, std::function<void(uint32_t count)> handler)
    {
        return RegisterUnreadCountHandlerIn(SharedEngine(), windowId, std::move(handler));
    }

    uint64_t RegisterUnreadCountHandlerIn(Engine& e, const std::wstring& windowId, std::function<void(uint32_t count)> handler)
    {
        if (!handler)
        {
            return 0;
        }
        std::lock_guard<std::mutex> lk(e.unreadMutex);
        const auto token = e.nextUnreadCountToken++;
        e.unreadCountSinks.push_back({ token, windowId, std::move(handler) });
        return token;
    }

    void UnregisterUnreadCountHandler(uint64_t token) noexcept
    {
        UnregisterUnreadCountHandlerIn(SharedEngine(), token);
    }

    void UnregisterUnreadCountHandlerIn(Engine& e, uint64_t token) noexcept
    {
        if (token == 0)
        {
            return;
        }
        std::lock_guard<std::mutex> lk(e.unreadMutex);
        for (auto it = e.unreadCountSinks.begin(); it != e.unreadCountSinks.end(); ++it)
        {
            if (it->token == token)
            {
                e.unreadCountSinks.erase(it); // vector::erase of a movable element: no allocation, no throw
                return;
            }
        }
    }

    std::vector<RecoverableWindow> RecoverableWindows()
    {
        return RecoverableWindowsIn(SharedEngine());
    }

    std::vector<RecoverableWindow> RecoverableWindowsIn(Engine& e)
    {
        // Snapshot the live set under the lock, then read records OUTSIDE the lock (disk IO).
        std::set<std::wstring> live;
        {
            std::lock_guard<std::mutex> lk(e.windowMutex);
            live = e.liveWindowIds;
        }
        std::vector<RecoverableWindow> out;
        const auto records = LoadWindowRecords(); // canonical sorted order -> the index IS the `-s <idx>`
        for (int i = 0; i < static_cast<int>(records.size()); ++i)
        {
            // Agentmaster (discard Manager-only windows): a record with NO tab refs is a window that
            // held nothing but the Manager tab — and every window auto-creates the Manager tab at
            // index 0, so reopening it reconstructs exactly what "+ new window" gives (plus stale
            // geometry/lens). Offering it is noise: skip it from every recover surface (the "Reopen
            // Windows (N)" count, the Archive page's "Saved window" rows, reopen-all, the per-row
            // windowId re-resolution — they all flow through here). The loop keeps `i` as the
            // CANONICAL LoadWindowRecords index, so the surviving entries' `-s <idx>` still addresses
            // the right record. (The Emperor's startup open-at-exit reopen reads windows/*.json
            // directly and is deliberately untouched — restoring what was open at exit is faithful.)
            if (records[i].tabs.empty())
            {
                continue;
            }
            if (live.find(records[i].windowId) == live.end())
            {
                out.push_back({ i, records[i] });
            }
        }
        return out;
    }
}
