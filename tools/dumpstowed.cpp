// Agentmaster: decode the STOWED_EXCEPTION_INFORMATION a 0xC000027B fail-fast points at
// (ExceptionInformation[0]). Prints the nested HRESULT + the ORIGINAL throw backtrace (symbolized),
// so a deferred/off-stack XAML fail-fast can be attributed to the real originating call.
// Usage: dumpstowed <dmp> <bindir-with-pdbs>
#define _AMD64_
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <vector>
#include <string>

#pragma comment(lib, "dbghelp.lib")

struct Mod { ULONG64 base; ULONG32 size; std::wstring name; std::wstring full; };
static std::vector<Mod> g_mods;
static Mod* findMod(ULONG64 a) { for (auto& m : g_mods) if (a >= m.base && a < m.base + m.size) return &m; return nullptr; }

// VA -> pointer into the mapped dump, via Memory64ListStream (full-memory) or MemoryListStream (compact).
static BYTE* g_base = nullptr;
static MINIDUMP_MEMORY64_LIST* g_ml64 = nullptr;
static MINIDUMP_MEMORY_LIST* g_ml = nullptr;

static const BYTE* readVA(ULONG64 va, size_t n) {
    if (g_ml64) {
        ULONG64 off = g_ml64->BaseRva;
        for (ULONG64 i = 0; i < g_ml64->NumberOfMemoryRanges; ++i) {
            auto& r = g_ml64->MemoryRanges[i];
            if (va >= r.StartOfMemoryRange && va + n <= r.StartOfMemoryRange + r.DataSize)
                return g_base + off + (va - r.StartOfMemoryRange);
            off += r.DataSize;
        }
    }
    if (g_ml) {
        for (ULONG32 i = 0; i < g_ml->NumberOfMemoryRanges; ++i) {
            auto& r = g_ml->MemoryRanges[i];
            if (va >= r.StartOfMemoryRange && va + n <= r.StartOfMemoryRange + r.Memory.DataSize)
                return g_base + r.Memory.Rva + (va - r.StartOfMemoryRange);
        }
    }
    return nullptr;
}

