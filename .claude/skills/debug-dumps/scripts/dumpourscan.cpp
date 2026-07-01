// Agentmaster: scan the FULL faulting-thread stack region (from Memory64List) for
// return addresses into OUR modules only, symbolized via DIA against the PDBs.
// Ignores MUX/system frames (no PDB, pure noise). Usage: dumpourscan <dmp> <bindir>
#define _AMD64_
#include <windows.h>
#include <dbghelp.h>
#include <dia2.h>
#include <diacreate.h>
#include <cstdio>
#include <vector>
#include <string>

#pragma comment(lib, "dbghelp.lib")

static const wchar_t* kMsdia =
    L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\Automation\\msdia140.dll";

struct Range { ULONG64 va, size, fileRva; };
static std::vector<Range> g_ranges;
static BYTE* g_base = nullptr;

struct Module { ULONG64 base; ULONG32 size; std::wstring name; IDiaSession* dia; bool ours; };
static std::vector<Module> g_mods;

static Module* findMod(ULONG64 a) { for (auto& m : g_mods) if (a >= m.base && a < m.base + m.size) return &m; return nullptr; }

static bool readMem(ULONG64 va, void* out, ULONG64 size) {
    for (auto& r : g_ranges) if (va >= r.va && va + size <= r.va + r.size) {
        memcpy(out, g_base + r.fileRva + (va - r.va), (size_t)size); return true;
    }
    return false;
}

static IDiaSession* openDia(const std::wstring& pdb) {
    IDiaDataSource* src = nullptr;
    if (FAILED(NoRegCoCreate(kMsdia, __uuidof(DiaSource), __uuidof(IDiaDataSource), (void**)&src)) || !src) return nullptr;
    if (FAILED(src->loadDataFromPdb(pdb.c_str()))) { src->Release(); return nullptr; }
    IDiaSession* s = nullptr;
    if (FAILED(src->openSession(&s))) { src->Release(); return nullptr; }
    return s;
}

