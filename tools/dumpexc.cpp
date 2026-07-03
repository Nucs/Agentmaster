// Agentmaster: dump the full exception record + faulting CONTEXT registers + module map.
// Usage: dumpexc <dmp>
#define _AMD64_
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <vector>
#include <string>

#pragma comment(lib, "dbghelp.lib")

struct Mod { ULONG64 base; ULONG32 size; std::wstring name; };
static std::vector<Mod> g_mods;
static Mod* findMod(ULONG64 a) { for (auto& m : g_mods) if (a >= m.base && a < m.base + m.size) return &m; return nullptr; }

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: dumpexc <dmp>\n"); return 2; }
    HANDLE hf = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE hm = CreateFileMappingW(hf, nullptr, PAGE_READONLY, 0, 0, nullptr);
    BYTE* base = (BYTE*)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (!base) { wprintf(L"map failed\n"); return 1; }

    // modules
    {
        void* stream = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
        if (MiniDumpReadDumpStream(base, ModuleListStream, &dir, &stream, &sz)) {
            auto* ml = (MINIDUMP_MODULE_LIST*)stream;
            for (ULONG32 i = 0; i < ml->NumberOfModules; ++i) {
                auto& m = ml->Modules[i];
                auto* ms = (MINIDUMP_STRING*)(base + m.ModuleNameRva);
                std::wstring full(ms->Buffer, ms->Length / 2);
                size_t slash = full.find_last_of(L"\\/");
                g_mods.push_back({ m.BaseOfImage, m.SizeOfImage, slash == std::wstring::npos ? full : full.substr(slash + 1) });
            }
        }
    }

    void* stream = nullptr; ULONG sz = 0; MINIDUMP_DIRECTORY* dir = nullptr;
    if (!MiniDumpReadDumpStream(base, ExceptionStream, &dir, &stream, &sz)) {
        wprintf(L"no exception stream\n"); return 1;
    }
    auto* es = (MINIDUMP_EXCEPTION_STREAM*)stream;
    auto& er = es->ExceptionRecord;
    wprintf(L"ThreadId          = 0x%X\n", es->ThreadId);
    wprintf(L"ExceptionCode     = 0x%08X\n", er.ExceptionCode);
    wprintf(L"ExceptionFlags    = 0x%08X\n", er.ExceptionFlags);
    wprintf(L"ExceptionAddress  = 0x%llX", (unsigned long long)er.ExceptionAddress);
    if (auto* m = findMod(er.ExceptionAddress)) wprintf(L"  (%s +0x%llX)", m->name.c_str(), (unsigned long long)(er.ExceptionAddress - m->base));
    wprintf(L"\n");
    wprintf(L"NumberParameters  = %u\n", er.NumberParameters);
    for (ULONG i = 0; i < er.NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) {
        wprintf(L"  ExceptionInformation[%u] = 0x%llX (%lld)\n", i,
                (unsigned long long)er.ExceptionInformation[i], (long long)er.ExceptionInformation[i]);
    }

    // Decode fastfail subcode
    if (er.ExceptionCode == 0xC0000409 && er.NumberParameters >= 1) {
        const wchar_t* n = L"?";
        switch (er.ExceptionInformation[0]) {
            case 0:  n = L"FAST_FAIL_LEGACY_GS_VIOLATION"; break;
            case 1:  n = L"FAST_FAIL_VTGUARD_CHECK_FAILURE"; break;
            case 2:  n = L"FAST_FAIL_STACK_COOKIE_CHECK_FAILURE"; break;
            case 3:  n = L"FAST_FAIL_CORRUPT_LIST_ENTRY"; break;
            case 4:  n = L"FAST_FAIL_INCORRECT_STACK"; break;
            case 5:  n = L"FAST_FAIL_INVALID_ARG"; break;
            case 6:  n = L"FAST_FAIL_GS_COOKIE_INIT"; break;
            case 7:  n = L"FAST_FAIL_FATAL_APP_EXIT"; break;
            case 8:  n = L"FAST_FAIL_RANGE_CHECK_FAILURE"; break;
            case 9:  n = L"FAST_FAIL_UNSAFE_REGISTRY_ACCESS"; break;
            case 0x1E: n = L"FAST_FAIL_INVALID_FAST_FAIL_CODE"; break;
            case 0x24: n = L"FAST_FAIL_INVALID_BUFFER_ACCESS"; break;
            case 0x27: n = L"FAST_FAIL_INVALID_JUMP_BUFFER"; break;
            default: break;
        }
        wprintf(L"  >>> fastfail subcode %lld = %s\n", (long long)er.ExceptionInformation[0], n);
    }

    // CONTEXT registers
    CONTEXT* ctx = (CONTEXT*)(base + es->ThreadContext.Rva);
    wprintf(L"\n--- faulting CONTEXT ---\n");
    wprintf(L"  Rip=0x%llX", (unsigned long long)ctx->Rip);
    if (auto* m = findMod(ctx->Rip)) wprintf(L"  (%s +0x%llX)", m->name.c_str(), (unsigned long long)(ctx->Rip - m->base));
    wprintf(L"\n");
    wprintf(L"  Rsp=0x%llX  Rbp=0x%llX\n", (unsigned long long)ctx->Rsp, (unsigned long long)ctx->Rbp);
    wprintf(L"  Rax=0x%llX  Rbx=0x%llX  Rcx=0x%llX  Rdx=0x%llX\n",
            (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx, (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx);
    wprintf(L"  Rsi=0x%llX  Rdi=0x%llX  R8 =0x%llX  R9 =0x%llX\n",
            (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi, (unsigned long long)ctx->R8, (unsigned long long)ctx->R9);
    wprintf(L"  R10=0x%llX  R11=0x%llX  R12=0x%llX  R13=0x%llX\n",
            (unsigned long long)ctx->R10, (unsigned long long)ctx->R11, (unsigned long long)ctx->R12, (unsigned long long)ctx->R13);
    wprintf(L"  R14=0x%llX  R15=0x%llX\n", (unsigned long long)ctx->R14, (unsigned long long)ctx->R15);

    // Which module holds rip if not found above
    wprintf(L"\n--- modules near rip ---\n");
    for (auto& m : g_mods) {
        if (ctx->Rip >= m.base && ctx->Rip < m.base + m.size)
            wprintf(L"  RIP in %s (base 0x%llX size 0x%X)\n", m.name.c_str(), (unsigned long long)m.base, m.size);
    }
    return 0;
}
