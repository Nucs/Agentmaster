# Memory: leaks and what holds the memory (production, 2026-10-19 → 20)

The user's request: "investigate and detect agentmaster for memory leak, use dumps from release (keep
it running and access readonly)". Production was never stopped, written to, or attached to; four
snapshot dumps were read offline. The tools and the workflow: [`tools/leakhunt/README.md`](../../tools/leakhunt/README.md).

⚠ **Corrected on 2026-10-20** (§7): the first version of this file read the heap's LFH busy bitmap
in the layout of the 2016 paper, which marks about half of the live small blocks free. Every number
below comes from the corrected walk; the old values are listed in §7.

## 1. What was measured

- **The instance:** production 0.6.8.22 (the build of ec966a2ca, `Q:\agentmaster-layouts\Release-B`),
  pid 41148, started 2026-10-19 07:35:36; 4 windows holding 703 tabs (556 + 93 + 7 + 47) by their window
  records at the first dump. In its life by then: 705 tab insertions (the restore + 10 new sessions + 1
  fork), 2 tabs closed. Five more tabs were opened between the first dump and the last.
- **Counters** (`memwatch.py`, every 15 s, 2026-10-19 15:00:40 → 2026-10-20 01:00:53, 2,395 samples):
  - private bytes, hourly means: 2,104 → 2,111 → 2,120 → 2,123 → 2,128 MB from 15:00 to 19:59
    (about +6 MB an hour), then 2,157 (20:00–20:59), and 2,161–2,168 MB flat from 21:00 to 01:00;
  - one sample at 20:24:19 read 2,654 MB (+505 MB for that sample alone, threads 176 → 197); the next
    one read 2,180 MB. Thread bursts up to 279 followed at 20:25–20:27;
  - GDI objects 56 → 60 at 20:26, then 60 → 183 at 21:35:34 and flat to 01:00; USER objects 135 → 164;
    handles 2,784 → 3,009; threads 145 → 157. The cause of the GDI step is not established (bounded:
    it never moved again).
  - Peak working set at startup: 3.1 GB.
