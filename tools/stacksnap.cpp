// Agentmaster: stacksnap — a NON-SUSPENDING stack sampler for a LIVE process (safe on production).
//
//   stacksnap top    <pid> [seconds=5]
//   stacksnap sample <pid> <seconds> <periodMs> <tid> [tid ...]
//
// Why it exists: tools/livesample.exe suspends the target thread and walks its stack WHILE it is
// suspended, so every lazy PDB load (and, with a symbol server, every PDB download) freezes the
// target — it froze the release UI 165 + 36 + 109 s on 2026-09-27. stacksnap never calls
// SuspendThread (its thread handles do not even carry THREAD_SUSPEND_RESUME). Per sample it:
//   1. reads the thread's register context with GetThreadContext on the RUNNING thread (the kernel
//      captures it through a special kernel APC: the thread pauses for microseconds, the way it does
//      for any interrupt, and nothing can leave it stopped);
//   2. copies the used part of the thread's stack ([RSP, StackBase)) with one ReadProcessMemory;
//   3. walks THAT COPY with StackWalk64 in this process (a ReadMemoryRoutine serves stack reads from
//      the copy and code/unwind reads from the live, immutable module images).
// Symbol loads and downloads therefore cost only this process's time. A symbol server in the path
// is safe here (set STACKSNAP_SYMSRV), unlike livesample.
//
// Trade-off: the thread keeps running between steps 1 and 2 (tens of microseconds), so a sample
// that lands while the thread returns from or calls into a function can carry a few bogus INNER
// frames. A statistical profile tolerates that; do not read a single sample as ground truth.
//
// Modes:
//   top    — every thread's CPU over the window (GetThreadTimes delta), its Win32 start address and
//            description, and ONE snapshot stack of each of the 25 busiest threads (the frames of our
//            modules name what a std::thread / thread-pool worker is really running).
//   sample — samples each <tid> every <periodMs> for <seconds>. Per thread it prints the CPU in the
//            window, the split of samples into ON-CPU / IN-KERNEL-CALL / WAITING, an inclusive
//            histogram over every frame and over our frames, the top collapsed stacks (our frames,
//            leaf first, with source lines), the exclusive leaf histogram, the waits, and writes every
//            stack in "folded" form (root;...;leaf count, inline frames expanded) to
//            stacksnap_<pid>_<tid>.folded in the current directory for grep / flame-graph use.
//
// Symbols: the search path is the target's executable directory (the release layout ships its PDBs
// next to the binaries; dbghelp checks each PDB's GUID+age, so a stale PDB is ignored, never
// misattributed), plus env STACKSNAP_SYMSRV when set (e.g. "srv*Q:\tmp\symcache*https://msdl.
// microsoft.com/download/symbols" — needs a symsrv-capable dbghelp.dll + symsrv.dll beside this
// exe, e.g. copied from "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64").
//
// Build: tools\_build_stacksnap.bat (vcvars64 + cl; links dbghelp.lib psapi.lib).
//
// Access needed: PROCESS_QUERY_INFORMATION | PROCESS_VM_READ on the process and THREAD_GET_CONTEXT |
// THREAD_QUERY_INFORMATION on each sampled thread — a same-user, non-elevated process grants both.
// Nothing is written to the target; no thread is suspended; no code is injected.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <psapi.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

namespace
{
    /// <summary>The NtQueryInformationThread signature (ntdll export, resolved at run time).</summary>
    using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

    /// <summary>
    /// THREAD_BASIC_INFORMATION (class 0), declared locally so this file does not depend on which
    /// SDK header happens to define CLIENT_ID. Only <c>TebBaseAddress</c> is read.
    /// </summary>
    struct ThreadBasicInformation
    {
        LONG ExitStatus;
        PVOID TebBaseAddress;
        HANDLE UniqueProcess;
        HANDLE UniqueThread;
        ULONG_PTR AffinityMask;
        LONG Priority;
        LONG BasePriority;
    };

    /// <summary>ThreadBasicInformation: the TEB address (whose NT_TIB holds the stack bounds).</summary>
    constexpr ULONG kThreadBasicInformation = 0;

    /// <summary>ThreadQuerySetWin32StartAddress: the routine the thread was created with.</summary>
    constexpr ULONG kThreadWin32StartAddress = 9;

    /// <summary>
    /// The most stack bytes one sample copies. A UI thread deep in XAML layout uses a few hundred KB;
    /// the cap only guards against a corrupt RSP making us copy megabytes per sample.
    /// </summary>
    constexpr ULONG64 kMaxStackCopyBytes = 2ull * 1024 * 1024;

    /// <summary>The deepest walk recorded per sample; deeper frames are dropped (rare, recursion).</summary>
    constexpr int kMaxFrames = 192;

    /// <summary>The target process handle every dbghelp call is keyed on (one target per run).</summary>
    HANDLE g_proc = nullptr;

    /// <summary>ntdll!NtQueryInformationThread, or null when the export is missing.</summary>
    NtQueryInformationThreadFn g_ntQueryInformationThread = nullptr;

