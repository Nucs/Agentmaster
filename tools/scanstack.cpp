// Agentmaster: symbolized SCAN of the faulting thread stack via
// dbghelp+symsrv (WUX names resolve from the MS symbol server). Unwind-free — works where
// StackWalkEx breaks (partial WER dumps). Noise (stale frames) acceptable; we hunt signatures.
#define _AMD64_
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <vector>
#include <string>
#pragma comment(lib, "dbghelp.lib")
struct Mod { ULONG64 base; ULONG32 size; std::wstring name; std::wstring full; };
static std::vector<Mod> g_mods;
static Mod* findMod(ULONG64 a){ for(auto&m:g_mods) if(a>=m.base&&a<m.base+m.size) return &m; return nullptr; }
static BYTE* g_base=nullptr; static MINIDUMP_MEMORY64_LIST* g_ml64=nullptr; static MINIDUMP_MEMORY_LIST* g_ml=nullptr;
static const BYTE* readVA(ULONG64 va,size_t n){
    if(g_ml64){ ULONG64 off=g_ml64->BaseRva; for(ULONG64 i=0;i<g_ml64->NumberOfMemoryRanges;++i){auto&r=g_ml64->MemoryRanges[i]; if(va>=r.StartOfMemoryRange&&va+n<=r.StartOfMemoryRange+r.DataSize) return g_base+off+(va-r.StartOfMemoryRange); off+=r.DataSize;} }
    if(g_ml){ for(ULONG32 i=0;i<g_ml->NumberOfMemoryRanges;++i){auto&r=g_ml->MemoryRanges[i]; if(va>=r.StartOfMemoryRange&&va+n<=r.StartOfMemoryRange+r.Memory.DataSize) return g_base+r.Memory.Rva+(va-r.StartOfMemoryRange);} }
    return nullptr;
}
int wmain(int argc, wchar_t** argv){
    if(argc<3){ wprintf(L"usage: scanstack <dmp> <sympath>\n"); return 2; }
    HANDLE hf=CreateFileW(argv[1],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE hm=CreateFileMappingW(hf,nullptr,PAGE_READONLY,0,0,nullptr);
    g_base=(BYTE*)MapViewOfFile(hm,FILE_MAP_READ,0,0,0);
    if(!g_base){ wprintf(L"map failed\n"); return 1; }
    void* s=nullptr; ULONG sz=0; MINIDUMP_DIRECTORY* dir=nullptr;
    if(MiniDumpReadDumpStream(g_base,ModuleListStream,&dir,&s,&sz)){
        auto* ml=(MINIDUMP_MODULE_LIST*)s;
        for(ULONG32 i=0;i<ml->NumberOfModules;++i){ auto&m=ml->Modules[i]; auto* ms=(MINIDUMP_STRING*)(g_base+m.ModuleNameRva);
            std::wstring full(ms->Buffer,ms->Length/2); size_t sl=full.find_last_of(L"\/");
            g_mods.push_back({m.BaseOfImage,m.SizeOfImage,sl==std::wstring::npos?full:full.substr(sl+1),full}); }
    }
    if(MiniDumpReadDumpStream(g_base,Memory64ListStream,&dir,&s,&sz)) g_ml64=(MINIDUMP_MEMORY64_LIST*)s;
    if(MiniDumpReadDumpStream(g_base,MemoryListStream,&dir,&s,&sz)) g_ml=(MINIDUMP_MEMORY_LIST*)s;
    SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_NO_PROMPTS);
    SymInitializeW(GetCurrentProcess(),argv[2],FALSE);
    for(auto&m:g_mods) SymLoadModuleExW(GetCurrentProcess(),nullptr,m.full.c_str(),m.name.c_str(),m.base,m.size,nullptr,0);
    if(!MiniDumpReadDumpStream(g_base,ExceptionStream,&dir,&s,&sz)){ wprintf(L"no exception stream\n"); return 1; }
    auto* es=(MINIDUMP_EXCEPTION_STREAM*)s;
    CONTEXT* ctx=(CONTEXT*)(g_base+es->ThreadContext.Rva);
    ULONG64 rsp=ctx->Rsp;
    // find the faulting thread's stack range from the thread list
    ULONG64 stackTop=rsp, stackEnd=rsp+512*1024;
    if(MiniDumpReadDumpStream(g_base,ThreadListStream,&dir,&s,&sz)){
        auto* tl=(MINIDUMP_THREAD_LIST*)s;
        for(ULONG32 i=0;i<tl->NumberOfThreads;++i){ auto&t=tl->Threads[i];
            if(t.ThreadId==es->ThreadId){ stackEnd=t.Stack.StartOfMemoryRange+t.Stack.Memory.DataSize; break; } }
    }
    wprintf(L"tid=0x%X rsp=0x%llX stackEnd=0x%llX span=%lluK\n",es->ThreadId,rsp,(unsigned long long)stackEnd,(stackEnd-rsp)/1024);
    char buf[sizeof(SYMBOL_INFO)+1024];
    for(ULONG64 a=rsp; a+8<=stackEnd; a+=8){
        const BYTE* p=readVA(a,8); if(!p) continue;
        ULONG64 v; memcpy(&v,p,8);
        Mod* m=findMod(v); if(!m) continue;
        auto* si=(SYMBOL_INFO*)buf; si->SizeOfStruct=sizeof(SYMBOL_INFO); si->MaxNameLen=1000; DWORD64 disp=0;
        if(SymFromAddr(GetCurrentProcess(),v,&disp,si))
            wprintf(L"  [rsp+0x%05llX] 0x%llX %-22s %S +0x%llX\n",a-rsp,v,m->name.c_str(),si->Name,disp);
        else
            wprintf(L"  [rsp+0x%05llX] 0x%llX %-22s +0x%llX\n",a-rsp,v,m->name.c_str(),v-m->base);
    }
    return 0;
}
