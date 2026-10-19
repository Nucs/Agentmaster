// Agentmaster leak hunt: heapcensus - count the C++ objects in a full-memory dump by their vtable.
//
// Why: a leak in a C++/WinRT + XAML app is almost always "N objects of some type that should have
// been freed". Every object with virtual functions starts each of its (sub)objects with a pointer
// to its class's vftable, and the PDBs name every vftable (public symbol `??_7<class>...@6B@`). So
// scanning every 8-byte slot of the dump's private read-write memory for values that equal a known
// vftable address gives a census of live objects BY TYPE - for our modules (full PDBs in the
// layout folder) and for the system XAML / composition / text stack (public PDBs from msdl). Two
// dumps of the same process minutes or hours apart then show which types GROW (heapdiff.py).
//
// What a count means, and its two biases (read before trusting a number):
//   * A C++/WinRT `implements<T, I1, I2...>` object carries one vptr per interface, all T's own
//     vftables ("T::`vftable'{for `I2'}"), so each vftable row counts T's instances once.
//   * FREED blocks keep their bytes until the heap reuses them, and C++/WinRT bases are novtable,
//     so a destroyed object can still read as its type: a raw count is live + not-yet-reused dead.
//     --refs measures that: it counts, per vftable, the slots that some other 8-byte slot in the
//     scanned memory POINTS AT (an interface pointer points exactly at its vptr slot). A live object
//     is held by someone; a freed one normally is not. "refd" is therefore the better live estimate.
//   * Identical vftables can be folded into one address by the linker (/OPT:ICF): such a row lists
//     every class name that shares the address, separated by " | ".
//
// Usage:
//   heapcensus <dump.dmp> --sym <dir> [--sym <dir> ...] [--only <module-substring> ...] [--out <census.tsv>]
//              [--top N] [--refs] [--sizes] [--addrs <name-substring> [--addrs-max N]]
//              [--slots <name-substring> --slots-out <file>] [--findref <hex-address> [--span <bytes>]]
//              [--refs-of <ranges.txt> --refs-out <refs.tsv>] [--symat <module> <rva-hex>] [--regions N]
//   --sym     a folder holding PDBs: either flat (<dir>\<name>.pdb, e.g. the layout folder) or a symbol
//             store (<dir>\<name>.pdb\<GUID><age>\<name>.pdb). Every module whose PDB matches the
//             dump's RSDS GUID + age exactly is loaded; the rest are listed as MISSING with their msdl URL.
//   --only    load symbols only for modules whose name contains one of these substrings (faster runs).
//   --symat   <module> <rva-hex>: print the symbol covering that RVA (any kind), e.g. to name the vftable
//             of an object type the census did not know (a non-public vftable). Repeatable.
//   --slots   write EVERY vptr slot (address + vftable) of the vftables whose name contains the substring
//             to --slots-out, for dumpview.py to read the objects' fields.
//   --out     write every vftable with a non-zero count as TSV (count, refd, module, rva, name).
//   --top     how many rows to print (default 120).
//   --refs    run the second pass that measures "refd" (doubles the scan time; needs RAM for the hit set).
//   --sizes   estimate each vftable's heap block size (the most common gap between its sorted slots,
//             i.e. its LFH bucket) and write it as the TSV's "block" column: count x block ranks bytes.
//   --addrs   print the addresses of the vptr slots whose vftable name contains the substring.
//   --findref print every slot (and its region + any vftable hit at the slot's object start guess) whose
//             value lies in [address, address + span) (span default 8): "who points at this object".
//   --regions print the N largest allocations (committed bytes per AllocationBase), default 40.
//   --refs-of <in> --refs-out <out>: the batch --findref - <in> holds "0x<start> <size>" lines, <out>
//             gets "0x<start>\t0x<slot>" for up to 16 slots pointing into each range (self-references
//             skipped). No symbols are loaded; one scan answers every range.
//
// Build (VS 2022 x64 developer prompt, DIA SDK beside VS):
//   cl /nologo /EHsc /O2 /std:c++20 /I"<VS>\DIA SDK\include" heapcensus.cpp
//      /link "<VS>\DIA SDK\lib\amd64\diaguids.lib" dbghelp.lib ole32.lib oleaut32.lib advapi32.lib
//   (build.bat beside this file does exactly that.)
// Exit codes: 0 ok, 1 usage, 2 the dump could not be opened/parsed, 3 DIA (msdia140.dll) unavailable.

