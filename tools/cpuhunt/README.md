# CPU hunt: where a RUNNING Agentmaster spends its CPU, read-only

Scripts for "is Agentmaster wasting CPU, and on what?", made for the measurement of 2026-10-20
(production 0.6.8.22, ~700 tabs). Its findings are in
[`doc/agentmaster/UI_THREAD.md`](../../doc/agentmaster/UI_THREAD.md) §13. Nothing here suspends, attaches
to, or writes into the target: every script reads the kernel's process list (one
`NtQuerySystemInformation` call per poll) or a file the profiler wrote.

They complete the two existing tools: `tools/stacksnap.cpp` (per-thread CPU and stack samples without
suspending a thread) and `tools/threadcensus.cs` (per-thread CPU and wake-ups by start module, the XAML
frame thread, render threads, GPU devices). Run the scripts with `python -I`.

| Script | What it answers |
|---|---|
| `proccpu.py <pid> <seconds>` | Who uses the CPU: the target, its process tree (sessions, ConPTY hosts, shells, short-lived helpers such as hook forwarders) and the rest of the machine. Polls every 250 ms, so processes that live inside the window are counted. |
| `wakes.py <pid> <frame tid>` | Is XAML's frame loop driven by an animation (steady 60 Hz runs) or by property changes (bursts and gaps)? Which threads move with it (correlation per 100 ms)? |
| `uiclass.py <stacksnap .folded> [ms/s]` | What the UI thread did, by activity: the frame tick's composition walk / animation tick / layout, our dispatcher lanes by name, input. |

Dump-side companion: `tools/leakhunt/xamltree.py` attributes XAML's composition nodes (what every frame
walks) and its measured TextBlocks (~17 KB of DirectWrite state each) to their places in the element tree.

## The order that worked

1. `proccpu.py` for 60 s: is it the app, its sessions, or the machine? Subtract the measuring shell when the
   session runs inside the target (its `claude.exe`, `bash`, `python` are in the tree).
2. `stacksnap top <pid> 10`: the busy threads and what each one is (its owning loop is in the stack).
3. `threadcensus` 20 s: the XAML frame thread's id and wake rate.
4. `stacksnap sample <pid> 60 3 <tids…>` on the busy threads, then `uiclass.py` on the UI thread's folded file.
5. `wakes.py` when the frame thread is busy, then `xamltree.py nodes` on a dump to see what each frame walks.
6. For a busy background thread: read its own frames in the sample's section (b), then the code.

## Traps

- **Copy the SDK's `dbghelp.dll` and `symsrv.dll` next to `stacksnap.exe`** (from
  `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64`), in a scratch folder: the system dbghelp cannot use
  the `srv*` cache. A symbol server is safe with stacksnap (it never suspends the target), never with
  `tools/livesample.exe`.
- **About 30% of the UI thread's samples end before the message pump:** the composition walk's internals
  (dcomp, CoreMessaging, critical sections). `uiclass.py` counts them as the walk's pieces; read the shares as
  a profile, not as exact values.
- **A big `ms/s` for a waiting thread in `stacksnap top`** is its CPU over the window; the one stack it prints
  is a single snapshot, often the wait. Sample it to see where the time goes.
