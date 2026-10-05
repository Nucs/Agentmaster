// Agentmaster debug-dumps skill: read TYPE LAYOUTS and FRAME LOCATIONS out of a PDB with the DIA SDK, so a
// crashed object's fields can be read straight from a dump (with dumpmem) instead of guessed.
//
// Why it exists: a stack walk says WHERE a crash happened, not what state the object was in. With the two
// answers this tool gives, the state is one hexdump away:
//   1. `func <rva>`  -> where a function keeps `this` and each local (register-relative offsets). In a Debug
//      build `this` is spilled to its home slot, so a return address found by dumpourscan pins the frame and
//      the spill slot gives the object's address.
//   2. `udt <name>`  -> every data member's offset (and base classes, recursively), so `dumpmem` at
//      `this + offset` reads the field.
// Used 2026-10-05 to prove TermControl::_initializedTerminal == 0 and an unarmed _layoutUpdatedRevoker in the
// crash of UI_THREAD.md §10k (the terminal-start-before-Loaded crash).
//
// Usage:
//   diatype <pdb> udt  <exact-udt-name>   e.g. winrt::Microsoft::Terminal::Control::implementation::TermControl
//   diatype <pdb> func <rva-hex>          e.g. 6142E0 (any RVA inside the function)
// Output: `udt` prints "+0xOFF (abs 0xABS) size=0xN name : type" per member, indented under each base class;
//         `func` prints "data <name> loc=<LocationType> reg=<CV_HREG> off=<n> type=<type>" per param/local
//         (loc 3 = LocIsRegRel; reg 335 = CV_AMD64_RSP, 334 = CV_AMD64_RBP; off is relative to that register
//         AFTER the prologue). Nested blocks are indented.
// Exit codes: 0 success (also when nothing matched - check the output), 1 DIA failure, 2 usage error.
// Build: scripts\build.bat (DIA tools). msdia140.dll is loaded from the VS Community path (kMsdia below).
#include <windows.h>
#include <dia2.h>
#include <diacreate.h>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <string>

static const wchar_t* kMsdia =
    L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\Automation\\msdia140.dll";

// The symbol's name, or "" when it has none.
// s: any DIA symbol. Returns the name copied out of the BSTR (freed here). Never throws (std::wstring
// allocation aside).
static std::wstring Name(IDiaSymbol* s)
{
    BSTR b = nullptr;
    std::wstring r;
    if (SUCCEEDED(s->get_name(&b)) && b)
    {
        r = b;
        SysFreeString(b);
    }
    return r;
}

// A readable name for a type symbol: pointers get "*", arrays "[count]", base types "base<kind>/<bytes>"
// (DIA's BasicType enum - e.g. base10/1 is a bool, base8/8 a double), anything unnamed "tag<SymTag>".
// t: a type symbol, or null ("?"). Returns the text. Recurses through pointer/array element types.
static std::wstring TypeName(IDiaSymbol* t)
{
    if (!t)
    {
        return L"?";
    }
    DWORD tag = 0;
    t->get_symTag(&tag);
    if (tag == SymTagPointerType)
    {
        IDiaSymbol* pt = nullptr;
        t->get_type(&pt);
        auto r = TypeName(pt) + L"*";
        if (pt)
        {
            pt->Release();
        }
        return r;
    }
    if (tag == SymTagBaseType)
    {
        DWORD bt = 0;
        t->get_baseType(&bt);
        ULONGLONG len = 0;
        t->get_length(&len);
        return L"base" + std::to_wstring(bt) + L"/" + std::to_wstring(len);
    }
    if (tag == SymTagArrayType)
    {
        IDiaSymbol* et = nullptr;
        t->get_type(&et);
        DWORD cnt = 0;
        t->get_count(&cnt);
        auto r = TypeName(et) + L"[" + std::to_wstring(cnt) + L"]";
        if (et)
        {
            et->Release();
        }
        return r;
    }
    auto n = Name(t);
    return n.empty() ? (L"tag" + std::to_wstring(tag)) : n;
}

// Print every data member of a UDT with its offset relative to the UDT and to the outermost object
// (baseOffset + offset), descending into base classes (indented, at most 6 levels).
// udt: the class/struct symbol. baseOffset: where this UDT starts inside the outermost object (0 at the top).
// depth: indentation level. Returns nothing; a DIA call that fails simply prints less.
static void DumpUdt(IDiaSymbol* udt, LONG baseOffset, int depth)
{
    IDiaEnumSymbols* children = nullptr;
    if (FAILED(udt->findChildren(SymTagNull, nullptr, nsNone, &children)) || !children)
    {
        return;
    }
    IDiaSymbol* c = nullptr;
    ULONG got = 0;
    while (SUCCEEDED(children->Next(1, &c, &got)) && got == 1)
    {
        DWORD tag = 0;
        c->get_symTag(&tag);
        if (tag == SymTagBaseClass)
        {
            LONG off = 0;
            c->get_offset(&off);
            IDiaSymbol* bt = nullptr;
            c->get_type(&bt);
            wprintf(L"%*s[base @0x%X] %s\n", depth * 2, L"", baseOffset + off, Name(c).c_str());
            if (bt && depth < 6)
            {
                DumpUdt(bt, baseOffset + off, depth + 1);
            }
            if (bt)
            {
                bt->Release();
            }
        }
        else if (tag == SymTagData)
        {
            // Only instance members carry an offset: statics (LocIsStatic) and constants are skipped.
            DWORD loc = 0;
            c->get_locationType(&loc);
            if (loc == LocIsThisRel || loc == LocIsBitField)
            {
                LONG off = 0;
                c->get_offset(&off);
                IDiaSymbol* t = nullptr;
                c->get_type(&t);
                ULONGLONG len = 0;
                if (t)
                {
                    t->get_length(&len);
                }
                std::wstring bits;
                if (loc == LocIsBitField)
                {
                    DWORD bitpos = 0;
                    ULONGLONG bitlen = 0;
                    c->get_bitPosition(&bitpos);
                    c->get_length(&bitlen);
                    bits = L" bit" + std::to_wstring(bitpos) + L":" + std::to_wstring(bitlen);
                }
                wprintf(L"%*s+0x%04X (abs 0x%04X) size=0x%llX %s : %s%s\n", depth * 2, L"", off, baseOffset + off,
                        (unsigned long long)len, Name(c).c_str(), TypeName(t).c_str(), bits.c_str());
                if (t)
                {
                    t->Release();
                }
            }
        }
        c->Release();
    }
    children->Release();
}