// windows.h's min/max macros would break std::min / std::max below.
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <dia2.h>
#include <diacreate.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    /// msdia140.dll is loaded registration-free from the VS install (the debug-dumps tools' path).
    const wchar_t* kMsdia = L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\Automation\\msdia140.dll";

    /// A read-only view of the whole dump file. The scan touches every byte once, so a mapping (no copy)
    /// keeps a multi-GB dump cheap; the view lives for the whole run.
    struct MappedDump
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        HANDLE mapping = nullptr;
        const uint8_t* base = nullptr;
        uint64_t size = 0;

        /// Opens and maps the dump read-only.
        /// @param path the dump file.
        /// @returns true when the whole file is mapped; false (with a message printed) otherwise.
        /// @remarks Never throws. A partial failure leaves the members for the destructor to release.
        bool Open(const wchar_t* path) noexcept
        {
            file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                fwprintf(stderr, L"cannot open %ls (%lu)\n", path, GetLastError());
                return false;
            }
            LARGE_INTEGER sz{};
            GetFileSizeEx(file, &sz);
            size = static_cast<uint64_t>(sz.QuadPart);
            mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (!mapping)
            {
                fwprintf(stderr, L"CreateFileMapping failed (%lu)\n", GetLastError());
                return false;
            }
            base = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
            if (!base)
            {
                fwprintf(stderr, L"MapViewOfFile failed (%lu)\n", GetLastError());
                return false;
            }
            return true;
        }

        /// Unmaps and closes; safe on a partially opened object. Never throws.
        ~MappedDump()
        {
            if (base)
            {
                UnmapViewOfFile(base);
            }
            if (mapping)
            {
                CloseHandle(mapping);
            }
            if (file != INVALID_HANDLE_VALUE)
            {
                CloseHandle(file);
            }
        }
    };

    /// One loaded module of the dumped process, with the CodeView identity of its PDB.
    struct Module
    {
        std::wstring name;     ///< the module's file leaf (e.g. TerminalApp.dll)
        uint64_t base = 0;     ///< image base in the dumped process
        uint64_t size = 0;     ///< SizeOfImage
        std::wstring pdbLeaf;  ///< the RSDS record's PDB file leaf
        GUID guid{};           ///< RSDS GUID
        DWORD age = 0;         ///< RSDS age
        bool hasCv = false;    ///< false when the module carries no RSDS record (then it cannot be symbolized)
        bool loaded = false;   ///< true once a matching PDB was found and its vftables registered
        std::wstring pdbPath;  ///< where the matching PDB was found
    };

    /// One committed or reserved region from the dump's MemoryInfoListStream.
    struct Region
    {
        uint64_t base = 0, allocBase = 0, size = 0;
        uint32_t state = 0, protect = 0, type = 0, allocProtect = 0;
    };

    /// One contiguous run of captured memory (Memory64ListStream) and where its bytes sit in the mapped file.
    struct Range
    {
        uint64_t va = 0, size = 0;
        const uint8_t* data = nullptr;
    };

    /// A known vftable: where it is, which module and every (possibly ICF-folded) class name at that address.
    struct VTable
    {
        uint64_t va = 0;
        uint32_t module = 0;
        uint32_t rva = 0;
        std::vector<std::string> decorated;  ///< one per public symbol at this address
        uint64_t count = 0;                  ///< slots holding this vftable's address
        uint64_t refd = 0;                   ///< of those, slots that some other slot points at (--refs)
        uint64_t blockSize = 0;              ///< --sizes: the most common gap between this vftable's slots (0 = unknown)
        std::vector<uint64_t> samples;       ///< the first few slot addresses (for --addrs and follow-ups)
    };

    /// Formats a GUID + age the way symbol stores name the PDB folder (GUID hex without dashes, then age hex).
    /// @param g the RSDS GUID. @param age the RSDS age.
    /// @returns e.g. "3F2504E04F8941D39A0C0305E82C33011". Never throws (beyond allocation).
    std::wstring SymStoreId(const GUID& g, DWORD age)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%lX", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                   g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], age);
        return buf;
    }

    /// Reads a MINIDUMP_STRING at a file RVA.
    /// @param dump the mapped dump. @param rva the string's file offset.
    /// @returns the string, or "" when the RVA is outside the file. Never throws (beyond allocation).
    std::wstring DumpString(const MappedDump& dump, RVA rva)
    {
        if (rva + sizeof(ULONG32) > dump.size)
        {
            return {};
        }
        auto s = reinterpret_cast<const MINIDUMP_STRING*>(dump.base + rva);
        if (rva + sizeof(ULONG32) + s->Length > dump.size)
        {
            return {};
        }
        return std::wstring(s->Buffer, s->Length / sizeof(wchar_t));
    }

    /// Returns the file leaf of a Windows path (the part after the last '\' or '/').
    /// @param path any path. @returns the leaf (the whole string when it has no separator). Never throws (beyond allocation).
    std::wstring Leaf(const std::wstring& path)
    {
        const auto p = path.find_last_of(L"\\/");
        return p == std::wstring::npos ? path : path.substr(p + 1);
    }

    /// Parses the module list, the memory-info list and the Memory64 list out of the mapped dump.
    /// @param dump the mapped dump.
    /// @param modules receives every module with its CodeView identity.
    /// @param regions receives every region, sorted by address.
    /// @param ranges receives every captured memory run, sorted by address.
    /// @returns false (with a message) when a stream is missing - e.g. not a full-memory dump.
    /// @remarks Never throws beyond allocation; a corrupt CodeView record just leaves hasCv false.
    bool ParseDump(const MappedDump& dump, std::vector<Module>& modules, std::vector<Region>& regions, std::vector<Range>& ranges)
    {
        PMINIDUMP_DIRECTORY dir = nullptr;
        PVOID stream = nullptr;
        ULONG streamSize = 0;
        if (!MiniDumpReadDumpStream(const_cast<uint8_t*>(dump.base), ModuleListStream, &dir, &stream, &streamSize) || !stream)
        {
            fwprintf(stderr, L"no ModuleListStream\n");
            return false;
        }
        auto ml = static_cast<const MINIDUMP_MODULE_LIST*>(stream);
        for (ULONG32 i = 0; i < ml->NumberOfModules; ++i)
        {
            const MINIDUMP_MODULE& m = ml->Modules[i];
            Module mod;
            mod.name = Leaf(DumpString(dump, m.ModuleNameRva));
            mod.base = m.BaseOfImage;
            mod.size = m.SizeOfImage;
            // RSDS layout: 'RSDS', GUID, DWORD age, NUL-terminated UTF-8 PDB path.
            if (m.CvRecord.DataSize >= 24 && m.CvRecord.Rva + m.CvRecord.DataSize <= dump.size)
            {
                const uint8_t* cv = dump.base + m.CvRecord.Rva;
                if (memcmp(cv, "RSDS", 4) == 0)
                {
                    memcpy(&mod.guid, cv + 4, sizeof(GUID));
                    memcpy(&mod.age, cv + 20, sizeof(DWORD));
                    std::string pdb(reinterpret_cast<const char*>(cv + 24), strnlen(reinterpret_cast<const char*>(cv + 24), m.CvRecord.DataSize - 24));
                    const int n = MultiByteToWideChar(CP_UTF8, 0, pdb.c_str(), -1, nullptr, 0);
                    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
                    if (n > 1)
                    {
                        MultiByteToWideChar(CP_UTF8, 0, pdb.c_str(), -1, w.data(), n);
                    }
                    mod.pdbLeaf = Leaf(w);
                    mod.hasCv = !mod.pdbLeaf.empty();
                }
            }
            modules.push_back(std::move(mod));
        }

        if (!MiniDumpReadDumpStream(const_cast<uint8_t*>(dump.base), MemoryInfoListStream, &dir, &stream, &streamSize) || !stream)
        {
            fwprintf(stderr, L"no MemoryInfoListStream (dump taken without MiniDumpWithFullMemoryInfo?)\n");
            return false;
        }
        auto mil = static_cast<const MINIDUMP_MEMORY_INFO_LIST*>(stream);
        auto entry = reinterpret_cast<const uint8_t*>(stream) + mil->SizeOfHeader;
        for (ULONG64 i = 0; i < mil->NumberOfEntries; ++i, entry += mil->SizeOfEntry)
        {
            auto mi = reinterpret_cast<const MINIDUMP_MEMORY_INFO*>(entry);
            regions.push_back(Region{ mi->BaseAddress, mi->AllocationBase, mi->RegionSize, mi->State, mi->Protect, mi->Type, mi->AllocationProtect });
        }
        std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) { return a.base < b.base; });

        if (!MiniDumpReadDumpStream(const_cast<uint8_t*>(dump.base), Memory64ListStream, &dir, &stream, &streamSize) || !stream)
        {
            fwprintf(stderr, L"no Memory64ListStream (not a full-memory dump?)\n");
            return false;
        }
        auto m64 = static_cast<const MINIDUMP_MEMORY64_LIST*>(stream);
        uint64_t fileOff = m64->BaseRva;
        for (ULONG64 i = 0; i < m64->NumberOfMemoryRanges; ++i)
        {
            const auto& d = m64->MemoryRanges[i];
            if (fileOff + d.DataSize <= dump.size)
            {
                ranges.push_back(Range{ d.StartOfMemoryRange, d.DataSize, dump.base + fileOff });
            }
            fileOff += d.DataSize;
        }
        std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.va < b.va; });
        return true;
    }

    /// Finds the region holding an address.
    /// @param regions sorted regions. @param va the address.
    /// @returns the region, or nullptr when the address is in no listed region. Never throws.
    const Region* RegionOf(const std::vector<Region>& regions, uint64_t va) noexcept
    {
        auto it = std::upper_bound(regions.begin(), regions.end(), va, [](uint64_t v, const Region& r) { return v < r.base; });
        if (it == regions.begin())
        {
            return nullptr;
        }
        --it;
        return va < it->base + it->size ? &*it : nullptr;
    }

    /// Whether a region holds heap-like data worth scanning: committed, private, writable, not a guard page.
    /// @param r the region. @returns true for MEM_COMMIT + MEM_PRIVATE + a writable protection. Never throws.
    /// @remarks Image and mapped pages are skipped on purpose: vftable pointers there are the vftables
    ///          themselves (or file data), not objects. Stacks are private RW and are scanned (few hits).
    bool Scannable(const Region& r) noexcept
    {
        constexpr uint32_t writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return r.state == MEM_COMMIT && r.type == MEM_PRIVATE && (r.protect & writable) && !(r.protect & PAGE_GUARD);
    }

    /// Calls fn(slotAddress, value) for every 8-byte-aligned slot of every scannable captured byte.
    /// @param regions sorted regions (to decide scannability). @param ranges sorted captured runs.
    /// @param fn the visitor; called a few hundred million times on a multi-GB dump, so keep it cheap.
    /// @remarks Never throws unless fn throws. A run that crosses region boundaries is split per region.
    template<typename Fn>
    void ForEachSlot(const std::vector<Region>& regions, const std::vector<Range>& ranges, Fn&& fn)
    {
        for (const Range& rg : ranges)
        {
            uint64_t va = rg.va;
            const uint64_t end = rg.va + rg.size;
            while (va < end)
            {
                const Region* reg = RegionOf(regions, va);
                const uint64_t stop = reg ? std::min(end, reg->base + reg->size) : end;
                if (reg && Scannable(*reg))
                {
                    uint64_t a = (va + 7) & ~7ull;
                    const uint8_t* p = rg.data + (a - rg.va);
                    for (; a + 8 <= stop; a += 8, p += 8)
                    {
                        uint64_t v;
                        memcpy(&v, p, 8);
                        fn(a, v);
                    }
                }
                va = stop;
            }
        }
    }

    /// Reads 8 bytes of the dumped process's memory at an address.
    /// @param ranges sorted captured runs. @param va the address. @param out receives the value.
    /// @returns false when the address was not captured. Never throws.
    bool ReadQword(const std::vector<Range>& ranges, uint64_t va, uint64_t& out) noexcept
    {
        auto it = std::upper_bound(ranges.begin(), ranges.end(), va, [](uint64_t v, const Range& r) { return v < r.va; });
        if (it == ranges.begin())
        {
            return false;
        }
        --it;
        if (va + 8 > it->va + it->size)
        {
            return false;
        }
        memcpy(&out, it->data + (va - it->va), 8);
        return true;
    }

    /// The symbol-loading half: one DIA data source per module whose PDB matches.
    struct Symbols
    {
        std::vector<std::wstring> dirs;  ///< --sym folders, searched flat then as a symbol store
        std::vector<std::wstring> only;  ///< --only module-name substrings (empty = every module with a PDB)
        std::vector<std::pair<std::wstring, DWORD>> symAt;  ///< --symat <module> <rva> lookups, answered while the PDB is open

        /// Locates and loads the PDB that matches a module's RSDS identity, then registers its vftables.
        /// @param mod the module (updated: loaded / pdbPath).
        /// @param modIndex the module's index (stored in each VTable).
        /// @param vts receives the module's vftables (one entry per distinct address).
        /// @returns true when a matching PDB was loaded. Never throws (DIA failures read as "no match").
        /// @remarks loadAndValidateDataFromPdb rejects a PDB whose GUID/age differ, so a stale PDB in a
        ///          layout folder (an older build) can never mislabel the dump's addresses.
        bool Load(Module& mod, uint32_t modIndex, std::vector<VTable>& vts)
        {
            if (!mod.hasCv)
            {
                return false;
            }
            std::vector<std::wstring> candidates;
            for (const auto& d : dirs)
            {
                candidates.push_back(d + L"\\" + mod.pdbLeaf);
                candidates.push_back(d + L"\\" + mod.pdbLeaf + L"\\" + SymStoreId(mod.guid, mod.age) + L"\\" + mod.pdbLeaf);
            }
            for (const auto& path : candidates)
            {
                // A symbol store names its FOLDERS after the PDB (<dir>\ntdll.pdb\<id>\ntdll.pdb), so the flat
                // candidate can be a directory: only a regular file is handed to DIA (a directory sent DIA
                // into a CPU-bound loop on the first run).
                const DWORD attrs = GetFileAttributesW(path.c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
                {
                    continue;
                }
                IDiaDataSource* source = nullptr;
                if (FAILED(NoRegCoCreate(kMsdia, __uuidof(DiaSource), __uuidof(IDiaDataSource), reinterpret_cast<void**>(&source))) || !source)
                {
                    return false;
                }
                GUID g = mod.guid;
                if (FAILED(source->loadAndValidateDataFromPdb(path.c_str(), &g, 0, mod.age)))
                {
                    source->Release();
                    continue;
                }
                IDiaSession* session = nullptr;
                IDiaSymbol* global = nullptr;
                if (FAILED(source->openSession(&session)) || !session || FAILED(session->get_globalScope(&global)) || !global)
                {
                    if (session)
                    {
                        session->Release();
                    }
                    source->Release();
                    continue;
                }
                EnumVTables(global, mod, modIndex, vts);
                for (const auto& [name, rva] : symAt)
                {
                    // --symat <module> <rva>: name whatever symbol covers that RVA (a vftable that is not a
                    // public ??_7, a global, a function) - the way to identify an unknown owner's first qword.
                    if (_wcsicmp(name.c_str(), mod.name.c_str()) != 0)
                    {
                        continue;
                    }
                    IDiaSymbol* s = nullptr;
                    LONG disp = 0;
                    if (SUCCEEDED(session->findSymbolByRVAEx(rva, SymTagNull, &s, &disp)) && s)
                    {
                        BSTR n = nullptr;
                        BSTR u = nullptr;
                        s->get_name(&n);
                        s->get_undecoratedName(&u);
                        wprintf(L"  symat %ls+0x%X = %ls (%ls) %+ld\n", name.c_str(), rva, n ? n : L"?", u ? u : L"", disp);
                        SysFreeString(n);
                        SysFreeString(u);
                        s->Release();
                    }
                    else
                    {
                        wprintf(L"  symat %ls+0x%X = <none>\n", name.c_str(), rva);
                    }
                }
                global->Release();
                session->Release();
                source->Release();
                mod.loaded = true;
                mod.pdbPath = path;
                return true;
            }
            return false;
        }

        /// Registers every public vftable symbol of one PDB (decorated `??_7...`), merging folded addresses.
        /// @param global the PDB's global scope. @param mod its module. @param modIndex the module index.
        /// @param vts receives the entries. Never throws (beyond allocation).
        static void EnumVTables(IDiaSymbol* global, const Module& mod, uint32_t modIndex, std::vector<VTable>& vts)
        {
            IDiaEnumSymbols* e = nullptr;
            if (FAILED(global->findChildrenEx(SymTagPublicSymbol, nullptr, nsNone, &e)) || !e)
            {
                return;
            }
            std::unordered_map<uint32_t, size_t> byRva;
            // Bounded by get_Count and fetched one at a time: on a public-only PDB (ntdll from msdl) a
            // batched Next(256) kept returning full batches forever (16 M "publics" for ntdll in the first
            // run), so the enumerator's own count is the only trusted bound.
            LONG total = 0;
            e->get_Count(&total);
            IDiaSymbol* batch[1];
            ULONG got = 0;
            for (LONG n = 0; n < total && SUCCEEDED(e->Next(1, batch, &got)) && got == 1; ++n)
            {
                for (ULONG i = 0; i < got; ++i)
                {
                    BSTR name = nullptr;
                    if (SUCCEEDED(batch[i]->get_name(&name)) && name)
                    {
                        // Public symbols carry the decorated name; "??_7" is MSVC's vftable prefix.
                        if (name[0] == L'?' && name[1] == L'?' && name[2] == L'_' && name[3] == L'7')
                        {
                            DWORD rva = 0;
                            if (SUCCEEDED(batch[i]->get_relativeVirtualAddress(&rva)) && rva != 0)
                            {
                                std::string narrow;
                                for (const wchar_t* p = name; *p; ++p)
                                {
                                    narrow.push_back(static_cast<char>(*p));
                                }
                                auto [it, fresh] = byRva.try_emplace(rva, vts.size());
                                if (fresh)
                                {
                                    VTable vt;
                                    vt.va = mod.base + rva;
                                    vt.module = modIndex;
                                    vt.rva = rva;
                                    vts.push_back(std::move(vt));
                                }
                                vts[it->second].decorated.push_back(std::move(narrow));
                            }
                        }
                        SysFreeString(name);
                    }
                    batch[i]->Release();
                }
            }
            e->Release();
        }
    };

    /// Undecorates one MSVC decorated name for display.
    /// @param decorated the `??_7...` name. @returns the readable form, or the decorated text when dbghelp
    ///        cannot undecorate it (e.g. a hashed `??@...@` name). Never throws (beyond allocation).
    std::string Undecorate(const std::string& decorated)
    {
        static char buf[16384];
        if (UnDecorateSymbolName(decorated.c_str(), buf, sizeof(buf), UNDNAME_COMPLETE) && buf[0])
        {
            return buf;
        }
        return decorated;
    }

    /// The display name of a vftable row: every folded class name, undecorated, joined with " | ".
    /// @param vt the row. @returns the text. Never throws (beyond allocation).
    std::string DisplayName(const VTable& vt)
    {
        std::string out;
        for (const auto& d : vt.decorated)
        {
            if (!out.empty())
            {
                out += " | ";
            }
            out += Undecorate(d);
        }
        return out;
    }

    /// Converts a wide string to UTF-8 for the TSV and console output.
    /// @param w the text. @returns the UTF-8 bytes. Never throws (beyond allocation).
    std::string Utf8(const std::wstring& w)
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n : 0, '\0');
        if (n > 0)
        {
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        }
        return s;
    }

    /// Prints the committed-memory summary: totals per type/protection and the largest allocations.
    /// @param regions sorted regions. @param top how many allocations to list. Never throws (beyond allocation).
    void PrintRegions(const std::vector<Region>& regions, size_t top)
    {
        uint64_t privRw = 0, privOther = 0, mapped = 0, image = 0, reserved = 0;
        std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> byAlloc;  // allocBase -> (committed, reserved)
        for (const auto& r : regions)
        {
            if (r.state == MEM_COMMIT)
            {
                if (r.type == MEM_PRIVATE)
                {
                    (Scannable(r) ? privRw : privOther) += r.size;
                }
                else if (r.type == MEM_MAPPED)
                {
                    mapped += r.size;
                }
                else if (r.type == MEM_IMAGE)
                {
                    image += r.size;
                }
            }
            else if (r.state == MEM_RESERVE)
            {
                reserved += r.size;
            }
            if (r.type == MEM_PRIVATE && r.state != MEM_FREE)
            {
                auto& slot = byAlloc[r.allocBase];
                (r.state == MEM_COMMIT ? slot.first : slot.second) += r.size;
            }
        }
        printf("== memory: private RW committed %.1f MB, private other %.1f MB, mapped %.1f MB, image %.1f MB, reserved %.1f GB\n",
               privRw / 1048576.0, privOther / 1048576.0, mapped / 1048576.0, image / 1048576.0, reserved / 1073741824.0);
        std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> v(byAlloc.begin(), byAlloc.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        printf("== the %zu largest private allocations (committed / reserved):\n", std::min(top, v.size()));
        for (size_t i = 0; i < v.size() && i < top; ++i)
        {
            printf("  0x%016llX  %9.1f MB / %9.1f MB\n", static_cast<unsigned long long>(v[i].first), v[i].second.first / 1048576.0,
                   (v[i].second.first + v[i].second.second) / 1048576.0);
        }
        // How many allocations of each committed size class - a flood of equal small VirtualAllocs is its own leak shape.
        std::unordered_map<uint64_t, uint64_t> sizeClass;
        for (const auto& a : v)
        {
            sizeClass[a.second.first]++;
        }
        std::vector<std::pair<uint64_t, uint64_t>> sc(sizeClass.begin(), sizeClass.end());
        std::sort(sc.begin(), sc.end(), [](const auto& a, const auto& b) { return a.second * a.first > b.second * b.first; });
        printf("== private allocations by committed size (top 15 by total): count x size\n");
        for (size_t i = 0; i < sc.size() && i < 15; ++i)
        {
            printf("  %7llu x %10.1f KB = %9.1f MB\n", static_cast<unsigned long long>(sc[i].second), sc[i].first / 1024.0,
                   sc[i].second * sc[i].first / 1048576.0);
        }
        printf("  (%zu private allocations in all)\n", v.size());
    }

    /// An open-addressing set of 8-byte values (the --refs hit set): far faster than unordered_set for the
    /// few hundred million probes of the second pass. Keys must be non-zero (0 marks an empty slot).
    struct U64Set
    {
        std::vector<uint64_t> slots;
        std::vector<uint32_t> refs;
        uint64_t mask = 0;

        /// Sizes the table for n keys at a load factor below 0.5. @param n the expected key count. Never throws beyond allocation.
        void Reserve(size_t n)
        {
            size_t cap = 1;
            while (cap < n * 2 + 16)
            {
                cap <<= 1;
            }
            slots.assign(cap, 0);
            refs.assign(cap, 0);
            mask = cap - 1;
        }

        /// The probe start for a key (a multiplicative hash of the address bits). @param k the key. Never throws.
        size_t Home(uint64_t k) const noexcept
        {
            return static_cast<size_t>((k * 0x9E3779B97F4A7C15ull) >> 20) & mask;
        }

        /// Inserts a key (no-op when present). @param k a non-zero key. Never throws.
        void Insert(uint64_t k) noexcept
        {
            for (size_t i = Home(k);; i = (i + 1) & mask)
            {
                if (slots[i] == k)
                {
                    return;
                }
                if (slots[i] == 0)
                {
                    slots[i] = k;
                    return;
                }
            }
        }

        /// Finds a key's slot. @param k the key. @returns the slot index, or SIZE_MAX when absent. Never throws.
        size_t Find(uint64_t k) const noexcept
        {
            for (size_t i = Home(k);; i = (i + 1) & mask)
            {
                if (slots[i] == k)
                {
                    return i;
                }
                if (slots[i] == 0)
                {
                    return SIZE_MAX;
                }
            }
        }
    };
}

