// Agentmaster leak hunt: snapdump - write a minidump of a RUNNING process from a PSS snapshot.
//
// Why a snapshot: MiniDumpWriteDump on a live process suspends every thread of the target until the
// whole dump is written (seconds to minutes for a multi-GB full dump), which freezes production's
// one UI thread - every window - and can trip its [ui-stall] watchdog. PssCaptureSnapshot with
// PSS_CAPTURE_VA_CLONE instead forks the address space copy-on-write into a hidden clone process; the
// target is paused only while the clone is made (typically well under a second), then keeps running
// while the dump is written FROM THE CLONE. This is what `procdump -r` does. Nothing in the target
// is read through its own threads, and nothing is written into it.
//
// Cost the target does pay: its private pages become copy-on-write for the clone's lifetime, so a
// page the target writes meanwhile is copied once (memory, not correctness), and the system commit
// charge rises by up to the target's private bytes while the clone lives. The pre-flight check below
// refuses to clone when the commit headroom is too small for that, because commit exhaustion is what
// latched production's UI dispatch dead on 2026-08-22 (memory: ui-dispatch-wedge-freeze).
//
// Deliberately NOT captured: handle names (PSS_CAPTURE_HANDLE_NAME_INFORMATION). Querying the name of
// a synchronous pipe handle can block, and the capture runs while the target is paused.
//
// Build (VS 2022 x64 developer prompt): cl /nologo /EHsc /O2 /std:c++20 snapdump.cpp /link dbghelp.lib
// Usage:   snapdump <pid> <out.dmp> [--heap] [--force]
//   --heap   private read-write memory only (heaps, stacks; no mapped files or image pages) - smaller
//   --force  skip the commit-headroom refusal (only when you have checked the headroom yourself)

#include <windows.h>
#include <dbghelp.h>
#include <processsnapshot.h>
#include <psapi.h>

#include <cstdio>
#include <cwchar>
#include <string>

namespace
{
    /// The commit headroom (in bytes) that must remain after the clone's worst-case charge. Below it the
    /// clone could push the machine into commit exhaustion, where every process's allocations fail.
    constexpr unsigned long long kCommitReserveBytes = 4ull << 30;

    /// Converts a QueryPerformanceCounter interval to milliseconds for the timing report.
    /// @param from the counter value taken before the measured step.
    /// @param to the counter value taken after it.
    /// @returns the elapsed milliseconds (fractional).
    /// @remarks Never throws; a zero frequency (impossible on supported Windows) would divide by zero,
    ///          so the frequency is read once and assumed valid.
    double ElapsedMs(LARGE_INTEGER from, LARGE_INTEGER to) noexcept
    {
        static const LONGLONG freq = [] {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return f.QuadPart;
        }();
        return (to.QuadPart - from.QuadPart) * 1000.0 / static_cast<double>(freq);
    }

    /// The MiniDumpWriteDump callback that tells dbghelp the "process handle" is really a PSS snapshot.
    /// @param param unused callback context.
    /// @param input the callback kind dbghelp is asking about.
    /// @param output where the answer goes; IsProcessSnapshotCallback must answer S_FALSE.
    /// @returns TRUE to keep the dump going for every callback kind.
    /// @remarks Without the S_FALSE answer dbghelp treats the HPSS as a process handle and fails. Never throws.
    BOOL CALLBACK SnapshotCallback(PVOID /*param*/, const PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output) noexcept
    {
        if (input && output && input->CallbackType == IsProcessSnapshotCallback)
        {
            output->Status = S_FALSE;
        }
        return TRUE;
    }

    /// Prints one GetLastError-style failure with its system message, for the refusal / failure paths.
    /// @param what the step that failed, printed first.
    /// @param code the Win32 error (or HRESULT-like DWORD) the step returned.
    /// @remarks Never throws; FormatMessageW failing leaves just the numeric code.
    void PrintError(const wchar_t* what, DWORD code) noexcept
    {
        wchar_t* text = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
        fwprintf(stderr, L"%ls failed: %lu (0x%08lX) %ls\n", what, code, code, text ? text : L"");
        if (text)
        {
            LocalFree(text);
        }
    }
}