// Print a function's (or block's) data symbols - params and locals - with their DIA location, then descend
// into nested blocks (indented).
// scope: the function or block symbol. depth: indentation level. Returns nothing.
static void DumpFuncLocals(IDiaSymbol* scope, int depth)
{
    IDiaEnumSymbols* children = nullptr;
    if (FAILED(scope->findChildren(SymTagNull, nullptr, nsNone, &children)) || !children)
    {
        return;
    }
    IDiaSymbol* c = nullptr;
    ULONG got = 0;
    while (SUCCEEDED(children->Next(1, &c, &got)) && got == 1)
    {
        DWORD tag = 0;
        c->get_symTag(&tag);
        if (tag == SymTagData)
        {
            DWORD loc = 0;
            DWORD reg = 0;
            LONG off = 0;
            c->get_locationType(&loc);
            c->get_registerId(&reg);
            c->get_offset(&off);
            IDiaSymbol* t = nullptr;
            c->get_type(&t);
            wprintf(L"%*sdata %-30s loc=%u reg=%u off=%d (0x%X) type=%s\n", depth * 2, L"", Name(c).c_str(), loc, reg, off,
                    (unsigned)off, TypeName(t).c_str());
            if (t)
            {
                t->Release();
            }
        }
        else if (tag == SymTagBlock)
        {
            DumpFuncLocals(c, depth + 1);
        }
        c->Release();
    }
    children->Release();
}

// Entry point: parse `<pdb> udt <name>` or `<pdb> func <rva-hex>`, open the PDB through msdia140.dll
// (registration-free), print the answer. Returns 0 / 1 (DIA failure) / 2 (usage).
int wmain(int argc, wchar_t** argv)
{
    if (argc < 4 || (wcscmp(argv[2], L"udt") != 0 && wcscmp(argv[2], L"func") != 0))
    {
        wprintf(L"usage: diatype <pdb> udt <exact-udt-name> | diatype <pdb> func <rva-hex>\n");
        return 2;
    }
    CoInitialize(nullptr);
    IDiaDataSource* source = nullptr;
    HRESULT hr = NoRegCoCreate(kMsdia, __uuidof(DiaSource), __uuidof(IDiaDataSource), (void**)&source);
    if (FAILED(hr) || !source)
    {
        wprintf(L"NoRegCoCreate failed 0x%08X (msdia140.dll at %s?)\n", hr, kMsdia);
        return 1;
    }
    if (FAILED(hr = source->loadDataFromPdb(argv[1])))
    {
        wprintf(L"loadDataFromPdb('%s') failed 0x%08X\n", argv[1], hr);
        return 1;
    }
    IDiaSession* session = nullptr;
    if (FAILED(hr = source->openSession(&session)) || !session)
    {
        wprintf(L"openSession failed 0x%08X\n", hr);
        return 1;
    }
    IDiaSymbol* global = nullptr;
    session->get_globalScope(&global);

    if (wcscmp(argv[2], L"udt") == 0)
    {
        // A name can match several records: forward declarations have length 0, so print the first two
        // with a real layout (two, in case of an ODR-split type).
        IDiaEnumSymbols* e = nullptr;
        if (global && SUCCEEDED(global->findChildren(SymTagUDT, argv[3], nsCaseSensitive, &e)) && e)
        {
            IDiaSymbol* u = nullptr;
            ULONG got = 0;
            int printed = 0;
            while (SUCCEEDED(e->Next(1, &u, &got)) && got == 1 && printed < 2)
            {
                ULONGLONG len = 0;
                u->get_length(&len);
                wprintf(L"UDT %s size=0x%llX\n", Name(u).c_str(), (unsigned long long)len);
                if (len > 0)
                {
                    DumpUdt(u, 0, 1);
                    ++printed;
                }
                u->Release();
            }
            e->Release();
        }
    }
    else
    {
        const DWORD rva = (DWORD)wcstoul(argv[3], nullptr, 16);
        IDiaSymbol* fn = nullptr;
        if (SUCCEEDED(session->findSymbolByRVA(rva, SymTagFunction, &fn)) && fn)
        {
            DWORD fr = 0;
            ULONGLONG len = 0;
            fn->get_relativeVirtualAddress(&fr);
            fn->get_length(&len);
            wprintf(L"FUNC %s rva=0x%X len=0x%llX\n", Name(fn).c_str(), fr, (unsigned long long)len);
            DumpFuncLocals(fn, 1);
            fn->Release();
        }
        else
        {
            wprintf(L"no function at RVA 0x%X\n", rva);
        }
    }
    return 0;
}
