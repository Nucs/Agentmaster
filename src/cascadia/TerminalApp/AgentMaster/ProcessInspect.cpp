// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and the
// standalone test harness compiles it directly). See ProcessInspect.h / OBSERVER.md §6, §8b.
#include "ProcessInspect.h"

#include <windows.h>
#include <appmodel.h> // GetPackageFamilyName (host terminal: real WT vs Agentmaster vs Dev)
#include <tlhelp32.h> // CreateToolhelp32Snapshot

// Bring Window To Front (BringClaudeWindowToFront): COM + UI Automation client for the WT tab
// pick. Raw COM via WRL ComPtr — still no WinRT, still standalone-harness friendly. The explicit
// objbase/oleauto includes keep this TU independent of WIN32_LEAN_AND_MEAN trimming windows.h.
#include <objbase.h> // CoInitializeEx / CoCreateInstance
#include <oleauto.h> // SysStringLen / SysFreeString (UIA names are BSTRs)
#include <UIAutomation.h> // IUIAutomation* (tab enumeration + SelectionItem.Select)
#include <wrl/client.h> // Microsoft::WRL::ComPtr

#include <algorithm>
#include <cwctype> // towlower (tab-name heuristics)
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "ClaudeSpawn.h" // ClaudeProjectsDir() — the live Claude transcript root
#include "Json.h" // transcript line parsing (ReadTranscriptInfo)
#include "TranscriptStore.h" // IsNoiseUserPrompt + PickDisplayTitle (the shared prompt-noise + title-precedence rules)

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
}

namespace Agentmaster
{
    // ===== OS-touching: enumeration + PEB reads ============================================

