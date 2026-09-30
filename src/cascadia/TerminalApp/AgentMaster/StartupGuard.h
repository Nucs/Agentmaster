// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Agentmaster — startup guard: a known-folder PRE-WARM + a startup-HANG WATCHDOG (header-only, plain Win32;
// no WinRT, no engine deps — the WindowsTerminal EXE links TerminalApp.dll, not the TerminalAppLib static lib).
//
// WHY (the 2026-09-30 incident — doc/agentmaster/STARTUP_GUARD.md): a release launch sat forever with no window,
// all 12 threads waiting, and every re-launch silently did nothing. A full dump symbolized with Microsoft's public
// PDBs showed a textbook ABBA deadlock inside the exe prelude's `App{}` + `ReloadSettings()`:
//   * MAIN thread: CascadiaSettings::LoadAll -> PowershellCoreProfileGenerator -> SHGetKnownFolderPath(LocalAppData)
//     -> windows.storage!kfapi::CFolderDefinitionCache::Load (TAKES the definition-cache lock) -> _LoadRegistry ->
//     a DELAY-LOADED import (api-ms-win-shlwapi-winrt-storage-l1-1-1) -> LdrResolveDelayLoadedAPI ->
//     LdrpDrainWorkQueue: waits for another thread's DLL load to finish.
//   * XAML's GPU thread (spawned by `App{}`): nvldumdx!OpenAdapter12 -> LoadLibraryExW (a DLL load IN FLIGHT) ->
//     NvMemMapStoragex.dll's load-time init calls SHGetKnownFolderPath -> CFolderDefinitionCache::Load ->
//     RtlEnterCriticalSection on the lock the main thread holds.
// Each waits on the other forever. The trigger is the NVIDIA driver calling a shell API from DLL initialization
// (it must not); it can only bite while the main thread performs the process's FIRST known-folder definition load —
// Agentmaster's AGENTMASTER_PROFILE redirect skips upstream's early GetBaseSettingsPath lookup, so the PowerShell
// generator, racing XAML's first device creation, was that first load.
//
// THE FIX (PrewarmKnownFolders): run the startup's known-folder lookups ONCE on the main thread BEFORE `App{}`
// starts any GPU thread. That resolves windows.storage's delay-loaded imports (their IAT slots are patched for the
// rest of the process) and fills the definition cache, so a later lookup — ours or the driver's — takes the cache
// lock only briefly and never needs the loader while holding it. Cheap (a few ms), no behavior change.
//
// THE BELT (HangWatchdog): an unknown future hang in the same UI-free window (App{} + ReloadSettings; ~150-330 ms
// normally) must not leave a zombie that owns the single-instance mutex. Measured (a DllMain parked in the loader):
// a thread created BEFORE the hang can still append a log line, but CreateProcessW BLOCKS and a thread created
// DURING it never runs — so a stuck process cannot relaunch itself. What it CAN do is log `[startup-hang]` and
// TerminateProcess, which frees the mutex: a launch the user already started while it hung is retrying the
// handoff for ~29 s (acquireMutexOrAttemptHandoff) and simply becomes the instance, and any later launch works.
// A marker file makes that a ONE-shot per episode (the next launch waits without a deadline), so an environment
// whose settings load is legitimately slower than the deadline can never be locked out.

#pragma once

#include <windows.h>
#include <ShlObj.h>

#include <atomic>
#include <cstdint>
#include <cstdio> // sprintf_s / swprintf (the failure + prewarm log texts)
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "StartupTiming.h" // Agentmaster::Startup::Note / Phase — the EXE-safe hooks.log writer

#pragma comment(lib, "ole32.lib") // CoTaskMemFree
#pragma comment(lib, "shell32.lib") // SHGetKnownFolderPath

namespace Agentmaster::StartupGuard
{
    // ---------------------------------------------------------------------------------------------------------
    // Known-folder pre-warm
    // ---------------------------------------------------------------------------------------------------------

