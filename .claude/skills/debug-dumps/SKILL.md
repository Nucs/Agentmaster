---
name: debug-dumps
description: |
  Analyze and debug Windows crash dumps (.dmp) and app crashes for Agentmaster (the C++/WinRT
  WindowsTerminal fork) WITHOUT cdb/WinDbg — using the in-repo DIA-SDK + DbgHelp tools. Covers the whole
  loop: find the crash (WER Application Error event id 1000 + the full dump in %LOCALAPPDATA%\CrashDumps),
  read the exception record + faulting registers (dumpexc), decode what it means (0xC0000005 access
  violation, 0xC0000409 __fastfail incl. the CFG subcode 0xA freed-vtable, 0xC000027B stowed fail-fast,
  the 0xDDDDDDDD "dead-land" freed-memory register signature, non-canonical -1 faultData), walk the
  faulting-thread stack (dumpwalk2 ordered StackWalkEx; dumpourscan full-stack scan for OUR frames with a
  module histogram; dumpstack/hangwalk fallbacks), and symbolize OUR frames against the matching build's
  PDBs to pin the exact source line + root cause. Encodes the hard-won patterns: matching the PDB to the
  CRASHED binary by build time, "zero of our frames == a deferred/async XAML fail-fast off our call stack
  (uncatchable)", use-after-free of reused XAML peers, fire_and_forget std::terminate, iterator/UAF
  classes, and how a try/catch canNOT catch an AV (/EHsc) or a __fastfail (bypasses SEH). Includes a
  self-contained crasher (makedump) that generates a tiny, non-sensitive test dump reproducing the
  freed-fill AV signature, plus that compressed fixture (test/sample-crash.dmp.gz). Use whenever an
  Agentmaster (or any native Windows app) instance crashes, hangs, or fail-fasts, when asked to analyze a
  .dmp / minidump / WER report, find a crash's root cause, symbolize a fault offset, or reason about an
  exception code / access violation / stowed exception / __fastfail.
keywords: crash, dump, minidump, .dmp, WER, Windows Error Reporting, CrashDumps, access violation, 0xC0000005, 0xC0000409, fastfail, __fastfail, STATUS_STACK_BUFFER_OVERRUN, 0xC000027B, STATUS_STOWED_EXCEPTION, stowed exception, 0xc000041d, STATUS_FATAL_USER_CALLBACK_EXCEPTION, CFG, FAST_FAIL_GUARD_ICALL_CHECK_FAILURE, use-after-free, UAF, dead-land, 0xDDDDDDDD, freed memory, dbghelp, DIA SDK, msdia140, StackWalkEx, symbolize, PDB, TerminalApp.pdb, fire_and_forget, std::terminate, XAML Islands, ToolTip, dumpexc, dumpwalk2, dumpourscan, dumpstack, hangwalk, diasym, makedump, hang, deadlock, faulting thread, exception record, root cause, no cdb, no WinDbg
---

# Debugging crashes & dumps (no cdb/WinDbg)

The battle-tested loop for finding the root cause of an Agentmaster (or any native Windows-app) crash on a
box with **no Debugging Tools for Windows installed** — using the DIA SDK + DbgHelp that ship with VS 2022.
Born from a run of C++/WinRT XAML-Islands crashes (tooltip use-after-frees, deferred fail-faults,
fire_and_forget terminates) diagnosed entirely from dumps.

## 0. Prerequisites (one-time)

- **VS 2022** with the C++ workload (any edition) — provides `cl.exe`, `dbghelp.lib`, and the **DIA SDK**
  (`<VS>\DIA SDK\include\dia2.h`) + `msdia140.dll`. No cdb/WinDbg needed.
- Build the tools once: `cmd /c .claude\skills\debug-dumps\scripts\build.bat` → the 6 analysis `.exe` + the
  `makedump` fixture generator land in `scripts\`. `build.bat` auto-detects VS via `vswhere` (falls back to
  Community) and finds the DIA SDK beside it. Re-run any time.
- **Git Bash note:** when passing Windows paths to these `.exe`, prefix the command with `MSYS_NO_PATHCONV=1`
  and use `\\`-escaped absolute paths, or MSYS mangles them.
- **msdia140.dll path:** the DIA tools `NoRegCoCreate` it from a hardcoded VS-Community path
  (`...\Common7\IDE\Automation\msdia140.dll`). On a non-Community edition, edit `kMsdia` in
  `dumpourscan.cpp`/`dumpstack.cpp`/`diasym.cpp` and rebuild.

## 1. Find the crash

Two artifacts. Get both:

```bash
# (a) The WER Application Error event — module + exception code + fault offset + package + timestamp:
powershell -NoProfile -Command "Get-WinEvent -FilterHashtable @{LogName='Application';Id=1000;StartTime=(Get-Date).AddHours(-1)} | ? { \$_.Message -like '*WindowsTerminal*' } | Select -First 3 | % { \$_.Message }"

