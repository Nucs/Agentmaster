# Measurements behind the skill

Every claim in `SKILL.md` traces to one of these. Environment: Claude Code **2.1.225**,
Windows 11 Pro N 26200, Python 3.12 (pywinpty + pyte), `claude.exe` at
`C:\Users\ELI\.local\bin\claude.exe`. Test cwd `C:\Users\ELI\.claude` (trusted).

Re-run anything below with `S=.claude/skills/claude-wrap-ConPTY/scripts/conpty_claude.py`.

---

## 1. Baseline: the original prototype fails, and why

`~/.claude/scripts/claude-wrap-proto.py` run as-is:

```
[proto] ready: False (958 bytes captured)
[proto] sent 'Hi!' (bracketed paste + CR)
[proto] transcript path: None
  hi! found      : FAIL
```

Dumping the 958 bytes and rendering them with pyte showed the actual state — not a hang, a
**modal**:

```
  Settings Error
  C:\Users\ELI\.claude\settings.json
  └ Invalid or malformed JSON
  ❯ 1. Fix with Claude
    2. Exit and fix manually
    3. Continue without these settings
  Enter to confirm · Esc to cancel
```

The prototype's ready check (`"❯" in buf and ("tokens" in buf or "effort" in buf)`) correctly
declined — but it then **sent the prompt anyway**, typing `Hi!` into a menu. Two lessons: render
the screen, and never send when not ready.

Byte-growth trace (the "hang" is really a settled modal):

```
t=1s bytes=23   t=4s bytes=90   t=5s bytes=958   t=6..20s bytes=958 (alive=True)
```

## 2. The blocker itself: merged-conflict settings.json

```
$ grep -n '^\(<<<<<<<\|=======\|>>>>>>>\)' ~/.claude/settings.json
22:<<<<<<< Updated upstream
28:=======
30:>>>>>>> Stashed changes
42/49/55, 61/63/64, 84/93/110   (four conflicts total)
```

`json.loads` → `Expecting property name enclosed in double quotes: line 22 column 1`.
Four unresolved conflicts from a `git stash pop`. Consequence: the modal on **every** launch, and
the entire file skipped even after answering. **Left unfixed — resolving a merge conflict is the
owner's call.**

## 3. `CLAUDE_CODE_CHILD_SESSION` silently disables transcript saving

One variable dropped at a time from a fully inherited env, checking the footer for
`Transcript saving is off`:

```
keep everything                       -> transcript_off=True
drop CLAUDE_CODE_CHILD_SESSION only   -> transcript_off=False   <-- the one that matters
drop CLAUDECODE only                  -> transcript_off=True
drop CLAUDE_CODE_SESSION_ID only      -> transcript_off=True
```

Footer text:

> ⚠ Transcript saving is off — inherited CLAUDE_CODE_CHILD_SESSION marker · restart with
> `CLAUDE_CODE_FORCE_SESSION_PERSISTENCE=1` to keep future transcripts

The TUI is otherwise **completely healthy** — it reaches the input box, accepts prompts, replies.
Only the on-disk verification fails. Neither original prototype stripped this variable, so any
wrapper run from inside a Claude session (i.e. by an agent) hits it.

Repro: `python "$S" probe --keep-env --answer-modals --cwd C:/Users/ELI/.claude`

## 4. Submit semantics: `\r` vs `\n` vs bracketed paste

**A — bracketed paste, no submit** (`fill(MULTI)` with a 3-line string):

```
❯ Line ONE of the draft
  Line TWO of the draft
  Reply with exactly: MULTI_OK
   line1/line2/line3 on screen: True/True/True
   turn started               : False
```

then one `submit()` →

```
recorded user message: 'Line ONE of the draft\nLine TWO of the draft\nReply with exactly: MULTI_OK'
```

One message, `\n` preserved.

**B — plain write, LF endings** (`"PLAIN alpha\nPLAIN beta\n"`):

```
❯ PLAIN alpha
  PLAIN beta
session: cdb45afb-… | user msgs: []
```

Both lines sit in the box; **nothing is ever submitted**. LF is a literal newline.

**C — plain write, CR endings** (`"PLAIN gamma\rPLAIN delta\r"`):

```
✽ Misting… (6s · thinking)
  ❯ PLAIN delta
❯ Press up to edit queued messages
session: e4fbc4b0-… | user msgs: ['PLAIN gamma']
```

CR submits. The second CR mid-turn became a **queued type-ahead** message, not a lost keystroke.

## 5. Session id == the debug-banner uuid