static void symLine(ULONG64 addr) {
    Mod* m = findMod(addr);
    const wchar_t* mn = m ? m->name.c_str() : L"?";
    ULONG64 rva = m ? (addr - m->base) : 0;
    char buf[sizeof(SYMBOL_INFO) + 1024];
    auto* si = (SYMBOL_INFO*)buf; si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 1000;
    DWORD64 disp = 0;
    bool ours = m && (_wcsicmp(m->name.c_str(), L"TerminalApp.dll") == 0);
    if (SymFromAddr(GetCurrentProcess(), addr, &disp, si)) {
        wprintf(L"    %s0x%llX  %-24s +0x%llX  %S +0x%llX\n",
                ours ? L">>OURS<< " : L"", (unsigned long long)addr, mn, (unsigned long long)rva, si->Name, (unsigned long long)disp);
    } else {
        wprintf(L"    %s0x%llX  %-24s +0x%llX\n", ours ? L">>OURS<< " : L"", (unsigned long long)addr, mn, (unsigned long long)rva);
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: dumpstowed <dmp> <bindir>\n"); return 2; }
    HANDLE hf = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE hm = CreateFileMappingW(hf, nullptr, PAGE_READONLY, 0, 0, nullptr);
    g_base = (BYTE*)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (!g_base) { wprintf(L"map failed\n"); return 1; }

    void* s = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
    if (MiniDumpReadDumpStream(g_base, ModuleListStream, &dir, &s, &sz)) {
        auto* ml = (MINIDUMP_MODULE_LIST*)s;
        for (ULONG32 i = 0; i < ml->NumberOfModules; ++i) {
            auto& m = ml->Modules[i];
            auto* ms = (MINIDUMP_STRING*)(g_base + m.ModuleNameRva);
            std::wstring full(ms->Buffer, ms->Length / 2);
            size_t slash = full.find_last_of(L"\\/");
            g_mods.push_back({ m.BaseOfImage, m.SizeOfImage, slash == std::wstring::npos ? full : full.substr(slash + 1), full });
        }
    }
    if (MiniDumpReadDumpStream(g_base, Memory64ListStream, &dir, &s, &sz)) g_ml64 = (MINIDUMP_MEMORY64_LIST*)s;
    if (MiniDumpReadDumpStream(g_base, MemoryListStream, &dir, &s, &sz)) g_ml = (MINIDUMP_MEMORY_LIST*)s;

    // symbols
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS);
    SymInitializeW(GetCurrentProcess(), argv[2], FALSE);
    for (auto& m : g_mods)
        SymLoadModuleExW(GetCurrentProcess(), nullptr, m.full.c_str(), m.name.c_str(), m.base, m.size, nullptr, 0);

    // exception record -> stowed pointer
    if (!MiniDumpReadDumpStream(g_base, ExceptionStream, &dir, &s, &sz)) { wprintf(L"no exception stream\n"); return 1; }
    auto* es = (MINIDUMP_EXCEPTION_STREAM*)s;
    auto& er = es->ExceptionRecord;
    wprintf(L"ExceptionCode=0x%08X  params=%u\n", er.ExceptionCode, er.NumberParameters);
    if (er.NumberParameters < 1) { wprintf(L"no stowed pointer\n"); return 1; }

    // Self-test the VA reader against a KNOWN-captured address (the faulting thread's stack pointer).
    {
        CONTEXT* ctx = (CONTEXT*)(g_base + es->ThreadContext.Rva);
        const BYTE* stk = readVA(ctx->Rsp, 64);
        wprintf(L"[selftest] ml64=%p ml=%p  readVA(Rsp=0x%llX)=%s\n",
                (void*)g_ml64, (void*)g_ml, (unsigned long long)ctx->Rsp, stk ? L"OK (reader works)" : L"FAIL (reader bug!)");
    }

    // ExceptionInformation[0] may be a pointer to an ARRAY of stowed-exception pointers, or a single struct.
    // Windows passes: [0]=count-or-ptr. For 0xC000027B it's a pointer to STOWED_EXCEPTION_INFORMATION*[] with
    // count in... practically, [0] points straight at the struct in most XAML cases. Try both.
    ULONG64 p0 = er.ExceptionInformation[0];
    ULONG64 p1 = er.NumberParameters >= 2 ? er.ExceptionInformation[1] : 0;
    wprintf(L"ExceptionInformation[0]=0x%llX  [1]=0x%llX\n\n", (unsigned long long)p0, (unsigned long long)p1);

    // Diagnostics: is the pointed-at memory even in this dump? (WER LocalDumps may omit arbitrary heap.)
    {
        const BYTE* d = readVA(p0, 64);
        wprintf(L"readVA(p0)=%s\n", d ? L"OK" : L"<NOT IN DUMP>");
        if (d) { wprintf(L"  hexdump p0: "); for (int i = 0; i < 32; ++i) wprintf(L"%02X ", d[i]); wprintf(L"\n"); }
        const BYTE* d1 = readVA(p1, 8);
        wprintf(L"readVA(p1)=%s\n\n", d1 ? L"OK" : L"<NOT IN DUMP or small>");
    }

    // Documented STATUS_STOWED_EXCEPTION layout: [0] = pointer to an array of STOWED_EXCEPTION_INFORMATION*,
    // [1] = count. Also try [0] as a direct struct, and [1] as an array ptr (some builds swap).
    std::vector<ULONG64> cands;
    cands.push_back(p0);
    // [0] as array of count [1]
    ULONG64 count = (p1 > 0 && p1 < 64) ? p1 : 8;
    if (const BYTE* arr = readVA(p0, (size_t)count * 8)) {
        for (ULONG64 i = 0; i < count; ++i) {
            ULONG64 v; memcpy(&v, arr + i * 8, 8);
            if (v > 0x10000) cands.push_back(v);
        }
    }
    // [1] as array ptr with count in [0]'s low bits (defensive)
    if (p1 > 0x10000) {
        cands.push_back(p1);
        if (const BYTE* arr = readVA(p1, 8 * 8)) {
            for (int i = 0; i < 8; ++i) { ULONG64 v; memcpy(&v, arr + i * 8, 8); if (v > 0x10000) cands.push_back(v); }
        }
    }

    bool decodedAny = false;
    for (size_t ci = 0; ci < cands.size(); ++ci) {
        ULONG64 sp = cands[ci];
        const BYTE* p = readVA(sp, 56);
        if (!p) continue;
        ULONG32 Size; memcpy(&Size, p + 0, 4);
        ULONG32 Sig;  memcpy(&Sig, p + 4, 4);
        // Signature 'SE01'/'SE02' -> bytes 53 45 30 31 / 32
        char sc[5] = { (char)(Sig & 0xFF), (char)((Sig >> 8) & 0xFF), (char)((Sig >> 16) & 0xFF), (char)((Sig >> 24) & 0xFF), 0 };
        bool looksStowed = (sc[0] == 'S' && sc[1] == 'E' && sc[2] == '0');
        if (!looksStowed) continue;
        decodedAny = true;
        LONG hr; memcpy(&hr, p + 8, 4);
        ULONG32 bits; memcpy(&bits, p + 12, 4);
        ULONG32 form = bits & 0x3;
        ULONG32 tid = bits >> 2;
        wprintf(L"=== STOWED_EXCEPTION @0x%llX  Size=%u Sig=%S ===\n", (unsigned long long)sp, Size, sc);
        wprintf(L"  ResultCode = 0x%08X\n", (unsigned)hr);
        wprintf(L"  ExceptionForm = %u  (1=memory/stack, 2=text)   ThreadId = 0x%X\n", form, tid);
        if (form == 2) {
            ULONG64 txt; memcpy(&txt, p + 16, 8);
            if (const BYTE* t = readVA(txt, 512)) {
                wprintf(L"  ErrorText = %.255s\n", (const wchar_t*)t);
            }
        } else {
            ULONG64 exAddr; memcpy(&exAddr, p + 16, 8);
            ULONG32 wordSz; memcpy(&wordSz, p + 24, 4);
            ULONG32 words; memcpy(&words, p + 28, 4);
            ULONG64 traceP; memcpy(&traceP, p + 32, 8);
            wprintf(L"  ExceptionAddress:\n"); if (exAddr) symLine(exAddr);
            wprintf(L"  Backtrace (%u frames, wordSize=%u) @0x%llX:\n", words, wordSz, (unsigned long long)traceP);
            if (words > 256) words = 256;
            const BYTE* tb = readVA(traceP, (size_t)words * (wordSz ? wordSz : 8));
            if (tb) {
                for (ULONG32 i = 0; i < words; ++i) {
                    ULONG64 a = 0; memcpy(&a, tb + (size_t)i * (wordSz ? wordSz : 8), wordSz == 4 ? 4 : 8);
                    if (a) symLine(a);
                }
            } else {
                wprintf(L"    <backtrace memory not in dump>\n");
            }
        }
        // V2 nested
        if (Size >= 56) {
            ULONG32 nestType; memcpy(&nestType, p + 40, 4);
            ULONG64 nest; memcpy(&nest, p + 48, 8);
            if (nest) { wprintf(L"  NestedExceptionType=0x%X NestedException=0x%llX\n", nestType, (unsigned long long)nest);
                        if (nestType != 0) cands.push_back(nest); }
        }
        wprintf(L"\n");
    }
    if (!decodedAny) wprintf(L"No STOWED_EXCEPTION signature found at the candidate pointers.\n");
    return 0;
}