    std::vector<ProcEntry> SnapshotProcesses()
    {
        std::vector<ProcEntry> out;
        const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
        {
            return out;
        }
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe); // MUST be set before Process32FirstW or it silently returns nothing
        if (::Process32FirstW(snap, &pe))
        {
            do
            {
                ProcEntry e;
                e.pid = pe.th32ProcessID;
                e.ppid = pe.th32ParentProcessID;
                e.image = pe.szExeFile;
                out.push_back(std::move(e));
            } while (::Process32NextW(snap, &pe));
        }
        ::CloseHandle(snap);
        return out;
    }

    std::wstring ReadProcessCwd(uint32_t pid)
    {
        const HANDLE h = OpenForRead(pid);
        if (h == nullptr)
        {
            return {};
        }
        std::wstring result;
        if (!TargetIsWow64(h))
        {
            if (void* params = ReadProcParams(h))
            {
                result = ReadUnicodeStringField(h, params, kParamsCurrentDirectory);
            }
        }
        ::CloseHandle(h);
        while (!result.empty() && (result.back() == L'\\' || result.back() == L'/'))
        {
            result.pop_back();
        }
        return result;
    }

    std::wstring ReadProcessCommandLine(uint32_t pid)
    {
        const HANDLE h = OpenForRead(pid);
        if (h == nullptr)
        {
            return {};
        }
        std::wstring result;
        if (!TargetIsWow64(h))
        {
            if (void* params = ReadProcParams(h))
            {
                result = ReadUnicodeStringField(h, params, kParamsCommandLine);
            }
        }
        ::CloseHandle(h);
        return result;
    }

    uint16_t ReadProcessImageSubsystem(uint32_t pid)
    {
        const HANDLE h = OpenForRead(pid);
        if (h == nullptr)
        {
            return 0;
        }
        uint16_t subsystem = 0;
        if (!TargetIsWow64(h)) // x64 PEB / PE layout only (matches the other reads)
        {
            if (const auto ntqip = GetNtQip())
            {
                struct PBI
                {
                    LONG_PTR ExitStatus;
                    PVOID PebBaseAddress;
                    ULONG_PTR Reserved[4];
                } pbi{};
                SIZE_T got = 0;
                BYTE* imageBase = nullptr;
                // PEB.ImageBaseAddress @ +0x10 == the main module's load address.
                if (ntqip(h, 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi), nullptr) == 0 && pbi.PebBaseAddress != nullptr &&
                    ::ReadProcessMemory(h, static_cast<BYTE*>(pbi.PebBaseAddress) + 0x10, &imageBase, sizeof(imageBase), &got) && imageBase != nullptr)
                {
                    LONG ntOff = 0; // IMAGE_DOS_HEADER.e_lfanew @ +0x3C -> the PE header
                    if (::ReadProcessMemory(h, imageBase + 0x3C, &ntOff, sizeof(ntOff), &got) && ntOff > 0 && ntOff < 0x1000000)
                    {
                        // IMAGE_NT_HEADERS = Signature(4) + IMAGE_FILE_HEADER(0x14) + IMAGE_OPTIONAL_HEADER.
                        // OptionalHeader.Subsystem is at +0x44 for BOTH PE32 and PE32+ (in PE32+ the wider
                        // 8-byte ImageBase exactly offsets the absent 4-byte BaseOfData), so no magic check.
                        uint16_t sub = 0;
                        if (::ReadProcessMemory(h, imageBase + ntOff + 0x18 + 0x44, &sub, sizeof(sub), &got))
                        {
                            subsystem = sub;
                        }
                    }
                }
            }
        }
        ::CloseHandle(h);
        return subsystem;
    }

    std::wstring ReadProcessPackageFamily(uint32_t pid)
    {
        const HANDLE h = OpenForQuery(pid); // PROCESS_QUERY_LIMITED_INFORMATION suffices
        if (h == nullptr)
        {
            return {};
        }
        std::wstring out;
        UINT32 len = 0;
        // First call sizes the buffer (ERROR_INSUFFICIENT_BUFFER); APPMODEL_ERROR_NO_PACKAGE for an
        // unpackaged process. `len` includes the NUL.
        if (::GetPackageFamilyName(h, &len, nullptr) == ERROR_INSUFFICIENT_BUFFER && len > 1)
        {
            std::wstring buf(len, L'\0');
            if (::GetPackageFamilyName(h, &len, buf.data()) == ERROR_SUCCESS && len > 0)
            {
                buf.resize(len - 1); // drop the trailing NUL
                out = std::move(buf);
            }
        }
        ::CloseHandle(h);
        return out;
    }

    std::wstring ReadProcessImagePath(uint32_t pid)
    {
        const HANDLE h = OpenForQuery(pid);
        if (h == nullptr)
        {
            return {};
        }
        std::wstring out;
        wchar_t buf[MAX_PATH * 2];
        DWORD sz = static_cast<DWORD>(std::size(buf));
        if (::QueryFullProcessImageNameW(h, 0, buf, &sz) && sz > 0)
        {
            out.assign(buf, sz);
        }
        ::CloseHandle(h);
        return out;
    }

    std::unordered_map<std::wstring, std::wstring> ReadProcessEnv(uint32_t pid)
    {
        std::unordered_map<std::wstring, std::wstring> out;
        const HANDLE h = OpenForRead(pid);
        if (h == nullptr)
        {
            return out;
        }
        if (!TargetIsWow64(h))
        {
            if (void* params = ReadProcParams(h))
            {
                void* envPtr = nullptr;
                ULONG_PTR envSize = 0;
                SIZE_T got = 0;
                ::ReadProcessMemory(h, static_cast<BYTE*>(params) + kParamsEnvironment, &envPtr, sizeof(envPtr), &got);
                ::ReadProcessMemory(h, static_cast<BYTE*>(params) + kParamsEnvironmentSize, &envSize, sizeof(envSize), &got);
                if (envPtr != nullptr)
                {
                    constexpr SIZE_T kCap = 1u << 20; // 1 MiB ceiling on the env block
                    std::wstring blob;
                    const auto tryRead = [&](SIZE_T bytes) -> bool {
                        if (bytes < sizeof(wchar_t))
                        {
                            return false;
                        }
                        bytes &= ~static_cast<SIZE_T>(1); // even byte count
                        blob.assign(bytes / sizeof(wchar_t), L'\0');
                        SIZE_T rd = 0;
                        if (::ReadProcessMemory(h, envPtr, blob.data(), bytes, &rd) && rd >= sizeof(wchar_t))
                        {
                            blob.resize(rd / sizeof(wchar_t));
                            return true;
                        }
                        return false;
                    };
                    bool ok = false;
                    if (envSize >= sizeof(wchar_t) && envSize <= kCap)
                    {
                        ok = tryRead(static_cast<SIZE_T>(envSize)); // the authoritative size (the common path)
                    }
                    if (!ok)
                    {
                        // EnvironmentSize unset/insane: fall back to bounded reads that stop on the
                        // first that doesn't cross unmapped memory.
                        for (const SIZE_T guess : { static_cast<SIZE_T>(65536), static_cast<SIZE_T>(16384), static_cast<SIZE_T>(4096) })
                        {
                            if (tryRead(guess))
                            {
                                ok = true;
                                break;
                            }
                        }
                    }
                    if (ok)
                    {
                        ParseEnvBlock(blob, out);
                    }
                }
            }
        }
        ::CloseHandle(h);
        return out;
    }

    int64_t ProcessStartUnixMs(uint32_t pid)
    {
        const HANDLE h = OpenForQuery(pid);
        if (h == nullptr)
        {
            return 0;
        }
        FILETIME creation{}, exit{}, kernel{}, user{};
        int64_t ms = 0;
        if (::GetProcessTimes(h, &creation, &exit, &kernel, &user))
        {
            ms = FileTimeToUnixMs(creation);
        }
        ::CloseHandle(h);
        return ms;
    }

    bool ProcessAlive(uint32_t pid)
    {
        if (pid == 0)
        {
            return false;
        }
        // Agentmaster: prefer the UNAMBIGUOUS wait-based liveness test over GetExitCodeProcess == STILL_ACTIVE
        // (259). The exit-code path has a classic footgun: a process that genuinely exits with code 259
        // reads as "still active" forever. WaitForSingleObject(h, 0) has no such ambiguity — the process
        // object is signaled (WAIT_OBJECT_0) once it has exited, and WAIT_TIMEOUT means it is still
        // running. This needs SYNCHRONIZE rights, which we have on our own claude children (the common
        // case). (Residual, still LOW: this keys on pid alone — a recycled pid for a NEW process reads as
        // alive. Pairing the recorded process start-time would close that, but the callers don't thread a
        // start-time through yet.)
        if (const HANDLE hs = ::OpenProcess(SYNCHRONIZE, FALSE, pid))
        {
            const DWORD w = ::WaitForSingleObject(hs, 0);
            ::CloseHandle(hs);
            return w == WAIT_TIMEOUT; // signaled => exited; timeout => still running
        }
        // No SYNCHRONIZE rights (an elevated / other-user external claude). Fall back to the query path:
        // an open failure here means gone/denied -> treat as not-alive for our cache (unchanged behavior).
        const HANDLE h = OpenForQuery(pid);
        if (h == nullptr)
        {
            return false;
        }
        DWORD code = 0;
        bool alive = false;
        if (::GetExitCodeProcess(h, &code))
        {
            alive = (code == STILL_ACTIVE);
        }
        ::CloseHandle(h);
        return alive;
    }

    // ===== PURE: tree helpers ==============================================================

    bool ImageNameEq(std::wstring_view a, std::wstring_view b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (LowerAscii(a[i]) != LowerAscii(b[i]))
            {
                return false;
            }
        }
        return true;
    }

    uint32_t FindDescendantByImage(const std::vector<ProcEntry>& snap, uint32_t root, std::wstring_view imageLeaf)
    {
        if (root == 0)
        {
            return 0;
        }
        // Descendant-OR-SELF: the root itself may BE the image. A Manager-launched claude is the
        // ConPTY ROOT (direct CreateProcessW — no shell in between), so the old children-only walk
        // never matched it: the observer's roster correlation read the tab as "no claude here",
        // the tab's activity misclassified, and the registry never received the out-of-band
        // enrichment (conversation timing / presence / model facts) for Launched sessions. The
        // root must EXIST in the snapshot to match (an unknown pid still returns 0).
        for (const auto& e : snap)
        {
            if (e.pid == root && ImageNameEq(e.image, imageLeaf))
            {
                return root;
            }
        }
        std::vector<uint32_t> frontier{ root };
        std::unordered_set<uint32_t> seen{ root };
        for (size_t i = 0; i < frontier.size(); ++i)
        {
            const uint32_t cur = frontier[i];
            for (const auto& e : snap)
            {
                if (e.ppid != cur || seen.count(e.pid) != 0)
                {
                    continue;
                }
                if (ImageNameEq(e.image, imageLeaf))
                {
                    return e.pid; // shallowest match (BFS level order), like the old FindClaudeDescendantPid
                }
                seen.insert(e.pid);
                frontier.push_back(e.pid);
            }
        }
        return 0;
    }

    std::vector<uint32_t> ChildrenOf(const std::vector<ProcEntry>& snap, uint32_t parent)
    {
        std::vector<uint32_t> out;
        for (const auto& e : snap)
        {
            if (e.ppid == parent)
            {
                out.push_back(e.pid);
            }
        }
        return out;
    }

    bool IsShellImage(std::wstring_view image)
    {
        static constexpr const wchar_t* kShells[] = {
            L"pwsh.exe", L"powershell.exe", L"cmd.exe", L"bash.exe", L"sh.exe", L"wsl.exe", L"zsh.exe"
        };
        for (const auto* s : kShells)
        {
            if (ImageNameEq(image, s))
            {
                return true;
            }
        }
        return false;
    }

    // Console host plumbing a shell/claude carries that is NOT "a command in progress".
    static bool IsConsoleInfra(std::wstring_view image)
    {
        return ImageNameEq(image, L"conhost.exe") || ImageNameEq(image, L"OpenConsole.exe");
    }

    bool HasActiveChild(const std::vector<ProcEntry>& snap, uint32_t pid)
    {
        if (pid == 0)
        {
            return false;
        }
        for (const auto& e : snap)
        {
            if (e.ppid == pid && !IsConsoleInfra(e.image))
            {
                return true;
            }
        }
        return false;
    }

    bool HasNonShellChild(const std::vector<ProcEntry>& snap, uint32_t pid)
    {
        if (pid == 0)
        {
            return false;
        }
        for (const auto& e : snap)
        {
            if (e.ppid == pid && !IsShellImage(e.image) && !IsConsoleInfra(e.image))
            {
                return true;
            }
        }
        return false;
    }

    std::vector<uint32_t> CommandChildrenOf(const std::vector<ProcEntry>& snap, uint32_t shellPid)
    {
        std::vector<uint32_t> out;
        if (shellPid == 0)
        {
            return out;
        }
        for (const auto& e : snap)
        {
            if (e.ppid == shellPid && !IsConsoleInfra(e.image))
            {
                out.push_back(e.pid);
            }
        }
        return out;
    }

    ShellCwd ResolveShellCwd(const std::vector<ProcEntry>& snap, uint32_t shellPid, std::wstring_view shellImage)
    {
        // Newest command child first: it was spawned at the shell's CURRENT $PWD, so its PEB cwd is
        // the most recent live working dir (the older ones may predate a `cd`). Its cwd is the truth
        // for ANY shell (cmd / pwsh / powershell) — children inherit the live cwd at spawn.
        uint32_t newestChild = 0;
        int64_t newestStart = -1;
        for (const uint32_t childPid : CommandChildrenOf(snap, shellPid))
        {
            const int64_t st = ProcessStartUnixMs(childPid);
            if (st >= newestStart)
            {
                newestStart = st;
                newestChild = childPid;
            }
        }
        if (newestChild != 0)
        {
            if (auto cwd = ReadProcessCwd(newestChild); !cwd.empty())
            {
                return { std::move(cwd), true };
            }
        }
        // No usable child: the shell's OWN PEB cwd. Accurate for cmd.exe (it syncs on `cd`); for
        // pwsh/powershell this is the frozen launch dir (Set-Location never touches the process cwd),
        // so it's flagged unreliable and a caller should prefer an earlier child-derived reading.
        auto own = ReadProcessCwd(shellPid);
        const bool reliable = !own.empty() && ImageNameEq(shellImage, L"cmd.exe");
        return { std::move(own), reliable };
    }

    uint32_t FindTerminalHostPid(const std::vector<ProcEntry>& snap, uint32_t pid)
    {
        if (pid == 0)
        {
            return 0;
        }
        std::unordered_map<uint32_t, const ProcEntry*> byPid;
        byPid.reserve(snap.size());
        for (const auto& e : snap)
        {
            byPid.emplace(e.pid, &e);
        }
        uint32_t cur = pid;
        std::unordered_set<uint32_t> seen;
        for (int depth = 0; depth < 24 && cur != 0 && seen.insert(cur).second; ++depth)
        {
            const auto it = byPid.find(cur);
            if (it == byPid.end())
            {
                break; // ancestor exited (an orphan)
            }
            const ProcEntry& e = *it->second;
            if (depth > 0 && (ImageNameEq(e.image, L"WindowsTerminal.exe") || ImageNameEq(e.image, L"wt.exe")))
            {
                return e.pid; // the hosting terminal (skip self at depth 0)
            }
            cur = e.ppid;
        }
        return 0;
    }

    // Name a hosting terminal PROCESS by identity: package family first (authoritative, works for both
    // loose-registered and MSIX installs), image path as the unpackaged fallback. (OBSERVER.md §11c)
    static std::wstring TerminalHostLabel(uint32_t terminalPid)
    {
        const std::wstring fam = ReadProcessPackageFamily(terminalPid);
        if (StartsWithCI(fam, L"AgentmasterDev")) // Dev FIRST — "Agentmaster" is a prefix of it
        {
            return L"Agentmaster Dev";
        }
        if (StartsWithCI(fam, L"Agentmaster"))
        {
            return L"Agentmaster";
        }
        if (StartsWithCI(fam, L"Microsoft.WindowsTerminal") || StartsWithCI(fam, L"WindowsTerminalDev"))
        {
            return L"Windows Terminal";
        }
        // No / foreign package identity: classify by the image path (a loose build).
        const std::wstring path = ReadProcessImagePath(terminalPid);
        if (ContainsCI(path, L"Agentmaster"))
        {
            if (ContainsCI(path, L"\\Debug\\"))
            {
                return L"Agentmaster Dev";
            }
            return L"Agentmaster"; // Release loose, or an MSIX path
        }
        return L"Windows Terminal"; // a WT-class host we can't pin further
    }

    std::wstring ResolveExternalHostLabel(const std::vector<ProcEntry>& snap, uint32_t claudePid, bool amSessionPresent)
    {
        if (const uint32_t hostPid = FindTerminalHostPid(snap, claudePid); hostPid != 0)
        {
            return TerminalHostLabel(hostPid); // a live terminal ancestor — authoritative
        }
        // No live WindowsTerminal ancestor. An AM_SESSION stamp means an Agentmaster instance launched
        // it (its host terminal has since exited — an orphan); we can't tell release/dev from a dead
        // host, so label it generically.
        if (amSessionPresent)
        {
            return L"Agentmaster";
        }
        // Console-hosted (cmd / pwsh launched outside a terminal): name the nearest shell ancestor.
        std::unordered_map<uint32_t, const ProcEntry*> byPid;
        byPid.reserve(snap.size());
        for (const auto& e : snap)
        {
            byPid.emplace(e.pid, &e);
        }
        uint32_t cur = claudePid;
        std::unordered_set<uint32_t> seen;
        for (int depth = 0; depth < 24 && cur != 0 && seen.insert(cur).second; ++depth)
        {
            const auto it = byPid.find(cur);
            if (it == byPid.end())
            {
                break;
            }
            const ProcEntry& e = *it->second;
            if (depth > 0 && IsShellImage(e.image))
            {
                std::wstring leaf{ e.image };
                if (const auto dot = leaf.rfind(L".exe"); dot != std::wstring::npos)
                {
                    leaf.resize(dot);
                }
                return leaf;
            }
            cur = e.ppid;
        }
        return {};
    }

    // ===== PURE: command-line + env parsing ================================================

    std::wstring EnvLookup(const std::unordered_map<std::wstring, std::wstring>& env, std::wstring_view name)
    {
        // Env names are case-insensitive on Windows; the map keeps original case, so scan with a
        // case-insensitive compare. Maps are small (a few dozen entries) and this runs a handful of
        // times per claude, so a linear scan is fine.
        for (const auto& [k, v] : env)
        {
            if (ImageNameEq(k, name))
            {
                return v;
            }
        }
        return {};
    }

    std::optional<std::wstring> ExtractCmdlineArg(std::wstring_view commandline, std::wstring_view flag)
    {
        const auto toks = TokenizeCmdline(commandline);
        for (size_t i = 0; i < toks.size(); ++i)
        {
            const std::wstring_view t = toks[i];
            if (t == flag)
            {
                // exact "--flag" -> the next token is its value
                if (i + 1 < toks.size())
                {
                    return toks[i + 1];
                }
                return std::nullopt;
            }
            // "--flag=value"
            if (t.size() > flag.size() + 1 && t.compare(0, flag.size(), flag) == 0 && t[flag.size()] == L'=')
            {
                return std::wstring{ t.substr(flag.size() + 1) };
            }
        }
        return std::nullopt;
    }

    void ParseClaudeFacts(std::wstring_view commandline, const std::unordered_map<std::wstring, std::wstring>& env, ClaudeProcessFacts& facts)
    {
        // --- correlation / ownership (env) ---
        facts.wtSession = EnvLookup(env, L"WT_SESSION");
        facts.amSession = EnvLookup(env, L"AM_SESSION");

        // --- model: --model X, else ANTHROPIC_MODEL, else CLAUDE_CODE_MODEL ---
        if (const auto v = ExtractCmdlineArg(commandline, L"--model"))
        {
            facts.model = *v;
        }
        if (facts.model.empty())
        {
            facts.model = EnvLookup(env, L"ANTHROPIC_MODEL");
        }
        if (facts.model.empty())
        {
            facts.model = EnvLookup(env, L"CLAUDE_CODE_MODEL");
        }

        // --- effort: --effort X, else CLAUDE_CODE_EFFORT_LEVEL ---
        if (const auto v = ExtractCmdlineArg(commandline, L"--effort"))
        {
            facts.effort = *v;
        }
        if (facts.effort.empty())
        {
            facts.effort = EnvLookup(env, L"CLAUDE_CODE_EFFORT_LEVEL");
        }

        // --- permission mode / resume / session-id (cmdline only) ---
        if (const auto v = ExtractCmdlineArg(commandline, L"--permission-mode"))
        {
            facts.permissionMode = *v;
        }
        if (const auto v = ExtractCmdlineArg(commandline, L"--resume"))
        {
            facts.resumeTarget = *v;
        }
        if (const auto v = ExtractCmdlineArg(commandline, L"--session-id"))
        {
            facts.sessionIdArg = *v;
        }

        // --- background job? (bg session kind / any CLAUDE_BG_* / daemon-run cmdline) ---
        facts.sessionName = EnvLookup(env, L"CLAUDE_CODE_SESSION_NAME");
        bool background = ImageNameEq(EnvLookup(env, L"CLAUDE_CODE_SESSION_KIND"), L"bg");
        if (!background)
        {
            for (const auto& [k, v] : env)
            {
                if (StartsWithCI(k, L"CLAUDE_BG_"))
                {
                    background = true;
                    break;
                }
            }
        }
        if (!background && (ContainsCI(commandline, L"daemon run") || ContainsCI(commandline, L"--bg-pty-host")))
        {
            background = true;
        }
        facts.background = background;
    }

    std::wstring WindowIdFromAmSession(std::wstring_view amSession)
    {
        const auto colon = amSession.find(L':');
        if (colon == std::wstring_view::npos)
        {
            return {}; // a bare "<processGuid>" (hand-typed `+`-tab claude) carries no window id
        }
        return std::wstring{ amSession.substr(colon + 1) };
    }

    RunningApp ClassifyRunningApp(std::wstring_view amSession, std::wstring_view wtSession, std::wstring_view ourAmSession)
    {
        // OBSERVER.md §7 / §19-Q1: our stamp -> Agentmaster; a bare WT_SESSION (no AM_SESSION) ->
        // external WindowsTerminal; anything else (incl. a FOREIGN AM_SESSION) -> Other. AM_SESSION
        // is "<processGuid>" for a hand-typed `+`-tab claude (it inherits our process env) or
        // "<processGuid>:<windowId>" for a Manager-Launched one (the launching window appends its
        // id via spec.env), so match on the GUID PREFIX — both forms are ours. A foreign-instance
        // claude is never bound regardless (its wtSession isn't in this window's roster).
        const auto guidPrefix = [](std::wstring_view s) -> std::wstring_view {
            const auto colon = s.find(L':');
            return colon == std::wstring_view::npos ? s : s.substr(0, colon);
        };
        if (!ourAmSession.empty() && !amSession.empty() && guidPrefix(amSession) == guidPrefix(ourAmSession))
        {
            return RunningApp::Agentmaster;
        }
        if (amSession.empty() && !wtSession.empty())
        {
            return RunningApp::WindowsTerminal;
        }
        return RunningApp::Other;
    }

    bool IsClaudeDesktopGuiApp(const ClaudeProcessFacts& facts)
    {
        // IMAGE_SUBSYSTEM_WINDOWS_GUI == 2. The Claude Code CLI is a console app (CUI == 3); the
        // Claude desktop app (Electron) + every renderer/gpu/utility/crashpad child share one GUI
        // binary. Only a confirmed GUI subsystem is the desktop app — 0 (undeterminable) stays a
        // candidate CLI so an elevated session is never hidden (see the header note).
        return facts.subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI;
    }

    // ===== OS-touching: full facts read ====================================================

    ClaudeProcessFacts ReadClaudeFacts(uint32_t pid)
    {
        ClaudeProcessFacts f;
        f.pid = pid;
        f.startUnixMs = ProcessStartUnixMs(pid);
        f.cwd = ReadProcessCwd(pid);
        f.commandline = ReadProcessCommandLine(pid);
        f.subsystem = ReadProcessImageSubsystem(pid); // console (CLI) vs GUI (the desktop Electron app)
        const auto env = ReadProcessEnv(pid);
        ParseClaudeFacts(f.commandline, env, f);
        // parentPid + runningApp are left for the caller (it has the snapshot + its own AM_SESSION).
        f.alive = true;
        return f;
    }

    // ===== transcript resolution ===========================================================

    std::wstring EncodeCwdToProjectDir(std::wstring_view cwd)
    {
        std::wstring out;
        out.reserve(cwd.size());
        for (const wchar_t c : cwd)
        {
            const bool alnum = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
            out.push_back(alnum ? c : L'-');
        }
        return out;
    }

    std::wstring ExtractCwdFromTranscriptHead(std::wstring_view headText)
    {
        size_t pos = 0;
        while ((pos = headText.find(L"\"cwd\"", pos)) != std::wstring_view::npos)
        {
            size_t i = pos + 5; // past the key
            while (i < headText.size() && (headText[i] == L' ' || headText[i] == L'\t'))
            {
                ++i;
            }
            if (i < headText.size() && headText[i] == L':')
            {
                ++i;
            }
            while (i < headText.size() && (headText[i] == L' ' || headText[i] == L'\t'))
            {
                ++i;
            }
            if (i < headText.size() && headText[i] == L'"')
            {
                ++i;
                std::wstring val;
                while (i < headText.size() && headText[i] != L'"')
                {
                    if (headText[i] == L'\\' && i + 1 < headText.size())
                    {
                        ++i;
                        const wchar_t e = headText[i];
                        val.push_back(e == L'n' ? L'\n' : (e == L't' ? L'\t' : e)); // \\ -> \, \" -> ", etc.
                    }
                    else
                    {
                        val.push_back(headText[i]);
                    }
                    ++i;
                }
                if (!val.empty())
                {
                    return val;
                }
            }
            pos += 5;
        }
        return {};
    }

    std::wstring PickNewestTranscript(const std::vector<TranscriptCandidate>& candidates, int64_t startUnixMs, int64_t tieWindowMs)
    {
        if (candidates.empty())
        {
            return {};
        }
        if (startUnixMs > 0)
        {
            // IDENTITY by creation time: a claude's OWN transcript is created when it first writes —
            // at/after its process start. Pick the candidate whose CREATION time is closest to (and
            // not significantly before) the claude's start, and REJECT transcripts created well
            // before it started (those belong to OTHER claudes / are stale). This is what keeps two
            // claudes sharing one cwd bound to their OWN conversation, and resolves a never-written-
            // yet claude to "" rather than collapsing it onto a stale transcript (§11d / §13). mtime
            // (activity) does NOT determine identity, so it is deliberately not used on this path.
            const TranscriptCandidate* best = nullptr;
            int64_t bestDelta = 0;
            for (const auto& c : candidates)
            {
                if (c.ctimeMs < startUnixMs - kTranscriptStartSkewMs)
                {
                    continue; // created before this claude started -> not its own
                }
                const int64_t d = c.ctimeMs >= startUnixMs ? c.ctimeMs - startUnixMs : startUnixMs - c.ctimeMs;
                if (best == nullptr || d < bestDelta)
                {
                    best = &c;
                    bestDelta = d;
                }
            }
            return best != nullptr ? best->stem : std::wstring{};
        }
        // No start hint (startUnixMs == 0): fall back to the newest by mtime (tie-broken by ctime).
        (void)tieWindowMs; // retained for API compat; the start>0 path keys on ctime, not an mtime band
        const TranscriptCandidate* best = nullptr;
        for (const auto& c : candidates)
        {
            if (best == nullptr || c.mtimeMs > best->mtimeMs || (c.mtimeMs == best->mtimeMs && c.ctimeMs > best->ctimeMs))
            {
                best = &c;
            }
        }
        return best != nullptr ? best->stem : std::wstring{};
    }

    std::wstring ResolveSessionIdIn(std::wstring_view projectsDir, std::wstring_view cwd, int64_t startUnixMs)
    {
        if (projectsDir.empty() || cwd.empty())
        {
            return {};
        }
        const std::wstring dir = std::wstring{ projectsDir } + L"\\" + EncodeCwdToProjectDir(cwd);
        const auto candidates = GlobTranscripts(dir);
        return PickNewestTranscript(candidates, startUnixMs);
    }

    std::wstring ResolveSessionId(std::wstring_view cwd, int64_t startUnixMs)
    {
        return ResolveSessionIdIn(ClaudeProjectsDir(), cwd, startUnixMs);
    }

    // ===== transcript content: timing + title + human prompts ================================

    bool TranscriptTimesIn(std::wstring_view projectsDir, std::wstring_view cwd, std::wstring_view sessionId, int64_t& createdUnixMs, int64_t& lastActivityUnixMs)
    {
        createdUnixMs = 0;
        lastActivityUnixMs = 0;
        if (projectsDir.empty() || cwd.empty() || sessionId.empty())
        {
            return false;
        }
        const std::wstring path = std::wstring{ projectsDir } + L"\\" + EncodeCwdToProjectDir(cwd) + L"\\" + std::wstring{ sessionId } + L".jsonl";
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            return false;
        }
        createdUnixMs = FileTimeToUnixMs(fad.ftCreationTime);
        lastActivityUnixMs = FileTimeToUnixMs(fad.ftLastWriteTime);
        return true;
    }

    bool TranscriptTimes(std::wstring_view cwd, std::wstring_view sessionId, int64_t& createdUnixMs, int64_t& lastActivityUnixMs)
    {
        return TranscriptTimesIn(ClaudeProjectsDir(), cwd, sessionId, createdUnixMs, lastActivityUnixMs);
    }

    TranscriptInfo ReadTranscriptInfoIn(std::wstring_view projectsDir, std::wstring_view cwd, std::wstring_view sessionId, size_t maxBytes, size_t maxPrompts)
    {
        TranscriptInfo info;
        if (projectsDir.empty() || cwd.empty() || sessionId.empty())
        {
            return info;
        }
        const std::wstring path = std::wstring{ projectsDir } + L"\\" + EncodeCwdToProjectDir(cwd) + L"\\" + std::wstring{ sessionId } + L".jsonl";

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            return info; // not found
        }
        info.found = true;
        info.createdUnixMs = FileTimeToUnixMs(fad.ftCreationTime);
        info.lastActivityUnixMs = FileTimeToUnixMs(fad.ftLastWriteTime);

        const std::string bytes = ReadFileHead(path, maxBytes);
        if (bytes.empty())
        {
            return info; // times only
        }
        const bool truncated = (maxBytes != 0); // a head read may end mid-line -> skip the last segment
        const std::wstring wide = Utf8ToWide(bytes);

        std::wstring firstPrompt;
        size_t start = 0;
        for (size_t i = 0; i <= wide.size(); ++i)
        {
            if (i < wide.size() && wide[i] != L'\n')
            {
                continue;
            }
            // The segment after the final '\n' (i == size) is partial on a truncated head read.
            if (i == wide.size() && truncated)
            {
                break;
            }
            std::wstring_view line(wide.data() + start, i - start);
            start = i + 1;
            while (!line.empty() && line.back() == L'\r')
            {
                line.remove_suffix(1);
            }
            if (line.empty())
            {
                continue;
            }
            const auto parsed = json::Parse(line);
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                continue;
            }
            const auto& obj = *parsed;
            const std::wstring lineType = obj.StrAt(L"type");
            // A user-SET conversation title ({"type":"custom-title","customTitle":...}). The LAST
            // one wins — a retitle appends a newer line. Display + tab matching prefer it over the
            // first prompt (it is the user's own label for the conversation).
            if (lineType == L"custom-title")
            {
                const std::wstring ct = obj.StrAt(L"customTitle");
                if (!ct.empty())
                {
                    info.customTitle = FirstLineTrim(ct);
                }
                continue;
            }
            // The async-generated picker title (second precedence) + the LEGACY summary line
            // (v2.0.75-2.1.25 only — kept so 100-versions-old transcripts still title sensibly).
            if (lineType == L"ai-title")
            {
                const std::wstring at = obj.StrAt(L"aiTitle");
                if (!at.empty())
                {
                    info.aiTitle = FirstLineTrim(at);
                }
                continue;
            }
            if (lineType == L"summary")
            {
                const std::wstring sm = obj.StrAt(L"summary");
                if (!sm.empty())
                {
                    info.summary = FirstLineTrim(sm);
                }
                continue;
            }
            // isCompactSummary == the synthetic post-compaction recap; isSidechain == an inline
            // subagent line (old strata wrote them into the main file) — neither is a human prompt.
            if (lineType != L"user" || obj.BoolAt(L"isMeta") || obj.BoolAt(L"isCompactSummary") || obj.BoolAt(L"isSidechain"))
            {
                continue;
            }
            if (info.gitBranch.empty())
            {
                const std::wstring gb = obj.StrAt(L"gitBranch");
                if (!gb.empty())
                {
                    info.gitBranch = gb;
                }
            }
            const auto* msg = obj.Find(L"message");
            if (!msg || msg->type != json::Value::Type::Obj)
            {
                continue;
            }
            const auto* content = msg->Find(L"content");
            if (!content)
            {
                continue;
            }
            std::wstring prompt;
            if (content->type == json::Value::Type::Str)
            {
                prompt = content->str;
            }
            else if (content->type == json::Value::Type::Arr)
            {
                // A pure-text user message is a human prompt; ANY tool_result block means a tool turn.
                bool hasToolResult = false;
                std::wstring text;
                for (const auto& blk : content->arr)
                {
                    if (blk.type != json::Value::Type::Obj)
                    {
                        continue;
                    }
                    const std::wstring bt = blk.StrAt(L"type");
                    if (bt == L"tool_result")
                    {
                        hasToolResult = true;
                        break;
                    }
                    if (bt == L"text")
                    {
                        text += blk.StrAt(L"text");
                    }
                }
                if (!hasToolResult)
                {
                    prompt = text;
                }
            }
            if (prompt.empty() || IsNoiseUserPrompt(prompt))
            {
                continue; // command echoes / task notifications / interrupts / reminders — control markers, not human messages
            }
            if (firstPrompt.empty())
            {
                firstPrompt = prompt;
            }
            if (info.userPrompts.size() < maxPrompts)
            {
                info.userPrompts.push_back(std::move(prompt));
            }
        }
        info.title = FirstLineTrim(firstPrompt);
        return info;
    }

    TranscriptInfo ReadTranscriptInfo(std::wstring_view cwd, std::wstring_view sessionId, size_t maxBytes, size_t maxPrompts)
    {
        return ReadTranscriptInfoIn(ClaudeProjectsDir(), cwd, sessionId, maxBytes, maxPrompts);
    }

    std::wstring TranscriptDisplayTitle(const TranscriptInfo& info)
    {
        return PickDisplayTitle(info.customTitle, info.aiTitle, info.summary, info.title);
    }
}