    /// <summary>
    /// FOLDERID_LocalAppData ({F1B32785-6FBA-4FCF-9D55-7B8E7F157091}), spelled locally so this header needs no
    /// uuid.lib (the EXE does not otherwise reference any FOLDERID_* symbol). The harness pins every constant here
    /// by resolving it and comparing with the matching environment variable, so a typo cannot ship.
    /// </summary>
    inline constexpr GUID kFolderLocalAppData{ 0xF1B32785, 0x6FBA, 0x4FCF, { 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91 } };

    /// <summary>FOLDERID_ProgramData ({62AB5D82-FDC1-4DC3-A9DD-070D1D495D97}); see kFolderLocalAppData for why it is local.</summary>
    inline constexpr GUID kFolderProgramData{ 0x62AB5D82, 0xFDC1, 0x4DC3, { 0xA9, 0xDD, 0x07, 0x0D, 0x1D, 0x49, 0x5D, 0x97 } };

    /// <summary>FOLDERID_RoamingAppData ({3EB685DB-65F9-4CF6-A03A-E3EF65729F3D}); see kFolderLocalAppData for why it is local.</summary>
    inline constexpr GUID kFolderRoamingAppData{ 0x3EB685DB, 0x65F9, 0x4CF6, { 0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D } };

    /// <summary>FOLDERID_Profile ({5E6C858F-0E22-4760-9AFE-EA3317B67173}); see kFolderLocalAppData for why it is local.</summary>
    inline constexpr GUID kFolderProfile{ 0x5E6C858F, 0x0E22, 0x4760, { 0x9A, 0xFE, 0xEA, 0x33, 0x17, 0xB6, 0x71, 0x73 } };

    /// <summary>
    /// One SHGetKnownFolderPath call the pre-warm replays: which folder, with which KF_FLAG_* flags, and a short label
    /// for the log line when it fails.
    /// </summary>
    /// <remarks>
    /// The FLAGS matter, not just the folder: package redirection (KF_FLAG_FORCE_APP_DATA_REDIRECTION /
    /// KF_FLAG_NO_PACKAGE_REDIRECTION) takes different code paths inside windows.storage, each with its own
    /// delay-loaded imports, so every flag combination the startup uses is replayed separately.
    /// </remarks>
    struct KnownFolderLookup
    {
        /// <summary>The KNOWNFOLDERID to resolve.</summary>
        GUID id;
        /// <summary>The KF_FLAG_* bits, exactly as the real startup caller passes them.</summary>
        DWORD flags;
        /// <summary>A stable name for the lookup in the failure log line (never shown to users).</summary>
        const wchar_t* label;
    };

    /// <summary>
    /// Every known-folder lookup the startup performs between `App{}` and the first window, plus two cheap belts —
    /// the set PrewarmKnownFolders replays before any GPU thread exists.
    /// </summary>
    /// <remarks>
    /// Keep in step with the settings loader: a NEW startup-time SHGetKnownFolderPath caller with a folder/flag
    /// combination missing here re-opens the race for its first call. Sources: PowershellCoreProfileGenerator.cpp
    /// (LocalAppData, 0 — the call that deadlocked), CascadiaSettingsSerialization.cpp's fragment scan (LocalAppData +
    /// ProgramData, 0), FileUtils.cpp GetBaseSettingsPath (LocalAppData, FORCE_APP_DATA_REDIRECTION — reached only
    /// when no AGENTMASTER_PROFILE is set) and GetReleaseSettingsPath (LocalAppData, NO_PACKAGE_REDIRECTION).
    /// RoamingAppData + Profile are belts: each extra definition load re-exercises _LoadRegistry's imports, so a
    /// future caller of another per-user folder is far less likely to be the first to need the loader.
    /// </remarks>
    inline constexpr KnownFolderLookup kStartupKnownFolderLookups[] = {
        { kFolderLocalAppData, 0, L"LocalAppData" },
        { kFolderLocalAppData, static_cast<DWORD>(KF_FLAG_FORCE_APP_DATA_REDIRECTION), L"LocalAppData/force-app-data-redirection" },
        { kFolderLocalAppData, static_cast<DWORD>(KF_FLAG_NO_PACKAGE_REDIRECTION), L"LocalAppData/no-package-redirection" },
        { kFolderProgramData, 0, L"ProgramData" },
        { kFolderRoamingAppData, 0, L"RoamingAppData" },
        { kFolderProfile, 0, L"Profile" },
    };

