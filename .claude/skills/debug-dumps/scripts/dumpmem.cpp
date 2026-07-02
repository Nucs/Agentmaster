// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// dumpmem — hexdump arbitrary virtual memory FROM a minidump (full dumps carry the pages).
// Usage: dumpmem <dmp> <hexAddress> <bytes>
// Locates the address in the Memory64ListStream (falls back to MemoryListStream) and prints a
// hex+wchar/ascii dump. Born for 0xC0000374 heap-corruption forensics: ExceptionInformation[0]
// points at ntdll's HEAP_FAILURE_INFORMATION — read it, then hexdump the corrupt block it names
// (overrun bytes often contain the WRITER'S data, e.g. a recognizable string).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

#pragma comment(lib, "dbghelp.lib")

int wmain(int argc, wchar_t** argv)
{
    if (argc < 4)
    {
        wprintf(L"usage: dumpmem <dmp> <hexAddress> <bytes>\n");
        return 2;
    }
    const ULONG64 want = _wcstoui64(argv[2], nullptr, 16);
    const ULONG64 count = _wcstoui64(argv[3], nullptr, 0);

    const HANDLE f = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        wprintf(L"open failed %lu\n", GetLastError());
        return 1;
    }
    const HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* base = m ? MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (!base)
    {
        wprintf(L"map failed %lu\n", GetLastError());
        return 1;
    }

    // Memory64ListStream: descriptors + one contiguous run of payload starting at BaseRva.
    PMINIDUMP_DIRECTORY dir{};
    void* stream{};
    ULONG streamSize{};
    if (MiniDumpReadDumpStream(base, Memory64ListStream, &dir, &stream, &streamSize) && stream)
    {
        const auto* list = static_cast<MINIDUMP_MEMORY64_LIST*>(stream);
        ULONG64 rva = list->BaseRva;
        for (ULONG64 i = 0; i < list->NumberOfMemoryRanges; ++i)
        {
            const auto& r = list->MemoryRanges[i];
            if (want >= r.StartOfMemoryRange && want < r.StartOfMemoryRange + r.DataSize)
            {
                const ULONG64 off = want - r.StartOfMemoryRange;
                const ULONG64 avail = r.DataSize - off;
                const ULONG64 n = count < avail ? count : avail;
                const auto* p = static_cast<const unsigned char*>(base) + rva + off;
                wprintf(L"range base=0x%llX size=0x%llX (dumping %llu bytes at 0x%llX)\n",
                        r.StartOfMemoryRange, r.DataSize, n, want);
                for (ULONG64 o = 0; o < n; o += 16)
                {
                    wprintf(L"%016llX  ", want + o);
                    for (ULONG64 b = 0; b < 16; ++b)
                    {
                        if (o + b < n)
                        {
                            wprintf(L"%02X ", p[o + b]);
                        }
                        else
                        {
                            wprintf(L"   ");
                        }
                    }
                    wprintf(L" ");
                    // wchar view (8 UTF-16 units per row) — heap spray from wide strings reads here
                    for (ULONG64 w = 0; w + 1 < 16 && o + w + 1 < n; w += 2)
                    {
                        const wchar_t c = *reinterpret_cast<const wchar_t*>(p + o + w);
                        wprintf(L"%lc", (c >= 0x20 && c < 0x7F) ? c : L'.');
                    }
                    wprintf(L"  ");
                    for (ULONG64 b = 0; b < 16 && o + b < n; ++b)
                    {
                        const unsigned char c = p[o + b];
                        wprintf(L"%lc", (c >= 0x20 && c < 0x7F) ? static_cast<wchar_t>(c) : L'.');
                    }
                    wprintf(L"\n");
                }
                return 0;
            }
            rva += r.DataSize;
        }
    }
    wprintf(L"address 0x%llX not present in Memory64ListStream\n", want);
    return 3;
}