// ============================================================================================
// Bring Window To Front (the Manager's EXTERNAL right-click, last item) — find + surface the
// top-level window that HOSTS a foreign claude, out-of-band: restore it when minimized,
// foreground it, and when the host is a Windows Terminal-class window best-effort select the
// claude's TAB via UI Automation. Window activation only — never console input (Rule #13).
// ============================================================================================

namespace
{
    // The visible terminal window class every Windows Terminal 1.x main window registers
    // (IslandWindow's XAML_HOSTING_WINDOW_CLASS_NAME) — ours included (the fork's PFN suffix is
    // on the Emperor's hidden MESSAGE-window class, not the island window). A window of this
    // class is "WT-like": it carries a tab strip whose TabItems UIA can enumerate + select.
    constexpr std::wstring_view kTerminalIslandClass = L"CASCADIA_HOSTING_WINDOW_CLASS";

    std::wstring LowerCopy(std::wstring_view s)
    {
        std::wstring r{ s };
        for (auto& c : r)
        {
            c = static_cast<wchar_t>(::towlower(c));
        }
        return r;
    }

    bool IsTerminalIslandWindow(HWND h)
    {
        wchar_t cls[64]{};
        const int n = ::GetClassNameW(h, cls, ARRAYSIZE(cls));
        return n > 0 && std::wstring_view{ cls, static_cast<size_t>(n) } == kTerminalIslandClass;
    }