    /// <summary>What PrewarmKnownFolders did — enough for one log line, and for the harness to assert on.</summary>
    struct PrewarmResult
    {
        /// <summary>How many lookups were issued (== the lookup list's size).</summary>
        size_t attempted = 0;
        /// <summary>How many returned success. A failure still warmed whatever code path it reached.</summary>
        size_t resolved = 0;
        /// <summary>The first failing lookup's HRESULT; S_OK when every lookup succeeded.</summary>
        HRESULT firstFailure = S_OK;
        /// <summary>The first failing lookup's label; nullptr when every lookup succeeded.</summary>
        const wchar_t* firstFailureLabel = nullptr;
        /// <summary>Wall-clock duration of the whole pre-warm, in milliseconds.</summary>
        uint64_t elapsedMs = 0;
    };

    /// <summary>
    /// Replays <paramref name="lookups"/> on the CALLING thread so windows.storage's known-folder machinery is fully
    /// initialized (delay-loads resolved, definitions cached) before any other thread can race it.
    /// </summary>
    /// <param name="lookups">The folder/flag combinations to resolve, in order.</param>
    /// <param name="count">How many entries <paramref name="lookups"/> holds.</param>
    /// <returns>The attempt/success counts, the first failure (if any) and the elapsed time.</returns>
    /// <remarks>
    /// Must run on the main thread BEFORE `App{}`: the point is that no other thread is loading a DLL yet, so the
    /// one-time loader work here cannot interleave with a driver's DllMain. A failing lookup is recorded, never
    /// fatal — the real caller would fail the same way later, and startup must not depend on a warm-up. Needs no
    /// COM apartment. Never throws (a C API loop over a fixed array).
    /// </remarks>
    inline PrewarmResult PrewarmKnownFolders(const KnownFolderLookup* lookups, size_t count) noexcept
    {
        PrewarmResult result;
        const auto start = ::GetTickCount64();
        for (size_t i = 0; i < count; ++i)
        {
            ++result.attempted;
            PWSTR path = nullptr;
            const HRESULT hr = ::SHGetKnownFolderPath(lookups[i].id, lookups[i].flags, nullptr, &path);
            // Documented contract: the buffer must be freed whether or not the call succeeded (CoTaskMemFree(nullptr)
            // is a no-op), so free unconditionally rather than only on success.
            ::CoTaskMemFree(path);
            if (SUCCEEDED(hr))
            {
                ++result.resolved;
            }
            else if (result.firstFailureLabel == nullptr)
            {
                result.firstFailure = hr;
                result.firstFailureLabel = lookups[i].label;
            }
        }
        result.elapsedMs = ::GetTickCount64() - start;
        return result;
    }

    /// <summary>Replays the startup's own lookup set (kStartupKnownFolderLookups) — the overload the EXE calls.</summary>
    /// <returns>The attempt/success counts, the first failure (if any) and the elapsed time.</returns>
    /// <remarks>Same threading contract as the two-argument overload: main thread, before `App{}`. Never throws.</remarks>
    inline PrewarmResult PrewarmKnownFolders() noexcept
    {
        return PrewarmKnownFolders(kStartupKnownFolderLookups, std::size(kStartupKnownFolderLookups));
    }

    /// <summary>
    /// Formats the pre-warm as the phase label of its `[startup]` line: "known-folder prewarm (6/6 resolved)", or
    /// with the first failure appended ("... first failure LocalAppData/... hr=0x80070002").
    /// </summary>
    /// <param name="result">What PrewarmKnownFolders returned.</param>
    /// <returns>The label; Startup::Phase appends the duration and the +Nms anchor.</returns>
    /// <exception cref="std::bad_alloc">Building the string.</exception>
    inline std::wstring DescribePrewarm(const PrewarmResult& result)
    {
        std::wstring text = L"known-folder prewarm (" + std::to_wstring(result.resolved) + L"/" + std::to_wstring(result.attempted) + L" resolved";
        if (result.firstFailureLabel != nullptr)
        {
            wchar_t hr[16]{};
            ::swprintf(hr, std::size(hr), L"0x%08X", static_cast<unsigned>(result.firstFailure));
            text += L"; first failure ";
            text += result.firstFailureLabel;
            text += L" hr=";
            text += hr;
        }
        text += L")";
        return text;
    }

