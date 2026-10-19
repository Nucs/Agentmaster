# Leak hunt: memory forensics of a running Agentmaster, read-only

Tools to answer "is Agentmaster leaking, and what holds its memory?" from dumps of a RUNNING
instance, without cdb/WinDbg and without stopping the app. Built for the investigation of
2026-10-19 (production 0.6.8.22, 703 tabs); its findings are in
[`doc/agentmaster/MEMORY_LEAKS.md`](../../doc/agentmaster/MEMORY_LEAKS.md).

Build the two native tools once: `cmd /c tools\leakhunt\build.bat` (VS 2022 + the DIA SDK
beside it; `msdia140.dll` is loaded from the VS Community path in `heapcensus.cpp`). The Python
scripts need Python 3.8+ and nothing else; run them with `python -I`.

## The workflow

1. **Sample the counters** while the app runs: `python -I memwatch.py <pid> <out.csv> --interval 15 --hours 10`.
   One `PROCESS_QUERY_LIMITED_INFORMATION` handle; nothing is suspended or read. A leak is a SLOPE:
   compare quarter means, not single samples (private bytes swing ±60 MB with UI activity).
2. **Dump without stopping it:** `snapdump.exe <pid> <out.dmp>`. `PssCaptureSnapshot` with a VA clone
   pauses the target only while the clone is made (production: 82–230 ms for 2.1 GB private), then
   the dump is written from the clone (~10 s for 2.6 GB). It refuses when the system commit headroom
   is below the target's private bytes + 4 GB: the clone may charge that much commit, and commit
   exhaustion is what froze production on 2026-08-22. Handle names are not captured (a name query
   can block while the target is paused). Write dumps to Q:, never C: or K:. ⚠ A dump holds
   everything in the process (transcripts, prompts, the Telegram bot token): keep it local, delete it
   when done. It ends the clone itself and prints `clone terminated`; any other line there means a
   thread-less copy of the target (same image, the target as parent) may still hold commit - end it.
3. **Census by vftable:** `heapcensus.exe <dmp> --sym <layout folder> --sym Q:\symcache --sym Q:\tmp\symcache --refs --out census.tsv`.
   Every 8-byte slot of private read-write memory whose value is a known vftable (public `??_7`
   symbols of every module whose PDB matches the dump's RSDS GUID + age) counts one (sub)object of
   that class. Our PDBs ship in the layout folder the instance runs from; a system PDB it lists as
   `MISSING` can be fetched into `Q:\symcache\<pdb>\<GUID><age>\` with curl from the msdl URL it
   prints (offline from the dump: never point a symbol server at the live process). ⚠ A raw count
   includes FREED blocks the heap has not reused yet; `refd` (slots some other slot points at) is a
   better live estimate, and step 4 is the exact one.
4. **Walk the segment heap - live blocks only:** `python -I segheap.py <dmp> --globals-rva <RtlpHpHeapGlobals RVA> --census census.tsv --live-out live.tsv --blocks-out blocks.tsv`.
   Packaged apps run on the segment heap; this reads its LFH busy bitmaps and VS chunk headers
   (keys from `ntdll!RtlpHpHeapGlobals`, whose RVA comes from ntdll's PDB: 0x1CCEC0 on 26200) and
   reports the exact live blocks, live objects per class with their bytes, and the blocks without a
   vftable grouped by size. The decoding facts that differ from the 2016 paper are measured and
   commented in the script (segment signature, range flags, the LFH bitmap layout). Before trusting
   a walk on a new Windows build, check it against objects whose number you know: one live `Tab` per
   open tab, and `walk stats` with no `lfh-freecount-above-clear-bits`.
5. **Diff two dumps of the same process:** `python -I censustool.py live live-1.tsv live-2.tsv`. Growth
   of LIVE bytes per class / per block size is the leak test a single dump cannot give.
   `censustool.py bytes census.tsv` and `censustool.py diff old.tsv new.tsv` work on raw censuses.
6. **Identify what grew or what is big:**
   - `dumpview.py read <dmp> <hex> [bytes]` (hex + UTF-16), `dumpview.py near|hist <dmp> <slots>` (the
     strings an object's fields point at; slots from `heapcensus --slots <vftable substring> --slots-out`).
   - `heapcensus --findref <hex> --span <n>` (who points into a block), `--refs-of` (the same for
     hundreds of ranges in one scan), `--symat <module> <rva>` (name a vftable the census did not know).
   - `ownerchain.py <dmp> <blocks> <census> <hex>` follows one block's owners upward;
     `owners.py ... --class <substring> [--exclude ...]` histograms the owners of a whole population.

## Files

| File | What it is |
|---|---|
| `memwatch.py` | counter sampler (private bytes, working set, handles, threads, GDI/USER, system commit) |
| `snapdump.cpp` | PSS-snapshot minidump of a running process (full memory, or `--heap` for private RW only) |
| `heapcensus.cpp` | vftable census + refs pass + slots export + findref / refs-of / symat |
| `segheap.py` | segment-heap walker: live blocks, live classes, unmatched sizes |
| `censustool.py` | class folding, byte ranking, raw and live diffs |
| `dumpview.py` | read memory and object strings out of a dump |
| `ownerchain.py`, `owners.py` | ownership: one chain, or the owners of a population |
| `utext_clone_leak_repro.cpp` | standalone proof of the UTextAdapter clone leak and its fix (system ICU) |
| `build.bat` | builds `snapdump.exe` and `heapcensus.exe` |

## Traps met while building these

- DIA: a batched `IDiaEnumSymbols::Next(256)` over ntdll's public PDB never ended (16 M "publics");
  enumerate with `Next(1)` bounded by `get_Count`. A symbol store names its FOLDERS `<x>.pdb`: hand
  DIA only regular files.
- The vftable of a template family can be ICF-folded: `CEventSource<...>` is one address shared by
  every XAML event-source instantiation, so its row counts all of them (heapcensus prints every
  folded name joined with " | ").
- `--sizes` (block size from the most common gap between same-type slots) is only right for
  densely allocated types; use `segheap.py` for real sizes.
- In `owners.py` output, "(outside the heap)" includes STALE pointers left in freed heap memory, not
  only stacks and globals.
- ⚠ The 26200 LFH busy bitmap is NOT the 2016 paper's two interleaved bits per block: it is one qword
  per 32 blocks, the low dword their busy bits, the high dword their unused-bytes bits. Read the
  paper's way it marks about half of the live blocks free: until 2026-10-20 `segheap.py` did, and
  called 313 of 706 live `Tab` objects free, halved the `UText` leak and invented an "unowned pool"
  of DirectWrite buffers whose owners it had misread (MEMORY_LEAKS.md §7).
- `PssFreeSnapshot` does not always end a VA clone: production's fourth dump left a thread-less clone
  holding ~1.8 GB of commit. Terminate it through a handle opened by its pid (the snapshot's own
  handle lacks the rights) before freeing the snapshot, as `snapdump.cpp` does. An ENDED clone can
  still be listed (0 threads, ~84 KB private, `GetExitCodeProcess` even reads STILL_ACTIVE) while some
  process holds a handle to it: before ending a listed clone again, open it with SYNCHRONIZE and check
  `WaitForSingleObject(h, 0)` - 0 means it is already gone.
