// Agentmaster: minidump stack reconstructor (no cdb/WinDbg needed).
// Reads the faulting thread's RIP/RSP + stack memory from a .dmp, scans for
// return addresses that fall inside loaded modules, and symbolizes each via DIA
// against <bindir>\<module>.pdb.  Usage: dumpstack <dmp> <bindir>
#define _AMD64_
#include <windows.h>
#include <dbghelp.h>
#include <dia2.h>
#include <diacreate.h>
#include <cstdio>
#include <vector>
#include <string>
#include <map>

#pragma comment(lib, "dbghelp.lib")

static const wchar_t* kMsdia =
    L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\Automation\\msdia140.dll";

struct Module { ULONG64 base; ULONG32 size; std::wstring name; IDiaSession* dia; };

static IDiaSession* openDia(const std::wstring& pdb) {
    IDiaDataSource* src = nullptr;
    if (FAILED(NoRegCoCreate(kMsdia, __uuidof(DiaSource), __uuidof(IDiaDataSource), (void**)&src)) || !src) return nullptr;
    if (FAILED(src->loadDataFromPdb(pdb.c_str()))) { src->Release(); return nullptr; }
    IDiaSession* s = nullptr;
    if (FAILED(src->openSession(&s))) { src->Release(); return nullptr; }
    return s;
}