    // All VISIBLE, unowned top-level windows of `pid`, in z-order (EnumWindows order, topmost
    // first). Owned popups/tool windows are skipped. A minimized window is still WS_VISIBLE, so
    // it IS found — restoring it is the whole point. (A headless ConPTY host / the Emperor's
    // message window are not visible and never match.)
    std::vector<HWND> TopLevelWindowsOfPid(uint32_t pid)
    {
        struct Ctx
        {
            uint32_t pid;
            std::vector<HWND> wins;
        } ctx{ pid, {} };
        ::EnumWindows(
            [](HWND h, LPARAM lp) -> BOOL {
                auto& c = *reinterpret_cast<Ctx*>(lp);
                DWORD wpid = 0;
                ::GetWindowThreadProcessId(h, &wpid);
                if (wpid == c.pid && ::IsWindowVisible(h) && ::GetWindow(h, GW_OWNER) == nullptr)
                {
                    c.wins.push_back(h);
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        return ctx.wins;
    }

    // Every foreign (not-our-process) visible WT-class window, z-order topmost first. The
    // default-terminal-handoff fallback scans these; our OWN windows are excluded — an external
    // claude never lives in our roster (rostered == ours, Rule #13), and a managed claude tab of
    // ours could otherwise false-match the heuristics.
    std::vector<HWND> ForeignTerminalIslandWindows()
    {
        struct Ctx
        {
            DWORD selfPid;
            std::vector<HWND> wins;
        } ctx{ ::GetCurrentProcessId(), {} };
        ::EnumWindows(
            [](HWND h, LPARAM lp) -> BOOL {
                auto& c = *reinterpret_cast<Ctx*>(lp);
                DWORD wpid = 0;
                ::GetWindowThreadProcessId(h, &wpid);
                if (wpid != c.selfPid && ::IsWindowVisible(h) && ::GetWindow(h, GW_OWNER) == nullptr && IsTerminalIslandWindow(h))
                {
                    c.wins.push_back(h);
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        return ctx.wins;
    }

    // Images the host-window ancestor walk stops AT: nothing meaningful for a terminal tab lives
    // above these, and explorer.exe OWNS windows we must never foreground (the desktop/taskbar).
    bool IsAncestorBoundaryImage(std::wstring_view image)
    {
        static constexpr std::wstring_view kStop[] = {
            L"explorer.exe", L"svchost.exe", L"services.exe", L"wininit.exe", L"winlogon.exe",
            L"csrss.exe", L"smss.exe", L"userinit.exe", L"dwm.exe", L"sihost.exe"
        };
        for (const auto s : kStop)
        {
            if (Agentmaster::ImageNameEq(image, s))
            {
                return true;
            }
        }
        return false;
    }

    // Restore-if-minimized + take foreground. Runs while OUR window is the foreground window (a
    // Manager menu click), so SetForegroundWindow is permitted to hand foreground away;
    // SwitchToThisWindow is the belt-and-braces fallback when the shell denies it anyway.
    void RestoreAndForeground(HWND hwnd)
    {
        if (::IsIconic(hwnd))
        {
            ::ShowWindow(hwnd, SW_RESTORE);
        }
        ::SetForegroundWindow(hwnd);
        if (::GetForegroundWindow() != hwnd)
        {
            ::SwitchToThisWindow(hwnd, TRUE);
        }
    }

    // One TabItem of a WT-class window, as UIA exposes it: the element (kept for Select) + name.
    struct UiaTab
    {
        Microsoft::WRL::ComPtr<IUIAutomationElement> element;
        std::wstring name;
    };

    // Enumerate the TabItem descendants of `hwnd`. Empty on ANY failure (an elevated target —
    // UIA is UIPI-blocked from a non-elevated client — or an island that isn't hydrated): the
    // caller then simply foregrounds without a tab pick.
    std::vector<UiaTab> UiaTabsOf(IUIAutomation* uia, HWND hwnd)
    {
        std::vector<UiaTab> tabs;
        if (uia == nullptr)
        {
            return tabs;
        }
        Microsoft::WRL::ComPtr<IUIAutomationElement> root;
        if (FAILED(uia->ElementFromHandle(hwnd, &root)) || !root)
        {
            return tabs;
        }
        VARIANT v{};
        v.vt = VT_I4;
        v.lVal = UIA_TabItemControlTypeId;
        Microsoft::WRL::ComPtr<IUIAutomationCondition> cond;
        if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond)) || !cond)
        {
            return tabs;
        }
        Microsoft::WRL::ComPtr<IUIAutomationElementArray> found;
        if (FAILED(root->FindAll(TreeScope_Descendants, cond.Get(), &found)) || !found)
        {
            return tabs;
        }
        int n = 0;
        if (FAILED(found->get_Length(&n)))
        {
            return tabs;
        }
        for (int i = 0; i < n; ++i)
        {
            Microsoft::WRL::ComPtr<IUIAutomationElement> el;
            if (FAILED(found->GetElement(i, &el)) || !el)
            {
                continue;
            }
            BSTR b = nullptr;
            std::wstring name;
            if (SUCCEEDED(el->get_CurrentName(&b)) && b != nullptr)
            {
                name.assign(b, ::SysStringLen(b));
                ::SysFreeString(b);
            }
            tabs.push_back(UiaTab{ std::move(el), std::move(name) });
        }
        return tabs;
    }

    // Everything a tab name can be matched against for ONE claude: the title hints (display title,
    // custom title, first prompt — equal/containment/head rules), the cwd leaf, and the tokenized
    // conversation corpus in two tiers (head = title + first prompt; full = every prompt).
    struct TabPickHints
    {
        std::vector<std::wstring> titles;
        std::wstring cwdLeaf;
        std::unordered_set<std::wstring> headTokens;
        std::unordered_set<std::wstring> fullTokens;
    };

    // UiaTab adapter over the pure pick (PickClaudeTab owns the scoring + the subset/unique rules).
    int BestClaudeTabIn(const std::vector<UiaTab>& tabs, const TabPickHints& h, int& outScore, bool& outUnique)
    {
        std::vector<std::wstring> names;
        names.reserve(tabs.size());
        for (const auto& t : tabs)
        {
            names.push_back(t.name);
        }
        return Agentmaster::PickClaudeTab(names, h.titles, h.cwdLeaf, h.headTokens, h.fullTokens, outScore, outUnique);
    }

    // Select one tab. TabViewItem exposes SelectionItem (the real path); Invoke is just-in-case.
    bool UiaSelectTab(const UiaTab& tab)
    {
        Microsoft::WRL::ComPtr<IUIAutomationSelectionItemPattern> sel;
        if (SUCCEEDED(tab.element->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&sel))) && sel)
        {
            return SUCCEEDED(sel->Select());
        }
        Microsoft::WRL::ComPtr<IUIAutomationInvokePattern> inv;
        if (SUCCEEDED(tab.element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&inv))) && inv)
        {
            return SUCCEEDED(inv->Invoke());
        }
        return false;
    }
}