static bool symbolize(IDiaSession* dia, DWORD rva, std::wstring& out) {
    IDiaSymbol* fn = nullptr;
    if (SUCCEEDED(dia->findSymbolByRVA(rva, SymTagFunction, &fn)) && fn) {
        BSTR name = nullptr;
        if (FAILED(fn->get_undecoratedNameEx(0, &name)) || !name) fn->get_name(&name);
        DWORD fnRva = 0; fn->get_relativeVirtualAddress(&fnRva);
        std::wstring fname;
        IDiaEnumLineNumbers* lines = nullptr;
        if (SUCCEEDED(dia->findLinesByRVA(rva, 1, &lines)) && lines) {
            IDiaLineNumber* line = nullptr; ULONG celt = 0;
            if (SUCCEEDED(lines->Next(1, &line, &celt)) && celt == 1) {
                DWORD num = 0; line->get_lineNumber(&num);
                IDiaSourceFile* sf = nullptr;
                if (SUCCEEDED(line->get_sourceFile(&sf)) && sf) {
                    BSTR f = nullptr; sf->get_fileName(&f);
                    if (f) { fname = std::wstring(f) + L":" + std::to_wstring(num); SysFreeString(f); }
                    sf->Release();
                }
                line->Release();
            }
            lines->Release();
        }
        wchar_t buf[1400];
        swprintf(buf, 1400, L"%s  [%s]  (+0x%X)", name ? name : L"<fn>", fname.c_str(), rva - fnRva);
        if (name) SysFreeString(name);
        fn->Release();
        out = buf;
        return true;
    }
    return false;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        wprintf(L"usage: dumpourscan <dmp> <bindir> [ourModuleSubstring]\n"
                L"  <bindir>             dir holding the CRASHED build's PDBs (for symbolization)\n"
                L"  [ourModuleSubstring] optional: treat any module whose name CONTAINS this\n"
                L"                       (case-insensitive) as \"ours\" -> symbolize + scan its frames.\n"
                L"                       Default (Agentmaster): TerminalApp.dll / Microsoft.Terminal.Control.dll\n"
                L"                       / Microsoft.Terminal.Settings.Model.dll.\n");
        return 2;
    }
    CoInitialize(nullptr);
    HANDLE hf = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE hm = CreateFileMappingW(hf, nullptr, PAGE_READONLY, 0, 0, nullptr);
    g_base = (BYTE*)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (!g_base) { wprintf(L"map failed\n"); return 1; }
    std::wstring bindir = argv[2];
    std::wstring ourFilter = (argc >= 4) ? argv[3] : L""; // optional: which module is "ours"
    for (auto& c : ourFilter) if (c >= L'A' && c <= L'Z') c = wchar_t(c - L'A' + L'a');

    // memory ranges (Memory64List)
    {
        void* stream = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
        if (MiniDumpReadDumpStream(g_base, Memory64ListStream, &dir, &stream, &sz)) {
            auto* ml = (MINIDUMP_MEMORY64_LIST*)stream;
            ULONG64 rva = ml->BaseRva;
            for (ULONG64 i = 0; i < ml->NumberOfMemoryRanges; ++i) {
                g_ranges.push_back({ ml->MemoryRanges[i].StartOfMemoryRange, ml->MemoryRanges[i].DataSize, rva });
                rva += ml->MemoryRanges[i].DataSize;
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

    // modules
    {
        void* stream = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
        if (MiniDumpReadDumpStream(g_base, ModuleListStream, &dir, &stream, &sz)) {
            auto* ml = (MINIDUMP_MODULE_LIST*)stream;
            for (ULONG32 i = 0; i < ml->NumberOfModules; ++i) {
                auto& m = ml->Modules[i];
                auto* ms = (MINIDUMP_STRING*)(g_base + m.ModuleNameRva);
                std::wstring full(ms->Buffer, ms->Length / 2);
                size_t slash = full.find_last_of(L"\\/");
                std::wstring leaf = slash == std::wstring::npos ? full : full.substr(slash + 1);
                bool ours;
                if (!ourFilter.empty()) {
                    std::wstring low = leaf;
                    for (auto& c : low) if (c >= L'A' && c <= L'Z') c = wchar_t(c - L'A' + L'a');
                    ours = low.find(ourFilter) != std::wstring::npos;
                } else {
                    ours = (leaf == L"TerminalApp.dll" || leaf == L"Microsoft.Terminal.Control.dll" ||
                            leaf == L"Microsoft.Terminal.Settings.Model.dll");
                }
                std::wstring pdbleaf = leaf; size_t dot = pdbleaf.find_last_of(L'.');
                if (dot != std::wstring::npos) pdbleaf = pdbleaf.substr(0, dot);
                std::wstring pdb = bindir + L"\\" + pdbleaf + L".pdb";
                IDiaSession* dia = nullptr;
                if (ours && GetFileAttributesW(pdb.c_str()) != INVALID_FILE_ATTRIBUTES) dia = openDia(pdb);
                g_mods.push_back({ m.BaseOfImage, m.SizeOfImage, leaf, dia, ours });
            }
        }
    }

    // faulting context
    ULONG64 rip = 0, rsp = 0; DWORD tid = 0; DWORD code = 0;
    {
        void* stream = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
        if (MiniDumpReadDumpStream(g_base, ExceptionStream, &dir, &stream, &sz)) {
            auto* es = (MINIDUMP_EXCEPTION_STREAM*)stream;
            tid = es->ThreadId; code = es->ExceptionRecord.ExceptionCode;
            CONTEXT* ctx = (CONTEXT*)(g_base + es->ThreadContext.Rva);
            rip = ctx->Rip; rsp = ctx->Rsp;
            auto& er = es->ExceptionRecord;
            wprintf(L"exception params: n=%u", er.NumberParameters);
            if (er.NumberParameters >= 2) {
                const wchar_t* op = er.ExceptionInformation[0] == 0 ? L"READ" :
                                    er.ExceptionInformation[0] == 1 ? L"WRITE" :
                                    er.ExceptionInformation[0] == 8 ? L"EXECUTE" : L"?";
                wprintf(L"  access=%s  faultData=0x%llX", op,
                        (unsigned long long)er.ExceptionInformation[1]);
            }
            wprintf(L"\n");
        }
    }
    wprintf(L"faulting tid=0x%X code=0x%08X rip=0x%llX rsp=0x%llX\n", tid, code,
            (unsigned long long)rip, (unsigned long long)rsp);
    { Module* m = findMod(rip); if (m) wprintf(L"#FAULT %s +0x%llX\n\n", m->name.c_str(), (unsigned long long)(rip - m->base)); }

    // scan a wide window upward from rsp (stacks fragment across page-ranges; readMem skips gaps)
    ULONG64 top = rsp + 0x80000; // 512 KiB
    wprintf(L"scanning stack 0x%llX .. 0x%llX (%llu bytes) for OUR frames:\n\n",
            (unsigned long long)rsp, (unsigned long long)top, (unsigned long long)(top - rsp));

    int printed = 0;
    for (ULONG64 a = rsp; a + 8 <= top; a += 8) {
        ULONG64 val = 0;
        if (!readMem(a, &val, 8)) continue;
        Module* m = findMod(val);
        if (!m || !m->ours) continue;
        DWORD rva = (DWORD)(val - 1 - m->base); // return addr → land inside the CALL
        std::wstring sym;
        if (m->dia && symbolize(m->dia, rva, sym))
            wprintf(L"  @0x%llX  %-28s +0x%06X  %s\n", (unsigned long long)a, m->name.c_str(), rva, sym.c_str());
        else
            wprintf(L"  @0x%llX  %-28s +0x%06X\n", (unsigned long long)a, m->name.c_str(), rva);
        ++printed;
    }
    wprintf(L"\n(%d OUR-module return addresses on the stack)\n", printed);

    // characterize: count module hits across the window (any module)
    wprintf(L"\n--- module histogram over the window ---\n");
    std::vector<std::pair<std::wstring, int>> hist;
    for (ULONG64 a = rsp; a + 8 <= top; a += 8) {
        ULONG64 val = 0; if (!readMem(a, &val, 8)) continue;
        Module* m = findMod(val); if (!m) continue;
        bool f = false;
        for (auto& h : hist) if (h.first == m->name) { h.second++; f = true; break; }
        if (!f) hist.push_back({ m->name, 1 });
    }
    for (auto& h : hist) wprintf(L"  %-34s  %d\n", h.first.c_str(), h.second);
    return 0;
}
