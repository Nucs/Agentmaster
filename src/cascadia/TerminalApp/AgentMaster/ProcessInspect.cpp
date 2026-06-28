// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster engine TU (no WinRT/PCH). ProcessInspect CORE: process enumeration + PEB reads + facts. Split by section; see ProcessInspect.h / OBSERVER.md. Transcript/Content/Window/Summary live in sibling TUs.
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
#include "ProcessInspect.Internal.h" // the shared file-local PEB/string/file primitives

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

}