    /// <summary>
    /// One captured stack: the register context at capture time and a byte copy of the stack from
    /// that RSP upward. <see cref="ReadSnapshotOrLive"/> serves the walk's stack reads from here.
    /// </summary>
    struct StackCopy
    {
        /// <summary>The captured registers (16-byte aligned, as GetThreadContext requires).</summary>
        alignas(16) CONTEXT context{};

        /// <summary>The address of <c>bytes[0]</c> in the target (the captured RSP).</summary>
        ULONG64 low = 0;

        /// <summary>The copied stack bytes, [low, low + bytes.size()).</summary>
        std::vector<BYTE> bytes;
    };

    /// <summary>The copy the current StackWalk64 call reads from (single-threaded tool).</summary>
    const StackCopy* g_walking = nullptr;

    /// <summary>
    /// StackWalk64's memory reader: reads that fall wholly inside the captured stack copy are served
    /// from the copy (so the walk sees ONE consistent stack even though the thread kept running);
    /// everything else — module code, .pdata unwind tables, read-only data — comes from the live
    /// process, where it does not change.
    /// </summary>
    /// <param name="process">The target process handle (dbghelp passes back what it was given).</param>
    /// <param name="address">The target address to read.</param>
    /// <param name="buffer">Receives the bytes.</param>
    /// <param name="size">How many bytes to read.</param>
    /// <param name="bytesRead">Receives how many bytes were read.</param>
    /// <returns>TRUE when every requested byte was read.</returns>
    BOOL CALLBACK ReadSnapshotOrLive(HANDLE process, DWORD64 address, PVOID buffer, DWORD size, LPDWORD bytesRead)
    {
        if (g_walking && address >= g_walking->low && address + size <= g_walking->low + g_walking->bytes.size())
        {
            memcpy(buffer, g_walking->bytes.data() + (address - g_walking->low), size);
            *bytesRead = size;
            return TRUE;
        }
        SIZE_T got = 0;
        const BOOL ok = ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), buffer, size, &got);
        *bytesRead = static_cast<DWORD>(got);
        return ok && got == size;
    }

    /// <summary>
    /// The per-thread state a sampling run keeps: the open handle and the stack's upper bound, read
    /// once from the thread's TEB (NT_TIB.StackBase never moves for a thread's lifetime).
    /// </summary>
    struct SampledThread
    {
        /// <summary>The thread id (as passed on the command line or found by the census).</summary>
        DWORD tid = 0;

        /// <summary>
        /// THREAD_GET_CONTEXT | THREAD_QUERY_* only — never THREAD_SUSPEND_RESUME, so the tool cannot
        /// suspend the thread. Owned by the caller; null when the open failed.
        /// </summary>
        HANDLE handle = nullptr;

        /// <summary>
        /// NT_TIB.StackBase (the stack's HIGH end; stacks grow down). The copy spans [RSP, stackBase),
        /// so a wrong value only shortens or truncates the walk, never reads past the stack.
        /// </summary>
        ULONG64 stackBase = 0;
    };

    /// <summary>
    /// Opens a thread for context reads and reads its stack base from the target's TEB.
    /// </summary>
    /// <param name="tid">The thread id.</param>
    /// <param name="out">Receives the handle and the stack base.</param>
    /// <returns>True when both the handle and the stack base were obtained.</returns>
    /// <remarks>
    /// The handle carries THREAD_GET_CONTEXT but NOT THREAD_SUSPEND_RESUME, so this tool cannot
    /// suspend the thread even by mistake. The caller closes <c>out.handle</c> when it is non-null.
    /// </remarks>
    bool OpenSampledThread(DWORD tid, SampledThread& out)
    {
        out.tid = tid;
        out.handle = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (!out.handle || !g_ntQueryInformationThread)
        {
            return false;
        }
        ThreadBasicInformation tbi{};
        if (g_ntQueryInformationThread(out.handle, kThreadBasicInformation, &tbi, sizeof(tbi), nullptr) < 0 || !tbi.TebBaseAddress)
        {
            return false;
        }
        // NT_TIB = { ExceptionList, StackBase, StackLimit, ... } at the start of the TEB.
        ULONG64 tib[3] = {};
        SIZE_T got = 0;
        if (!ReadProcessMemory(g_proc, tbi.TebBaseAddress, tib, sizeof(tib), &got) || got != sizeof(tib))
        {
            return false;
        }
        out.stackBase = tib[1];
        return out.stackBase != 0;
    }

    /// <summary>
    /// Captures one sample: the context of the RUNNING thread, then a copy of its used stack.
    /// </summary>
    /// <param name="thread">The thread to sample.</param>
    /// <param name="out">Receives the context and the stack copy.</param>
    /// <returns>False when the context or the stack could not be read (the thread may have exited).</returns>
    bool CaptureStack(const SampledThread& thread, StackCopy& out)
    {
        out.context = {};
        out.context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        // No SuspendThread: the kernel delivers a special APC that records the user-mode registers
        // and lets the thread continue. The stack copy right below races the running thread by a few
        // microseconds — the documented trade-off in the file header.
        if (!GetThreadContext(thread.handle, &out.context))
        {
            return false;
        }
        const ULONG64 low = out.context.Rsp;
        if (low == 0 || thread.stackBase <= low)
        {
            return false;
        }
        const ULONG64 length = (std::min)(thread.stackBase - low, kMaxStackCopyBytes);
        out.bytes.resize(static_cast<size_t>(length));
        SIZE_T got = 0;
        // A partial read (a guard page in a corrupt range) still leaves the inner frames walkable.
        ReadProcessMemory(g_proc, reinterpret_cast<LPCVOID>(low), out.bytes.data(), out.bytes.size(), &got);
        out.bytes.resize(got);
        out.low = low;
        return got > 0;
    }

    /// <summary>
    /// Walks a stack copy with StackWalk64 — entirely in this process. Inline frames are NOT
    /// enumerated here: the walk records raw return addresses only (fast, so the sampling period
    /// holds), and <see cref="SymbolCache::Expand"/> expands each unique address into its inline
    /// chain once, at report time.
    /// </summary>
    /// <param name="thread">The sampled thread (its handle is only passed through to dbghelp).</param>
    /// <param name="copy">The captured context + stack bytes.</param>
    /// <returns>The physical frames' program counters, leaf first.</returns>
    std::vector<ULONG64> WalkCopy(const SampledThread& thread, const StackCopy& copy)
    {
        std::vector<ULONG64> pcs;
        alignas(16) CONTEXT context = copy.context; // StackWalk64 mutates the context as it unwinds.
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        g_walking = &copy;
        for (int i = 0; i < kMaxFrames; ++i)
        {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, g_proc, thread.handle, &frame, &context, ReadSnapshotOrLive,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            {
                break;
            }
            if (frame.AddrPC.Offset == 0)
            {
                break;
            }
            if (pcs.empty() || pcs.back() != frame.AddrPC.Offset)
            {
                pcs.push_back(frame.AddrPC.Offset);
            }
        }
        g_walking = nullptr;
        return pcs;
    }

    /// <summary>
    /// Symbol cache: one expansion per unique (pc, leaf-or-return-address) pair, since dbghelp lookups
    /// dominate the run time once thousands of samples share a few hundred addresses.
    /// </summary>
    struct SymbolCache
    {
        /// <summary>A resolved logical frame: "module!function" (no displacement) and "file:line" when known.</summary>
        struct Entry
        {
            /// <summary>"module!function", without displacement, so samples of one function aggregate.</summary>
            std::wstring function;

            /// <summary>"file.cpp:123", or empty when the module has no line info (OS modules, exports only).</summary>
            std::wstring line;
        };

        /// <summary>
        /// (pc, isLeaf) → its logical frames. Keyed on isLeaf too, because a leaf is looked up at pc and
        /// a return address at pc-1, which can land in a different inline scope.
        /// </summary>
        std::map<std::pair<ULONG64, bool>, std::vector<Entry>> chains;

        /// <summary>Module base → short module name; one SymGetModuleInfoW64 per module, not per frame.</summary>
        std::unordered_map<ULONG64, std::wstring> moduleNames;

        /// <summary>
        /// Expands one physical frame into its logical frames: the functions inlined at that address
        /// (innermost first), then the physical function itself. A return address points AFTER its
        /// call, so non-leaf frames are looked up at pc-1 (the call's own scope and line, not the next
        /// statement's).
        /// </summary>
        /// <param name="pc">The frame's program counter.</param>
        /// <param name="isLeaf">True for the innermost frame (its pc is the executing instruction).</param>
        /// <returns>The cached chain (a stable reference; never empty).</returns>
        const std::vector<Entry>& Expand(ULONG64 pc, bool isLeaf)
        {
            const auto key = std::make_pair(pc, isLeaf);
            if (const auto it = chains.find(key); it != chains.end())
            {
                return it->second;
            }
            const ULONG64 address = isLeaf ? pc : pc - 1;
            const std::wstring module = ModuleOf(address);
            std::vector<Entry> chain;
            const DWORD inlineCount = SymAddrIncludeInlineTrace(g_proc, address);
            if (inlineCount > 0)
            {
                DWORD context = 0;
                DWORD frameIndex = 0;
                if (SymQueryInlineTrace(g_proc, address, 0, address, address, &context, &frameIndex))
                {
                    // The inline contexts of one address are consecutive, innermost first.
                    for (DWORD i = 0; i < inlineCount; ++i, ++context)
                    {
                        alignas(16) BYTE storage[sizeof(SYMBOL_INFOW) + 1024 * sizeof(wchar_t)] = {};
                        auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(storage);
                        symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
                        symbol->MaxNameLen = 1024;
                        DWORD64 displacement = 0;
                        if (!SymFromInlineContextW(g_proc, address, context, &displacement, symbol))
                        {
                            continue;
                        }
                        Entry entry;
                        entry.function = module + L"!" + symbol->Name;
                        IMAGEHLP_LINEW64 line{};
                        line.SizeOfStruct = sizeof(line);
                        DWORD lineDisplacement = 0;
                        if (SymGetLineFromInlineContextW(g_proc, address, context, 0, &lineDisplacement, &line))
                        {
                            entry.line = LineText(line);
                        }
                        chain.push_back(std::move(entry));
                    }
                }
            }
            chain.push_back(Physical(address, module));
            return chains.emplace(key, std::move(chain)).first->second;
        }

        /// <summary>
        /// Resolves the physical function containing an address (no inline expansion), with its line.
        /// </summary>
        /// <param name="address">The address to resolve (already adjusted to pc-1 for return addresses).</param>
        /// <param name="module">The address's module name (from <see cref="ModuleOf"/>).</param>
        /// <returns>The entry; "module+0xOFFSET" or "0xADDRESS" when no symbol covers it.</returns>
        Entry Physical(ULONG64 address, const std::wstring& module)
        {
            Entry entry;
            alignas(16) BYTE storage[sizeof(SYMBOL_INFOW) + 1024 * sizeof(wchar_t)] = {};
            auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
            symbol->MaxNameLen = 1024;
            DWORD64 displacement = 0;
            if (SymFromAddrW(g_proc, address, &displacement, symbol))
            {
                entry.function = module + L"!" + symbol->Name;
            }
            else
            {
                wchar_t text[64];
                const DWORD64 base = SymGetModuleBase64(g_proc, address);
                if (base)
                {
                    swprintf_s(text, L"+0x%llx", address - base);
                    entry.function = module + text;
                }
                else
                {
                    swprintf_s(text, L"0x%llx", address);
                    entry.function = text;
                }
            }
            IMAGEHLP_LINEW64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            if (SymGetLineFromAddrW64(g_proc, address, &lineDisplacement, &line))
            {
                entry.line = LineText(line);
            }
            return entry;
        }

        /// <summary>Formats a dbghelp line record as "file.cpp:123" (directory stripped).</summary>
        /// <param name="line">The line record.</param>
        /// <returns>The short "file:line" text.</returns>
        static std::wstring LineText(const IMAGEHLP_LINEW64& line)
        {
            const wchar_t* fileName = line.FileName ? line.FileName : L"?";
            if (const wchar_t* slash = wcsrchr(fileName, L'\\'))
            {
                fileName = slash + 1;
            }
            return std::wstring(fileName) + L":" + std::to_wstring(line.LineNumber);
        }

        /// <summary>
        /// Expands a whole walked stack into logical frames, leaf first.
        /// </summary>
        /// <param name="pcs">The physical program counters, leaf first.</param>
        /// <returns>Pointers into the cache (valid for the cache's lifetime).</returns>
        std::vector<const Entry*> Logical(const std::vector<ULONG64>& pcs)
        {
            std::vector<const Entry*> frames;
            for (size_t i = 0; i < pcs.size(); ++i)
            {
                for (const auto& entry : Expand(pcs[i], i == 0))
                {
                    frames.push_back(&entry);
                }
            }
            return frames;
        }

        /// <summary>The short module name for an address ("TerminalApp", "ntdll"), cached per module base.</summary>
        /// <param name="address">An address inside a module.</param>
        /// <returns>The module name, or "?" when the address is outside every known module.</returns>
        std::wstring ModuleOf(ULONG64 address)
        {
            const DWORD64 base = SymGetModuleBase64(g_proc, address);
            if (!base)
            {
                return L"?";
            }
            if (const auto it = moduleNames.find(base); it != moduleNames.end())
            {
                return it->second;
            }
            IMAGEHLP_MODULEW64 info{};
            info.SizeOfStruct = sizeof(info);
            std::wstring name = L"?";
            if (SymGetModuleInfoW64(g_proc, address, &info))
            {
                name = info.ModuleName;
            }
            moduleNames.emplace(base, name);
            return name;
        }
    };

    /// <summary>
    /// True for Agentmaster's own modules — the frames a "what is OUR code doing" view keeps.
    /// </summary>
    /// <param name="function">A resolved "module!function" string.</param>
    /// <returns>True when the module is one we build.</returns>
    bool IsOurModule(const std::wstring& function)
    {
        static const wchar_t* const prefixes[] = {
            L"TerminalApp!", L"Agentmaster!", L"Microsoft_Terminal_Control!", L"Microsoft.Terminal.Control!",
            L"TerminalConnection!", L"Microsoft_Terminal_Settings_Model!", L"Microsoft.Terminal.Settings.Model!",
            L"Microsoft_Terminal_Settings_Editor!", L"Microsoft.Terminal.Settings.Editor!", L"Microsoft_Terminal_UI!",
            L"Microsoft.Terminal.UI!", L"Microsoft_Terminal_UI_Markdown!", L"WindowsTerminal!",
        };
        for (const wchar_t* prefix : prefixes)
        {
            if (function.rfind(prefix, 0) == 0)
            {
                return true;
            }
        }
        return false;
    }

    /// <summary>What the thread was doing at a sample, judged from its leaf frame.</summary>
    enum class SampleClass
    {
        OnCpu,      // the leaf is ordinary user-mode code: the thread was running
        KernelCall, // the leaf is a syscall stub that is not a wait (file I/O, a kernel query, ...)
        Waiting,    // the leaf is a wait syscall (message pump, lock, event, sleep, I/O port)
    };

    /// <summary>
    /// Classifies a sample by its leaf. A captured context of a thread inside a syscall shows RIP at
    /// the instruction after the stub's <c>syscall</c>, so the stub's NAME says which call it is.
    /// </summary>
    /// <param name="leafFunction">The resolved leaf ("ntdll!NtWaitForSingleObject", ...).</param>
    /// <returns>The sample's class.</returns>
    SampleClass Classify(const std::wstring& leafFunction)
    {
        const bool isStub = leafFunction.rfind(L"ntdll!Nt", 0) == 0 || leafFunction.rfind(L"ntdll!Zw", 0) == 0 ||
                            leafFunction.rfind(L"win32u!Nt", 0) == 0;
        if (!isStub)
        {
            return SampleClass::OnCpu;
        }
        static const wchar_t* const waits[] = {
            L"Wait", L"GetMessage", L"RemoveIoCompletion", L"DelayExecution", L"AlertByThreadId", L"WorkerFactory",
            L"ReplyWaitReceivePort", L"AlpcSendWaitReceivePort", L"SignalAndWait",
        };
        for (const wchar_t* w : waits)
        {
            if (leafFunction.find(w) != std::wstring::npos)
            {
                return SampleClass::Waiting;
            }
        }
        return SampleClass::KernelCall;
    }

    /// <summary>Converts a FILETIME duration to milliseconds.</summary>
    /// <param name="ft">A duration in 100 ns units.</param>
    /// <returns>The duration in milliseconds.</returns>
    double FileTimeMs(const FILETIME& ft)
    {
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return static_cast<double>(u.QuadPart) / 10000.0;
    }

    /// <summary>A thread's total (kernel + user) CPU time so far, in milliseconds; -1 when unreadable.</summary>
    /// <param name="thread">An open thread handle with THREAD_QUERY_LIMITED_INFORMATION.</param>
    /// <returns>The CPU milliseconds, or -1.</returns>
    double ThreadCpuMs(HANDLE thread)
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetThreadTimes(thread, &created, &exited, &kernel, &user))
        {
            return -1;
        }
        return FileTimeMs(kernel) + FileTimeMs(user);
    }

    /// <summary>Lists the thread ids of one process (Toolhelp snapshot).</summary>
    /// <param name="pid">The process id.</param>
    /// <returns>The thread ids, in snapshot order.</returns>
    std::vector<DWORD> ListThreads(DWORD pid)
    {
        std::vector<DWORD> tids;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return tids;
        }
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID == pid)
            {
                tids.push_back(entry.th32ThreadID);
            }
        }
        CloseHandle(snapshot);
        return tids;
    }

    /// <summary>
    /// The thread's Win32 start address as "module!symbol" (or module+offset).
    /// </summary>
    /// <param name="thread">An open thread handle with THREAD_QUERY_INFORMATION.</param>
    /// <param name="symbols">The symbol cache.</param>
    /// <returns>The resolved start routine, or "?".</returns>
    std::wstring StartAddressOf(HANDLE thread, SymbolCache& symbols)
    {
        ULONG64 start = 0;
        if (!g_ntQueryInformationThread || g_ntQueryInformationThread(thread, kThreadWin32StartAddress, &start, sizeof(start), nullptr) < 0)
        {
            return L"?";
        }
        return symbols.Physical(start, symbols.ModuleOf(start)).function;
    }

    /// <summary>The thread's description (SetThreadDescription), empty when none.</summary>
    /// <param name="thread">An open thread handle with THREAD_QUERY_LIMITED_INFORMATION.</param>
    /// <returns>The description text.</returns>
    std::wstring DescriptionOf(HANDLE thread)
    {
        PWSTR text = nullptr;
        std::wstring result;
        if (SUCCEEDED(GetThreadDescription(thread, &text)) && text)
        {
            result = text;
            LocalFree(text);
        }
        return result;
    }

    /// <summary>
    /// `top`: per-thread CPU over a window + one snapshot stack for each of the busiest threads.
    /// </summary>
    /// <param name="pid">The target process id.</param>
    /// <param name="seconds">The window length.</param>
    /// <returns>The process exit code (0 = success).</returns>
    int RunTop(DWORD pid, int seconds)
    {
        // One thread of the census: identity + its CPU at the window start and its delta at the end.
        struct Row
        {
            DWORD tid;     // the thread id
            HANDLE handle; // query-only handle (no context, no suspend), closed at the end of the run
            double cpu0;   // kernel + user CPU ms at the window start; -1 when unreadable
            double cpuMs;  // CPU ms used inside the window (0 when either read failed)
        };
        std::vector<Row> rows;
        for (const DWORD tid : ListThreads(pid))
        {
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            if (h)
            {
                rows.push_back({ tid, h, ThreadCpuMs(h), 0 });
            }
        }
        const ULONGLONG t0 = GetTickCount64();
        Sleep(static_cast<DWORD>(seconds) * 1000);
        const double wallMs = static_cast<double>(GetTickCount64() - t0);
        for (auto& row : rows)
        {
            const double now = ThreadCpuMs(row.handle);
            row.cpuMs = (now < 0 || row.cpu0 < 0) ? 0 : now - row.cpu0;
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.cpuMs > b.cpuMs; });
        double total = 0;
        for (const auto& row : rows)
        {
            total += row.cpuMs;
        }
        wprintf(L"pid %lu  threads %zu  window %.1fs  process CPU %.1f ms/s (%.1f%% of one core)\n\n", pid, rows.size(), wallMs / 1000,
                total * 1000 / wallMs, total * 100 / wallMs);
        SymbolCache symbols;
        int shown = 0;
        for (const auto& row : rows)
        {
            if (shown++ >= 25 || row.cpuMs <= 0)
            {
                break;
            }
            wprintf(L"tid %6lu  cpu %7.2f ms/s  start %s", row.tid, row.cpuMs * 1000 / wallMs, StartAddressOf(row.handle, symbols).c_str());
            const std::wstring description = DescriptionOf(row.handle);
            if (!description.empty())
            {
                wprintf(L"  [%s]", description.c_str());
            }
            wprintf(L"\n");
            SampledThread thread;
            StackCopy copy;
            if (OpenSampledThread(row.tid, thread) && CaptureStack(thread, copy))
            {
                const auto frames = symbols.Logical(WalkCopy(thread, copy));
                for (size_t i = 0; i < frames.size() && i < 48; ++i)
                {
                    wprintf(L"      %s%s%s\n", frames[i]->function.c_str(), frames[i]->line.empty() ? L"" : L"  ", frames[i]->line.c_str());
                }
            }
            if (thread.handle)
            {
                CloseHandle(thread.handle);
            }
        }
        for (auto& row : rows)
        {
            CloseHandle(row.handle);
        }
        return 0;
    }

    /// <summary>Prints the top <paramref name="limit"/> entries of a count map, as % of a denominator.</summary>
    /// <param name="title">The section title.</param>
    /// <param name="counts">Name → sample count.</param>
    /// <param name="denominator">The sample count the first percentage column is taken of.</param>
    /// <param name="all">The total sample count (the second percentage column).</param>
    /// <param name="limit">How many rows to print.</param>
    void PrintHistogram(const wchar_t* title, const std::map<std::wstring, int>& counts, size_t denominator, size_t all, int limit)
    {
        wprintf(L"\n--- %s ---\n", title);
        std::vector<std::pair<std::wstring, int>> sorted(counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        int shown = 0;
        for (const auto& [name, count] : sorted)
        {
            if (shown++ >= limit)
            {
                break;
            }
            wprintf(L"%6.1f%% %6.1f%%  %s\n", denominator ? count * 100.0 / denominator : 0.0, all ? count * 100.0 / all : 0.0, name.c_str());
        }
    }

    /// <summary>
    /// `sample`: samples each thread every <paramref name="periodMs"/> for <paramref name="seconds"/>
    /// and prints the per-thread profile (see the file header).
    /// </summary>
    /// <param name="pid">The target process id.</param>
    /// <param name="seconds">The run length.</param>
    /// <param name="periodMs">The pause between rounds (all threads are sampled each round).</param>
    /// <param name="tids">The threads to sample.</param>
    /// <returns>The process exit code (0 = success, 1 = no thread could be opened).</returns>
    int RunSample(DWORD pid, int seconds, int periodMs, const std::vector<DWORD>& tids)
    {
        // One sampled thread's run: its raw stacks (symbolized only at report time) and the costs.
        struct Profile
        {
            SampledThread thread;                     // the open thread (handle closed after its report)
            std::vector<std::vector<ULONG64>> stacks; // one raw walk per sample, leaf first
            double cpu0 = 0;                          // thread CPU ms at the run start (-1 = unreadable)
            double cpuMs = 0;                         // thread CPU ms inside the run — the ground truth the
                                                      // on-CPU sample share should roughly match
            int captureFails = 0;                     // GetThreadContext / stack-copy failures (an exited thread)
            double captureMsTotal = 0; // time spent in GetThreadContext + the stack copy (this process)
            double captureMsMax = 0;
            double walkMsTotal = 0;    // time spent in StackWalk64 over the copy (this process)
            double walkMsMax = 0;
            size_t stackBytesMax = 0;  // the largest stack copy, to spot a runaway RSP-to-base span
        };
        std::vector<Profile> profiles;
        for (const DWORD tid : tids)
        {
            Profile p;
            if (!OpenSampledThread(tid, p.thread))
            {
                wprintf(L"tid %lu: cannot open / read its TEB (gle=%lu) - skipped\n", tid, GetLastError());
                if (p.thread.handle)
                {
                    CloseHandle(p.thread.handle);
                }
                continue;
            }
            p.cpu0 = ThreadCpuMs(p.thread.handle);
            profiles.push_back(std::move(p));
        }
        if (profiles.empty())
        {
            return 1;
        }

        LARGE_INTEGER frequency{}, start{}, now{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        StackCopy copy;
        long long rounds = 0;
        for (;;)
        {
            QueryPerformanceCounter(&now);
            const double elapsed = double(now.QuadPart - start.QuadPart) / double(frequency.QuadPart);
            if (elapsed >= seconds)
            {
                break;
            }
            for (auto& p : profiles)
            {
                LARGE_INTEGER q0{}, q1{}, q2{};
                QueryPerformanceCounter(&q0);
                const bool captured = CaptureStack(p.thread, copy);
                QueryPerformanceCounter(&q1);
                const double captureMs = double(q1.QuadPart - q0.QuadPart) * 1000.0 / double(frequency.QuadPart);
                p.captureMsTotal += captureMs;
                p.captureMsMax = (std::max)(p.captureMsMax, captureMs);
                if (captured)
                {
                    p.stackBytesMax = (std::max)(p.stackBytesMax, copy.bytes.size());
                    p.stacks.push_back(WalkCopy(p.thread, copy));
                    QueryPerformanceCounter(&q2);
                    const double walkMs = double(q2.QuadPart - q1.QuadPart) * 1000.0 / double(frequency.QuadPart);
                    p.walkMsTotal += walkMs;
                    p.walkMsMax = (std::max)(p.walkMsMax, walkMs);
                }
                else
                {
                    ++p.captureFails;
                }
            }
            ++rounds;
            // The walk's own cost (first-touch module loads included) delays the next round; only
            // this process waits — the target never does.
            Sleep(static_cast<DWORD>(periodMs));
        }
        QueryPerformanceCounter(&now);
        const double wallSeconds = double(now.QuadPart - start.QuadPart) / double(frequency.QuadPart);
        for (auto& p : profiles)
        {
            const double cpu1 = ThreadCpuMs(p.thread.handle);
            p.cpuMs = (cpu1 < 0 || p.cpu0 < 0) ? 0 : cpu1 - p.cpu0;
        }

        SymbolCache symbols;
        for (auto& p : profiles)
        {
            const size_t total = p.stacks.size();
            size_t onCpu = 0, kernelCall = 0, waiting = 0;
            std::map<std::wstring, int> inclusiveAll, inclusiveOurs, leafAll, collapsed, waitLeaves, folded;
            for (const auto& pcs : p.stacks)
            {
                const auto stack = symbols.Logical(pcs);
                if (stack.empty())
                {
                    continue;
                }
                const auto& leaf = *stack.front();
                const SampleClass cls = Classify(leaf.function);
                leafAll[leaf.function]++;
                // Folded form (root first) for every sample, waits included, so the file can be cut
                // either way later.
                std::wstring fold;
                for (size_t i = stack.size(); i-- > 0;)
                {
                    fold += stack[i]->function;
                    if (i)
                    {
                        fold += L";";
                    }
                }
                folded[fold]++;
                if (cls == SampleClass::Waiting)
                {
                    ++waiting;
                    // Name the wait by the first frame of our code above it, so "waiting in our lock"
                    // and "idle in the pump" can be told apart.
                    std::wstring owner = leaf.function;
                    for (size_t i = 1; i < stack.size(); ++i)
                    {
                        if (IsOurModule(stack[i]->function))
                        {
                            owner += L"  <=  " + stack[i]->function;
                            break;
                        }
                    }
                    waitLeaves[owner]++;
                    continue;
                }
                (cls == SampleClass::OnCpu ? onCpu : kernelCall)++;
                std::set<std::wstring> seen;
                std::wstring signature, last;
                int ourFrames = 0;
                for (const auto* e : stack)
                {
                    if (seen.insert(e->function).second)
                    {
                        inclusiveAll[e->function]++;
                        if (IsOurModule(e->function))
                        {
                            inclusiveOurs[e->function]++;
                        }
                    }
                    if (IsOurModule(e->function) && ourFrames < 18 && e->function != last)
                    {
                        signature += e->function + (e->line.empty() ? L"" : L"  (" + e->line + L")") + L"\n";
                        last = e->function;
                        ++ourFrames;
                    }
                }
                if (signature.empty())
                {
                    signature = L"[no frame of ours]\n";
                    for (size_t i = 0; i < stack.size() && i < 6; ++i)
                    {
                        signature += stack[i]->function + L"\n";
                    }
                }
                collapsed[signature]++;
            }
            const size_t busy = onCpu + kernelCall;
            wprintf(L"\n================================================================================\n");
            wprintf(L"tid %lu: %zu samples in %.1fs (%lld rounds, pause %dms, %d capture failures)\n", p.thread.tid, total, wallSeconds, rounds,
                    periodMs, p.captureFails);
            wprintf(L"thread CPU in window: %.0f ms = %.1f ms/s (%.1f%% of one core)\n", p.cpuMs, p.cpuMs / wallSeconds, p.cpuMs / wallSeconds / 10);
            wprintf(L"sampler cost (this process): capture avg %.3f ms max %.3f ms; walk avg %.3f ms max %.3f ms; largest stack copy %zu KB\n",
                    total ? p.captureMsTotal / (total + p.captureFails) : 0.0, p.captureMsMax, total ? p.walkMsTotal / total : 0.0, p.walkMsMax,
                    p.stackBytesMax / 1024);
            wprintf(L"samples: on-CPU %zu (%.1f%%)  in-kernel-call %zu (%.1f%%)  waiting %zu (%.1f%%)\n", onCpu, total ? onCpu * 100.0 / total : 0,
                    kernelCall, total ? kernelCall * 100.0 / total : 0, waiting, total ? waiting * 100.0 / total : 0);
            PrintHistogram(L"(a) INCLUSIVE, every frame (% of busy samples / % of all), top 90", inclusiveAll, busy, total, 90);
            PrintHistogram(L"(b) INCLUSIVE, our frames only, top 70", inclusiveOurs, busy, total, 70);
            wprintf(L"\n--- (c) top collapsed busy stacks (our frames, leaf first), top 25 ---\n");
            {
                std::vector<std::pair<std::wstring, int>> sorted(collapsed.begin(), collapsed.end());
                std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                int shown = 0;
                for (const auto& [signature, count] : sorted)
                {
                    if (shown++ >= 25)
                    {
                        break;
                    }
                    wprintf(L"\n[%d samples, %.1f%% of busy]\n", count, busy ? count * 100.0 / busy : 0.0);
                    size_t pos = 0;
                    while (pos < signature.size())
                    {
                        size_t nl = signature.find(L'\n', pos);
                        if (nl == std::wstring::npos)
                        {
                            nl = signature.size();
                        }
                        wprintf(L"    %s\n", signature.substr(pos, nl - pos).c_str());
                        pos = nl + 1;
                    }
                }
            }
            PrintHistogram(L"(d) EXCLUSIVE leaf, all samples, top 40", leafAll, total, total, 40);
            PrintHistogram(L"(e) WAITS by leaf <= first frame of ours (% of waiting samples), top 25", waitLeaves, waiting, total, 25);

            wchar_t path[MAX_PATH];
            swprintf_s(path, L"stacksnap_%lu_%lu.folded", pid, p.thread.tid);
            FILE* file = nullptr;
            if (_wfopen_s(&file, path, L"w, ccs=UTF-8") == 0 && file)
            {
                for (const auto& [stack, count] : folded)
                {
                    fwprintf(file, L"%s %d\n", stack.c_str(), count);
                }
                fclose(file);
                wprintf(L"\nfolded stacks: %s\n", path);
            }
            CloseHandle(p.thread.handle);
        }
        return 0;
    }
}

/// <summary>
/// Entry point: parses the mode, opens the target read-only, initializes dbghelp on it and runs.
/// </summary>
/// <param name="argc">Argument count.</param>
/// <param name="argv">Arguments (see the file header).</param>
/// <returns>0 on success; 1 when the target cannot be opened or nothing could be sampled; 2 on a usage error.</returns>
int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        wprintf(L"usage:\n  stacksnap top <pid> [seconds=5]\n  stacksnap sample <pid> <seconds> <periodMs> <tid> [tid ...]\n");
        return 2;
    }
    const std::wstring mode = argv[1];
    const DWORD pid = static_cast<DWORD>(_wtoi(argv[2]));
    g_ntQueryInformationThread = reinterpret_cast<NtQueryInformationThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    g_proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!g_proc)
    {
        wprintf(L"OpenProcess(%lu) failed gle=%lu\n", pid, GetLastError());
        return 1;
    }

    // The search path: the target's own directory (layout PDBs sit beside the binaries), plus the
    // optional symbol server. Safe here because the target is never suspended (file header).
    wchar_t image[MAX_PATH] = L"";
    std::wstring searchPath;
    if (GetModuleFileNameExW(g_proc, nullptr, image, MAX_PATH))
    {
        searchPath = image;
        searchPath = searchPath.substr(0, searchPath.find_last_of(L'\\'));
    }
    wchar_t server[1024] = L"";
    if (GetEnvironmentVariableW(L"STACKSNAP_SYMSRV", server, 1024))
    {
        searchPath += L";";
        searchPath += server;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    if (!SymInitializeW(g_proc, searchPath.c_str(), TRUE))
    {
        wprintf(L"SymInitializeW failed gle=%lu (symbols degraded)\n", GetLastError());
    }
    wprintf(L"symbol path: %s\n", searchPath.c_str());

    int result = 2;
    if (mode == L"top")
    {
        result = RunTop(pid, argc > 3 ? (std::max)(1, _wtoi(argv[3])) : 5);
    }
    else if (mode == L"sample" && argc >= 6)
    {
        std::vector<DWORD> tids;
        for (int i = 5; i < argc; ++i)
        {
            tids.push_back(static_cast<DWORD>(_wtoi(argv[i])));
        }
        result = RunSample(pid, (std::max)(1, _wtoi(argv[3])), (std::max)(1, _wtoi(argv[4])), tids);
    }
    else
    {
        wprintf(L"unknown mode or missing arguments\n");
    }
    SymCleanup(g_proc);
    CloseHandle(g_proc);
    return result;
}
