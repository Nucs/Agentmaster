// Agentmaster: hang-dump thread stack walker (no exception stream needed).
// Walks a chosen thread (or ALL) from the ThreadList of a procdump minidump.
// Two methods per thread:
//   WALK  - proper x64 StackWalkEx (needs every module's .pdata; can stop short)
//   SCAN  - scan the thread's captured stack memory for any 8-byte value that
//           points into a loaded module (robust; no unwind). Collapses runs.
// Reports per-module whether the PDB MATCHED (sym=Pdb) vs exports-only.
//   usage: hangwalk <dmp> <imageDir> [pdbDir] [tid|ALL] [WALK|SCAN|BOTH]
#define _AMD64_
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <vector>
#include <string>

#pragma comment(lib, "dbghelp.lib")

static BYTE* g_base = nullptr;
struct Range { ULONG64 va, size, fileRva; };
static std::vector<Range> g_ranges;
struct Mod { ULONG64 base; ULONG32 size; std::wstring name; };
static std::vector<Mod> g_mods;

static Mod* findMod(ULONG64 a) { for (auto& m : g_mods) if (a >= m.base && a < m.base + m.size) return &m; return nullptr; }

static bool readDumpMem(ULONG64 va, void* out, DWORD size, DWORD* read) {
    DWORD done = 0; BYTE* dst = (BYTE*)out;
    while (size > 0) {
        bool found = false;
        for (auto& r : g_ranges)
            if (va >= r.va && va < r.va + r.size) {
                ULONG64 avail = r.va + r.size - va; DWORD chunk = (DWORD)(avail < size ? avail : size);
                memcpy(dst, g_base + r.fileRva + (va - r.va), chunk);
                va += chunk; dst += chunk; size -= chunk; done += chunk; found = true; break;
            }
        if (!found) break;
    }
    if (read) *read = done; return done > 0;
}
static BOOL CALLBACK readMemCb(HANDLE, DWORD64 a, PVOID b, DWORD s, LPDWORD r) { return readDumpMem(a, b, s, r) ? TRUE : FALSE; }

static const wchar_t* symTypeName(SYM_TYPE t) {
    switch (t) { case SymNone: return L"None"; case SymPdb: return L"Pdb"; case SymExport: return L"Export";
        case SymDeferred: return L"Deferred"; case SymDia: return L"Dia"; default: return L"other"; }
}

static std::wstring symAt(HANDLE proc, ULONG64 pc, DWORD64& disp) {
    alignas(16) BYTE buf[sizeof(SYMBOL_INFOW) + 1024 * sizeof(wchar_t)];
    auto* si = (SYMBOL_INFOW*)buf; si->SizeOfStruct = sizeof(SYMBOL_INFOW); si->MaxNameLen = 1024; disp = 0;
    if (SymFromAddrW(proc, pc, &disp, si)) return si->Name;
    return L"";
}

static void walkThread(HANDLE proc, CONTEXT ctx, int maxFrames) {
    STACKFRAME_EX fr{};
    fr.AddrPC.Offset = ctx.Rip; fr.AddrPC.Mode = AddrModeFlat;
    fr.AddrFrame.Offset = ctx.Rbp; fr.AddrFrame.Mode = AddrModeFlat;
    fr.AddrStack.Offset = ctx.Rsp; fr.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < maxFrames; ++i) {
        if (!StackWalkEx(IMAGE_FILE_MACHINE_AMD64, proc, nullptr, &fr, &ctx, readMemCb,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr, 0)) break;
        if (fr.AddrPC.Offset == 0) break;
        ULONG64 pc = fr.AddrPC.Offset; Mod* m = findMod(pc);
        DWORD64 disp = 0; std::wstring fn = symAt(proc, pc, disp);
        ULONG64 rva = m ? pc - m->base : 0;
        if (!fn.empty()) wprintf(L"  #%02d %-26s +0x%06llX  %s +0x%llX\n", i, m ? m->name.c_str() : L"?", (unsigned long long)rva, fn.c_str(), (unsigned long long)disp);
        else             wprintf(L"  #%02d %-26s +0x%06llX\n", i, m ? m->name.c_str() : L"?", (unsigned long long)rva);
    }
}

