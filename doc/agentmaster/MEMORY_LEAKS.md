# Memory: leaks and what holds the memory (production, 2026-10-19)

The user's request: "investigate and detect agentmaster for memory leak, use dumps from release (keep
it running and access readonly)". Production was never stopped, written to, or attached to; three
snapshot dumps were read offline. The tools and the workflow: [`tools/leakhunt/README.md`](../../tools/leakhunt/README.md).

## 1. What was measured

- **The instance:** production 0.6.8.22 (the build of ec966a2ca, `Q:\agentmaster-layouts\Release-B`),
  pid 41148, started 07:35:36; 4 windows holding 703 tabs (556 + 93 + 7 + 47). In its life so far:
  705 tab insertions (the restore + 10 new sessions + 1 fork), 2 tabs closed.
- **Counters** (`memwatch.py`, every 15 s, 15:00:40 → 16:40): private bytes 2,088–2,159 MB, quarter means
  2,101 → 2,105 → 2,109 → 2,111 MB (about +6 MB/hour); handles 2,784 → 2,824; threads 145 → 151;
  GDI 56, USER 135, flat. Peak working set at startup: 3.1 GB.
- **Dumps** (`snapdump.exe`, PSS VA clone): 15:02:51, 15:56:01, 16:35:12; the target paused 139, 230
  and 82 ms; 2.6 GB each (`Q:\tmp\agentmaster-dumps\leak-20261019\`, with the censuses).

## 2. Where the 2.0 GB of private memory is

| Part | MB | Notes |
|---|---|---|
| Live heap blocks | 1,501 / 1,500 / 1,510 | the three dumps; 10.4 M busy blocks |
| Committed but free heap space | ~452 | the segment heap keeps it after the 3.1 GB startup peak |
| Outside the segment heap | ~78 | 24.6 MB is `SaveSessions`' cached JSON (§4.3); 13.8 MB GPU-driver tables; 8 MB heap metadata |

Inside the live heap (dump 1; exact sizes from the heap's own metadata, `segheap.py`):

| Holder | Live MB | Count | What it is |
|---|---|---|---|
| DirectWrite text-analysis scratch buffers + layouts | ~492 | 30,683 × 16 KB buffers | §4.1: ~19,000 attached to the layouts of live TextBlocks, ~5,300 in a pool DirectWrite reuses |
| Per-tab context menus | ~180 | 68,370 MenuFlyoutItems, 25,209 ToolTips | §4.2 |
| XAML event sources | 86.5 | 711,147 | one per (element, event) with a subscriber; a linker-folded vftable counts every `CEventSource<>` |
| Other XAML elements, brushes, composition visuals | ~200 | TextBlocks 30,006, Borders 17,902, StackPanels 16,791, … | the per-tab UI, side panels, board, Sessions page |
| Leaked ICU `UText`s | 14.2 → 18.1 → 20.2 | 103,758 → 131,622 → 147,449 | §3 - the one leak |
| Everything else without a vftable | ~470 | 7.2 M small blocks | strings, vectors, property stores, engine records |

Our own objects match the open tabs (no closed-tab trees left behind): `Tab` 701, `TermControl` 701 raw /
661 live, `TabHeaderControl` 702 / 671, `AgentTabOverlay` 704 / 661 (the raw extras are replaced, already
freed controls), `AgentManagerContent` 3–4 for 4 windows. The leak classes fixed on 2026-10-14 did not come
back: 570 live `CScrollViewer`s and one overlay per tab (UI_THREAD.md §12).

## 3. The leak: one 144-byte ICU `UText` per URL scan (FIXED in source 2026-10-19, NOT BUILT)

- **Evidence:** between the dumps only one population grew steadily: live 144-byte blocks with the
  signature magic `0x345AD82C` (UTEXT_MAGIC), flags 0, providerProperties 6, sizeOfStruct 0x90, pFuncs NULL,
  context = a `TextBuffer` (69 distinct terminal buffers). +27,864 in 53 minutes, +43,691 in 92 minutes
  (about 28,000 an hour at that afternoon's activity, ~4.5 MB/hour, ~0.1 GB a day); 103,758 had accumulated
  in the 7.5 hours before the first dump. No other block signature grew by more than ~2,900 in 92 minutes.
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

## 4. Large and bounded: what scales with the number of tabs and sessions

None of these grows by itself; each is paid again for every tab, card or row, and stays until it closes.

### 4.1 A measured XAML TextBlock keeps ~17 KB of DirectWrite state

- The chain, read from the dump: a 16 KB scratch block ← `DWriteTextAnalyzer` (48 B) ← DirectWrite
  `TextLayout` (352 B) ⇄ `DWriteTextLayout` (384 B) ← `CTextBlock` (+0x1F8) or a DirectWrite
  `InlineLayout` (a TextBlock with Runs). The live heap holds 30,683 such 16 KB blocks = 479 MB, about a
  third of it. Owners (dump 1, every block attributed in one `--refs-of` pass): 18,722 belong to a live
  `DWriteTextAnalyzer`; ~5,300 have no live owner but are not leaked - they are a pool DirectWrite reuses
  (of dump 1's 5,672 unowned ones, 768 were attached to an analyzer again and 59 freed by dump 3, so some
  reference that a private-memory scan does not see still reaches them); the rest hang off small internal
  blocks without a vftable.
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
  `AgentFillModelPickItems`, …), and about 40 `ToolTipService::SetToolTip(item, box_value(text))` calls -
  each creates a XAML `ToolTip` + template settings + a size-changed handler at once. Measured live:
  68,370 MenuFlyoutItems (79 MB with their core objects), 25,209 ToolTips (44 MB with theirs), separators,
  submenus, icons, boxed tooltip strings (110,290): ~180 MB for 703 tabs, ~0.25 MB per tab, for menus that
  are rarely opened. (The Sessions page's row menus are built eagerly too: ~600 rows' menus were alive.)
- The fix shape is the Manager's own (UI_THREAD.md / manager-lens rebuild tax): an empty flyout filled in
  its `Opening` handler, and tooltips through `AgentSetTip` (no ToolTip object until a hover).

### 4.3 `SaveSessions` keeps a full copy of the last `sessions.json`

`static std::wstring s_last` (Persistence.cpp) skips identical writes by comparing the whole serialized
fleet: 9.4 M characters (18 MB) in a 24.6 MB allocation, growing with `sessions.json`, which is never
pruned. A 64-bit hash plus the length would do the same job.

### 4.4 Heap space kept after the startup peak

The restore of 703 tabs peaks at 3.1 GB working set; afterwards ~452 MB of the 1,953 MB committed in heap
segments is free space the segment heap keeps committed. It is reused by later allocations; it is not a leak.

## 5. Recommendations, by effect

1. Build and ship the `UText` fix (§3): the only unbounded growth found.
2. Build tab context menus lazily (§4.2): ~180 MB at 700 tabs.
3. Fewer live measured TextBlocks (§4.1): virtualized lists and released hidden pages; each one saved is
   ~17 KB, and the TextBlocks of the per-tab overlays, cards, tab headers and Sessions rows hold ~0.3 GB.
4. Replace `s_last` with a hash (§4.3): ~25 MB, growing with the fleet.

## 6. Limits of this check

- 92 minutes of one afternoon. A leak that needs an action not done in that window (many tab closes - only
  2 happened in this process's life -, window close and reopen, the Sessions page opened and closed over
  and over) is not excluded; repeat steps 2–5 of the toolkit around such an action.
- `segheap.py` stops a VS subsegment walk at the first uncommitted chunk header, so a few live chunks after
  a decommitted hole may be missed; the totals agree with the committed sizes within the free space.
- "No live owner" is not "leaked": owners in module data sections were checked separately (none), but
  pointers in mapped memory or stored encoded are not seen. Only GROWTH across dumps proves a leak (the
  `UText`s grew, the unowned DirectWrite buffers did not).
- The ctx1 (16 MB) segments held 5 MB committed and were not walked; blocks over 8 MB are outside the
  segment walk and were read from the region list instead (§2).