namespace Agentmaster
{
    std::wstring PathLeaf(std::wstring_view path)
    {
        while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        {
            path.remove_suffix(1);
        }
        const auto cut = path.find_last_of(L"\\/");
        return std::wstring{ cut == std::wstring_view::npos ? path : path.substr(cut + 1) };
    }

    std::unordered_set<std::wstring> TokenizeTextLower(std::wstring_view text)
    {
        std::unordered_set<std::wstring> tokens;
        std::wstring cur;
        const auto flush = [&]() {
            if (cur.size() >= 3) // "am" / "gh" / "of" are pure noise
            {
                tokens.insert(cur);
            }
            cur.clear();
        };
        for (const wchar_t raw : text)
        {
            const wchar_t c = static_cast<wchar_t>(::towlower(raw));
            if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))
            {
                cur.push_back(c);
            }
            else
            {
                flush();
            }
        }
        flush();
        return tokens;
    }

    int ScoreTabNameTokens(std::wstring_view tabName, const std::unordered_set<std::wstring>& corpusTokens)
    {
        if (corpusTokens.empty())
        {
            return 0;
        }
        const auto nameTokens = TokenizeTextLower(tabName);
        if (nameTokens.empty())
        {
            return 0;
        }
        for (const auto& t : nameTokens)
        {
            if (corpusTokens.find(t) != corpusTokens.end())
            {
                continue; // exact whole word
            }
            // Tab labels abbreviate ("act" for actions, "perf" for performance): a token also
            // matches as a PREFIX of a corpus word. One direction only — a tab token LONGER than
            // the corpus word ("resumes" vs "resume") stays a miss.
            bool prefixHit = false;
            for (const auto& w : corpusTokens)
            {
                if (w.size() > t.size() && w.compare(0, t.size(), t) == 0)
                {
                    prefixHit = true;
                    break;
                }
            }
            if (!prefixHit)
            {
                return 0; // all-or-nothing — a partial overlap on one generic word is noise
            }
        }
        return 50;
    }

    int PickClaudeTab(const std::vector<std::wstring>& tabNames, const std::vector<std::wstring>& titleHints, std::wstring_view cwdLeaf, const std::unordered_set<std::wstring>& headCorpusTokens, const std::unordered_set<std::wstring>& fullCorpusTokens, int& outScore, bool& outUnique)
    {
        // Token sets per tab, for the generic-prefix disqualifier (see the header): a tab whose
        // token set is a STRICT subset of a sibling's may not win on the token tier.
        std::vector<std::unordered_set<std::wstring>> tokenSets;
        tokenSets.reserve(tabNames.size());
        for (const auto& n : tabNames)
        {
            tokenSets.push_back(TokenizeTextLower(n));
        }
        const auto strictSubsetOfASibling = [&](size_t i) {
            const auto& a = tokenSets[i];
            if (a.empty())
            {
                return false;
            }
            for (size_t j = 0; j < tokenSets.size(); ++j)
            {
                const auto& b = tokenSets[j];
                if (j == i || b.size() <= a.size())
                {
                    continue; // equal sets stay eligible — a genuine tie is the unique-guard's job
                }
                bool subset = true;
                for (const auto& t : a)
                {
                    if (b.find(t) == b.end())
                    {
                        subset = false;
                        break;
                    }
                }
                if (subset)
                {
                    return true;
                }
            }
            return false;
        };

        int best = -1;
        outScore = 0;
        outUnique = true;
        for (size_t i = 0; i < tabNames.size(); ++i)
        {
            int s = 0;
            if (titleHints.empty())
            {
                s = ScoreClaudeTabName(tabNames[i], {}, cwdLeaf); // claude-word/glyph/cwd rules still apply
            }
            for (const auto& t : titleHints)
            {
                const int v = ScoreClaudeTabName(tabNames[i], t, cwdLeaf);
                s = v > s ? v : s;
            }
            if (!strictSubsetOfASibling(i))
            {
                // Head tier (50) over full tier (45): a purpose-named tab must outrank a tab that
                // merely matches a word from some later prompt.
                if (ScoreTabNameTokens(tabNames[i], headCorpusTokens) > 0)
                {
                    s = 50 > s ? 50 : s;
                }
                else if (ScoreTabNameTokens(tabNames[i], fullCorpusTokens) > 0)
                {
                    s = 45 > s ? 45 : s;
                }
            }
            if (s > outScore)
            {
                outScore = s;
                best = static_cast<int>(i);
                outUnique = true;
            }
            else if (s == outScore && s > 0)
            {
                outUnique = false;
            }
        }
        return best;
    }

    int ScoreClaudeTabName(std::wstring_view tabName, std::wstring_view titleHint, std::wstring_view cwdLeaf)
    {
        const auto trim = [](std::wstring_view s) {
            while (!s.empty() && (s.front() == L' ' || s.front() == L'\t'))
            {
                s.remove_prefix(1);
            }
            while (!s.empty() && (s.back() == L' ' || s.back() == L'\t'))
            {
                s.remove_suffix(1);
            }
            return s;
        };
        const std::wstring name = LowerCopy(trim(tabName));
        if (name.empty())
        {
            return 0;
        }
        const std::wstring hint = LowerCopy(trim(titleHint));
        int score = 0;
        if (!hint.empty() && name == hint)
        {
            score = 100; // a tab named exactly the conversation title — the strongest signal
        }
        else if (name.find(L"claude") != std::wstring::npos)
        {
            score = 90; // the word itself — claude's own OSC title or a user rename
        }
        else if (name[0] == L'\x2733' || name[0] == L'\x2736' || name[0] == L'\x273D' || name[0] == L'\x2738')
        {
            score = 80; // claude's OSC status glyph leads the title it sets while working
        }
        if (score < 70)
        {
            // Full containment either way (a tab named with the title plus decoration, or with a
            // truncated title). The CONTAINED side must carry >= 6 chars of signal.
            if ((hint.size() >= 6 && name.find(hint) != std::wstring::npos) ||
                (name.size() >= 6 && !hint.empty() && hint.find(name) != std::wstring::npos))
            {
                score = 70;
            }
        }
        if (score < 60)
        {
            // The hint's HEAD, capped at 16 chars (so a name that is a strict prefix of the hint —
            // or glyph-prefixed — still hits) and at least 8 (no noise).
            const size_t cap = hint.size() < 16 ? hint.size() : 16;
            if (cap >= 8 && name.find(hint.substr(0, cap)) != std::wstring::npos)
            {
                score = 60;
            }
        }
        if (score < 40 && !cwdLeaf.empty())
        {
            // Weakest: the working-dir leaf (shells commonly title tabs by cwd).
            if (name.find(LowerCopy(cwdLeaf)) != std::wstring::npos)
            {
                score = 40;
            }
        }
        return score;
    }

    bool BringClaudeWindowToFront(uint32_t claudePid, uint32_t hostShellPid, std::wstring_view sessionId, std::wstring_view titleHint, std::wstring_view cwd)
    {
        // Everything a tab can be matched against. The caller's display title is one hint; the
        // transcript (one head read) contributes the user-set custom title + the first prompt as
        // further hints, and the conversation corpus (title + human prompts, tokenized) for the
        // hand-renamed-tab overlap tier. Duplicated hints are harmless (scores are max-combined).
        TabPickHints hints;
        hints.cwdLeaf = PathLeaf(cwd);
        if (!titleHint.empty())
        {
            hints.titles.emplace_back(titleHint);
        }
        if (!sessionId.empty())
        {
            // 2 MB head: human prompts are sparse among assistant/tool lines, and a 256 KB read
            // proved too shallow on real transcripts (one giant first turn swallowed it — corpus
            // of 1 prompt). One-shot on a click, off the UI thread — the depth is affordable.
            const auto ti = ReadTranscriptInfo(cwd, sessionId, 2 * 1024 * 1024, 200);
            if (ti.found)
            {
                if (!ti.customTitle.empty())
                {
                    hints.titles.push_back(ti.customTitle);
                }
                if (!ti.title.empty())
                {
                    hints.titles.push_back(ti.title);
                }
                // Head corpus = the conversation's PURPOSE (title + custom title + first prompt);
                // full corpus = every prompt read. PickClaudeTab ranks head hits above full hits.
                std::wstring head{ ti.title };
                head += L'\n';
                head += ti.customTitle;
                if (!ti.userPrompts.empty())
                {
                    head += L'\n';
                    head += ti.userPrompts.front();
                }
                std::wstring full{ head };
                for (size_t i = 1; i < ti.userPrompts.size(); ++i)
                {
                    full += L'\n';
                    full += ti.userPrompts[i];
                }
                hints.headTokens = TokenizeTextLower(head);
                hints.fullTokens = TokenizeTextLower(full);
            }
        }

        // COM for the UIA client. MTA per UIA client guidance — we are on a worker thread, never
        // the UI one. RPC_E_CHANGED_MODE (already initialized STA here) is still usable; it is
        // just not ours to uninitialize.
        const HRESULT coInit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        const bool ownCom = SUCCEEDED(coInit);

        // Inner scope so every ComPtr (uia + cached tab elements) releases BEFORE CoUninitialize.
        const bool ok = [&]() -> bool {
            Microsoft::WRL::ComPtr<IUIAutomation> uia; // created lazily, only if a tab strip needs reading
            const auto ensureUia = [&]() -> IUIAutomation* {
                if (!uia)
                {
                    ::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
                }
                return uia.Get();
            };

            const auto snap = SnapshotProcesses();
            std::unordered_map<uint32_t, const ProcEntry*> byPid;
            byPid.reserve(snap.size());
            for (const auto& e : snap)
            {
                byPid[e.pid] = &e;
            }

            // 1) Ancestor chain, nearest-first: claude -> host shell -> ... -> the terminal/editor
            //    that owns a visible window (WindowsTerminal.exe / ConEmu / Code.exe / ...).
            //    Bounded + cycle-guarded (pids recycle); stops at explorer/system images.
            //    hostShellPid roots the walk when the claude itself already exited (a dead pid is
            //    simply absent from the snapshot — never a stale re-used one followed blindly).
            std::vector<uint32_t> chain;
            {
                uint32_t cur = byPid.count(claudePid) != 0 ? claudePid :
                                                             (byPid.count(hostShellPid) != 0 ? hostShellPid : 0);
                for (int depth = 0; cur != 0 && depth < 16; ++depth)
                {
                    const auto it = byPid.find(cur);
                    if (it == byPid.end() || IsAncestorBoundaryImage(it->second->image))
                    {
                        break;
                    }
                    chain.push_back(cur);
                    const uint32_t parent = it->second->ppid;
                    if (parent == cur)
                    {
                        break;
                    }
                    cur = parent;
                }
            }

            HWND target = nullptr;
            int tabIndex = -1; // the claude's tab in targetTabs, when already resolved
            bool tabUnique = false; // selection requires a UNIQUE best (never flip to a guessed tab)
            std::vector<UiaTab> targetTabs;

            for (const auto pid : chain)
            {
                auto wins = TopLevelWindowsOfPid(pid);
                if (wins.empty())
                {
                    // Classic console: the visible console window belongs to a conhost.exe CHILD
                    // of the shell. (A headless ConPTY host — OpenConsole, or a conhost that
                    // delegated to the default terminal — owns no visible window and falls through.)
                    for (const auto child : ChildrenOf(snap, pid))
                    {
                        const auto cit = byPid.find(child);
                        if (cit != byPid.end() && (ImageNameEq(cit->second->image, L"conhost.exe") || ImageNameEq(cit->second->image, L"openconsole.exe")))
                        {
                            wins = TopLevelWindowsOfPid(child);
                            if (!wins.empty())
                            {
                                break;
                            }
                        }
                    }
                }
                if (wins.empty())
                {
                    continue;
                }
                target = wins.front(); // topmost in z-order
                if (wins.size() > 1)
                {
                    // One process, several windows (a real WT hosts N windows in ONE
                    // WindowsTerminal.exe): pick the window whose tab strip actually matches the
                    // claude; the topmost stays the fallback when nothing scores.
                    int bestScore = 0;
                    for (const auto h : wins)
                    {
                        if (!IsTerminalIslandWindow(h))
                        {
                            continue;
                        }
                        auto tabs = UiaTabsOf(ensureUia(), h);
                        int score = 0;
                        bool unique = true;
                        const int idx = BestClaudeTabIn(tabs, hints, score, unique);
                        if (idx >= 0 && score > bestScore)
                        {
                            bestScore = score;
                            target = h;
                            tabIndex = idx;
                            tabUnique = unique;
                            targetTabs = std::move(tabs);
                        }
                    }
                }
                break; // nearest ancestor with a visible window wins
            }

            // 2) The process tree owns no visible window: a Win11 DEFAULT-TERMINAL handoff console
            //    (the chain's conhost delegated the session; the visible window is a Windows
            //    Terminal in an UNRELATED process). Best-effort: scan foreign WT-class windows for
            //    a CONFIDENT tab match — claude word / glyph / title summary, never the cwd leaf
            //    alone (too generic to gamble a foreground on).
            if (target == nullptr)
            {
                int bestScore = 59;
                for (const auto h : ForeignTerminalIslandWindows())
                {
                    auto tabs = UiaTabsOf(ensureUia(), h);
                    int score = 0;
                    bool unique = true;
                    const int idx = BestClaudeTabIn(tabs, hints, score, unique);
                    if (idx >= 0 && score > bestScore)
                    {
                        bestScore = score;
                        target = h;
                        tabIndex = idx;
                        tabUnique = unique;
                        targetTabs = std::move(tabs);
                    }
                }
            }

            if (target == nullptr)
            {
                return false;
            }

            RestoreAndForeground(target);

            // 3) A WT-class host also gets the claude's TAB selected (the window may host many).
            //    Probe now if the single-window path skipped it; select only on a real signal and
            //    only when there IS another tab to switch from — never guess.
            if (IsTerminalIslandWindow(target))
            {
                if (targetTabs.empty())
                {
                    targetTabs = UiaTabsOf(ensureUia(), target);
                    int score = 0;
                    tabIndex = BestClaudeTabIn(targetTabs, hints, score, tabUnique);
                    if (score <= 0)
                    {
                        tabIndex = -1;
                    }
                }
                if (tabIndex >= 0 && tabUnique && targetTabs.size() > 1)
                {
                    UiaSelectTab(targetTabs[static_cast<size_t>(tabIndex)]);
                }
            }
            return true;
        }();

        if (ownCom)
        {
            ::CoUninitialize();
        }
        return ok;
    }
}