/// Entry point: parse arguments, map + parse the dump, load matching PDBs, scan, report.
/// @param argc the argument count. @param argv see the header comment.
/// @returns 0 ok, 1 usage, 2 dump problem, 3 DIA unavailable.
/// @remarks Never throws on purpose; an allocation failure on a huge dump would terminate (acceptable for a
///          diagnostic tool, and nothing is written outside --out).
int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        fwprintf(stderr, L"usage: heapcensus <dump.dmp> --sym <dir> [--sym <dir>...] [--out <tsv>] [--top N] [--refs]\n"
                         L"                  [--addrs <substring> [--addrs-max N]] [--findref <hex> [--span N]] [--regions N]\n");
        return 1;
    }
    // Unbuffered: the phases take minutes on a big dump, and a redirected stdout would otherwise show
    // nothing until the end (which is how a slow phase hid during the first run).
    setvbuf(stdout, nullptr, _IONBF, 0);
    Symbols syms;
    std::wstring outPath, addrsFilter, slotsFilter, slotsOut, refsOfIn, refsOfOut;
    size_t top = 120, addrsMax = 40, regionsTop = 40;
    bool refs = false, sizes = false;
    uint64_t findref = 0, span = 8;
    for (int i = 2; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        auto next = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--sym") syms.dirs.push_back(next());
        else if (a == L"--only") syms.only.push_back(next());
        else if (a == L"--symat")
        {
            const std::wstring mod = next();
            syms.symAt.emplace_back(mod, static_cast<DWORD>(wcstoul(next(), nullptr, 16)));
        }
        else if (a == L"--out") outPath = next();
        else if (a == L"--top") top = wcstoull(next(), nullptr, 10);
        else if (a == L"--refs") refs = true;
        else if (a == L"--sizes") sizes = true;
        else if (a == L"--addrs") addrsFilter = next();
        else if (a == L"--slots") slotsFilter = next();
        else if (a == L"--refs-of") refsOfIn = next();
        else if (a == L"--refs-out") refsOfOut = next();
        else if (a == L"--slots-out") slotsOut = next();
        else if (a == L"--addrs-max") addrsMax = wcstoull(next(), nullptr, 10);
        else if (a == L"--findref") findref = wcstoull(next(), nullptr, 16);
        else if (a == L"--span") span = wcstoull(next(), nullptr, 0);
        else if (a == L"--regions") regionsTop = wcstoull(next(), nullptr, 10);
        else
        {
            fwprintf(stderr, L"unknown argument %ls\n", a.c_str());
            return 1;
        }
    }

    MappedDump dump;
    if (!dump.Open(argv[1]))
    {
        return 2;
    }
    std::vector<Module> modules;
    std::vector<Region> regions;
    std::vector<Range> ranges;
    if (!ParseDump(dump, modules, regions, ranges))
    {
        return 2;
    }
    printf("== dump: %zu modules, %zu regions, %zu captured ranges, %.1f MB\n", modules.size(), regions.size(), ranges.size(), dump.size / 1048576.0);
    PrintRegions(regions, regionsTop);

    // --refs-of: the batch form of --findref for many ranges at once (one scan instead of one per range):
    // every line of the input is "0x<start> <size>", every output line "0x<start>\t0x<slot>" (at most 16
    // referrers per range). ownerchain-style attribution of hundreds of samples needs this to finish in
    // seconds rather than one 5-second scan per address.
    if (!refsOfIn.empty() && !refsOfOut.empty())
    {
        std::vector<std::pair<uint64_t, uint64_t>> targets;
        FILE* in = nullptr;
        if (_wfopen_s(&in, refsOfIn.c_str(), L"rb") == 0 && in)
        {
            unsigned long long s = 0, n = 0;
            while (fscanf_s(in, "%llx %llu", &s, &n) == 2)
            {
                targets.emplace_back(s, s + n);
            }
            fclose(in);
        }
        std::sort(targets.begin(), targets.end());
        std::vector<std::vector<uint64_t>> hitsPer(targets.size());
        const uint64_t tlo = targets.empty() ? 0 : targets.front().first;
        const uint64_t thi = targets.empty() ? 0 : targets.back().second;
        ForEachSlot(regions, ranges, [&](uint64_t slot, uint64_t v) {
            if (v < tlo || v >= thi)
            {
                return;
            }
            auto it = std::upper_bound(targets.begin(), targets.end(), std::make_pair(v, UINT64_MAX));
            if (it == targets.begin())
            {
                return;
            }
            --it;
            if (v < it->second)
            {
                auto& h = hitsPer[static_cast<size_t>(it - targets.begin())];
                // A slot inside the target range itself is a self-reference, not an owner.
                if (h.size() < 16 && !(slot >= it->first && slot < it->second))
                {
                    h.push_back(slot);
                }
            }
        });
        FILE* out = nullptr;
        if (_wfopen_s(&out, refsOfOut.c_str(), L"wb") == 0 && out)
        {
            for (size_t i = 0; i < targets.size(); ++i)
            {
                for (const uint64_t slot : hitsPer[i])
                {
                    fprintf(out, "0x%llX\t0x%llX\n", static_cast<unsigned long long>(targets[i].first), static_cast<unsigned long long>(slot));
                }
            }
            fclose(out);
        }
        printf("== refs-of: %zu ranges -> %s\n", targets.size(), Utf8(refsOfOut).c_str());
        return 0;
    }

    // --findref needs no symbols: it answers "which slots point into [findref, findref+span)".
    if (findref)
    {
        printf("== slots pointing into [0x%llX, 0x%llX):\n", static_cast<unsigned long long>(findref), static_cast<unsigned long long>(findref + span));
        size_t shown = 0;
        ForEachSlot(regions, ranges, [&](uint64_t slot, uint64_t v) {
            if (v >= findref && v < findref + span && shown < 200)
            {
                const Region* r = RegionOf(regions, slot);
                printf("  slot 0x%016llX = 0x%016llX (region alloc 0x%llX)\n", static_cast<unsigned long long>(slot), static_cast<unsigned long long>(v),
                       r ? static_cast<unsigned long long>(r->allocBase) : 0ull);
                ++shown;
            }
        });
        printf("  (%zu shown)\n", shown);
        return 0;
    }

    CoInitialize(nullptr);
    {
        IDiaDataSource* probe = nullptr;
        if (FAILED(NoRegCoCreate(kMsdia, __uuidof(DiaSource), __uuidof(IDiaDataSource), reinterpret_cast<void**>(&probe))) || !probe)
        {
            fwprintf(stderr, L"cannot load %ls\n", kMsdia);
            return 3;
        }
        probe->Release();
    }

    std::vector<VTable> vts;
    printf("== symbols:\n");
    for (uint32_t i = 0; i < modules.size(); ++i)
    {
        if (!syms.only.empty())
        {
            // --only: skip every module whose name contains none of the listed substrings (case-insensitive).
            bool wanted = false;
            for (const auto& o : syms.only)
            {
                std::wstring n = modules[i].name, s = o;
                std::transform(n.begin(), n.end(), n.begin(), ::towlower);
                std::transform(s.begin(), s.end(), s.begin(), ::towlower);
                wanted |= n.find(s) != std::wstring::npos;
            }
            if (!wanted)
            {
                continue;
            }
        }
        const size_t before = vts.size();
        const ULONGLONG ts = GetTickCount64();
        fwprintf(stderr, L"  [loading %ls]\n", modules[i].name.c_str());
        if (syms.Load(modules[i], i, vts))
        {
            printf("  loaded  %-44ls %6zu vftables  %5.1f s  %ls\n", modules[i].name.c_str(), vts.size() - before,
                   (GetTickCount64() - ts) / 1000.0, modules[i].pdbPath.c_str());
        }
    }
    for (const auto& m : modules)
    {
        if (!m.loaded && m.hasCv)
        {
            const std::wstring id = SymStoreId(m.guid, m.age);
            printf("  MISSING %-44ls https://msdl.microsoft.com/download/symbols/%ls/%ls/%ls\n", m.name.c_str(), m.pdbLeaf.c_str(), id.c_str(),
                   m.pdbLeaf.c_str());
        }
    }

    std::unordered_map<uint64_t, uint32_t> byVa;
    byVa.reserve(vts.size() * 2);
    uint64_t lo = UINT64_MAX, hi = 0;
    for (uint32_t i = 0; i < vts.size(); ++i)
    {
        byVa.emplace(vts[i].va, i);
        lo = std::min(lo, vts[i].va);
        hi = std::max(hi, vts[i].va);
    }

    const ULONGLONG t0 = GetTickCount64();
    uint64_t slots = 0, hits = 0;
    ForEachSlot(regions, ranges, [&](uint64_t slot, uint64_t v) {
        ++slots;
        if (v < lo || v > hi)
        {
            return;
        }
        auto it = byVa.find(v);
        if (it == byVa.end())
        {
            return;
        }
        VTable& vt = vts[it->second];
        ++vt.count;
        ++hits;
        if (vt.samples.size() < 64)
        {
            vt.samples.push_back(slot);
        }
    });
    printf("== scanned %.1f M slots in %.1f s: %llu vftable hits\n", slots / 1e6, (GetTickCount64() - t0) / 1000.0, static_cast<unsigned long long>(hits));

    if (refs)
    {
        // Second pass: put every vptr slot address into a set, then count the slots whose VALUE is one of
        // them. The slot addresses are re-collected here (only the first 64 per vftable were kept above).
        U64Set set;
        set.Reserve(static_cast<size_t>(hits));
        ForEachSlot(regions, ranges, [&](uint64_t slot, uint64_t v) {
            if (v >= lo && v <= hi && byVa.count(v))
            {
                set.Insert(slot);
            }
        });
        const ULONGLONG t1 = GetTickCount64();
        ForEachSlot(regions, ranges, [&](uint64_t, uint64_t v) {
            if ((v & 7) == 0 && v)
            {
                const size_t i = set.Find(v);
                if (i != SIZE_MAX && set.refs[i] < UINT32_MAX)
                {
                    ++set.refs[i];
                }
            }
        });
        ForEachSlot(regions, ranges, [&](uint64_t slot, uint64_t v) {
            if (v < lo || v > hi)
            {
                return;
            }
            auto it = byVa.find(v);
            if (it == byVa.end())
            {
                return;
            }
            const size_t i = set.Find(slot);
            if (i != SIZE_MAX && set.refs[i] > 0)
            {
                ++vts[it->second].refd;
            }
        });
        printf("== refs pass in %.1f s\n", (GetTickCount64() - t1) / 1000.0);
    }

    if (sizes)
    {
        // --sizes: estimate each type's heap block size from its own slot addresses. Same-type objects
        // of one size land in the same LFH bucket, so after sorting, the most common gap between
        // neighbouring slots (taken only within 64 KB, a multiple of 16) is the bucket's block size.
        // count x size then ranks what the heap actually spends its bytes on.
        std::vector<std::vector<uint64_t>> all(vts.size());
        for (uint32_t i = 0; i < vts.size(); ++i)
        {
            if (vts[i].count >= 16)
            {
                all[i].reserve(static_cast<size_t>(vts[i].count));
            }
        }
        ForEachSlot(regions, ranges, [&](uint64_t slot, uint64_t v) {
            if (v < lo || v > hi)
            {
                return;
            }
            auto it = byVa.find(v);
            if (it != byVa.end() && vts[it->second].count >= 16)
            {
                all[it->second].push_back(slot);
            }
        });
        for (uint32_t i = 0; i < vts.size(); ++i)
        {
            auto& v = all[i];
            if (v.size() < 16)
            {
                continue;
            }
            std::sort(v.begin(), v.end());
            std::unordered_map<uint64_t, uint32_t> gaps;
            for (size_t k = 1; k < v.size(); ++k)
            {
                const uint64_t g = v[k] - v[k - 1];
                if (g >= 16 && g < 65536 && (g & 15) == 0)
                {
                    ++gaps[g];
                }
            }
            uint64_t best = 0;
            uint32_t bestN = 0;
            for (const auto& [g, n] : gaps)
            {
                if (n > bestN || (n == bestN && g < best))
                {
                    best = g;
                    bestN = n;
                }
            }
            vts[i].blockSize = best;
            std::vector<uint64_t>().swap(v);
        }
    }

    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < vts.size(); ++i)
    {
        if (vts[i].count)
        {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return vts[a].count > vts[b].count; });

    printf("== top %zu vftables by slot count%s:\n", std::min(top, order.size()), refs ? " (count / refd)" : "");
    for (size_t k = 0; k < order.size() && k < top; ++k)
    {
        const VTable& vt = vts[order[k]];
        std::string name = DisplayName(vt);
        if (name.size() > 260)
        {
            name = name.substr(0, 257) + "...";
        }
        if (refs)
        {
            printf("%9llu %9llu  %-22s %s\n", static_cast<unsigned long long>(vt.count), static_cast<unsigned long long>(vt.refd),
                   Utf8(modules[vt.module].name).c_str(), name.c_str());
        }
        else
        {
            printf("%9llu  %-22s %s\n", static_cast<unsigned long long>(vt.count), Utf8(modules[vt.module].name).c_str(), name.c_str());
        }
    }

    if (!addrsFilter.empty())
    {
        const std::string needle = Utf8(addrsFilter);
        printf("== vptr slots of vftables whose name contains \"%s\":\n", needle.c_str());
        for (uint32_t i : order)
        {
            const std::string name = DisplayName(vts[i]);
            if (name.find(needle) == std::string::npos)
            {
                continue;
            }
            printf("  [%llu] %s\n", static_cast<unsigned long long>(vts[i].count), name.substr(0, 300).c_str());
            for (size_t s = 0; s < vts[i].samples.size() && s < addrsMax; ++s)
            {
                printf("    0x%016llX\n", static_cast<unsigned long long>(vts[i].samples[s]));
            }
        }
    }

    if (!slotsFilter.empty() && !slotsOut.empty())
    {
        // --slots: one more pass that writes EVERY vptr slot of the matching vftables (not just the 64
        // kept per row), so a follow-up script (dumpview.py) can read each object's fields.
        const std::string needle = Utf8(slotsFilter);
        std::vector<char> wanted(vts.size(), 0);
        for (uint32_t i : order)
        {
            wanted[i] = DisplayName(vts[i]).find(needle) != std::string::npos;
        }
        FILE* f = nullptr;
        if (_wfopen_s(&f, slotsOut.c_str(), L"wb") == 0 && f)
        {
            uint64_t written = 0;
            ForEachSlot(regions, ranges, [&](uint64_t slot, uint64_t v) {
                if (v < lo || v > hi)
                {
                    return;
                }
                auto it = byVa.find(v);
                if (it != byVa.end() && wanted[it->second])
                {
                    fprintf(f, "0x%llX\t0x%llX\n", static_cast<unsigned long long>(slot), static_cast<unsigned long long>(v));
                    ++written;
                }
            });
            fclose(f);
            printf("== wrote %llu slots of vftables matching \"%s\" to %s\n", static_cast<unsigned long long>(written), needle.c_str(),
                   Utf8(slotsOut).c_str());
        }
    }

    if (!outPath.empty())
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, outPath.c_str(), L"wb") == 0 && f)
        {
            fprintf(f, "count\trefd\tmodule\trva\tblock\tname\n");
            for (uint32_t i : order)
            {
                const VTable& vt = vts[i];
                fprintf(f, "%llu\t%llu\t%s\t0x%X\t%llu\t%s\n", static_cast<unsigned long long>(vt.count), static_cast<unsigned long long>(vt.refd),
                        Utf8(modules[vt.module].name).c_str(), vt.rva, static_cast<unsigned long long>(vt.blockSize), DisplayName(vt).c_str());
            }
            fclose(f);
            printf("== wrote %zu rows to %s\n", order.size(), Utf8(outPath).c_str());
        }
        else
        {
            fwprintf(stderr, L"cannot write %ls\n", outPath.c_str());
        }
    }
    return 0;
}