static bool symbolize(IDiaSession* dia, DWORD rva, std::wstring& out) {
    wchar_t buf[1024]; buf[0] = 0;
    IDiaSymbol* fn = nullptr;
    if (SUCCEEDED(dia->findSymbolByRVA(rva, SymTagFunction, &fn)) && fn) {
        BSTR name = nullptr;
        if (SUCCEEDED(fn->get_undecoratedNameEx(0, &name)) && name) {}
        else if (SUCCEEDED(fn->get_name(&name)) && name) {}
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
        swprintf(buf, 1024, L"%s\t[%s]  (+0x%X)", name ? name : L"<fn>", fname.c_str(), rva - fnRva);
        if (name) SysFreeString(name);
        fn->Release();
        out = buf;
        return true;
    }
    return false;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: dumpstack <dmp> <bindir>\n"); return 2; }
    CoInitialize(nullptr);

    HANDLE hf = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hf == INVALID_HANDLE_VALUE) { wprintf(L"open dmp failed\n"); return 1; }
    HANDLE hm = CreateFileMappingW(hf, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* base = MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (!base) { wprintf(L"map failed\n"); return 1; }

    std::wstring bindir = argv[2];

    // --- modules ---
    std::vector<Module> mods;
    {
        void* stream = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
        if (MiniDumpReadDumpStream(base, ModuleListStream, &dir, &stream, &sz)) {
            auto* ml = (MINIDUMP_MODULE_LIST*)stream;
            for (ULONG32 i = 0; i < ml->NumberOfModules; ++i) {
                auto& m = ml->Modules[i];
                auto* ms = (MINIDUMP_STRING*)((BYTE*)base + m.ModuleNameRva);
                std::wstring full(ms->Buffer, ms->Length / 2);
                size_t slash = full.find_last_of(L"\\/");
                std::wstring leaf = slash == std::wstring::npos ? full : full.substr(slash + 1);
                std::wstring pdbleaf = leaf;
                size_t dot = pdbleaf.find_last_of(L'.');
                if (dot != std::wstring::npos) pdbleaf = pdbleaf.substr(0, dot);
                std::wstring pdb = bindir + L"\\" + pdbleaf + L".pdb";
                IDiaSession* dia = nullptr;
                if (GetFileAttributesW(pdb.c_str()) != INVALID_FILE_ATTRIBUTES) dia = openDia(pdb);
                mods.push_back({ m.BaseOfImage, m.SizeOfImage, leaf, dia });
            }
        }
    }
    auto findMod = [&](ULONG64 addr) -> Module* {
        for (auto& m : mods) if (addr >= m.base && addr < m.base + m.size) return &m;
        return nullptr;
    };

    // --- faulting thread context ---
    DWORD faultTid = 0; CONTEXT* ctx = nullptr;
    {
        void* stream = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
        if (MiniDumpReadDumpStream(base, ExceptionStream, &dir, &stream, &sz)) {
            auto* es = (MINIDUMP_EXCEPTION_STREAM*)stream;
            faultTid = es->ThreadId;
            ctx = (CONTEXT*)((BYTE*)base + es->ThreadContext.Rva);
            wprintf(L"Faulting thread : 0x%X\n", faultTid);
            wprintf(L"Exception code  : 0x%08X  addr=0x%llX\n",
                    es->ExceptionRecord.ExceptionCode,
                    (unsigned long long)es->ExceptionRecord.ExceptionAddress);
        }
    }
    if (!ctx) { wprintf(L"no exception context\n"); return 1; }

    ULONG64 rip = ctx->Rip, rsp = ctx->Rsp, rbp = ctx->Rbp;
    wprintf(L"RIP=0x%llX RSP=0x%llX RBP=0x%llX\n\n", (unsigned long long)rip,
            (unsigned long long)rsp, (unsigned long long)rbp);

    auto printFrame = [&](const wchar_t* tag, ULONG64 addr) {
        Module* m = findMod(addr);
        if (!m) { wprintf(L"%s 0x%llX  <no module>\n", tag, (unsigned long long)addr); return; }
        DWORD rva = (DWORD)(addr - m->base);
        std::wstring sym;
        if (m->dia && symbolize(m->dia, rva, sym))
            wprintf(L"%s %-32s +0x%06X  %s\n", tag, m->name.c_str(), rva, sym.c_str());
        else
            wprintf(L"%s %-32s +0x%06X\n", tag, m->name.c_str(), rva);
    };

    printFrame(L"#FAULT ", rip);

    // --- find the faulting thread's stack memory ---
    ULONG64 stackStart = 0, stackEnd = 0; BYTE* stackData = nullptr;
    {
        void* stream = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
        if (MiniDumpReadDumpStream(base, ThreadListStream, &dir, &stream, &sz)) {
            auto* tl = (MINIDUMP_THREAD_LIST*)stream;
            for (ULONG32 i = 0; i < tl->NumberOfThreads; ++i) {
                auto& t = tl->Threads[i];
                if (t.ThreadId == faultTid) {
                    stackStart = t.Stack.StartOfMemoryRange;
                    stackEnd = stackStart + t.Stack.Memory.DataSize;
                    stackData = (BYTE*)base + t.Stack.Memory.Rva;
                    break;
                }
            }
        }
    }
    if (!stackData) { wprintf(L"\nno stack memory for faulting thread\n"); return 1; }

    wprintf(L"\n--- stack scan (RSP..stacktop): every pointer into a loaded module ---\n");
    ULONG64 startScan = (rsp >= stackStart && rsp < stackEnd) ? rsp : stackStart;
    int printed = 0;
    std::wstring lastSym;
    for (ULONG64 a = startScan; a + 8 <= stackEnd; a += 8) {
        ULONG64 val = *(ULONG64*)(stackData + (a - stackStart));
        Module* m = findMod(val);
        if (!m) continue;
        // a return address points just past a CALL; symbolize val-1 to land in the call.
        DWORD rva = (DWORD)(val - 1 - m->base);
        std::wstring sym;
        bool got = m->dia && symbolize(m->dia, rva, sym);
        if (got) {
            if (sym == lastSym) continue; // collapse runs of the same frame
            lastSym = sym;
            wprintf(L"  @%llX  %-30s +0x%06X  %s\n",
                    (unsigned long long)a, m->name.c_str(), rva, sym.c_str());
        } else {
            wprintf(L"  @%llX  %-30s +0x%06X\n", (unsigned long long)a, m->name.c_str(), rva);
        }
        if (++printed > 200) break;
    }
    wprintf(L"\n(%d in-module stack pointers)\n", printed);
    return 0;
}
