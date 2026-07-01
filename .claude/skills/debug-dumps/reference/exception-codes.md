# Exception codes & register signatures — reference

Companion to `../SKILL.md`. The codes and memory-fill patterns you'll actually see in Agentmaster dumps.

## Exception codes

| Code | Name | What it means | Where to look |
|------|------|---------------|---------------|
| `0xC0000005` | ACCESS_VIOLATION | Bad read/write/execute. `ExceptionInformation[0]`=0 read /1 write /8 exec; `[1]`=faulting data address. | `[1]==0` null deref; `[1]==0xFFFF…FFFF` non-canonical (garbage/freed ptr); else the bad address — check which register holds it. |
| `0xC0000409` | STACK_BUFFER_OVERRUN / **`__fastfail`** | Generic fast-fail (NOT only /GS). `ExceptionInformation[0]` = subcode (table below). Bypasses SEH — **uncatchable** by try/catch. | Decode the subcode. `7`=terminate/abort; `0xA`=CFG freed-vtable; `2`=stack cookie. |
| `0xC000027B` | **STOWED_EXCEPTION** | WinRT/XAML stowed an internal error, then fail-fasts (via `KERNELBASE!RaiseException`). Uncatchable; usually off-stack. | Classic XAML-Islands owner-less/unplaceable popup (tooltips). Correlate by repro + timing (§5). `ExceptionInformation[0]` points at a `STOWED_EXCEPTION_INFORMATION` (nested HRESULT). |
| `0xC000041D` | FATAL_USER_CALLBACK_EXCEPTION | Exception thrown through a kernel→user callback (window proc / XAML callback). | Usually the **escalation** of a primary AV moments earlier — find the earlier dump at the same offset; that's the root cause. |
| `0xC00000FD` | STACK_OVERFLOW | Real stack exhaustion (unbounded recursion). | Look for a repeating frame cycle in the walk. |
| `0x80000003` | BREAKPOINT | `__debugbreak` / a tripped assert / `RoFailFastWithErrorContext`. | The message often names the failing check. |
| `0xC0000374` | HEAP_CORRUPTION | Heap metadata corrupted (an earlier overflow/double-free). | The corrupting write already happened; enable PageHeap (`gflags`) to catch it live. |

## `__fastfail` subcodes (`0xC0000409`, `ExceptionInformation[0]`)

`dumpexc` decodes these. The ones that actually show up:

| Subcode | Name | Meaning |
|---------|------|---------|
| `2` | STACK_COOKIE_CHECK_FAILURE | /GS stack cookie corrupted — a stack buffer overflow clobbered the return frame. |
| `3` | CORRUPT_LIST_ENTRY | A `LIST_ENTRY` (often a critical section / handle table) was corrupted. |
| `5` | INVALID_ARG | An API's fast-fail on a bad argument. |
| `7` | FATAL_APP_EXIT | `std::terminate` / `abort` / `winrt::terminate` — **an exception escaped a `noexcept` / `fire_and_forget`**, or an explicit fatal exit. |
| **`0xA` (10)** | **GUARD_ICALL_CHECK_FAILURE** | **CFG** caught an indirect/virtual call to an address not in the valid-target map → a call **through a freed or corrupted vtable** (a use-after-free, caught at the call site). |
| `0xB` (11) | GUARD_WRITE_CHECK_FAILURE | CFG write-guard: a write through a bad pointer. |
| `0xE` (14) | INVALID_REFERENCE_COUNT | A COM/WinRT refcount underflow (over-release). |

## Debug-memory fill patterns (a register or faultData holding one of these == that kind of bad memory)

| Pattern | Meaning |
|---------|---------|
| `0xDDDDDDDDDDDDDDDD` (byte `0xDD`) | **MSVC debug-CRT FREED block** ("dead-land"). A register/pointer holding `0xDDDD…` → **use-after-free** of a debug-heap object. (Often `0xDDDD…0000` — a freed object base whose low offset field was read.) |
| `0xCDCDCDCD` (byte `0xCD`) | Debug-CRT **uninitialized heap** (`new`'d but not set). "Clean memory". |
| `0xFDFDFDFD` (byte `0xFD`) | Debug-CRT **no-man's-land** guard bytes around an allocation → a **buffer overrun/underrun**. |
| `0xFEEEFEEE` | Freed by `HeapFree` (the OS heap free fill, non-debug). |
| `0xBAADF00D` | Uninitialized memory from `LocalAlloc(LMEM_FIXED)`. |
| `0xCCCCCCCC` | Uninitialized **stack** (/GZ or debug) — or int3 padding in code. |
| `0xFFFFFFFFFFFFFFFF` as an AV faultData | Not a fill — a **non-canonical** address (x64 requires bits 63:48 == bit 47). A garbage pointer with high bits set faults as a GP → reported as `-1`. Check the register that fed the deref for the real garbage value. |

## The interpretation shortcuts that paid off

- **Register `0xDDDD…` + AV** = use-after-free. Often the entire diagnosis from `dumpexc` alone.
- **`0xC0000409` subcode `0xA`** = the SAME use-after-free, hitting a virtual call instead of a data read.
- **`0xC000027B`, zero of our frames, GPU/composition modules on the stack** = deferred XAML fail-fast
  off our call stack — uncatchable; fix structurally, not at a call site (SKILL.md §5).
- **AV then a `0xC000041D` at the same offset seconds later** = one bug (the AV) + its escalation; debug
  the AV.
- **Crashed binary built AFTER your fix commit** = the fix was present and insufficient — real signal.