    // ---------------------------------------------------------------------------------------------------------
    // Startup-hang watchdog
    // ---------------------------------------------------------------------------------------------------------

    /// <summary>
    /// How long the guarded startup phase may run before the watchdog fires: 25 s, against a normal 150-330 ms.
    /// </summary>
    /// <remarks>
    /// Two constraints pick the value. It must dwarf any legitimately slow start (a CPU-saturated box measured ~20x
    /// slower on 2026-09-27, i.e. seconds, not tens of seconds). And it must stay BELOW the ~29 s a second launch
    /// keeps retrying the single-instance handoff, so a launch the user started while this one hung is still waiting
    /// when the mutex frees — and becomes the instance without another click.
    /// </remarks>
    inline constexpr DWORD kStartupHangTimeoutMs = 25'000;

    /// <summary>The exit code a watchdog termination leaves behind (ERROR_TIMEOUT), so it is recognizable in any process tooling.</summary>
    inline constexpr UINT kStartupHangExitCode = ERROR_TIMEOUT;

    /// <summary>The marker file (in the profile dir) that records "the previous launch was killed by the watchdog".</summary>
    inline constexpr std::wstring_view kStartupHangMarkerLeaf = L"startup-hang.marker";

    /// <summary>What a fired watchdog does.</summary>
    enum class HangAction
    {
        /// <summary>Log the hang and leave the process as it is.</summary>
        LogOnly,
        /// <summary>Log, drop the marker, and TerminateProcess — freeing the single-instance mutex.</summary>
        Terminate,
    };

    /// <summary>
    /// Decides whether a fired watchdog may kill the process. Pure — the whole policy in one testable place.
    /// </summary>
    /// <param name="debuggerAttached">A debugger is attached: a breakpoint inside the guarded phase is not a hang,
    /// and killing a debuggee out from under its developer would be hostile.</param>
    /// <param name="previousLaunchKilled">The marker says the PREVIOUS launch was already terminated by this watchdog.
    /// Killing twice in a row would lock out an environment whose settings load is simply slower than the deadline,
    /// so this launch waits without one (a real deadlock is a rare race — the second launch almost always wins it).</param>
    /// <returns>LogOnly when either guard holds; otherwise Terminate.</returns>
    constexpr HangAction DecideHangAction(bool debuggerAttached, bool previousLaunchKilled) noexcept
    {
        return (debuggerAttached || previousLaunchKilled) ? HangAction::LogOnly : HangAction::Terminate;
    }

    /// <summary>Full path of the hang marker inside <paramref name="profileDir"/>.</summary>
    /// <param name="profileDir">The active profile directory (ResolveProfileDir()).</param>
    /// <returns>&lt;profileDir&gt;\startup-hang.marker.</returns>
    /// <remarks>The allocating convenience form, for callers that are not racing a deadlock (the harness, docs).
    /// The marker helpers below build the same path into a stack buffer instead (FormatHangMarkerPath).</remarks>
    /// <exception cref="std::bad_alloc">Building the path.</exception>
    inline std::filesystem::path HangMarkerPath(const std::filesystem::path& profileDir)
    {
        return profileDir / std::wstring{ kStartupHangMarkerLeaf };
    }

    /// <summary>Capacity (in wchar_t) of the stack buffer the marker helpers build their path in.</summary>
    /// <remarks>4 KB of stack; a profile dir longer than ~2000 chars simply gets no marker (documented degradation).</remarks>
    inline constexpr size_t kHangMarkerPathCapacity = 2048;