- **Dumps** (`snapdump.exe`, PSS VA clone): 15:02:51, 15:56:01, 16:35:12 and 01:02:19; the target paused
  139, 230, 82 and 132 ms; 2.6 GB each (`Q:\tmp\agentmaster-dumps\leak-20261019\`, with the censuses).
  ⚠ Dump 4's clone (pid 46688, no threads, its parent = production) was still alive after
  `snapdump.exe` had exited, holding ~1.8 GB of commit: `PssFreeSnapshot` alone does not always end it.
  It was terminated after its image, parent, creation time and thread count were checked again
  (production untouched; free commit 31.0 → 32.8 GB), and `snapdump.exe` now ends its clone itself and
  says so. The ended clone's process object is still listed at 02:15 (0 threads, 84 KB private,
  signaled), because some other process holds a handle to it.
- **Dev for comparison:** dev 0.0.1.31 (pid 78828), which holds the §3 fix, dumped at 01:22 (0.75 GB):
  123.8 MB of live heap.

## 2. Where the 2.1 GB of private memory is

| Part | MB (dumps 1 / 2 / 3 / 4) | Notes |
|---|---|---|
| Live heap blocks | 1,778 / 1,779 / 1,793 / 1,824 | 13.0 M → 13.3 M busy blocks: LFH 1,275 → 1,318 MB, VS 503 → 507 MB |
| The rest of the heap segments' committed memory | ~180 / 180 / 179 / 195 | the heap's own counter (`MemStats`, equal to the segment pages the dump holds) reads 1,958 → 2,020 MB committed: free blocks inside partly used subsegments, and metadata; whole free pages are only 1–5 MB |
| Outside the segment heap | ~78 | 24.6 MB is `SaveSessions`' cached JSON (§4.3; one of the heap's 32.6 MB of large allocations, which live outside its segments); 13.8 MB GPU-driver tables; 8 MB heap metadata |

Inside the live heap (dump 1; exact sizes from the heap's own metadata, `segheap.py`):

| Holder | Live MB | Count | What it is |
|---|---|---|---|
| DirectWrite text-analysis scratch buffers + layouts | ~504 | 30,683 × 16 KB buffers (479 MB) + 32,931 layouts | §4.1: 98.7% of the buffers held by a live text analyzer of a TextBlock's layout |
| Per-tab context menus and their tooltips | ~205 | 70,952 MenuFlyoutItems, 47,976 ToolTips | §4.2 |
| XAML event sources | 96.2 | 790,630 | one per (element, event) with a subscriber; a linker-folded vftable counts every `CEventSource<>` |
| Every other object with a vftable | ~404 | TextBlocks 31,935, brushes 73,973, FontIcons 29,039, Borders 21,679, StackPanels 17,231, Grids 16,656, … | the per-tab UI, side panels, board, Sessions page, our own objects |
| Leaked ICU `UText`s | 27.1 → 34.5 → 38.7 → 52.3 | 197,395 → 251,065 → 282,111 → 381,056 | §3 - the one leak |
| Everything else without a vftable | ~542 | 8.8 M small blocks | strings, vectors, property stores, engine records |

Our own objects match the open tabs (no closed-tab trees left behind), and every one is live: dump 1
holds 701 `Tab`, 701 `TermControl`, 701 `ControlCore`, 702 `TabHeaderControl`, 704 `AgentTabOverlay`;
dump 4 holds 706 / 706 / 706 / 707 / 709 (five more tabs); `AgentManagerContent` 4 for 4 windows. The
leak classes fixed on 2026-10-14 did not come back: 797 → 804 live `CScrollViewer`s follow the tab count
(UI_THREAD.md §12 had 2,920 board ScrollViewers alone).

## 3. The leak: one 144-byte ICU `UText` per URL scan (FIXED in source 2026-10-19; in dev since 0.0.1.31, not in production)

- **Evidence:** between the dumps only one population grew steadily: live 144-byte blocks holding a
  closed `UText` (magic `0x345AD82C` = UTEXT_MAGIC, flags 0, providerProperties 6, sizeOfStruct 0x90,
  pFuncs NULL, context = a `TextBuffer`; 65 → 75 distinct terminal buffers). Every such image but one
  sits in a busy 144-byte block (381,056 in dump 4; the other lies outside the heap's blocks), and the
  blocks' unused-bytes bit is clear, as an exact 144-byte `sizeof(UText)` request leaves it.

  | Dump | Leaked | Growth | Per hour |
  |---|---|---|---|
  | 1 (15:02:51) | 197,395 | since the start 7 h 27 min earlier | ~26,500 (the restore included) |
  | 2 (15:56:01) | 251,065 | +53,670 in 53 min | ~60,600 (8.3 MB) |
  | 3 (16:35:12) | 282,111 | +31,046 in 39 min | ~47,500 |
  | 4 (01:02:19) | 381,056 = 52.3 MB | +98,945 in 8 h 27 min | ~11,700 (a quiet evening) |

  No other population grew by more than ~7,500 blocks (0.8 MB) in the 92 afternoon minutes.
- **Cause:** `utextClone` in `src/buffer/out/UTextAdapter.cpp` (upstream Windows Terminal code; upstream
  main has the same code at our last fetch, 2026-07-27): `dest = utext_setup(dest, 0, status); memcpy(dest,
  src, sizeof(UText));`. `utext_setup` marks a clone ICU allocates `UTEXT_HEAP_ALLOCATED | UTEXT_OPEN`; the
  memcpy replaces those flags with the source's (a stack `UText` from `UTextFromTextBuffer`: `UTEXT_OPEN`
  only). `RegexMatcher::reset` clones every input through the provider, so when the matcher is closed
  (`uregex_close` → `utext_close`), ICU runs the close callback, NULLs pFuncs, and never frees the struct.
  `Terminal::_getPatterns` (URL detection, re-run after output in every live terminal) and
  `TextBuffer::SearchText` (Find) each create a matcher, give it a `UText` and close it: one leak per call.
  ICU's own `shallowTextClone` saves and restores `flags` and `pExtra` around its memcpy for this reason.
- **Proof outside the app:** `tools/leakhunt/utext_clone_leak_repro.cpp` runs `_getPatterns`' sequence
  against the system ICU: 200,000 scans cost +33 MB (165 bytes each) with the current clone, +0 with the
  fixed one; both find the URL in all 200,000.
- **Fix:** keep `dest`'s `flags`, `pExtra` and `extraSize` across the memcpy. The adapter keeps its state in
  `p`, `q`, `a`, `b`, never in them; the text buffer it AddRefs is released as before. A candidate to send
  upstream.
- **Dev:** dev 0.0.1.31's dump holds no closed adapter `UText` at all, which the fix predicts (a freed
  clone's magic is zeroed); dev had seen little terminal output (~6 hook events since 17:43), so that is
  not a proof by itself. The repro is.

## 4. Large and bounded: what scales with the number of tabs and sessions

None of these grows by itself; each is paid again for every tab, card or row, and stays until it closes.

### 4.1 A measured XAML TextBlock keeps ~17 KB of DirectWrite state

- The chain, read from the dump: a 16 KB scratch block ← `DWriteTextAnalyzer` (48 B) ← DirectWrite
  `TextLayout` (352 B) ⇄ `DWriteTextLayout` (384 B) ← `CTextBlock` (+0x1F8) or a DirectWrite
  `InlineLayout` (a TextBlock with Runs). The live heap holds 30,683 such 16 KB blocks = 479 MB, about a
  quarter of it. Owners (dump 1, every block attributed in one `--refs-of` pass): 30,295 (98.7%) are
  held by a live `DWriteTextAnalyzer` (33,298 analyzers are live); 276 by another live block; 112 by no
  live block at all.
- The texts of the live layouts name the surfaces: per-tab overlay labels ("waiting-for-you" 712, the
  model "opus-5.5" / "opus-4.8", the `folder / branch` subline), the side-panel and board cards (titles,
  working dirs, "created 2d, active 1d23h ago", "⚙ 3/3"), tab headers, and the Sessions page's rows.
- XAML builds the layout at the first measure and keeps it while the TextBlock lives, even when its
  page is hidden or the element is collapsed. The lever is fewer live measured TextBlocks: virtualize the
  long lists (side panel, Sessions table, board columns), release a hidden page's rows, and avoid
  TextBlocks that never change in the per-tab overlay.

### 4.2 Each tab builds its whole context menu up front

- `Tab::_CreateContextMenu` (Tab.cpp), called from the `Tab` constructor, builds about 97 items per tab
  (Copy ▸ Transcript ▸, Deactivate ▸, Close ▸, the two model submenus of 11 items each from
  `AgentFillModelPickItems`, …), and makes about 45 `ToolTipService::SetToolTip(item, box_value(text))`
  calls (two of them in loops) - each creates a XAML `ToolTip` + template settings + a size-changed
  handler at once. Measured (dump 1, 701 tabs): 70,952 MenuFlyoutItems (84.5 MB with their core
  objects), 11,384 separators, 7,767 submenus, 7,074 flyouts: 121 MB of menu objects; 47,976 ToolTips with
  their template settings and handlers: 84 MB (68 a tab: the menu's, the `TabViewItem`'s own, the six
  string tooltips on `TabHeaderControl.xaml`'s indicator icons, and others). ~205 MB, ~0.29 MB a tab,
  for menus that are rarely opened. (The Sessions page's row menus are built eagerly too: ~600 rows'
  menus were alive.)
- The fix shape is the Manager's own (UI_THREAD.md / manager-lens rebuild tax): an empty flyout filled in
  its `Opening` handler, and tooltips through `AgentSetTip` (no ToolTip object until a hover).

### 4.3 `SaveSessions` keeps a full copy of the last `sessions.json`

`static std::wstring s_last` (Persistence.cpp) skips identical writes by comparing the whole serialized
fleet: 9.4 M characters (18 MB) in a 24.6 MB allocation, growing with `sessions.json`, which is never
pruned. A 64-bit hash plus the length would do the same job.

### 4.4 The summary's upstream cache holds every line uuid of its chains

`CollectConversationUpstream`'s cache (`g_upstreamCache`, TranscriptTurns.cpp, at most 32 results) keeps
each result's `ConversationUpstream::uuids` set as well as its prompt texts. Dump 3 held 20 results with
16,709 uuids; dump 4 held 26 with 54,724 (~7.8 MB; one set alone 34,904). The +38,015 uuids are most of
the 48- and 80-byte blocks that grew next to the `UText`s between those dumps (a 48-byte set node and an
80-byte string buffer each). The cap counts results, not bytes, so one long chain holds tens of
thousands of strings; its comment ("a result holds only prompt texts, so the cap bounds memory") is
wrong; and a hit returns a copy of the whole set.

## 5. Recommendations, by effect

1. Build and ship the `UText` fix (§3): the only unbounded growth found (52 MB in the first 17.5 hours,
   up to ~8 MB an hour while terminals print).
2. Fewer live measured TextBlocks (§4.1): ~0.5 GB; each one saved is ~17 KB.
3. Build tab context menus lazily (§4.2): ~205 MB at 701 tabs.
4. Replace `s_last` with a hash (§4.3): ~25 MB, growing with the fleet.
5. Cap the upstream cache by size, or keep results without their uuid set (§4.4): ~8 MB here.

## 6. Limits of this check

- 92 minutes of one afternoon and 8.5 hours of one evening. A leak that needs an action not done in that
  window (many tab closes - only 2 happened in this process's life -, window close and reopen, the
  Sessions page opened and closed over and over) is not excluded; repeat steps 2–5 of the toolkit around
  such an action.
- `segheap.py` stops a VS subsegment walk at the first uncommitted chunk header, so a few live chunks after
  a decommitted hole may be missed; the totals agree with the heap's own committed counter.
- A thread-private LFH subsegment's own free list is not subtracted: 160 blocks in dump 4.
- "No live owner" is not "leaked": owners in module data sections were checked separately (none), but
  pointers in mapped memory or stored encoded are not seen. Only GROWTH across dumps proves a leak.
- The ctx1 (16 MB) segments held 5 MB committed and were not walked; blocks over 8 MB are outside the
  segment walk and were read from the region list instead (§2).

## 7. Correction, 2026-10-20: the LFH busy bitmap of Windows 11 26200

- **What was wrong:** `segheap.py` read each LFH block's busy bit as bit `2 × index` of `BlockBitmap`
  (two interleaved bits per block, the layout of Yason's 2016 paper). On 26200 the bitmap holds one
  qword per 32 blocks: its low dword is their busy bits, its high dword their "unused bytes" bits. The
  old reading took a neighbor's busy bit or an unused-bytes bit, so an exact-size block (no unused bytes)
  read free half of the time.
- **How it showed:** the raw vftable scan found 706 `Tab` objects, every one referenced, for 706 open
  tabs, and the walk called 313 of them live; 51.6% of the closed `UText` images read busy, close to a
  coin flip. ntdll's PDB showed a subsegment layout newer than the paper's (State, HighWatermarkBlockIndex,
  PrivateFormat); the raw bitmap of a full subsegment read `ff ff ff ff` + 4 other bytes per qword.
- **The check of the new reading:** all 2,828 referenced objects of four tab-scoped classes read busy;
  FreeCount equals the clear busy bits in 13,646 of 13,848 subsegments and is never above them; the
  unused-bytes bit is never set on a free block; every closed `UText` image reads busy.
- **What changed (dump 1 unless named):**

  | Figure | First version | Corrected |
  |---|---|---|
  | Live heap | 1,501 MB (10.4 M blocks) | 1,778 MB (13.0 M blocks) |
  | Committed but free heap space | ~452 MB | ~180 MB (only 4 MB of it whole pages) |
  | Leaked `UText`s, dumps 1 / 2 / 3 | 103,758 / 131,622 / 147,449 | 197,395 / 251,065 / 282,111 |
  | Afternoon leak rate | ~28,000 an hour | ~47,500–60,600 an hour |
  | Menus and their tooltips | ~180 MB: 68,370 items, 25,209 ToolTips | ~205 MB: 70,952 items, 47,976 ToolTips |
  | XAML event sources | 711,147 | 790,630 |
  | 16 KB DirectWrite blocks without a live owner | ~5,300 ("a pool DirectWrite reuses") | 112 |
  | Our objects, live / raw | e.g. `TermControl` 661 / 701 | all live (701 / 701) |
