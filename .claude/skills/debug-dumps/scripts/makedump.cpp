// Agentmaster debug-dumps skill: generate a SMALL, NON-SENSITIVE crash dump for testing the analysis
// tools. It deliberately reproduces the exact signature of the real bugs this skill was born from — an
// access violation READING a "freed-fill" pointer (0xDDDDDDDDDDDD.... = the MSVC debug-CRT dead-land
// pattern) from a couple of frames deep — so the generated dump exercises the whole toolchain end-to-end
// (exception record + faultData + registers via dumpexc, an ordered stack walk via dumpwalk2, a symbolized
// scan via dumpourscan when makedump.pdb sits beside the dump). Writes a full-memory minidump of THIS tiny
// process (no user data), so the .dmp is a safe, committable fixture.
//   Usage: makedump [outfile]   (default: sample-crash.dmp in the cwd)
#define _AMD64_
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

static const wchar_t* g_out = L"sample-crash.dmp";

static LONG WINAPI WriteDumpFilter(EXCEPTION_POINTERS* ep)
{
    HANDLE hf = CreateFileW(g_out, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        // COMPACT on purpose: capture the thread STACKS + module list + exception + thread info — enough
        // for every tool here (dumpexc/dumpwalk2/dumpourscan/hangwalk) — WITHOUT the full heap, so the
        // committed fixture stays tiny (~100 KB vs ~30 MB). This produces a MemoryListStream (32-bit
        // descriptors); real WER MoAppCrash dumps use MiniDumpWithFullMemory -> Memory64ListStream, and the
        // tools read BOTH (see the range-loading fallback in dumpwalk2/dumpourscan). To make a
        // full-memory fixture instead, OR in MiniDumpWithFullMemory below.
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
                                                     MiniDumpWithUnloadedModules);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hf, type, &mei, nullptr, nullptr);
        CloseHandle(hf);
        wprintf(L"wrote %s\n", g_out);
    }
    else
    {
        wprintf(L"could not create %s (err %lu)\n", g_out, GetLastError());
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// noinline + a distinct name so the walk shows a real, symbolizable frame (makedump!FaultDeep) when
// makedump.pdb is beside the dump.
__declspec(noinline) static void FaultDeep(volatile unsigned char* p)
{
    volatile unsigned char sink = *p; // AV: READ @ 0xDDDDDDDDDDDD0000 (the freed-fill signature)
    (void)sink;
}

__declspec(noinline) static void FaultMid()
{
    // 0xDDDD..0000: the low word cleared, exactly like a real freed object pointer whose offset field was
    // read — matches the Rax/Rcx values seen in the real Agentmaster use-after-free dumps.
    FaultDeep(reinterpret_cast<volatile unsigned char*>(0xDDDDDDDDDDDD0000ULL));
}

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 2) { g_out = argv[1]; }
    __try
    {
        FaultMid();
    }
    __except (WriteDumpFilter(GetExceptionInformation()))
    {
    }
    return 0;
}