    /// <summary>
    /// Builds "&lt;profileDir&gt;\startup-hang.marker" into <paramref name="out"/> WITHOUT allocating — the marker
    /// helpers run on the watchdog thread while the main thread may be deadlocked, and must not throw.
    /// </summary>
    /// <param name="profileDir">The active profile directory; empty yields false.</param>
    /// <param name="out">Receives the NUL-terminated path on success.</param>
    /// <returns>True when the path was built; false for an empty dir or one too long for the buffer.</returns>
    /// <remarks>path::native() is a reference to the stored string, so no copy is made. Never throws.</remarks>
    inline bool FormatHangMarkerPath(const std::filesystem::path& profileDir, wchar_t (&out)[kHangMarkerPathCapacity]) noexcept
    {
        const std::wstring& dir = profileDir.native();
        const bool needsSeparator = !dir.empty() && dir.back() != L'\\' && dir.back() != L'/';
        const size_t needed = dir.size() + (needsSeparator ? 1 : 0) + kStartupHangMarkerLeaf.size() + 1;
        if (dir.empty() || needed > kHangMarkerPathCapacity)
        {
            return false;
        }
        size_t at = 0;
        for (const wchar_t ch : dir)
        {
            out[at++] = ch;
        }
        if (needsSeparator)
        {
            out[at++] = L'\\';
        }
        for (const wchar_t ch : kStartupHangMarkerLeaf)
        {
            out[at++] = ch;
        }
        out[at] = L'\0';
        return true;
    }

    /// <summary>Whether the previous launch left a hang marker in <paramref name="profileDir"/>.</summary>
    /// <param name="profileDir">The active profile directory; empty means "unknown" and reads as no marker.</param>
    /// <returns>True when the marker file exists.</returns>
    /// <remarks>An attribute probe only — never opens the file. Never throws. A path that cannot be built reads as
    /// absent, which only ever makes the watchdog MORE willing to recover a hang, never less.</remarks>
    inline bool HangMarkerPresent(const std::filesystem::path& profileDir) noexcept
    {
        wchar_t path[kHangMarkerPathCapacity];
        return FormatHangMarkerPath(profileDir, path) && ::GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    }

    /// <summary>
    /// Writes the hang marker (content: a human-readable note) — called by a watchdog that is about to terminate.
    /// </summary>
    /// <param name="profileDir">The active profile directory; empty (or too long) skips the write.</param>
    /// <returns>True when the file was written.</returns>
    /// <remarks>
    /// Runs on the watchdog thread WHILE the main thread may be deadlocked in the loader, so it uses only a stack
    /// buffer and CreateFileW/WriteFile (already-resolved kernel32 imports — measured safe in that state). Never throws.
    /// </remarks>
    inline bool WriteHangMarker(const std::filesystem::path& profileDir) noexcept
    {
        wchar_t path[kHangMarkerPathCapacity];
        if (!FormatHangMarkerPath(profileDir, path))
        {
            return false;
        }
        const HANDLE file = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        static constexpr char kNote[] =
            "The previous Agentmaster launch was terminated by the startup-hang watchdog "
            "(doc/agentmaster/STARTUP_GUARD.md). The next launch waits without a deadline; "
            "a normal start deletes this file.\r\n";
        DWORD written = 0;
        const BOOL ok = ::WriteFile(file, kNote, static_cast<DWORD>(sizeof(kNote) - 1), &written, nullptr);
        ::CloseHandle(file);
        return ok != FALSE;
    }

    /// <summary>Deletes the hang marker after a launch got through the guarded phase normally.</summary>
    /// <param name="profileDir">The active profile directory; empty (or too long) is a no-op.</param>
    /// <returns>True when a marker existed and was deleted (the caller logs that recovery).</returns>
    /// <remarks>Never throws. A delete that fails keeps the NEXT launch in wait-without-deadline mode — the safe side.</remarks>
    inline bool ClearHangMarker(const std::filesystem::path& profileDir) noexcept
    {
        wchar_t path[kHangMarkerPathCapacity];
        return FormatHangMarkerPath(profileDir, path) && ::DeleteFileW(path) != FALSE;
    }

