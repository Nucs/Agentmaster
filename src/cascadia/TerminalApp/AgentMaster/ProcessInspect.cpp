// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and the
// standalone test harness compiles it directly). See ProcessInspect.h / OBSERVER.md §6, §8b.
#include "ProcessInspect.h"

#include <windows.h>
#include <tlhelp32.h> // CreateToolhelp32Snapshot

#include <string_view>
#include <unordered_set>

#include "ClaudeSpawn.h" // ClaudeProjectsDir() — the live Claude transcript root

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
        const HANDLE h = OpenForQuery(pid);
        if (h == nullptr)
        {
            return false; // can't open (gone / denied) -> treat as not-alive for our cache
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

    RunningApp ClassifyRunningApp(std::wstring_view amSession, std::wstring_view wtSession, std::wstring_view ourAmSession)
    {
        // OBSERVER.md §7: our stamp -> Agentmaster; a bare WT_SESSION (no AM_SESSION) -> external
        // WindowsTerminal; anything else (incl. a FOREIGN AM_SESSION) -> Other. A foreign-instance
        // claude is never bound regardless, because its wtSession isn't in this window's roster.
        if (!ourAmSession.empty() && !amSession.empty() && amSession == ourAmSession)
        {
            return RunningApp::Agentmaster;
        }
        if (amSession.empty() && !wtSession.empty())
        {
            return RunningApp::WindowsTerminal;
        }
        return RunningApp::Other;
    }

    // ===== OS-touching: full facts read ====================================================

    ClaudeProcessFacts ReadClaudeFacts(uint32_t pid)
    {
        ClaudeProcessFacts f;
        f.pid = pid;
        f.startUnixMs = ProcessStartUnixMs(pid);
        f.cwd = ReadProcessCwd(pid);
        f.commandline = ReadProcessCommandLine(pid);
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
        int64_t maxMtime = candidates[0].mtimeMs;
        for (const auto& c : candidates)
        {
            if (c.mtimeMs > maxMtime)
            {
                maxMtime = c.mtimeMs;
            }
        }
        const TranscriptCandidate* best = nullptr;
        int64_t bestDelta = 0;
        for (const auto& c : candidates)
        {
            if (maxMtime - c.mtimeMs > tieWindowMs)
            {
                continue; // not in the recently-written band -> not the active conversation
            }
            if (startUnixMs > 0)
            {
                // tie-break: the transcript created closest to this claude's start time
                const int64_t d = c.ctimeMs >= startUnixMs ? c.ctimeMs - startUnixMs : startUnixMs - c.ctimeMs;
                if (best == nullptr || d < bestDelta)
                {
                    best = &c;
                    bestDelta = d;
                }
            }
            else if (best == nullptr || c.mtimeMs > best->mtimeMs || (c.mtimeMs == best->mtimeMs && c.ctimeMs > best->ctimeMs))
            {
                best = &c; // no start hint -> the strictly newest
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
}