```
▎ Debug mode enabled · logging to C:\Users\ELI\.claude\debug\b7b15376-4e26-4ef0-b652-8537e62a9b4e.txt
...
session id : b7b15376-4e26-4ef0-b652-8537e62a9b4e
transcript : …\projects\C--Users-ELI--claude\b7b15376-4e26-4ef0-b652-8537e62a9b4e.jsonl
```

Same uuid in the banner and the transcript name — so scraping the banner resolves the session
before any transcript exists. Falls back to new-file-by-ctime when debug logging is off.

## 6. Ready / busy signatures

Healthy input box (the two cues `input_box_ready()` needs):

```
────────────────────────────────────────────────────────────  <- rule above
❯
────────────────────────────────────────────────────────────  <- rule below
  ⏸ manual mode on · ? for shortcuts                    0 tokens
```

Mid-turn — note the box is **still rendered**, so it cannot mean "idle":

```
✽ Misting… (6s · thinking)
❯ Press up to edit queued messages
  ⏸ manual mode on · esc to interrupt        <- the only reliable busy signal
```

After a turn: `✻ Brewed for 2s`, and `esc to interrupt` is gone.

## 7. pyte rendering artifact

Raw output opens with:

```
ESC[1t ESC[c ESC[?1004h ESC[?9001h ESC7 ESC[r ESC8 … ESC[<u ESC[>1u ESC[>4;2m ESC[>0q
```

pyte doesn't implement kitty-keyboard / modifyOtherKeys / XTVERSION / window-manipulation, and
leaks their tails — a lone `u` on row 0. `_PYTE_NOISE` strips
`ESC[[<>]…[a-zA-Z]` and `ESC[…t` before feeding. Lossless: none carry cell content.

Also note the stream is cursor-addressed per word (`ESC[3G`, `ESC[12G`, …), which is why raw
substring matching is hopeless and rendering is mandatory.

## 8. Raw Win32 ConPTY — reproducible dead end

Minimal C# (`CreatePipe` → `CreatePseudoConsole` → `UpdateProcThreadAttribute` →
`CreateProcessW`), child = `cmd.exe`, payload `echo CONPTY_SANITY_OK\r`:

```
[min] pid=68996 sizeofEX=112 attrsz=48
Microsoft Windows [Version 10.0.26200.8875]      <- child output on the PARENT's console
(c) Microsoft Corporation. All rights reserved.
[min] captured 16 chars
[min] SANITY FAIL
---- tail ----
[?9001h[?1004h                                   <- ONLY conhost's own mode-set bytes
```

Variants tried, all failing **identically**:

| variant | result |
|---|---|
| `bInheritHandles = false` (the MS sample's value) | 16 chars, SANITY FAIL |
| `bInheritHandles = true` | 16 chars, SANITY FAIL |
| `FreeConsole()` before `CreateProcessW` | 16 chars, SANITY FAIL |

Ruled out by measurement: `Marshal.SizeOf<STARTUPINFOEX>()` = 112 (correct for x64),
attribute-list size 48, attribute value `0x00020016` (= `22 | 0x20000`, correct),
`CreatePseudoConsole` → `S_OK`, `CreateProcessW` → success.

Diagnosis: the child attaches to the **parent's** console rather than the pty; we read the pty
and get only what conhost itself emitted. Unresolved — and it is where both `claude-wrap-proto.cs`
and `claude-wrap-proto-cmd.cs` stalled (the latter is literally a debugging session, with
`// EXPERIMENT: keep child-side handles open` and `// EXPERIMENT: do NOT forward to stdout`).

**Use pywinpty.** Its native layer performs the attach correctly; the full chain is verified
green below. Only revisit the P/Invoke path with genuinely new information.

## 9. Verified green: `selftest`

```
$ python "$S" selftest --cwd "C:/Users/ELI/.claude" --answer-modals
  [PASS] claude.exe resolves  C:\Users\ELI\.local\bin\claude.exe
  [PASS] spawned on a ConPTY  pid=59272
  [PASS] TUI reached the input box
  [PASS] transcript saving is on  (a CLAUDE_CODE_CHILD_SESSION leak turns it off)
  [PASS] bracketed paste fills the box without sending
  [PASS] Ctrl+U clears the draft
  [PASS] turn completed
  [PASS] session id resolved  0190d7ec-9695-426b-8d70-3072867bd292
  [PASS] transcript exists  …\projects\C--Users-ELI--claude\0190d7ec-….jsonl
  [PASS] prompt recorded in transcript  Reply with exactly: CONPTY_SELFTEST_OK
  [PASS] assistant replied  CONPTY_SELFTEST_OK

[selftest] 11/11 passed
```

Note this run needed `--answer-modals` **only** because of the broken `settings.json` (§2). Fix
that and plain `selftest` passes.
