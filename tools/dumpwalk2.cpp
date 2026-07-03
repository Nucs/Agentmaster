// Agentmaster: proper x64 minidump stack walk via DbgHelp StackWalkEx.
// Loads each module from disk (bindir) so DbgHelp reads its .pdata unwind info,
// serves stack/memory reads out of the dump, and symbolizes each frame.
// Usage: dumpwalk2 <dmp> <bindir>
#define _AMD64_
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <vector>
#include <string>

#pragma comment(lib, "dbghelp.lib")

static BYTE* g_base = nullptr;

// dump memory ranges
struct Range { ULONG64 va; ULONG64 size; ULONG64 fileRva; };
static std::vector<Range> g_ranges;

static bool readDumpMem(ULONG64 va, void* out, DWORD size, DWORD* read) {
    DWORD done = 0;
    BYTE* dst = (BYTE*)out;
    while (size > 0) {
        bool found = false;
        for (auto& r : g_ranges) {
            if (va >= r.va && va < r.va + r.size) {
                ULONG64 avail = r.va + r.size - va;
                DWORD chunk = (DWORD)(avail < size ? avail : size);
                memcpy(dst, g_base + r.fileRva + (va - r.va), chunk);
                va += chunk; dst += chunk; size -= chunk; done += chunk;
                found = true; break;
            }
        }
        if (!found) break;
    }
    if (read) *read = done;
    return done > 0;
}

static BOOL CALLBACK readMemCb(HANDLE, DWORD64 addr, PVOID buf, DWORD size, LPDWORD read) {
    return readDumpMem(addr, buf, size, read) ? TRUE : FALSE;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: dumpwalk2 <dmp> <bindir>\n"); return 2; }
    std::wstring bindir = argv[2];

    HANDLE hf = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hf == INVALID_HANDLE_VALUE) { wprintf(L"open dmp failed\n"); return 1; }
    HANDLE hm = CreateFileMappingW(hf, nullptr, PAGE_READONLY, 0, 0, nullptr);
    g_base = (BYTE*)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (!g_base) { wprintf(L"map failed\n"); return 1; }

    // memory ranges (prefer Memory64List)
    {
        void* stream = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
        if (MiniDumpReadDumpStream(g_base, Memory64ListStream, &dir, &stream, &sz)) {
            auto* ml = (MINIDUMP_MEMORY64_LIST*)stream;
            ULONG64 off = ml->BaseRva;
            for (ULONG64 i = 0; i < ml->NumberOfMemoryRanges; ++i) {
                auto& d = ml->MemoryRanges[i];
                g_ranges.push_back({ d.StartOfMemoryRange, d.DataSize, off });
                off += d.DataSize;
            }
        } else if (MiniDumpReadDumpStream(g_base, MemoryListStream, &dir, &stream, &sz)) {
            auto* ml = (MINIDUMP_MEMORY_LIST*)stream;
            for (ULONG32 i = 0; i < ml->NumberOfMemoryRanges; ++i) {
                auto& d = ml->MemoryRanges[i];
                g_ranges.push_back({ d.StartOfMemoryRange, d.Memory.DataSize, d.Memory.Rva });
            }
        }
    }
    wprintf(L"memory ranges: %zu\n", g_ranges.size());

    HANDLE proc = (HANDLE)0x1234; // fake process handle for dump symbol api
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitializeW(proc, bindir.c_str(), FALSE)) { wprintf(L"SymInitialize failed %lu\n", GetLastError()); }

    // load each module from disk so .pdata + pdb resolve
    {
        void* stream = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
        if (MiniDumpReadDumpStream(g_base, ModuleListStream, &dir, &stream, &sz)) {
            auto* ml = (MINIDUMP_MODULE_LIST*)stream;
            for (ULONG32 i = 0; i < ml->NumberOfModules; ++i) {
                auto& m = ml->Modules[i];
                auto* ms = (MINIDUMP_STRING*)(g_base + m.ModuleNameRva);
                std::wstring full(ms->Buffer, ms->Length / 2);
                size_t slash = full.find_last_of(L"\\/");
                std::wstring leaf = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
                std::wstring onDisk = bindir + L"\\" + leaf;
                const wchar_t* imgPath = (GetFileAttributesW(onDisk.c_str()) != INVALID_FILE_ATTRIBUTES)
                                         ? onDisk.c_str() : full.c_str();
                SymLoadModuleExW(proc, nullptr, imgPath, leaf.c_str(), m.BaseOfImage, m.SizeOfImage, nullptr, 0);
            }
        }
    }

    // faulting thread context
    DWORD faultTid = 0; CONTEXT ctx{};
    {
        void* stream = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
        if (MiniDumpReadDumpStream(g_base, ExceptionStream, &dir, &stream, &sz)) {
            auto* es = (MINIDUMP_EXCEPTION_STREAM*)stream;
            faultTid = es->ThreadId;
            ctx = *(CONTEXT*)(g_base + es->ThreadContext.Rva);
            wprintf(L"Faulting thread 0x%X  code 0x%08X  faultAddr 0x%llX\n\n",
                    faultTid, es->ExceptionRecord.ExceptionCode,
                    (unsigned long long)es->ExceptionRecord.ExceptionAddress);
        }
    }

    STACKFRAME_EX frame{};
    frame.AddrPC.Offset = ctx.Rip;      frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;   frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;   frame.AddrStack.Mode = AddrModeFlat;

    alignas(16) BYTE symbuf[sizeof(SYMBOL_INFOW) + 1024 * sizeof(wchar_t)];
    auto* si = (SYMBOL_INFOW*)symbuf;

    for (int i = 0; i < 80; ++i) {
        if (!StackWalkEx(IMAGE_FILE_MACHINE_AMD64, proc, nullptr, &frame, &ctx,
                         readMemCb, SymFunctionTableAccess64, SymGetModuleBase64, nullptr, 0))
            break;
        if (frame.AddrPC.Offset == 0) break;

        ULONG64 pc = frame.AddrPC.Offset;
        IMAGEHLP_MODULEW64 mi{}; mi.SizeOfStruct = sizeof(mi);
        std::wstring modName = L"?";
        if (SymGetModuleInfoW64(proc, pc, &mi)) modName = mi.ModuleName;

        si->SizeOfStruct = sizeof(SYMBOL_INFOW); si->MaxNameLen = 1024;
        DWORD64 disp = 0;
        std::wstring fn = L"<no-sym>";
        if (SymFromAddrW(proc, pc, &disp, si)) fn = si->Name;

        IMAGEHLP_LINEW64 line{}; line.SizeOfStruct = sizeof(line);
        DWORD ld = 0; std::wstring loc;
        if (SymGetLineFromAddrW64(proc, pc, &ld, &line))
            loc = std::wstring(line.FileName) + L":" + std::to_wstring(line.LineNumber);

        wprintf(L"#%02d  %-22s  %s +0x%llX\n        %s\n",
                i, modName.c_str(), fn.c_str(), (unsigned long long)disp, loc.c_str());
    }

    SymCleanup(proc);
    return 0;
}
