// Agentmaster: minimal DIA symbolizer. Maps an RVA in a module to function + source:line.
// Usage: diasym.exe <pdb-path> <rva-hex> [more-rva-hex...]
#include <windows.h>
#include <dia2.h>
#include <diacreate.h>
#include <cstdio>
#include <cwchar>
#include <cstdlib>

static const wchar_t* kMsdia =
    L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\Automation\\msdia140.dll";

static void symbolize(IDiaSession* session, DWORD rva) {
    wprintf(L"\n=== RVA 0x%06X ===\n", rva);

    IDiaSymbol* fn = nullptr;
    if (SUCCEEDED(session->findSymbolByRVA(rva, SymTagFunction, &fn)) && fn) {
        BSTR name = nullptr;
        if (SUCCEEDED(fn->get_name(&name)) && name) {
            wprintf(L"Function : %s\n", name);
            SysFreeString(name);
        }
        DWORD undecOpts = 0; // UNDNAME_COMPLETE
        BSTR undec = nullptr;
        if (SUCCEEDED(fn->get_undecoratedNameEx(undecOpts, &undec)) && undec) {
            wprintf(L"Undecor. : %s\n", undec);
            SysFreeString(undec);
        }
        ULONGLONG len = 0; fn->get_length(&len);
        DWORD fnRva = 0; fn->get_relativeVirtualAddress(&fnRva);
        wprintf(L"FuncRVA  : 0x%06X  len=0x%llX  (offset into fn: 0x%llX)\n",
                fnRva, (unsigned long long)len, (unsigned long long)(rva - fnRva));
        fn->Release();
    } else {
        wprintf(L"Function : <not found>\n");
    }

    IDiaEnumLineNumbers* lines = nullptr;
    if (SUCCEEDED(session->findLinesByRVA(rva, 1, &lines)) && lines) {
        IDiaLineNumber* line = nullptr;
        ULONG celt = 0;
        while (SUCCEEDED(lines->Next(1, &line, &celt)) && celt == 1) {
            DWORD num = 0; line->get_lineNumber(&num);
            IDiaSourceFile* src = nullptr;
            if (SUCCEEDED(line->get_sourceFile(&src)) && src) {
                BSTR fname = nullptr;
                if (SUCCEEDED(src->get_fileName(&fname)) && fname) {
                    wprintf(L"Source   : %s:%u\n", fname, num);
                    SysFreeString(fname);
                }
                src->Release();
            }
            line->Release();
        }
        lines->Release();
    } else {
        wprintf(L"Source   : <no line info>\n");
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { wprintf(L"usage: diasym <pdb> <rva-hex> [rva-hex...]\n"); return 2; }
    HRESULT hr = CoInitialize(nullptr);

    IDiaDataSource* source = nullptr;
    hr = NoRegCoCreate(kMsdia, __uuidof(DiaSource), __uuidof(IDiaDataSource), (void**)&source);
    if (FAILED(hr) || !source) { wprintf(L"NoRegCoCreate failed 0x%08X\n", hr); return 1; }

    hr = source->loadDataFromPdb(argv[1]);
    if (FAILED(hr)) { wprintf(L"loadDataFromPdb('%s') failed 0x%08X\n", argv[1], hr); return 1; }

    IDiaSession* session = nullptr;
    hr = source->openSession(&session);
    if (FAILED(hr) || !session) { wprintf(L"openSession failed 0x%08X\n", hr); return 1; }

    for (int i = 2; i < argc; ++i) {
        DWORD rva = (DWORD)wcstoul(argv[i], nullptr, 16);
        symbolize(session, rva);
    }

    session->Release();
    source->Release();
    CoUninitialize();
    return 0;
}