static void scanThread(HANDLE proc, ULONG64 stackStart, BYTE* data, ULONG64 size) {
    if (!data || size < 8) { wprintf(L"  (no captured stack memory)\n"); return; }
    std::wstring last; int printed = 0;
    for (ULONG64 off = 0; off + 8 <= size; off += 8) {
        ULONG64 val = *(ULONG64*)(data + off);
        Mod* m = findMod(val); if (!m) continue;
        ULONG64 rva = val - 1 - m->base; // return addr points past the CALL
        DWORD64 disp = 0; std::wstring fn = symAt(proc, val - 1, disp);
        std::wstring tag = m->name + L"+" + std::to_wstring(rva) + fn;
        if (tag == last) continue; last = tag;
        if (!fn.empty()) wprintf(L"  @+%05llX  %-26s +0x%06llX  %s\n", (unsigned long long)off, m->name.c_str(), (unsigned long long)rva, fn.c_str());
        else             wprintf(L"  @+%05llX  %-26s +0x%06llX\n", (unsigned long long)off, m->name.c_str(), (unsigned long long)rva);
        if (++printed > 120) { wprintf(L"  ... (truncated)\n"); break; }
    }
    if (!printed) wprintf(L"  (no in-module pointers on stack)\n");
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: hangwalk <dmp> <imageDir> [pdbDir] [tid|ALL] [WALK|SCAN|BOTH]\n"); return 2; }
    std::wstring imageDir = argv[2];
    std::wstring pdbDir = (argc > 3) ? argv[3] : imageDir;
    bool all = true; DWORD onlyTid = 0;
    if (argc > 4) { std::wstring t = argv[4]; if (t != L"ALL") { all = false; onlyTid = wcstoul(t.c_str(), nullptr, 0); } }
    std::wstring mode = (argc > 5) ? argv[5] : (all ? L"WALK" : L"BOTH");

    HANDLE hf = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hf == INVALID_HANDLE_VALUE) { wprintf(L"open dmp failed\n"); return 1; }
    HANDLE hm = CreateFileMappingW(hf, nullptr, PAGE_READONLY, 0, 0, nullptr);
    g_base = (BYTE*)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (!g_base) { wprintf(L"map failed\n"); return 1; }

    { void* st = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
      if (MiniDumpReadDumpStream(g_base, Memory64ListStream, &dir, &st, &sz)) {
          auto* ml = (MINIDUMP_MEMORY64_LIST*)st; ULONG64 off = ml->BaseRva;
          for (ULONG64 i = 0; i < ml->NumberOfMemoryRanges; ++i) { auto& d = ml->MemoryRanges[i]; g_ranges.push_back({ d.StartOfMemoryRange, d.DataSize, off }); off += d.DataSize; }
      } else if (MiniDumpReadDumpStream(g_base, MemoryListStream, &dir, &st, &sz)) {
          auto* ml = (MINIDUMP_MEMORY_LIST*)st;
          for (ULONG32 i = 0; i < ml->NumberOfMemoryRanges; ++i) { auto& d = ml->MemoryRanges[i]; g_ranges.push_back({ d.StartOfMemoryRange, d.Memory.DataSize, d.Memory.Rva }); }
      }
    }
    wprintf(L"[info] memory ranges=%zu\n", g_ranges.size());

    HANDLE proc = (HANDLE)0x1234;
    std::wstring sympath = imageDir + L";" + pdbDir;
    // NOTE: no SYMOPT_EXACT_SYMBOLS -> allow export-table fallback so system
    // DLLs (win32u/user32/ntdll) resolve to nearest export without a PDB.
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS);
    SymInitializeW(proc, sympath.c_str(), FALSE);

    { void* st = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
      if (MiniDumpReadDumpStream(g_base, ModuleListStream, &dir, &st, &sz)) {
          auto* ml = (MINIDUMP_MODULE_LIST*)st;
          for (ULONG32 i = 0; i < ml->NumberOfModules; ++i) { auto& m = ml->Modules[i];
              auto* ms = (MINIDUMP_STRING*)(g_base + m.ModuleNameRva);
              std::wstring full(ms->Buffer, ms->Length / 2);
              size_t sl = full.find_last_of(L"\\/"); std::wstring leaf = (sl == std::wstring::npos) ? full : full.substr(sl + 1);
              g_mods.push_back({ m.BaseOfImage, m.SizeOfImage, leaf });
              std::wstring onDisk = imageDir + L"\\" + leaf;
              const wchar_t* img = (GetFileAttributesW(onDisk.c_str()) != INVALID_FILE_ATTRIBUTES) ? onDisk.c_str() : full.c_str();
              SymLoadModuleExW(proc, nullptr, img, leaf.c_str(), m.BaseOfImage, m.SizeOfImage, nullptr, 0);
              if (leaf.find(L"TerminalApp") != std::wstring::npos) {
                  IMAGEHLP_MODULEW64 mi{}; mi.SizeOfStruct = sizeof(mi);
                  if (SymGetModuleInfoW64(proc, m.BaseOfImage, &mi))
                      wprintf(L"[mod] %-30s sym=%s pdb=%s\n", leaf.c_str(), symTypeName(mi.SymType), mi.LoadedPdbName[0] ? mi.LoadedPdbName : L"(none)");
              }
          }
      }
    }

    { void* st = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
      if (MiniDumpReadDumpStream(g_base, ThreadListStream, &dir, &st, &sz)) {
          auto* tl = (MINIDUMP_THREAD_LIST*)st;
          for (ULONG32 i = 0; i < tl->NumberOfThreads; ++i) { auto& t = tl->Threads[i];
              if (!all && t.ThreadId != onlyTid) continue;
              if (t.ThreadContext.Rva == 0 || t.ThreadContext.DataSize < sizeof(CONTEXT)) continue;
              CONTEXT ctx = *(CONTEXT*)(g_base + t.ThreadContext.Rva);
              wprintf(L"\n===== thread tid=%u (0x%X)  rip=0x%llX rsp=0x%llX =====\n", t.ThreadId, t.ThreadId,
                      (unsigned long long)ctx.Rip, (unsigned long long)ctx.Rsp);
              if (mode == L"WALK" || mode == L"BOTH") { wprintf(L"-- unwind --\n"); walkThread(proc, ctx, all ? 6 : 60); }
              if (mode == L"SCAN" || mode == L"BOTH") {
                  wprintf(L"-- stack scan --\n");
                  // Full-memory dumps (comsvcs/WER) don't duplicate stacks into the
                  // ThreadList descriptors (Stack.Memory.Rva is stale/garbage there) —
                  // read the stack out of the Memory64 ranges from RSP upward instead.
                  ULONG64 stackTop = ctx.Rsp & ~7ull;
                  ULONG64 cap = 512 * 1024;
                  std::vector<BYTE> sbuf(cap);
                  DWORD got = 0; readDumpMem(stackTop, sbuf.data(), (DWORD)cap, &got);
                  scanThread(proc, stackTop, sbuf.data(), got);
              }
          }
      }
    }
    SymCleanup(proc);
    return 0;
}