    /// <summary>
    /// Formats the `[startup-hang]` log line a fired watchdog writes.
    /// </summary>
    /// <param name="phase">The guarded phase's name.</param>
    /// <param name="timeoutMs">The deadline that elapsed.</param>
    /// <param name="action">What the watchdog is about to do (the line says which, and why).</param>
    /// <param name="debuggerAttached">Whether LogOnly was chosen because of a debugger (vs the marker).</param>
    /// <returns>The complete tagged line (Startup::Note adds the timestamp).</returns>
    /// <exception cref="std::bad_alloc">Building the string.</exception>
    inline std::wstring DescribeHang(std::wstring_view phase, DWORD timeoutMs, HangAction action, bool debuggerAttached)
    {
        std::wstring line = L"[startup-hang] \"" + std::wstring{ phase } + L"\" still running after " + std::to_wstring(timeoutMs) +
                            L"ms (normally ~0.3 s) \x2014 a startup deadlock (see doc/agentmaster/STARTUP_GUARD.md). ";
        if (action == HangAction::Terminate)
        {
            line += L"Terminating this instance (exit " + std::to_wstring(kStartupHangExitCode) +
                    L") so the single-instance lock frees: a launch already waiting in the handoff takes over, any later launch works; "
                    L"the next launch waits without a deadline.";
        }
        else if (debuggerAttached)
        {
            line += L"A debugger is attached \x2014 NOT terminating.";
        }
        else
        {
            line += L"The previous launch was already terminated by this watchdog \x2014 NOT terminating again (end it from Task Manager if it never comes up).";
        }
        return line;
    }

    /// <summary>
    /// A one-shot deadline around a startup phase: armed on construction, disarmed by Disarm() (or the destructor);
    /// if the deadline passes first, the hang callback runs ONCE on the watchdog thread.
    /// </summary>
    /// <remarks>
    /// Why a thread created up-front: once the main thread is stuck in the loader, a NEW thread never even starts
    /// (its DLL_THREAD_ATTACH needs the loader) — only a thread that already exists can react.
    /// Race-free by construction: arming, disarming and firing are one atomic state machine (Armed -> Disarmed |
    /// Fired), so a phase that finishes at the very instant the deadline elapses is either disarmed OR fired, never
    /// both — a healthy launch can never be terminated by a late timer.
    /// Arming never fails loudly: if the event or thread cannot be created the watchdog is simply inert (Armed()
    /// reports it) — a guard must never be the reason a launch fails.
    /// </remarks>
    class HangWatchdog
    {
    public:
        /// <summary>The action a fired watchdog runs (on the watchdog thread).</summary>
        using OnHang = std::function<void()>;

        /// <summary>
        /// Arms a watchdog whose fired action is the production policy (FireProductionHang): log, then — unless a
        /// debugger is attached or the previous launch was already killed — drop the marker and terminate.
        /// </summary>
        /// <param name="phase">The guarded phase's name, for the log line.</param>
        /// <param name="timeoutMs">The deadline.</param>
        /// <param name="profileDir">The active profile dir (marker + log location). The marker is READ here, at arm
        /// time, so the decision never touches the disk from a thread racing a deadlock more than it must.</param>
        /// <exception cref="std::bad_alloc">Copying the arguments.</exception>
        HangWatchdog(std::wstring phase, DWORD timeoutMs, std::filesystem::path profileDir) :
            _timeoutMs{ timeoutMs }, _previousLaunchKilled{ HangMarkerPresent(profileDir) }
        {
            _onHang = [phase = std::move(phase), timeoutMs, profileDir = std::move(profileDir), previousKilled = _previousLaunchKilled]() {
                FireProductionHang(phase, timeoutMs, profileDir, previousKilled);
            };
            _Start();
        }