# (b) The full dump WER wrote:
ls -la --time-style=full-iso "$LOCALAPPDATA/CrashDumps"/*.dmp | sort -k6,7 | tail
```

The WER event's `Faulting application path` + `package full name` tell you WHICH install crashed (Debug
`AgentmasterDev` vs Release `Agentmaster`), and its version. The dump filename is `<image>.<pid>.dmp`
(pid in decimal; the WER event prints it in hex — convert to match).

## 2. Match the PDB to the CRASHED binary — do this FIRST

Symbolization is only correct if the PDB matches the exact binary that crashed. **Compare timestamps:**

```bash
ls -la --time-style=full-iso src/cascadia/CascadiaPackage/bin/x64/Debug/TerminalApp.{dll,pdb}
git log --format='%h %ci %s' -1 <your-fix-commit>   # did the crashed binary even contain your fix?
```

If the crashed binary was built AFTER a fix commit, that fix was present and **insufficient** — a real
signal (this is how we proved 5 tooltip fixes were each incomplete). Debug PDBs are in
`bin\x64\Debug`, Release in `bin\x64\Release`. Pass that dir as `<bindir>` to the tools.

## 3. Read the exception — `dumpexc`

```bash
MSYS_NO_PATHCONV=1 ./scripts/dumpexc.exe "C:\path\to\crash.dmp"
```

Prints the exception code, `ExceptionInformation[]`, the decoded `__fastfail` subcode, and **all faulting
registers** + which module holds RIP. The registers are the highest-signal evidence — see §6.

## 4. Walk the faulting-thread stack

Three strategies, in order of preference — real optimized/stripped stacks defeat any single one, so try
them all and corroborate:

```bash
# (a) ORDERED walk via DbgHelp StackWalkEx (needs each module's .pdata; serves memory from the dump).
#     Best when it works; can stop short inside a module with no unwind info (e.g. MUX/system XAML).
MSYS_NO_PATHCONV=1 ./scripts/dumpwalk2.exe "<dmp>" "<bindir-with-PDBs>"

# (b) FULL-STACK SCAN for OUR frames — scans 512 KiB above RSP for return addresses into our modules,
#     symbolized via DIA, + an exception summary + a module histogram. Includes STALE frames (it's a
#     scan, not an unwind) so read it as "which of our functions are near the crash", corroborated by (a).
#     Optional 3rd arg = the "our module" substring (default: the Agentmaster set) for any app.
MSYS_NO_PATHCONV=1 ./scripts/dumpourscan.exe "<dmp>" "<bindir>" [ourModuleSubstring]

# (c) Fallbacks: dumpstack (naive scan of the inline thread-stack) / hangwalk (per-thread, for HANG dumps
#     with NO exception stream — procdump -h). diasym symbolizes a single module RVA (e.g. a WER offset).
MSYS_NO_PATHCONV=1 ./scripts/hangwalk.exe "<dmp>" "<imageDir>" "<pdbDir>" ALL SCAN
MSYS_NO_PATHCONV=1 ./scripts/diasym.exe "<bindir>\TerminalApp.pdb" 0x1234AB
```

## 5. The #1 interpretation rule — "zero of our frames"

If `dumpourscan` reports **0 OUR-module return addresses** on the faulting thread (histogram is all
`Windows.UI.Xaml.dll` / `Microsoft.UI.Xaml.dll` / `CoreMessaging.dll` / GPU `nvwgf2umx.dll` / the message
pump), the fault is **NOT synchronously called from our code** — it is a **deferred / async framework pass**
(XAML layout / render / composition / input routing) choking on state we set on an EARLIER, already-unwound
stack. Consequences, learned the hard way:

- The trigger is off your stack, so **no `try/catch` you add can catch it.** (And note: a `try/catch`
  can't catch an **access violation** at all under `/EHsc`, nor a **`__fastfail`** which bypasses SEH by
  design — regardless of stack.)
- Correlate by the **repro + exception type + timing** instead of a clean call chain. `0xC000027B` during a
  render pass while hovering tabs == the tooltip; an AV in a MUX TabView method during input == a recycled
  container; etc.
- The fix is **structural** — never leave the bad state for the async pass to find (drop/detach the object
  at teardown; don't reopen a torn-down peer) — not a guard at the (wrong) call site.

When `dumpourscan` DOES show our frames (e.g. a synchronous AV), the chain is the answer — read top-down.

## 6. Reading the evidence — exception codes & register signatures

Quick reference (full table in `reference/exception-codes.md`):

- **`0xC0000005` — access violation.** `ExceptionInformation[0]` = 0 read / 1 write / 8 execute;
  `[1]` = the faulting data address. `[1] == 0` → null deref. `[1] == 0xFFFFFFFFFFFFFFFF` → a
  **non-canonical** address (a garbage/freed pointer with high bits set → a GP fault reported as -1).
- **`0xDDDDDDDDDDDD....` in a faulting register (Rax/Rcx/…)** → the MSVC **debug-CRT freed-block fill**
  ("dead-land", byte `0xDD`). A register holding it == the code dereferenced **freed memory** →
  **use-after-free.** (`0xCDCDCDCD` = uninitialized heap; `0xFEEEFEEE` = freed by HeapFree; `0xBAADF00D`
  = uninitialized LocalAlloc.) This one register is often the whole diagnosis.
- **`0xC0000409` — `__fastfail` / STATUS_STACK_BUFFER_OVERRUN.** NOT always a stack overrun — it's the
  generic fast-fail. `ExceptionInformation[0]` is the **subcode**: `2` = /GS stack-cookie corruption,
  `7` = `FATAL_APP_EXIT` (a `std::terminate` / `winrt::terminate` — e.g. an exception escaping a
  `fire_and_forget` or a `noexcept`), **`0xA` = `GUARD_ICALL_CHECK_FAILURE`** (CFG caught an
  indirect/virtual call through a **freed/corrupt vtable** → also a UAF, just caught at the call).
- **`0xC000027B` — STATUS_STOWED_EXCEPTION.** A WinRT/XAML **fail-fast**: an internal error was "stowed"
  then the process fail-fasts (via `KERNELBASE!RaiseException`). Classic under XAML Islands for an
  **owner-less / unplaceable popup** (tooltips). Uncatchable; almost always off-stack (§5).
- **`0xC000041D` — STATUS_FATAL_USER_CALLBACK_EXCEPTION.** An exception thrown through a kernel→user
  callback (a window proc / XAML callback). Usually the **escalation** of a primary AV a moment earlier —
  find the earlier dump/event at the same offset (that's the root; this is the death rattle).
- **`0xC0000374` — STATUS_HEAP_CORRUPTION.** `ExceptionInformation[0]` points at ntdll's
  `HEAP_FAILURE_INFORMATION` — read it from a FULL dump with `dumpmem` (`{u32 Version; u32 Size;
  u32 FailureType; …; pvoid Heap; pvoid Address; …}`), then hexdump the block it names (overrun bytes
  often contain the WRITER'S data). The corruption was planted EARLIER than the reporting stack — the
  detector fires on a later alloc/free. A named Address "not present in Memory64ListStream" on a full
  dump == a **decommitted page** (heap metadata pointing into freed space). **Before deep heap
  forensics:** a DETERMINISTIC startup `0xC0000374` inside the XamlTypeInfo / type-activation machinery,
  on a binary built incrementally AFTER a mid-compile out-of-memory failure (C1076 "internal heap limit"
  / C3859 "Failed to create virtual memory for PCH"), has been a **poisoned incremental build** — the
  failed run leaves ABI-mixed objs/tlogs and the next incremental link mixes them (verified 2026-07:
  three identical startup crashes, zero changed code on the stack). Run the discriminating experiment
  first: purge `obj/x64/<cfg>/TerminalApp*` + both `Generated Files` dirs + the layout `resources.pri`,
  clean-rebuild, relaunch — vanishing with zero source changes == build state, not code.

## 7. Common Agentmaster crash CLASSES + the fix shape (from real dumps)

- **Reused XAML peer use-after-free.** A held WinRT strong ref keeps a *projection* alive while the
  framework tore down its *native peer* (owner recycled, popup closed async). Calling any method → AV
  (`0xDDDD`) or CFG fail-fast (`0xA`). Fix: **drop + detach** the object at every teardown; **create fresh
  per use**; never reopen across a close; serialize open-after-close across siblings.
- **`fire_and_forget` that can `std::terminate`.** Any exception escaping a `winrt::fire_and_forget`
  fatal-fails (`0xC0000409` subcode 7). Fix: wrap the body in `try/catch(...)`. If it must `co_await`
  mid-body, make the body an `IAsyncAction` and `co_await` it inside the shell's `try` (co-awaiting an
  IAsyncAction *propagates* the exception to the awaiter instead of terminating).
- **Iterator / map-node use-after-free.** Using a `[key, val]` structured-binding reference after
  `erase(key)` hashes freed memory. Fix: copy the key first.
- **Eager `connection.Start()` before control init** → AV in `Microsoft.Terminal.Control.dll` on startup.
  (See the project CLAUDE.md "Gotchas".)

## 8. Confidence & honesty

State confidence explicitly. A register-level proof (`0xDDDD` freed-fill, a decoded fastfail subcode) is
**conclusive**. A `dumpourscan` chain is **strong** but includes stale frames — corroborate with
`dumpwalk2`. An off-stack fail-fast (§5) tied to a repro is **circumstantial** — say so, and prefer a
structural fix over a point guard. If a fix ships and the SAME crash recurs on a binary built AFTER it
(§2), the hypothesis was wrong or incomplete — escalate to the structural/architectural fix, don't
re-patch the same spot.

## 9. Testing the toolchain

A self-contained fixture — a tiny crasher (`scripts/makedump.cpp`) that reproduces the freed-fill AV
signature and writes a small, **non-sensitive** dump — plus that compressed dump. See `test/README.md`:

```bash
gunzip -k test/sample-crash.dmp.gz                                   # or: ./scripts/makedump.exe test\sample-crash.dmp
MSYS_NO_PATHCONV=1 ./scripts/dumpexc.exe   "<abs>\test\sample-crash.dmp"                    # AV, Rcx=0xDDDD.., faultData=-1
MSYS_NO_PATHCONV=1 ./scripts/dumpwalk2.exe "<abs>\test\sample-crash.dmp" "<abs>\scripts"    # FaultDeep -> wmain (+ source lines)
MSYS_NO_PATHCONV=1 ./scripts/dumpourscan.exe "<abs>\test\sample-crash.dmp" "<abs>\scripts" makedump
```

## Files

- `scripts/dumpexc.cpp` — exception record + `ExceptionInformation` + decoded fastfail subcode + faulting registers + module-of-RIP.
- `scripts/dumpwalk2.cpp` — ordered `StackWalkEx` walk; serves stack/memory from the dump; DbgHelp symbols.
- `scripts/dumpourscan.cpp` — 512 KiB stack scan for OUR frames (DIA-symbolized) + exception params + module histogram; optional module filter.
- `scripts/dumpstack.cpp` — naive inline-stack scan + DIA symbolize (fallback for small/odd dumps).
- `scripts/dumpmem.cpp` — hexdump arbitrary virtual memory FROM a full dump (`dumpmem <dmp> <hexAddr> <bytes>`; hex + wchar + ascii columns) — the `0xC0000374` heap-forensics companion (read the `HEAP_FAILURE_INFORMATION`, then the corrupt block). Build: `cl /nologo /std:c++20 /EHsc /W3 dumpmem.cpp` in `scripts\` (no DIA needed).
- `scripts/hangwalk.cpp` — per-thread walk/scan for HANG dumps (no exception stream; procdump `-h`).
- `scripts/diasym.cpp` — symbolize a single `<pdb> <rva>` (e.g. a WER fault offset).
- `scripts/makedump.cpp` — the fixture generator (a safe, tiny crash dump reproducing the UAF signature).
- `scripts/build.bat` — build them all (VS + DIA auto-detect).
- `reference/exception-codes.md` — the full code / register-signature reference.
- `test/sample-crash.dmp.gz` + `test/README.md` — the compressed test fixture + how to use it.