/// Entry point: validate, pre-flight the commit headroom, clone, dump, free the clone.
/// @param argc the argument count.
/// @param argv `<pid> <out.dmp> [--heap] [--force]`.
/// @returns 0 when the dump was written; 1 usage; 2 open failed; 3 refused for commit headroom;
///          4 snapshot failed; 5 the dump file could not be created; 6 MiniDumpWriteDump failed.
/// @remarks Never throws (no C++ exceptions are raised by the Win32 calls used). On a failure after the
///          snapshot exists the snapshot is still freed, so no clone process is left behind.
int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        fwprintf(stderr, L"usage: snapdump <pid> <out.dmp> [--heap] [--force]\n");
        return 1;
    }
    const DWORD pid = wcstoul(argv[1], nullptr, 10);
    const wchar_t* outPath = argv[2];
    bool heapOnly = false;
    bool force = false;
    for (int i = 3; i < argc; ++i)
    {
        heapOnly |= _wcsicmp(argv[i], L"--heap") == 0;
        force |= _wcsicmp(argv[i], L"--force") == 0;
    }

    // The rights a VA clone + thread capture need: CREATE_PROCESS makes the target the clone's parent,
    // VM_READ + QUERY_INFORMATION let PSS walk the address space and threads. No write/suspend rights
    // are requested, so this handle cannot change the target even by mistake.
    HANDLE process = OpenProcess(PROCESS_CREATE_PROCESS | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process)
    {
        PrintError(L"OpenProcess", GetLastError());
        return 2;
    }

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    // ullAvailPageFile is the commit still available system-wide. The clone may charge up to the
    // target's private bytes; refuse unless that leaves kCommitReserveBytes for everyone else.
    const unsigned long long need = static_cast<unsigned long long>(pmc.PrivateUsage) + kCommitReserveBytes;
    wprintf(L"target pid %lu: private %.1f MB, working set %.1f MB; system commit available %.1f GB (need %.1f GB)\n",
            pid, pmc.PrivateUsage / 1048576.0, pmc.WorkingSetSize / 1048576.0,
            ms.ullAvailPageFile / 1073741824.0, need / 1073741824.0);
    if (ms.ullAvailPageFile < need && !force)
    {
        fwprintf(stderr, L"refused: the clone could exhaust the system commit (pass --force to override)\n");
        CloseHandle(process);
        return 3;
    }

    const DWORD captureFlags = PSS_CAPTURE_VA_CLONE | PSS_CAPTURE_THREADS | PSS_CAPTURE_THREAD_CONTEXT |
                               PSS_CREATE_BREAKAWAY_OPTIONAL | PSS_CREATE_BREAKAWAY | PSS_CREATE_USE_VM_ALLOCATIONS |
                               PSS_CREATE_RELEASE_SECTION;
    HPSS snapshot = nullptr;
    LARGE_INTEGER t0{}, t1{}, t2{}, t3{};
    QueryPerformanceCounter(&t0);
    // The target is paused inside this call while the clone is made; its duration is the pause.
    const DWORD snapRc = PssCaptureSnapshot(process, static_cast<PSS_CAPTURE_FLAGS>(captureFlags), CONTEXT_ALL, &snapshot);
    QueryPerformanceCounter(&t1);
    if (snapRc != ERROR_SUCCESS)
    {
        PrintError(L"PssCaptureSnapshot", snapRc);
        CloseHandle(process);
        return 4;
    }
    wprintf(L"snapshot captured in %.1f ms (the target's pause)\n", ElapsedMs(t0, t1));

    int rc = 0;
    HANDLE file = CreateFileW(outPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        PrintError(L"CreateFileW (the dump file must not exist yet)", GetLastError());
        rc = 5;
    }
    else
    {
        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
            (heapOnly ? MiniDumpWithPrivateReadWriteMemory : MiniDumpWithFullMemory) | MiniDumpWithFullMemoryInfo |
            MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        MINIDUMP_CALLBACK_INFORMATION callback{};
        callback.CallbackRoutine = SnapshotCallback;
        // dbghelp reads everything from the clone through the snapshot handle cast to a HANDLE; the
        // callback above is what tells it so.
        const BOOL ok = MiniDumpWriteDump(reinterpret_cast<HANDLE>(snapshot), pid, file, type, nullptr, nullptr, &callback);
        const DWORD writeErr = ok ? 0 : GetLastError();
        QueryPerformanceCounter(&t2);
        LARGE_INTEGER size{};
        GetFileSizeEx(file, &size);
        CloseHandle(file);
        if (!ok)
        {
            PrintError(L"MiniDumpWriteDump", writeErr);
            rc = 6;
        }
        else
        {
            wprintf(L"dump written in %.1f s: %.1f MB -> %ls\n", ElapsedMs(t1, t2) / 1000.0, size.QuadPart / 1048576.0, outPath);
        }
    }

    // Freeing the snapshot terminates the clone; done on every path so no hidden clone outlives us.
    PssFreeSnapshot(GetCurrentProcess(), snapshot);
    QueryPerformanceCounter(&t3);
    wprintf(L"snapshot freed (%.1f ms)\n", ElapsedMs(t2.QuadPart ? t2 : t1, t3));
    CloseHandle(process);
    return rc;
}