        /// <summary>
        /// Arms a watchdog with a caller-supplied fired action — the harness's seam (it must never terminate itself).
        /// </summary>
        /// <param name="timeoutMs">The deadline.</param>
        /// <param name="onHang">Runs once, on the watchdog thread, if the deadline passes before Disarm(). An empty
        /// function leaves the instance inert (never armed, never fires).</param>
        /// <exception cref="std::bad_alloc">Copying <paramref name="onHang"/>.</exception>
        HangWatchdog(DWORD timeoutMs, OnHang onHang) :
            _timeoutMs{ timeoutMs }, _onHang{ std::move(onHang) }
        {
            if (_onHang)
            {
                _Start();
            }
            else
            {
                // Nothing to run: mark it Disarmed so Disarm() truthfully reports "was not armed" and it can never fire.
                _state = State::Disarmed;
            }
        }

        /// <summary>Disarms (if still armed) and joins the watchdog thread — never leaves it running past the owner.</summary>
        ~HangWatchdog() noexcept
        {
            Disarm();
        }

        HangWatchdog(const HangWatchdog&) = delete;
        HangWatchdog& operator=(const HangWatchdog&) = delete;
        HangWatchdog(HangWatchdog&&) = delete;
        HangWatchdog& operator=(HangWatchdog&&) = delete;

        /// <summary>
        /// Marks the guarded phase finished: a watchdog that has not fired yet never will; then wakes and joins the
        /// watchdog thread. Idempotent.
        /// </summary>
        /// <returns>True when THIS call disarmed it in time; false when it had already fired, was never armed (inert), or
        /// was already disarmed by an earlier call.</returns>
        /// <remarks>
        /// If the watchdog fired with LogOnly, the thread has finished logging by the time the join returns. (With
        /// Terminate there is no return — the process is gone.) Never throws: a failed join cannot be recovered here,
        /// and the thread only ever waits on the event this call sets.
        /// </remarks>
        bool Disarm() noexcept
        {
            State expected = State::Armed;
            const bool disarmedInTime = _state.compare_exchange_strong(expected, State::Disarmed);
            if (_event)
            {
                ::SetEvent(_event);
            }
            if (_thread.joinable())
            {
                try
                {
                    _thread.join();
                }
                catch (const std::exception& e)
                {
                    // join() throws only for a self-join or a non-joinable thread — both excluded above (Disarm never
                    // runs on the watchdog thread, and joinable() was just checked) — so this is "impossible", and
                    // therefore worth a line if it ever happens (Rule #18). Nothing to recover: the thread only waits on
                    // the event set above.
                    _LogGuardFailure(L"watchdog join failed", e.what());
                }
            }
            if (_event)
            {
                ::CloseHandle(_event);
                _event = nullptr;
            }
            return disarmedInTime;
        }

        /// <summary>Whether the watchdog was successfully armed (event + thread created).</summary>
        /// <returns>False for an inert watchdog.</returns>
        bool Armed() const noexcept
        {
            return _armed;
        }

        /// <summary>Whether the deadline passed before Disarm() — the hang action ran (or is running).</summary>
        /// <returns>True once fired.</returns>
        bool Fired() const noexcept
        {
            return _state.load() == State::Fired;
        }

        /// <summary>Whether the production constructor found the previous launch's hang marker (this launch waits without a deadline).</summary>
        /// <returns>True when the marker was present at arm time.</returns>
        bool PreviousLaunchKilled() const noexcept
        {
            return _previousLaunchKilled;
        }

        /// <summary>
        /// The production hang action: write the `[startup-hang]` line, then — when DecideHangAction allows — write the
        /// marker and TerminateProcess(kStartupHangExitCode).
        /// </summary>
        /// <param name="phase">The guarded phase's name.</param>
        /// <param name="timeoutMs">The deadline that elapsed.</param>
        /// <param name="profileDir">Where the marker goes.</param>
        /// <param name="previousLaunchKilled">The marker was already present at arm time.</param>
        /// <remarks>
        /// Runs on the watchdog thread while the main thread may be deadlocked inside the loader: only already-warm
        /// code runs here (the Startup writer's ofstream path was used by the prelude's earlier [startup] lines;
        /// CreateFileW/WriteFile/TerminateProcess are resolved kernel32 imports) — measured to work in that state.
        /// Never throws out: a failed log line must not prevent the termination that frees the mutex.
        /// </remarks>
        static void FireProductionHang(const std::wstring& phase, DWORD timeoutMs, const std::filesystem::path& profileDir, bool previousLaunchKilled) noexcept
        {
            const bool debugger = ::IsDebuggerPresent() != FALSE;
            const HangAction action = DecideHangAction(debugger, previousLaunchKilled);
            try
            {
                ::Agentmaster::Startup::Note(DescribeHang(phase, timeoutMs, action, debugger));
            }
            catch (...)
            {
                // ⚠ DELIBERATELY BARE (Rule #18's logging-path exemption): this guards building the log line itself —
                // only bad_alloc can land here, and the logger that would report it needs the same allocation.
                // Degradation: no [startup-hang] line; the termination below still frees the mutex.
            }
            if (action == HangAction::Terminate)
            {
                WriteHangMarker(profileDir);
                ::TerminateProcess(::GetCurrentProcess(), kStartupHangExitCode);
            }
        }

