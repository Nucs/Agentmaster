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

    int64_t SubagentActivityUnixMs(std::wstring_view transcriptPath)
    {
        // The side files live in a sibling directory named after the session id: strip the
        // ".jsonl" off "<...>/<id>.jsonl" to get "<...>/<id>", then scan its "subagents" and
        // "tool-results" children for the newest FILE write time. (We enumerate files, not the
        // dir, because Windows does NOT bump a directory's mtime when a file inside it is appended
        // to — only on add/remove — and Claude APPENDS to agent-<id>.jsonl as a subagent works.)
        if (transcriptPath.size() < 7) // shorter than "x.jsonl"
        {
            return 0;
        }
        std::wstring base{ transcriptPath };
        constexpr std::wstring_view kExt = L".jsonl";
        if (base.size() >= kExt.size())
        {
            const size_t off = base.size() - kExt.size();
            bool isJsonl = true;
            for (size_t i = 0; i < kExt.size(); ++i)
            {
                if (towlower(base[off + i]) != kExt[i]) // kExt is lowercase; the path's ext is too, but fold to be safe
                {
                    isJsonl = false;
                    break;
                }
            }
            if (isJsonl)
            {
                base.resize(off);
            }
        }
        int64_t newest = 0;
        for (const wchar_t* sub : { L"\\subagents\\*", L"\\tool-results\\*" })
        {
            const std::wstring pattern = base + sub;
            WIN32_FIND_DATAW fd{};
            const HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE)
            {
                continue; // no such side dir (the common case) — instant miss
            }
            do
            {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    continue; // ".", "..", or a nested-subagent dir (presence-"busy" covers those)
                }
                const int64_t m = FileTimeToUnixMs(fd.ftLastWriteTime);
                if (m > newest)
                {
                    newest = m;
                }
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
        return newest;
    }

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
        // Agentmaster (subagent activity): while a Task/Agent subagent runs, the side files
        // (<id>/subagents/*.jsonl, <id>/tool-results/*) grow but THIS transcript stays quiescent —
        // so its mtime alone reads "stale". Fold the newest side-file write into "last activity" so
        // the per-session timing adornment (and the scanner's "transcript advanced" Enter-retry
        // check) reflect work happening inside a subagent. createdUnixMs (the conversation start) is
        // left as the parent's — a subagent never predates its parent.
        const int64_t subMs = SubagentActivityUnixMs(path);
        if (subMs > lastActivityUnixMs)
        {
            lastActivityUnixMs = subMs;
        }
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

    // Agentmaster: the CURRENT git branch of a working dir (see ProcessInspect.h). Out-of-band,
    // filesystem only — reuses the anon-namespace ReadFileHead / Utf8ToWide primitives above.
    std::wstring ReadGitBranchForDir(const std::wstring& dir)
    {
        if (dir.empty())
        {
            return {};
        }
        std::wstring cur = dir;
        while (cur.size() > 1 && (cur.back() == L'\\' || cur.back() == L'/'))
        {
            cur.pop_back(); // strip trailing separators
        }

        // Walk UP to the nearest .git: a directory is a normal repo root; a FILE is a worktree/
        // submodule whose contents are "gitdir: <path>" (the real git dir), which we resolve.
        std::wstring gitDir;
        for (;;)
        {
            const std::wstring dot = cur + L"\\.git";
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (::GetFileAttributesExW(dot.c_str(), GetFileExInfoStandard, &fad))
            {
                if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    gitDir = dot; // normal repo root
                    break;
                }
                std::wstring text = Utf8ToWide(ReadFileHead(dot, 4096)); // ".git" FILE: "gitdir: <path>"
                const std::wstring key = L"gitdir:";
                if (const auto k = text.find(key); k != std::wstring::npos)
                {
                    std::wstring g = text.substr(k + key.size());
                    const auto b = g.find_first_not_of(L" \t\r\n");
                    const auto e = g.find_last_not_of(L" \t\r\n");
                    if (b != std::wstring::npos)
                    {
                        g = g.substr(b, e - b + 1);
                        for (auto& ch : g)
                        {
                            if (ch == L'/')
                            {
                                ch = L'\\';
                            }
                        }
                        const bool absolute = (g.size() >= 2 && g[1] == L':') || (g.size() >= 2 && g[0] == L'\\' && g[1] == L'\\');
                        gitDir = absolute ? g : (cur + L"\\" + g); // relative gitdir is relative to .git's dir
                    }
                }
                break;
            }
            const auto slash = cur.find_last_of(L"\\/");
            if (slash == std::wstring::npos || slash < 2)
            {
                break; // reached the drive root (e.g. "C:") -> not under a repo
            }
            cur = cur.substr(0, slash);
        }
        if (gitDir.empty())
        {
            return {};
        }

        std::wstring head = Utf8ToWide(ReadFileHead(gitDir + L"\\HEAD", 4096));
        while (!head.empty() && (head.back() == L'\n' || head.back() == L'\r' || head.back() == L' ' || head.back() == L'\t'))
        {
            head.pop_back();
        }
        const std::wstring branchPrefix = L"ref: refs/heads/";
        if (head.rfind(branchPrefix, 0) == 0)
        {
            return head.substr(branchPrefix.size()); // keep slashes: e.g. feature/issue123
        }
        if (head.rfind(L"ref: ", 0) == 0)
        {
            const auto s = head.find_last_of(L'/'); // a non-branch ref (tag / note) -> its leaf
            return (s != std::wstring::npos) ? head.substr(s + 1) : head.substr(5);
        }
        // Detached HEAD: a raw commit SHA. Show a short SHA, not the bare word "HEAD".
        return head.size() >= 7 ? head.substr(0, 7) : head;
    }

    // ===== Codex (OpenAI Codex CLI) — observe-only enrichment (OBSERVER.md §19-Q3, Phase C1) ====
    // File-local helpers (internal linkage). They reuse the anon-namespace primitives above
    // (GlobTranscripts / ReadFileHead / Utf8ToWide / FileTimeToUnixMs / FirstLineTrim) and the
    // public TU functions (ExtractCwdFromTranscriptHead / PickNewestTranscript / EnvLookup /
    // ExtractCmdlineArg / TokenizeCmdline).

    // Our OWN process environment variable (CODEX_HOME / USERPROFILE). Empty if unset.
    static std::wstring GetOwnEnvW(const wchar_t* name)
    {
        const DWORD need = ::GetEnvironmentVariableW(name, nullptr, 0); // includes NUL; 0 == absent
        if (need == 0)
        {
            return {};
        }
        std::wstring v(need, L'\0');
        const DWORD got = ::GetEnvironmentVariableW(name, v.data(), need); // got == chars w/o NUL
        v.resize(got);
        return v;
    }

    static std::wstring TrimTrailingSlashes(std::wstring s)
    {
        while (!s.empty() && (s.back() == L'\\' || s.back() == L'/'))
        {
            s.pop_back();
        }
        return s;
    }

    // A 36-char hyphenated UUID (8-4-4-4-12). Codex uses time-ordered UUIDv7, still this shape.
    static bool LooksLikeGuidStr(std::wstring_view s)
    {
        if (s.size() != 36)
        {
            return false;
        }
        for (size_t i = 0; i < 36; ++i)
        {
            const wchar_t c = s[i];
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (c != L'-')
                {
                    return false;
                }
            }
            else
            {
                const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
                if (!hex)
                {
                    return false;
                }
            }
        }
        return true;
    }

    static int64_t NowUnixMsSrc() // current unix ms (the startUnixMs == 0 day-dir anchor)
    {
        FILETIME ft{};
        ::GetSystemTimeAsFileTime(&ft);
        return FileTimeToUnixMs(ft);
    }

    static std::wstring PadNum(unsigned v, size_t width)
    {
        std::wstring s = std::to_wstring(v);
        while (s.size() < width)
        {
            s.insert(s.begin(), L'0');
        }
        return s;
    }

    // "YYYY\\MM\\DD" in LOCAL time for a unix-ms instant ("" on failure). Codex shards rollouts by
    // the LOCAL date (the filename uses local wall-clock; session_meta.timestamp is UTC), so the
    // day dir is derived in local time.
    static std::wstring CodexDayDirLocal(int64_t unixMs)
    {
        if (unixMs <= 0)
        {
            return {};
        }
        ULARGE_INTEGER u;
        u.QuadPart = static_cast<uint64_t>(unixMs) * 10000ull + 116444736000000000ull; // unix ms -> FILETIME (100ns since 1601)
        FILETIME ut;
        ut.dwLowDateTime = u.LowPart;
        ut.dwHighDateTime = u.HighPart;
        FILETIME lt{};
        if (!::FileTimeToLocalFileTime(&ut, &lt))
        {
            return {};
        }
        SYSTEMTIME st{};
        if (!::FileTimeToSystemTime(&lt, &st))
        {
            return {};
        }
        return PadNum(st.wYear, 4) + L"\\" + PadNum(st.wMonth, 2) + L"\\" + PadNum(st.wDay, 2);
    }

    // Filesystem-aware cwd compare (Windows): normalize '/'->'\\', trim trailing separators, then
    // ordinal case-insensitive. The codex PEB cwd ("K:\\source\\proxmox") vs session_meta.cwd.
    static bool CodexPathEq(std::wstring_view a, std::wstring_view b)
    {
        const auto norm = [](std::wstring_view s) {
            std::wstring r;
            r.reserve(s.size());
            for (const wchar_t c : s)
            {
                r.push_back(c == L'/' ? L'\\' : c);
            }
            while (!r.empty() && r.back() == L'\\')
            {
                r.pop_back();
            }
            return r;
        };
        const std::wstring na = norm(a), nb = norm(b);
        return ::CompareStringOrdinal(na.c_str(), static_cast<int>(na.size()), nb.c_str(), static_cast<int>(nb.size()), TRUE) == CSTR_EQUAL;
    }

    // The immediate subdirectory NAMES of `dir` (no "." / ".."). Used to walk the year/month/day
    // shards when resolving a rollout by a known uuid.
    static std::vector<std::wstring> ListSubdirs(const std::wstring& dir)
    {
        std::vector<std::wstring> out;
        WIN32_FIND_DATAW fd{};
        const HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return out;
        }
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                continue;
            }
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }
            out.push_back(name);
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
        return out;
    }

    std::wstring CodexDefaultHome()
    {
        std::wstring base = GetOwnEnvW(L"CODEX_HOME");
        if (base.empty())
        {
            const std::wstring home = GetOwnEnvW(L"USERPROFILE");
            if (home.empty())
            {
                return {};
            }
            base = home + L"\\.codex";
        }
        return TrimTrailingSlashes(std::move(base));
    }

    void ParseCodexFacts(std::wstring_view commandline, const std::unordered_map<std::wstring, std::wstring>& env, CodexProcessFacts& facts)
    {
        facts.wtSession = EnvLookup(env, L"WT_SESSION");
        facts.amSession = EnvLookup(env, L"AM_SESSION");
        facts.codexHome = EnvLookup(env, L"CODEX_HOME");

        // model: --model X / -m X (often absent — config.toml carries it; read from the rollout then)
        if (const auto v = ExtractCmdlineArg(commandline, L"--model"))
        {
            facts.model = *v;
        }
        if (facts.model.empty())
        {
            if (const auto v = ExtractCmdlineArg(commandline, L"-m"))
            {
                facts.model = *v;
            }
        }
        // sandbox: --sandbox X / -s X
        if (const auto v = ExtractCmdlineArg(commandline, L"--sandbox"))
        {
            facts.sandbox = *v;
        }
        if (facts.sandbox.empty())
        {
            if (const auto v = ExtractCmdlineArg(commandline, L"-s"))
            {
                facts.sandbox = *v;
            }
        }
        // approval: --ask-for-approval X / -a X
        if (const auto v = ExtractCmdlineArg(commandline, L"--ask-for-approval"))
        {
            facts.approvalMode = *v;
        }
        if (facts.approvalMode.empty())
        {
            if (const auto v = ExtractCmdlineArg(commandline, L"-a"))
            {
                facts.approvalMode = *v;
            }
        }
        // explicit `codex resume <guid>` (authoritative id — beats cwd->rollout discovery): the
        // first non-flag token after a `resume` token, when it is a guid.
        const auto toks = TokenizeCmdline(commandline);
        for (size_t i = 0; i + 1 < toks.size(); ++i)
        {
            if (toks[i] == L"resume")
            {
                for (size_t j = i + 1; j < toks.size(); ++j)
                {
                    if (!toks[j].empty() && toks[j][0] == L'-')
                    {
                        continue; // skip flags (--last / --all / -C ...)
                    }
                    if (LooksLikeGuidStr(toks[j]))
                    {
                        facts.resumeTarget = toks[j];
                    }
                    break;
                }
                break;
            }
        }
    }

    CodexProcessFacts ReadCodexFacts(uint32_t pid)
    {
        CodexProcessFacts f;
        f.pid = pid;
        f.startUnixMs = ProcessStartUnixMs(pid);
        f.cwd = ReadProcessCwd(pid);
        f.commandline = ReadProcessCommandLine(pid);
        f.subsystem = ReadProcessImageSubsystem(pid);
        const auto env = ReadProcessEnv(pid);
        ParseCodexFacts(f.commandline, env, f);
        f.alive = true;
        return f;
    }

    std::wstring CodexRolloutUuid(std::wstring_view rolloutStem)
    {
        if (rolloutStem.size() < 36)
        {
            return {};
        }
        const std::wstring_view tail = rolloutStem.substr(rolloutStem.size() - 36);
        return LooksLikeGuidStr(tail) ? std::wstring{ tail } : std::wstring{};
    }

    CodexSession ResolveCodexSessionIn(std::wstring_view codexHome, std::wstring_view cwd, int64_t startUnixMs)
    {
        CodexSession out;
        if (codexHome.empty() || cwd.empty())
        {
            return out;
        }
        const std::wstring sessions = std::wstring{ codexHome } + L"\\sessions";
        const int64_t anchor = startUnixMs > 0 ? startUnixMs : NowUnixMsSrc();

        // Candidate day dirs: the start's local day ± 1 (midnight boundary / small clock skew).
        std::vector<std::wstring> days;
        for (const int delta : { -1, 0, 1 })
        {
            const std::wstring d = CodexDayDirLocal(anchor + static_cast<int64_t>(delta) * 86400000ll);
            if (!d.empty() && std::find(days.begin(), days.end(), d) == days.end())
            {
                days.push_back(d);
            }
        }

        // Gather rollout candidates (uuid + ctime/mtime + path) across those day dirs. The uuid is in
        // the filename, so no read is needed for the id — only the cwd confirmation below reads.
        struct Cand
        {
            std::wstring uuid;
            std::wstring path;
            int64_t ctime{};
            int64_t mtime{};
        };
        std::vector<Cand> cands;
        for (const auto& day : days)
        {
            const std::wstring dir = sessions + L"\\" + day;
            for (const auto& tc : GlobTranscripts(dir))
            {
                const std::wstring uuid = CodexRolloutUuid(tc.stem);
                if (uuid.empty())
                {
                    continue;
                }
                cands.push_back({ uuid, dir + L"\\" + tc.stem + L".jsonl", tc.ctimeMs, tc.mtimeMs });
            }
        }
        if (cands.empty())
        {
            return out;
        }

        // Newest-mtime first, then bound the cwd-confirm head reads (the date dir mixes all cwds, so
        // each candidate must be confirmed against the target cwd before the start-time pick).
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.mtime > b.mtime; });
        constexpr size_t kMaxConfirm = 64;
        if (cands.size() > kMaxConfirm)
        {
            cands.resize(kMaxConfirm);
        }

        std::vector<TranscriptCandidate> cwdCands;
        std::unordered_map<std::wstring, Cand> byUuid;
        for (const auto& c : cands)
        {
            const std::string head = ReadFileHead(c.path, 4096); // session_meta is line 1; cwd precedes the bulky base_instructions
            if (head.empty())
            {
                continue;
            }
            const std::wstring rcwd = ExtractCwdFromTranscriptHead(Utf8ToWide(head));
            if (rcwd.empty() || !CodexPathEq(rcwd, cwd))
            {
                continue;
            }
            TranscriptCandidate t;
            t.stem = c.uuid;
            t.ctimeMs = c.ctime;
            t.mtimeMs = c.mtime;
            cwdCands.push_back(t);
            byUuid[c.uuid] = c;
        }
        if (cwdCands.empty())
        {
            return out;
        }

        // ctime ≈ start IDENTITY for a FRESH session (rejects rollouts created before this process);
        // newest-mtime-in-cwd FALLBACK for a RESUMED one (its rollout predates the process, so the
        // identity path rejects it). Mirrors PickNewestTranscript's two modes.
        std::wstring pick = PickNewestTranscript(cwdCands, startUnixMs);
        if (pick.empty())
        {
            pick = PickNewestTranscript(cwdCands, 0);
        }
        if (pick.empty())
        {
            return out;
        }
        const auto it = byUuid.find(pick);
        if (it == byUuid.end())
        {
            return out;
        }
        out.sessionId = it->second.uuid;
        out.rolloutPath = it->second.path;
        out.createdUnixMs = it->second.ctime;
        out.lastActivityUnixMs = it->second.mtime;
        return out;
    }

    CodexSession ResolveCodexSession(std::wstring_view cwd, int64_t startUnixMs)
    {
        return ResolveCodexSessionIn(CodexDefaultHome(), cwd, startUnixMs);
    }

    std::wstring ResolveCodexRolloutPathIn(std::wstring_view codexHome, std::wstring_view sessionId)
    {
        if (codexHome.empty() || sessionId.empty() || !LooksLikeGuidStr(sessionId))
        {
            return {};
        }
        const std::wstring sessions = std::wstring{ codexHome } + L"\\sessions";
        const std::wstring needle = L"*" + std::wstring{ sessionId } + L".jsonl";
        // Walk year\month\day. The uuid is unique, so the first match is THE rollout. (The explicit
        // `codex resume <guid>` path; the cwd-discovery path above carries the path directly.)
        for (const auto& year : ListSubdirs(sessions))
        {
            const std::wstring ydir = sessions + L"\\" + year;
            for (const auto& month : ListSubdirs(ydir))
            {
                const std::wstring mdir = ydir + L"\\" + month;
                for (const auto& day : ListSubdirs(mdir))
                {
                    const std::wstring ddir = mdir + L"\\" + day;
                    WIN32_FIND_DATAW fd{};
                    const HANDLE h = ::FindFirstFileW((ddir + L"\\" + needle).c_str(), &fd);
                    if (h != INVALID_HANDLE_VALUE)
                    {
                        std::wstring found;
                        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                        {
                            found = ddir + L"\\" + fd.cFileName;
                        }
                        ::FindClose(h);
                        if (!found.empty())
                        {
                            return found;
                        }
                    }
                }
            }
        }
        return {};
    }

    void ParseCodexRolloutText(std::wstring_view text, bool truncated, size_t maxPrompts, CodexRolloutInfo& out)
    {
        std::wstring firstPrompt;
        bool haveTurnCtx = false;
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i)
        {
            if (i < text.size() && text[i] != L'\n')
            {
                continue;
            }
            if (i == text.size() && truncated)
            {
                break; // a head read may end mid-line — leave the partial segment for a fuller read
            }
            std::wstring_view line = text.substr(start, i - start);
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
            const auto* pl = obj.Find(L"payload");
            if (!pl || pl->type != json::Value::Type::Obj)
            {
                continue;
            }
            if (lineType == L"session_meta")
            {
                if (out.cwd.empty())
                {
                    out.cwd = pl->StrAt(L"cwd");
                }
                if (out.gitBranch.empty())
                {
                    if (const auto* git = pl->Find(L"git"); git && git->type == json::Value::Type::Obj)
                    {
                        out.gitBranch = git->StrAt(L"branch"); // best-effort (codex may record git info)
                    }
                }
                continue;
            }
            if (lineType == L"turn_context")
            {
                if (!haveTurnCtx) // the FIRST turn_context carries the session's model/effort/sandbox/approval
                {
                    haveTurnCtx = true;
                    out.model = pl->StrAt(L"model");
                    out.approvalMode = pl->StrAt(L"approval_policy");
                    if (const auto* sp = pl->Find(L"sandbox_policy"); sp && sp->type == json::Value::Type::Obj)
                    {
                        out.sandbox = sp->StrAt(L"type");
                    }
                    else
                    {
                        out.sandbox = pl->StrAt(L"sandbox_mode"); // flat fallback
                    }
                    std::wstring eff;
                    if (const auto* cm = pl->Find(L"collaboration_mode"); cm && cm->type == json::Value::Type::Obj)
                    {
                        if (const auto* se = cm->Find(L"settings"); se && se->type == json::Value::Type::Obj)
                        {
                            eff = se->StrAt(L"reasoning_effort");
                        }
                    }
                    if (eff.empty())
                    {
                        eff = pl->StrAt(L"reasoning_effort");
                    }
                    if (eff.empty())
                    {
                        eff = pl->StrAt(L"model_reasoning_effort");
                    }
                    out.effort = eff;
                }
                continue;
            }
            if (lineType == L"event_msg" && pl->StrAt(L"type") == L"user_message")
            {
                // event_msg/user_message is the CLEAN human prompt (the AGENTS.md / context blobs are
                // response_item user messages, skipped). Noise-filter via the shared rule.
                std::wstring msg = pl->StrAt(L"message");
                if (msg.empty() || IsNoiseUserPrompt(msg))
                {
                    continue;
                }
                if (firstPrompt.empty())
                {
                    firstPrompt = msg;
                }
                if (out.userPrompts.size() < maxPrompts)
                {
                    out.userPrompts.push_back(std::move(msg));
                }
            }
        }
        out.title = FirstLineTrim(firstPrompt);
    }

    CodexRolloutInfo ReadCodexRolloutInfo(std::wstring_view rolloutPath, size_t maxBytes, size_t maxPrompts)
    {
        CodexRolloutInfo info;
        if (rolloutPath.empty())
        {
            return info;
        }
        const std::wstring path{ rolloutPath };
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
        ParseCodexRolloutText(Utf8ToWide(bytes), maxBytes != 0, maxPrompts, info);
        return info;
    }

    std::wstring ReadConversationText(std::wstring_view transcriptPath, bool codex, size_t maxBytes)
    {
        if (transcriptPath.empty())
        {
            return {};
        }
        const std::wstring path{ transcriptPath };
        const std::string bytes = ReadFileHead(path, maxBytes);
        if (bytes.empty())
        {
            return {};
        }
        const bool truncated = (maxBytes != 0); // a head read may end mid-line -> skip the last segment
        const std::wstring wide = Utf8ToWide(bytes);

        std::wstring out;
        const auto emit = [&out](const wchar_t* who, std::wstring t) {
            // Trim surrounding whitespace/newlines so blocks pack cleanly.
            while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' ' || t.back() == L'\t'))
            {
                t.pop_back();
            }
            size_t b = 0;
            while (b < t.size() && (t[b] == L'\n' || t[b] == L'\r' || t[b] == L' ' || t[b] == L'\t'))
            {
                ++b;
            }
            if (b)
            {
                t.erase(0, b);
            }
            if (t.empty())
            {
                return;
            }
            if (!out.empty())
            {
                out += L"\n\n";
            }
            out += who;
            out += L":\n";
            out += t;
        };

        size_t start = 0;
        for (size_t i = 0; i <= wide.size(); ++i)
        {
            if (i < wide.size() && wide[i] != L'\n')
            {
                continue;
            }
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

            if (codex)
            {
                // Codex rollout: the CLEAN visible messages are event_msg/{user_message,agent_message};
                // reasoning, tool calls, response_item context blobs, etc. are all skipped.
                if (obj.StrAt(L"type") != L"event_msg")
                {
                    continue;
                }
                const auto* pl = obj.Find(L"payload");
                if (!pl || pl->type != json::Value::Type::Obj)
                {
                    continue;
                }
                const std::wstring pt = pl->StrAt(L"type");
                if (pt == L"user_message")
                {
                    const std::wstring msg = pl->StrAt(L"message");
                    if (!msg.empty() && !IsNoiseUserPrompt(msg))
                    {
                        emit(L"User", msg);
                    }
                }
                else if (pt == L"agent_message")
                {
                    emit(L"Assistant", pl->StrAt(L"message"));
                }
                continue;
            }

            // Claude transcript: type user/assistant only; drop meta/compact/sidechain turns.
            const std::wstring lineType = obj.StrAt(L"type");
            const bool isUser = (lineType == L"user");
            const bool isAssistant = (lineType == L"assistant");
            if ((!isUser && !isAssistant) || obj.BoolAt(L"isMeta") || obj.BoolAt(L"isCompactSummary") || obj.BoolAt(L"isSidechain"))
            {
                continue;
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
            std::wstring text;
            bool hasToolResult = false;
            if (content->type == json::Value::Type::Str)
            {
                text = content->str;
            }
            else if (content->type == json::Value::Type::Arr)
            {
                for (const auto& blk : content->arr)
                {
                    if (blk.type != json::Value::Type::Obj)
                    {
                        continue;
                    }
                    const std::wstring bt = blk.StrAt(L"type");
                    if (bt == L"tool_result")
                    {
                        hasToolResult = true; // a tool-result turn (user role), not a human message
                        break;
                    }
                    if (bt == L"text")
                    {
                        if (!text.empty())
                        {
                            text += L"\n";
                        }
                        text += blk.StrAt(L"text");
                    }
                    // tool_use / thinking / image / etc. -> skipped (only visible TEXT is kept)
                }
            }
            if (isUser)
            {
                if (hasToolResult || text.empty() || IsNoiseUserPrompt(text))
                {
                    continue; // tool-result turn, empty, or a control marker — not a human message
                }
                emit(L"User", text);
            }
            else // assistant
            {
                if (text.empty())
                {
                    continue; // a pure tool_use / thinking turn — no visible assistant text
                }
                emit(L"Assistant", text);
            }
        }
        return out;
    }

    // --- session-end.js port helpers (TAB_OVERLAY.md summary panel) ---
    static std::wstring SeBasename(const std::wstring& fp)
    {
        const auto pos = fp.find_last_of(L"/\\");
        return pos == std::wstring::npos ? fp : fp.substr(pos + 1);
    }
    static bool SeIsPlansPath(const std::wstring& fp)
    {
        std::wstring n = fp; // normalize separators + case, then look for a /plans/ segment (JS: /[/\\]plans[/\\]/i)
        for (auto& c : n)
        {
            if (c == L'\\')
                c = L'/';
            else if (c >= L'A' && c <= L'Z')
                c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        return n.find(L"/plans/") != std::wstring::npos;
    }
    static bool SeIsCommandNoise(const std::wstring& c)
    {
        const auto has = [&](const wchar_t* s) { return c.find(s) != std::wstring::npos; };
        const auto starts = [&](const wchar_t* p) { return c.rfind(p, 0) == 0; };
        return has(L"<command-message>") || has(L"<command-name>") || has(L"<command-args>") || has(L"<local-command-") ||
               has(L"<bash-input>") || has(L"<bash-stdout>") || has(L"<bash-stderr>") ||
               // Agentmaster (summary refinement): system-injected execution noise that arrives as a
               // "user" message but is NOT a human prompt. (a) background task-finished notices
               // <task-notification> (carry <task-id>/<tool-use-id>/<output-file>); (b) finished
               // background-command results (<output-file>, or a <status>...</status> + <summary>...
               // block); (c) subagent token/timing telemetry footers (<usage>/<subagent_tokens>).
               // Teammate messages (<teammate-message>) are intentionally NOT filtered.
               has(L"<task-notification>") ||
               has(L"<output-file>") || (has(L"<status>") && has(L"<summary>")) ||
               has(L"<usage>") || has(L"<subagent_tokens>") ||
               // Agentmaster (summary refinement 2 — chosen from a full-corpus jq scan): the bash
               // analog of task-notification (a finished BACKGROUND bash command + its shell id /
               // persisted output — mostly already inside a <bash-stdout> block, but explicit here
               // closes the gap for an output-less command), the system echo of input forwarded to a
               // background task, and injected system reminders.
               has(L"<bash-notification>") || has(L"<shell-id>") || has(L"<persisted-output>") ||
               has(L"<background-task-input>") || has(L"<system-reminder>") ||
               starts(L"Caveat:") || starts(L"Overview:") || starts(L"\n") || starts(L"[Request interrupted");
    }
    static bool SeAllWhitespace(const std::wstring& s)
    {
        for (const wchar_t c : s)
        {
            if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n')
            {
                return false;
            }
        }
        return true;
    }
    static bool SeParseIso(std::wstring_view iso, FILETIME& outUtc)
    {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        if (::swscanf_s(std::wstring{ iso }.c_str(), L"%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6)
        {
            return false;
        }
        SYSTEMTIME st{};
        st.wYear = static_cast<WORD>(y);
        st.wMonth = static_cast<WORD>(mo);
        st.wDay = static_cast<WORD>(d);
        st.wHour = static_cast<WORD>(h);
        st.wMinute = static_cast<WORD>(mi);
        st.wSecond = static_cast<WORD>(s);
        return ::SystemTimeToFileTime(&st, &outUtc) != FALSE;
    }

    SessionSummary AnalyzeSessionTranscript(std::wstring_view transcriptPath, size_t maxBytes)
    {
        SessionSummary out;
        if (transcriptPath.empty())
        {
            return out;
        }
        const std::string bytes = ReadFileHead(std::wstring{ transcriptPath }, maxBytes);
        if (bytes.empty())
        {
            return out;
        }
        out.found = true;
        const bool truncated = (maxBytes != 0);
        const std::wstring wide = Utf8ToWide(bytes);

        std::unordered_set<std::wstring> seenMsgs, seenRead, seenEdit, seenCreated;
        // A Write's created-vs-overwrote verdict is in its tool_result ("File created successfully at:"
        // for a NEW file, "...has been updated successfully" otherwise), which arrives in a later user
        // message — so defer Write classification: map the Write's tool_use id -> its file_path here,
        // resolve it when the matching tool_result is seen, and fall back to "edited" for any with no
        // result by end-of-transcript (a truncated tail).
        std::unordered_map<std::wstring, std::wstring> pendingWrites;
        // Agentmaster: tool_use ids of interactive (AskUserQuestion) blocks seen so far — a later user
        // tool_result answering one is a REAL user interaction (the user chose an answer), so it must
        // advance "last user msg" (out.lastUserTs) like a typed prompt. The interactive-tool set mirrors
        // SessionScanner.h's IsInteractiveTool (today: AskUserQuestion).
        std::unordered_set<std::wstring> interactiveAskIds;
        json::Value lastTodos;
        bool haveTodos = false;
        bool isFirstUser = true;

        size_t start = 0;
        for (size_t i = 0; i <= wide.size(); ++i)
        {
            if (i < wide.size() && wide[i] != L'\n')
            {
                continue;
            }
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

            const std::wstring ts = obj.StrAt(L"timestamp");
            if (!ts.empty())
            {
                if (out.firstTs.empty())
                {
                    out.firstTs = ts;
                }
                out.lastTs = ts;
            }
            if (out.branch.empty())
            {
                const std::wstring gb = obj.StrAt(L"gitBranch");
                if (!gb.empty())
                {
                    out.branch = gb;
                }
            }

            const std::wstring type = obj.StrAt(L"type");
            const bool externalUser = (type == L"user" && obj.StrAt(L"userType") == L"external");

            if (externalUser && isFirstUser)
            {
                isFirstUser = false;
                if (const auto* pc = obj.Find(L"planContent");
                    pc && pc->type != json::Value::Type::Null &&
                    !(pc->type == json::Value::Type::Bool && !pc->boolean) &&
                    !(pc->type == json::Value::Type::Str && pc->str.empty()))
                {
                    out.hasPlanContent = true;
                }
                // Parent: "read the full transcript at: <...>.jsonl" in the (string) content.
                const auto* msg = obj.Find(L"message");
                std::wstring contentStr;
                if (msg && msg->type == json::Value::Type::Obj)
                {
                    if (const auto* c = msg->Find(L"content"); c && c->type == json::Value::Type::Str)
                    {
                        contentStr = c->str;
                    }
                }
                if (!contentStr.empty())
                {
                    std::wstring lc = contentStr;
                    for (auto& ch : lc)
                    {
                        if (ch >= L'A' && ch <= L'Z')
                        {
                            ch = static_cast<wchar_t>(ch - L'A' + L'a');
                        }
                    }
                    const auto mk = lc.find(L"read the full transcript at:");
                    if (mk != std::wstring::npos)
                    {
                        size_t p = mk + 28; // == len("read the full transcript at:")
                        while (p < contentStr.size() && (contentStr[p] == L' ' || contentStr[p] == L'\t'))
                        {
                            ++p;
                        }
                        size_t e = p;
                        while (e < contentStr.size() && contentStr[e] != L' ' && contentStr[e] != L'\t' && contentStr[e] != L'\r' && contentStr[e] != L'\n')
                        {
                            ++e;
                        }
                        const std::wstring tok = contentStr.substr(p, e - p);
                        if (tok.size() >= 6)
                        {
                            std::wstring tl = tok;
                            for (auto& ch : tl)
                            {
                                if (ch >= L'A' && ch <= L'Z')
                                {
                                    ch = static_cast<wchar_t>(ch - L'A' + L'a');
                                }
                            }
                            if (tl.rfind(L".jsonl") == tl.size() - 6)
                            {
                                std::wstring stem = SeBasename(tok);
                                stem.resize(stem.size() - 6); // drop ".jsonl"
                                out.parentSessionId = stem;
                            }
                        }
                    }
                }
            }

            if (externalUser)
            {
                const auto* msg = obj.Find(L"message");
                if (msg && msg->type == json::Value::Type::Obj)
                {
                    const auto* c = msg->Find(L"content");
                    std::wstring content;
                    if (c)
                    {
                        if (c->type == json::Value::Type::Str)
                        {
                            content = c->str;
                        }
                        else if (c->type == json::Value::Type::Arr)
                        {
                            for (const auto& blk : c->arr)
                            {
                                if (blk.type == json::Value::Type::Obj && blk.StrAt(L"type") == L"text")
                                {
                                    content = blk.StrAt(L"text");
                                    break;
                                }
                            }
                        }
                    }
                    if (!content.empty() && !SeAllWhitespace(content) && !SeIsCommandNoise(content))
                    {
                        // Track the LAST real user prompt's time (for the "last user msg" ago), even if
                        // the text dedups against an earlier identical prompt — recency is what matters.
                        out.lastUserTs = ts;
                        if (seenMsgs.insert(content).second)
                        {
                            out.userMsgs.push_back(content);
                        }
                    }
                    // Agentmaster: answering an AskUserQuestion is a real user interaction, so it advances
                    // "last user msg" too — but the answer's content is a tool_result block (not a typed
                    // text prompt), so the text path above skips it. Detect the answer by correlating the
                    // tool_result's tool_use_id back to a prior interactive (AskUserQuestion) tool_use
                    // (interactiveAskIds, populated in the assistant branch below). Advance the TIMESTAMP
                    // only — the synthetic "User has answered your questions…" text stays OUT of the
                    // Messages list (userMsgs is real typed prompts).
                    else if (c && c->type == json::Value::Type::Arr && !interactiveAskIds.empty())
                    {
                        for (const auto& blk : c->arr)
                        {
                            if (blk.type == json::Value::Type::Obj && blk.StrAt(L"type") == L"tool_result" &&
                                interactiveAskIds.count(blk.StrAt(L"tool_use_id")) != 0)
                            {
                                out.lastUserTs = ts;
                                break;
                            }
                        }
                    }
                }
            }

            if (type == L"assistant")
            {
                const auto* msg = obj.Find(L"message");
                if (msg && msg->type == json::Value::Type::Obj)
                {
                    if (const auto* c = msg->Find(L"content"); c && c->type == json::Value::Type::Arr)
                    {
                        for (const auto& blk : c->arr)
                        {
                            if (blk.type != json::Value::Type::Obj || blk.StrAt(L"type") != L"tool_use")
                            {
                                continue;
                            }
                            const std::wstring name = blk.StrAt(L"name");
                            const auto* input = blk.Find(L"input");
                            std::wstring fp;
                            if (input && input->type == json::Value::Type::Obj)
                            {
                                fp = input->StrAt(L"file_path");
                            }
                            if (!fp.empty())
                            {
                                const std::wstring base = SeBasename(fp);
                                if (name == L"Read")
                                {
                                    if (seenRead.insert(base).second)
                                    {
                                        out.filesRead.push_back(base);
                                    }
                                    if (SeIsPlansPath(fp))
                                    {
                                        out.planFilesRead.push_back(fp);
                                    }
                                }
                                else if (name == L"Edit")
                                {
                                    // Edit always targets an EXISTING file (Claude requires a prior Read).
                                    if (seenCreated.find(base) == seenCreated.end() && seenEdit.insert(base).second)
                                    {
                                        out.filesEdited.push_back(base);
                                    }
                                }
                                else if (name == L"Write")
                                {
                                    // Defer: created (new file) vs edited (overwrite) is decided by the
                                    // tool_result text, resolved below. Keep the plan-file capture here.
                                    if (const std::wstring id = blk.StrAt(L"id"); !id.empty())
                                    {
                                        pendingWrites[id] = fp;
                                    }
                                    if (SeIsPlansPath(fp))
                                    {
                                        out.planFilePath = fp;
                                    }
                                }
                            }
                            if (name == L"TodoWrite" && input && input->type == json::Value::Type::Obj)
                            {
                                if (const auto* todos = input->Find(L"todos"); todos && todos->type == json::Value::Type::Arr)
                                {
                                    lastTodos = *todos;
                                    haveTodos = true;
                                }
                            }
                            if (name == L"ExitPlanMode")
                            {
                                out.hasExitPlanMode = true;
                            }
                            // Agentmaster: remember each interactive (AskUserQuestion) tool_use id so the
                            // user's later tool_result answering it advances "last user msg" (see the
                            // externalUser branch above). Mirrors SessionScanner.h's IsInteractiveTool.
                            if (name == L"AskUserQuestion")
                            {
                                if (const std::wstring askId = blk.StrAt(L"id"); !askId.empty())
                                {
                                    interactiveAskIds.insert(askId);
                                }
                            }
                        }
                    }
                }
            }

            // tool_result blocks (carried in user messages) resolve a deferred Write: "File created
            // successfully at:" => a NEW file (Files Created), anything else (an overwrite) => Files
            // Edited. Created wins over Edited for the same basename.
            if (type == L"user" && !pendingWrites.empty())
            {
                const auto* msg = obj.Find(L"message");
                if (msg && msg->type == json::Value::Type::Obj)
                {
                    if (const auto* c = msg->Find(L"content"); c && c->type == json::Value::Type::Arr)
                    {
                        for (const auto& blk : c->arr)
                        {
                            if (blk.type != json::Value::Type::Obj || blk.StrAt(L"type") != L"tool_result")
                            {
                                continue;
                            }
                            const auto pw = pendingWrites.find(blk.StrAt(L"tool_use_id"));
                            if (pw == pendingWrites.end())
                            {
                                continue;
                            }
                            const std::wstring base = SeBasename(pw->second);
                            pendingWrites.erase(pw);
                            std::wstring res; // the tool_result text (a string, or {type:text} blocks)
                            if (const auto* rc = blk.Find(L"content"))
                            {
                                if (rc->type == json::Value::Type::Str)
                                {
                                    res = rc->str;
                                }
                                else if (rc->type == json::Value::Type::Arr)
                                {
                                    for (const auto& rb : rc->arr)
                                    {
                                        if (rb.type == json::Value::Type::Obj && rb.StrAt(L"type") == L"text")
                                        {
                                            res += rb.StrAt(L"text");
                                        }
                                    }
                                }
                            }
                            if (res.find(L"File created successfully at:") != std::wstring::npos)
                            {
                                if (seenCreated.insert(base).second)
                                {
                                    out.filesCreated.push_back(base);
                                }
                            }
                            else if (seenCreated.find(base) == seenCreated.end() && seenEdit.insert(base).second)
                            {
                                out.filesEdited.push_back(base);
                            }
                        }
                    }
                }
            }
        }

        // Any Write whose tool_result never arrived (a truncated tail) falls back to "edited".
        for (const auto& [id, fp] : pendingWrites)
        {
            const std::wstring base = SeBasename(fp);
            if (seenCreated.find(base) == seenCreated.end() && seenEdit.insert(base).second)
            {
                out.filesEdited.push_back(base);
            }
        }

        std::sort(out.filesRead.begin(), out.filesRead.end());
        std::sort(out.filesCreated.begin(), out.filesCreated.end());
        std::sort(out.filesEdited.begin(), out.filesEdited.end());
        if (haveTodos)
        {
            for (const auto& t : lastTodos.arr)
            {
                if (t.type != json::Value::Type::Obj)
                {
                    continue;
                }
                const std::wstring st = t.StrAt(L"status");
                if (st == L"completed")
                {
                    ++out.tasksCompleted;
                }
                else if (st == L"pending" || st == L"in_progress")
                {
                    ++out.tasksPending;
                }
            }
        }
        return out;
    }

    std::wstring FormatSessionDuration(std::wstring_view startIso, std::wstring_view endIso)
    {
        if (startIso.empty() || endIso.empty())
        {
            return {};
        }
        FILETIME a{}, b{};
        if (!SeParseIso(startIso, a) || !SeParseIso(endIso, b))
        {
            return {};
        }
        ULARGE_INTEGER ua{}, ub{};
        ua.LowPart = a.dwLowDateTime;
        ua.HighPart = a.dwHighDateTime;
        ub.LowPart = b.dwLowDateTime;
        ub.HighPart = b.dwHighDateTime;
        const long long diffSec = ub.QuadPart >= ua.QuadPart ? static_cast<long long>((ub.QuadPart - ua.QuadPart) / 10000000ULL) : 0;
        const long long h = diffSec / 3600, m = (diffSec % 3600) / 60, s = diffSec % 60;
        wchar_t dur[48];
        if (h > 0)
        {
            ::swprintf(dur, 48, L"%lldh %lldm", h, m);
        }
        else if (m > 0)
        {
            ::swprintf(dur, 48, L"%lldm %llds", m, s);
        }
        else
        {
            ::swprintf(dur, 48, L"%llds", s);
        }
        const auto hhmm = [](const FILETIME& utc) -> std::wstring {
            FILETIME lf{};
            SYSTEMTIME st{};
            if (::FileTimeToLocalFileTime(&utc, &lf) && ::FileTimeToSystemTime(&lf, &st))
            {
                wchar_t t[8];
                ::swprintf(t, 8, L"%02d:%02d", st.wHour, st.wMinute);
                return t;
            }
            return L"--:--";
        };
        return std::wstring{ dur } + L" (" + hhmm(a) + L" -> " + hhmm(b) + L")";
    }

    std::wstring FindPlanFileInTranscript(std::wstring_view transcriptPath)
    {
        if (transcriptPath.empty())
        {
            return {};
        }
        const std::string bytes = ReadFileHead(std::wstring{ transcriptPath }, 0);
        if (bytes.empty())
        {
            return {};
        }
        const std::wstring wide = Utf8ToWide(bytes);
        std::wstring planFile;
        size_t start = 0;
        for (size_t i = 0; i <= wide.size(); ++i)
        {
            if (i < wide.size() && wide[i] != L'\n')
            {
                continue;
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
            if (!parsed || parsed->type != json::Value::Type::Obj || parsed->StrAt(L"type") != L"assistant")
            {
                continue;
            }
            const auto* msg = parsed->Find(L"message");
            if (!msg || msg->type != json::Value::Type::Obj)
            {
                continue;
            }
            const auto* c = msg->Find(L"content");
            if (!c || c->type != json::Value::Type::Arr)
            {
                continue;
            }
            for (const auto& blk : c->arr)
            {
                if (blk.type != json::Value::Type::Obj || blk.StrAt(L"type") != L"tool_use" || blk.StrAt(L"name") != L"Write")
                {
                    continue;
                }
                if (const auto* input = blk.Find(L"input"); input && input->type == json::Value::Type::Obj)
                {
                    const std::wstring fp = input->StrAt(L"file_path");
                    if (!fp.empty() && SeIsPlansPath(fp))
                    {
                        planFile = fp; // keep the LAST
                    }
                }
            }
        }
        return planFile;
    }

    // ===== Codex turn-state (Phase C2): rollout-tail -> Running / Waiting / Idle ===============

    CodexBoundary ClassifyCodexLine(std::wstring_view jsonLine)
    {
        CodexBoundary b;
        while (!jsonLine.empty() && (jsonLine.back() == L'\r' || jsonLine.back() == L'\n'))
        {
            jsonLine.remove_suffix(1);
        }
        if (jsonLine.empty())
        {
            return b;
        }
        const auto parsed = json::Parse(jsonLine);
        if (!parsed || parsed->type != json::Value::Type::Obj)
        {
            return b;
        }
        const auto& obj = *parsed;
        if (obj.StrAt(L"type") != L"event_msg") // only event_msg payloads carry the turn lifecycle
        {
            return b;
        }
        const auto* pl = obj.Find(L"payload");
        if (!pl || pl->type != json::Value::Type::Obj)
        {
            return b;
        }
        const std::wstring pt = pl->StrAt(L"type");
        if (pt == L"task_started")
        {
            b.isBoundary = true;
            b.state = CodexState::Running; // a turn opened -> the agent is working
        }
        else if (pt == L"task_complete")
        {
            b.isBoundary = true;
            b.state = CodexState::Waiting; // turn done -> waiting for the user
            b.lastAgentMessage = pl->StrAt(L"last_agent_message");
        }
        else if (pt == L"turn_aborted" || pt == L"thread_rolled_back")
        {
            b.isBoundary = true;
            b.state = CodexState::Waiting; // user-interrupted / rolled back -> the turn ended; waiting
        }
        return b;
    }

    CodexState ReadCodexStateDelta(std::wstring_view rolloutPath, int64_t& offsetInOut, CodexState prior, std::wstring* lastAgentMessageOut)
    {
        if (rolloutPath.empty())
        {
            return prior;
        }
        // The last bytes that always contain the most-recent boundary (task_complete ends every
        // at-rest session; turns are frequent enough that ~1 MiB covers several on the heaviest
        // sessions measured). First sight / fall-behind seeks here; a steady forward delta is tiny.
        constexpr int64_t kCodexTailWindowBytes = 1 << 20; // 1 MiB
        constexpr int64_t kCodexMaxCatchupBytes = 4 << 20; // a forward delta beyond this -> tail-seek instead

        const std::wstring path{ rolloutPath };
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return prior;
        }
        LARGE_INTEGER szli{};
        if (!::GetFileSizeEx(h, &szli) || szli.QuadPart <= 0)
        {
            ::CloseHandle(h);
            return prior;
        }
        const int64_t size = szli.QuadPart;

        int64_t start = offsetInOut;
        bool skipPartialLead = false;
        if (start <= 0 || start > size || (size - start) > kCodexMaxCatchupBytes)
        {
            start = (size > kCodexTailWindowBytes) ? (size - kCodexTailWindowBytes) : 0;
            skipPartialLead = (start > 0); // a tail seek lands mid-line — drop the partial leading line
        }
        if (start >= size) // nothing new since the last read
        {
            ::CloseHandle(h);
            return prior;
        }

        LARGE_INTEGER mv{};
        mv.QuadPart = start;
        if (!::SetFilePointerEx(h, mv, nullptr, FILE_BEGIN))
        {
            ::CloseHandle(h);
            return prior;
        }
        std::string buf(static_cast<size_t>(size - start), '\0');
        size_t got = 0;
        while (got < buf.size())
        {
            DWORD rd = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(buf.size() - got, 1u << 20));
            if (!::ReadFile(h, buf.data() + got, chunk, &rd, nullptr) || rd == 0)
            {
                break;
            }
            got += rd;
        }
        buf.resize(got);
        ::CloseHandle(h);

        const size_t lastNl = buf.rfind('\n');
        if (lastNl == std::string::npos)
        {
            return prior; // no complete line yet (a partial append, or a line longer than the window) — don't advance
        }
        size_t lineStart = 0;
        if (skipPartialLead)
        {
            const size_t firstNl = buf.find('\n');
            lineStart = (firstNl == std::string::npos) ? buf.size() : firstNl + 1;
        }

        CodexState result = prior;
        bool saw = false;
        std::wstring lastMsg;
        size_t ls = lineStart;
        for (size_t i = lineStart; i <= lastNl; ++i)
        {
            if (buf[i] != '\n')
            {
                continue;
            }
            if (i > ls)
            {
                const CodexBoundary b = ClassifyCodexLine(Utf8ToWide(buf.substr(ls, i - ls)));
                if (b.isBoundary)
                {
                    result = b.state;
                    saw = true;
                    if (!b.lastAgentMessage.empty())
                    {
                        lastMsg = b.lastAgentMessage;
                    }
                }
            }
            ls = i + 1;
        }
        offsetInOut = start + static_cast<int64_t>(lastNl) + 1; // byte cursor past the last consumed newline
        if (saw && lastAgentMessageOut && !lastMsg.empty())
        {
            *lastAgentMessageOut = lastMsg;
        }
        return saw ? result : prior;
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

    // ===== Agentmaster: shared summary-box renderers (TAB_OVERLAY.md summary panel + the Sessions
    // page detail). Moved here from AgentTabOverlay.cpp so the overlay and the Sessions page share
    // ONE renderer (single source of truth — they can never drift). Pure string work; the only OS
    // dependency lives in the callers (AnalyzeSessionTranscript / path resolution), so these are
    // safe to invoke off the UI thread.
    namespace
    {
        // The project-folder name = basename(dirname(transcriptPath)) — session-end.js getFolderName.
        std::wstring SummaryFolderFromPath(const std::wstring& p)
        {
            const auto s1 = p.find_last_of(L"/\\");
            if (s1 == std::wstring::npos)
            {
                return {};
            }
            const std::wstring dir = p.substr(0, s1);
            const auto s2 = dir.find_last_of(L"/\\");
            return s2 == std::wstring::npos ? dir : dir.substr(s2 + 1);
        }

        // Escape a message to ONE line (newlines/tabs -> \n / \t, like session-end.js) + truncate.
        std::wstring SummaryEscapeMsg(const std::wstring& m)
        {
            std::wstring esc;
            for (const wchar_t ch : m)
            {
                if (ch == L'\n')
                    esc += L"\\n";
                else if (ch == L'\r')
                    ; // dropped
                else if (ch == L'\t')
                    esc += L"\\t";
                else
                    esc += ch;
            }
            if (esc.size() > 240)
            {
                esc = esc.substr(0, 237) + L"...";
            }
            return esc;
        }
    }

    std::wstring RenderSessionSummaryBox(const SessionSummary& a, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, const std::wstring& planFile, bool full)
    {
        std::wstring glyph = liveGlyph, label = liveLabel;
        const bool isPlan = a.hasPlanContent || a.hasExitPlanMode;
        if (a.hasPlanContent)
        {
            glyph = L"\U0001F680"; // 🚀
            label = L"plan-start";
        }
        else if (a.hasExitPlanMode)
        {
            glyph = L"\U0001F4CB"; // 📋
            label = L"plan-end";
        }

        std::wstring o;
        const auto line = [&o](const std::wstring& s) { o += s; o += L"\n"; };
        // A section divider: a lone sentinel line, suppressed at the very top (a leading rule with
        // nothing above it reads as a stray bar). The display turns it into a full-width Border rule.
        const auto sep = [&o]() { if (!o.empty()) { o += kSummarySepMark; o += L"\n"; } };

        // Header: a live-state line when full AND a live state was passed, OR a plan signal. A caller
        // with no live state (the Sessions page passes an empty label) suppresses the otherwise-
        // redundant header while still surfacing plan-start/plan-end (isPlan overrides glyph+label).
        if ((full && !label.empty()) || isPlan)
        {
            line(glyph + L"  " + label);
        }
        if (full)
        {
            line(id);
        }
        if (a.hasPlanContent && !a.parentSessionId.empty())
        {
            line(L"Parent: " + a.parentSessionId);
            if (!planFile.empty())
            {
                line(L"Plan:   " + planFile);
            }
        }
        else if (!planFile.empty())
        {
            line(L"Plan:   " + planFile);
        }
        if (full)
        {
            line(L"Dir:    " + cwd);
            if (const std::wstring folder = SummaryFolderFromPath(transcriptPath); !folder.empty())
            {
                line(L"Folder: " + folder);
            }
            line(L"Resume: " + resumeCmd);
        }
        if (full && !a.branch.empty())
        {
            line(L"Branch: " + a.branch);
        }
        if (a.tasksCompleted > 0 || a.tasksPending > 0)
        {
            line(L"Tasks:  " + std::to_wstring(a.tasksCompleted) + L" done / " + std::to_wstring(a.tasksPending) + L" pending");
        }
        if (!a.userMsgs.empty())
        {
            sep();
            int i = 1;
            for (const auto& m : a.userMsgs)
            {
                line(L" " + std::to_wstring(i++) + L". " + SummaryEscapeMsg(m));
            }
        }
        if (!a.filesRead.empty())
        {
            sep();
            line(L"Files Read:");
            for (const auto& f : a.filesRead)
            {
                line(L"   * " + f);
            }
        }
        if (!a.filesCreated.empty())
        {
            sep();
            line(L"Files Created:");
            for (const auto& f : a.filesCreated)
            {
                line(L"   * " + f);
            }
        }
        if (!a.filesEdited.empty())
        {
            sep();
            line(L"Files Edited:");
            for (const auto& f : a.filesEdited)
            {
                line(L"   * " + f);
            }
        }
        while (!o.empty() && o.back() == L'\n')
        {
            o.pop_back();
        }
        return o;
    }

    std::wstring RenderCodexSummaryBox(const CodexRolloutInfo& info, const std::wstring& id, const std::wstring& cwd, const std::wstring& transcriptPath, const std::wstring& resumeCmd, const std::wstring& liveGlyph, const std::wstring& liveLabel, bool full)
    {
        std::wstring o;
        const auto line = [&o](const std::wstring& s) { o += s; o += L"\n"; };
        const auto sep = [&o]() { if (!o.empty()) { o += kSummarySepMark; o += L"\n"; } };

        if (full)
        {
            line(liveGlyph + L"  " + liveLabel + L"  \x00B7 codex");
            line(id);
            line(L"Dir:    " + cwd);
            if (const std::wstring folder = SummaryFolderFromPath(transcriptPath); !folder.empty())
            {
                line(L"Folder: " + folder);
            }
            line(L"Resume: " + resumeCmd);
            std::wstring me;
            const auto add = [&me](const std::wstring& p) { if (!p.empty()) { if (!me.empty()) me += L" \x00B7 "; me += p; } };
            add(info.model);
            add(info.effort);
            add(info.sandbox);
            if (!me.empty())
            {
                line(L"Model:  " + me);
            }
            if (!info.gitBranch.empty())
            {
                line(L"Branch: " + info.gitBranch);
            }
        }
        if (!info.userPrompts.empty())
        {
            sep();
            int i = 1;
            for (const auto& m : info.userPrompts)
            {
                line(L" " + std::to_wstring(i++) + L". " + SummaryEscapeMsg(m));
            }
        }
        while (!o.empty() && o.back() == L'\n')
        {
            o.pop_back();
        }
        return o;
    }
}
