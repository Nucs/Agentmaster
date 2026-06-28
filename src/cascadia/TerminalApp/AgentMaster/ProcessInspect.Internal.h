// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: ProcessInspect file-local low-level primitives (PEB / RTL_USER_PROCESS_PARAMETERS
// reads, Toolhelp-free string/file helpers, transcript globbing). Factored out of ProcessInspect.cpp
// so the by-section partial TUs (ProcessInspect.{cpp,Transcript,Content,Window,Summary}.cpp) all share
// ONE copy of these helpers. Kept in an ANONYMOUS namespace (internal linkage, a per-TU copy) exactly
// as before the split -- no behavior change; the primitives are private to the ProcessInspect TUs.
#pragma once

#include <windows.h>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>
#include <filesystem>
#include <algorithm>
#include <cwctype>

#include "ProcessInspect.h" // TranscriptCandidate (GlobTranscripts result type)

namespace
{
    using namespace Agentmaster;

    // ===== x64 PEB / RTL_USER_PROCESS_PARAMETERS field offsets ==============================
    // The stable x64 layout WT itself relies on in ConptyConnection::_commandlineFromProcess.
    // x64-ONLY — a WOW64 (32-bit) target has a different layout, so the reads below first guard
    // with IsWow64Process and bail (empty -> the session falls back to observe-only, never
    // mis-bound). claude.exe / pwsh.exe / cmd.exe are all x64 here, so this never triggers.
    constexpr uintptr_t kPebProcessParameters = 0x20; // PEB.ProcessParameters
    constexpr uintptr_t kParamsCurrentDirectory = 0x38; // RTL_USER_PROCESS_PARAMETERS.CurrentDirectory.DosPath (UNICODE_STRING)
    constexpr uintptr_t kParamsCommandLine = 0x70; // .CommandLine (UNICODE_STRING)
    constexpr uintptr_t kParamsEnvironment = 0x80; // .Environment (PVOID)
    constexpr uintptr_t kParamsEnvironmentSize = 0x3F0; // .EnvironmentSize (ULONG_PTR, bytes)

    // A claude's own transcript is created when it first writes — at/after its process start. Allow
    // this much slack before start (clock resolution / measurement) when deciding "is this MY
    // transcript"; anything created earlier than (start - skew) belongs to another/older claude.
    constexpr int64_t kTranscriptStartSkewMs = 2000;

    struct UnicodeStr
    {
        USHORT Length; // bytes in use (NOT including a terminator)
        USHORT MaxLength;
        PVOID Buffer;
    };