    private:
        /// <summary>The watchdog's lifecycle; transitions only Armed -> Disarmed or Armed -> Fired.</summary>
        enum class State
        {
            Armed,
            Disarmed,
            Fired,
        };

        /// <summary>
        /// Writes one `[startup-guard]` line for a guard failure that would otherwise vanish (Rule #18), e.g.
        /// "[startup-guard] watchdog could not start (...) — this launch runs unguarded".
        /// </summary>
        /// <param name="what">What failed.</param>
        /// <param name="detail">The exception text or error code, ASCII (widened byte-for-byte).</param>
        /// <remarks>Only ever called after the profile is resolved (the watchdog is armed after it), so Rule #15 holds.
        /// Never throws.</remarks>
        static void _LogGuardFailure(const wchar_t* what, const char* detail) noexcept
        {
            try
            {
                std::wstring line = L"[startup-guard] ";
                line += what;
                line += L" (";
                for (const char* p = detail; p && *p; ++p)
                {
                    line.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
                }
                line += L") \x2014 this launch runs without the startup-hang watchdog";
                ::Agentmaster::Startup::Note(line);
            }
            catch (...)
            {
                // ⚠ DELIBERATELY BARE (Rule #18's logging-path exemption): this guards building the log line itself —
                // only bad_alloc can land here, and the logger that would report it needs the same allocation.
                // Degradation: the failure line is missing; the watchdog is inert either way.
            }
        }

        /// <summary>
        /// Creates the event and the waiting thread. On any failure the watchdog stays inert (Armed() == false),
        /// is marked Disarmed so it can never fire, and the failure is logged.
        /// </summary>
        void _Start() noexcept
        {
            _event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!_event)
            {
                char code[32]{};
                ::sprintf_s(code, "CreateEventW gle=%lu", ::GetLastError());
                _state = State::Disarmed;
                _LogGuardFailure(L"watchdog could not start", code);
                return;
            }
            try
            {
                _thread = std::thread([this]() {
                    // Wake on Disarm() (event) or on the deadline. Only a timeout that wins the Armed -> Fired race
                    // runs the action; a Disarm() that got there first turns a simultaneous timeout into a no-op.
                    if (::WaitForSingleObject(_event, _timeoutMs) == WAIT_TIMEOUT)
                    {
                        State expected = State::Armed;
                        if (_state.compare_exchange_strong(expected, State::Fired))
                        {
                            _onHang();
                        }
                    }
                });
                _armed = true;
            }
            catch (const std::exception& e)
            {
                // std::thread's constructor throws std::system_error when the thread cannot be created. The guard
                // degrades to inert rather than failing the launch it exists to protect — and says so.
                _state = State::Disarmed;
                ::CloseHandle(_event);
                _event = nullptr;
                _LogGuardFailure(L"watchdog could not start", e.what());
            }
        }

        // Declaration order matters: _timeoutMs and _previousLaunchKilled are initialized in the production
        // constructor's mem-initializer list, before its body builds _onHang from them.
        DWORD _timeoutMs = 0;
        bool _previousLaunchKilled = false;
        OnHang _onHang;
        HANDLE _event = nullptr;
        std::thread _thread;
        std::atomic<State> _state{ State::Armed };
        bool _armed = false;
    };
}