    using NtQIP = LONG(__stdcall*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    NtQIP GetNtQip()
    {
        static const auto fn = reinterpret_cast<NtQIP>(
            ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
        return fn;
    }

    // Open a process for a PEB read (query + VM read). nullptr on failure (denied / exited).
    HANDLE OpenForRead(uint32_t pid)
    {
        if (pid == 0)
        {
            return nullptr;
        }
        return ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    }

    // Open a process for a metadata query only (times / liveness — no VM read needed).
    HANDLE OpenForQuery(uint32_t pid)
    {
        if (pid == 0)
        {
            return nullptr;
        }
        return ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    }

    // True iff the (open) target is a 32-bit process under WOW64 — the x64 offsets above would
    // misread it, so callers skip rather than return garbage. False ("native") if undeterminable.
    bool TargetIsWow64(HANDLE h)
    {
        BOOL w = FALSE;
        if (::IsWow64Process(h, &w))
        {
            return w == TRUE;
        }
        return false;
    }

    // PEB base (via NtQueryInformationProcess) -> the ProcessParameters pointer. nullptr on failure.
    void* ReadProcParams(HANDLE h)
    {
        const auto ntqip = GetNtQip();
        if (ntqip == nullptr)
        {
            return nullptr;
        }
        struct PBI
        {
            LONG_PTR ExitStatus;
            PVOID PebBaseAddress;
            ULONG_PTR Reserved[4];
        } pbi{};
        if (ntqip(h, 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi), nullptr) != 0 || pbi.PebBaseAddress == nullptr)
        {
            return nullptr;
        }
        void* params = nullptr;
        SIZE_T got = 0;
        if (!::ReadProcessMemory(h, static_cast<BYTE*>(pbi.PebBaseAddress) + kPebProcessParameters, &params, sizeof(params), &got) || params == nullptr)
        {
            return nullptr;
        }
        return params;
    }

    // Read a UNICODE_STRING field at `fieldOffset` within ProcessParameters into a wstring.
    std::wstring ReadUnicodeStringField(HANDLE h, void* params, uintptr_t fieldOffset)
    {
        UnicodeStr us{};
        SIZE_T got = 0;
        if (!::ReadProcessMemory(h, static_cast<BYTE*>(params) + fieldOffset, &us, sizeof(us), &got))
        {
            return {};
        }
        if (us.Length == 0 || us.Buffer == nullptr)
        {
            return {};
        }
        std::wstring buf;
        buf.resize(us.Length / sizeof(wchar_t));
        if (!::ReadProcessMemory(h, us.Buffer, buf.data(), us.Length, &got))
        {
            return {};
        }
        buf.resize(us.Length / sizeof(wchar_t));
        return buf;
    }

    // FILETIME (100ns since 1601) -> Unix epoch milliseconds. 0 for an unset/pre-epoch time.
    int64_t FileTimeToUnixMs(const FILETIME& ft)
    {
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        constexpr uint64_t kEpochDiff = 116444736000000000ull; // 1601-01-01 -> 1970-01-01 in 100ns
        if (u.QuadPart < kEpochDiff)
        {
            return 0;
        }
        return static_cast<int64_t>((u.QuadPart - kEpochDiff) / 10000ull);
    }

    // --- tiny ASCII case-insensitive string helpers (exe names / env keys / flags) ---
    wchar_t LowerAscii(wchar_t c)
    {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
    }
    bool StartsWithCI(std::wstring_view s, std::wstring_view p)
    {
        if (s.size() < p.size())
        {
            return false;
        }
        for (size_t i = 0; i < p.size(); ++i)
        {
            if (LowerAscii(s[i]) != LowerAscii(p[i]))
            {
                return false;
            }
        }
        return true;
    }
    bool ContainsCI(std::wstring_view hay, std::wstring_view needle)
    {
        if (needle.empty())
        {
            return true;
        }
        if (hay.size() < needle.size())
        {
            return false;
        }
        for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        {
            if (StartsWithCI(hay.substr(i), needle))
            {
                return true;
            }
        }
        return false;
    }

    // Split a command line into args, honoring double quotes and stripping them ("--settings=\"C:/a
    // b\"" -> one token "--settings=C:/a b"). Sufficient for reading our own claude command lines.
    std::vector<std::wstring> TokenizeCmdline(std::wstring_view cl)
    {
        std::vector<std::wstring> toks;
        std::wstring cur;
        bool inq = false;
        bool has = false;
        for (const wchar_t c : cl)
        {
            if (c == L'"')
            {
                inq = !inq;
                has = true;
                continue;
            }
            if (!inq && (c == L' ' || c == L'\t'))
            {
                if (has)
                {
                    toks.push_back(cur);
                    cur.clear();
                    has = false;
                }
                continue;
            }
            cur.push_back(c);
            has = true;
        }
        if (has)
        {
            toks.push_back(cur);
        }
        return toks;
    }

    // Walk a NUL-separated environment block ("NAME=VALUE\0NAME=VALUE\0\0") into NAME->VALUE.
    void ParseEnvBlock(const std::wstring& blob, std::unordered_map<std::wstring, std::wstring>& out)
    {
        size_t i = 0;
        while (i < blob.size())
        {
            size_t nul = blob.find(L'\0', i);
            if (nul == std::wstring::npos)
            {
                nul = blob.size();
            }
            if (nul == i)
            {
                break; // empty entry == the block's double-NUL terminator
            }
            const std::wstring_view entry(blob.data() + i, nul - i);
            const size_t eq = entry.find(L'=');
            if (eq != std::wstring_view::npos && eq > 0) // eq==0 => a cmd "=C:" drive var; skip
            {
                out.insert_or_assign(std::wstring{ entry.substr(0, eq) }, std::wstring{ entry.substr(eq + 1) });
            }
            i = nul + 1;
        }
    }

    // Glob <dir>\*.jsonl into transcript candidates (stem + mtime + ctime). Empty if the dir is absent.
    std::vector<TranscriptCandidate> GlobTranscripts(const std::wstring& dir)
    {
        std::vector<TranscriptCandidate> out;
        const std::wstring pattern = dir + L"\\*.jsonl";
        WIN32_FIND_DATAW fd{};
        const HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return out;
        }
        static const std::wstring ext = L".jsonl";
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }
            std::wstring name = fd.cFileName;
            if (name.size() <= ext.size())
            {
                continue;
            }
            TranscriptCandidate c;
            c.stem = name.substr(0, name.size() - ext.size());
            c.mtimeMs = FileTimeToUnixMs(fd.ftLastWriteTime);
            c.ctimeMs = FileTimeToUnixMs(fd.ftCreationTime);
            out.push_back(std::move(c));
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
        return out;
    }

    // UTF-8 bytes -> wide. Claude transcripts are UTF-8 .jsonl. Empty on empty/failure.
    std::wstring Utf8ToWide(const std::string& bytes)
    {
        if (bytes.empty())
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (n <= 0)
        {
            return {};
        }
        std::wstring w(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), w.data(), n);
        return w;
    }

    // Collapse a (possibly multi-line) prompt to a single trimmed display line: take the first
    // non-blank line, trim surrounding whitespace. Used for the external row's title.
    std::wstring FirstLineTrim(const std::wstring& s)
    {
        size_t i = 0;
        // skip leading blank lines / whitespace
        while (i < s.size() && (s[i] == L'\n' || s[i] == L'\r' || s[i] == L' ' || s[i] == L'\t'))
        {
            ++i;
        }
        size_t j = i;
        while (j < s.size() && s[j] != L'\n' && s[j] != L'\r')
        {
            ++j;
        }
        // trim trailing whitespace of [i, j)
        while (j > i && (s[j - 1] == L' ' || s[j - 1] == L'\t'))
        {
            --j;
        }
        return s.substr(i, j - i);
    }

    // Read up to maxBytes from the START of a file (0 == whole file). Empty on failure. Shared
    // read/write/delete so a live, append-only transcript can be read while Claude writes it.
    std::string ReadFileHead(const std::wstring& path, size_t maxBytes)
    {
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return {};
        }
        LARGE_INTEGER sz{};
        ::GetFileSizeEx(h, &sz);
        uint64_t want = static_cast<uint64_t>(sz.QuadPart);
        if (maxBytes != 0 && want > maxBytes)
        {
            want = maxBytes;
        }
        std::string bytes(static_cast<size_t>(want), '\0');
        size_t off = 0;
        while (off < bytes.size())
        {
            DWORD got = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(bytes.size() - off, 1u << 20));
            if (!::ReadFile(h, bytes.data() + off, chunk, &got, nullptr) || got == 0)
            {
                break;
            }
            off += got;
        }
        bytes.resize(off);
        ::CloseHandle(h);
        return bytes;
    }

    // Agentmaster: read the LAST `maxBytes` of a file (the TAIL), mirroring ReadFileHead's share
    // flags. This is the read for the idle RECAP (away_summary): Claude appends the recap near the
    // END of the transcript when a session goes idle, so it lives in the TAIL region — the OPPOSITE
    // end from the first-prompt title ReadFileHead pulls. The read may START mid-line; that leading
    // partial line just fails json::Parse downstream (JSONL is line-framed, a recap is one whole
    // line), so no special-casing is needed.
    std::string ReadFileTail(const std::wstring& path, size_t maxBytes)
    {
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return {};
        }
        LARGE_INTEGER sz{};
        ::GetFileSizeEx(h, &sz);
        const uint64_t total = static_cast<uint64_t>(sz.QuadPart);
        uint64_t want = (maxBytes != 0 && total > maxBytes) ? maxBytes : total;
        if (const uint64_t startAt = total - want; startAt != 0)
        {
            LARGE_INTEGER li{};
            li.QuadPart = static_cast<LONGLONG>(startAt);
            ::SetFilePointerEx(h, li, nullptr, FILE_BEGIN); // seek to the tail window
        }
        std::string bytes(static_cast<size_t>(want), '\0');
        size_t off = 0;
        while (off < bytes.size())
        {
            DWORD got = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(bytes.size() - off, 1u << 20));
            if (!::ReadFile(h, bytes.data() + off, chunk, &got, nullptr) || got == 0)
            {
                break;
            }
            off += got;
        }
        bytes.resize(off);
        ::CloseHandle(h);
        return bytes;
    }
}
