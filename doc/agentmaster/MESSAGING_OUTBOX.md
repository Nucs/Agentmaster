# The durable Telegram outbox — every message survives (designed 2026-11-10; first part implemented, NOT BUILT)

> **Status: designed 2026-11-10; its first part was implemented the same day ([MESSAGING.md](MESSAGING.md) §21.50, NOT BUILT
> into any install yet). §14 says what was built and where it differs from the design below.** The owner's ask (2026-11-10): "What design will allow us to never lose a message? Can we
> queue messages while persisting to disk and dequeueing, all efficiently. Even bad gateway, all messages must be durable and
> queue-wise recovering. Consider persisting ways, one of them single-line jsonl writing and reading concurrently on disk."
> The decisions are the owner's (AskUserHtmlQuestion, 2026-11-10), each recorded where it applies:
> **D-O1** storage = a JSONL journal with the queue in memory ("But does it protect against concurrency well?" — §5);
> **D-O2** a send whose outcome is unknown is sent again (at-least-once);
> **D-O3** crash-safe at once, power-safe within 250 ms;
> **D-O4** a deleted topic is made again ("But make sure to not fail to find it, sometimes its titled differently as we lose
> title sync" — §8).
> It replaces the in-memory outbox of [MESSAGING.md](MESSAGING.md) (`MessagingService::_outbox`, §21.4 – §21.14's pacing
> and flood parking stay) and closes SPEC.md §27 G1 / G2 and MESSAGING.md §21.21's lost sends and deleted topics.
> A `§21.x` below is always MESSAGING.md's; `§1` – `§13` are this document's.

## 1. What is lost today

Read in the code at f2854acd6 / b4321c3dc and in production's logs (2026-10-17 → 2026-11-10):

| Loss | Where | Evidence |
|---|---|---|
| A **502** or a **transport failure** drops the item: the send loop retries only a 429, then calls the item's callback once | `MessagingService::_sendLoop` | ~1,500 `502 Bad Gateway` (584 editMessageText, 350 sendRichMessage, 317 createForumTopic, 224 getUpdates) and ~250 transport failures in `messaging.log` |
| A turn's **final reply**, a remark, a prompt line are queued with NO callback, so nothing ever sends them again | `_postFinal`, `_postRemark`, most of the 172 `_reply` calls | — |
| **Every restart empties the queue** (`_outbox.clear()`), and the app quits with `TerminateProcess` | `_start` | a 21,890 s flood hold (~2026-11-09 11:33 → 17:38) parked everything; whatever waited was gone at the next start |
| The mirror's transcript **cursor is saved before its messages are sent**: a kill in between skips them for good | `_followTranscript` → `CopyBackTopicProgress` → state.json within 10 s | — |
| An idle or closed topic's delete **purges its queued items** | `_deleteTopic` | during a long hold, a session that goes idle loses what waited |
| A **deleted topic is never made again**: its card is posted into the dead thread every 60 s, forever | `_postCard`'s callback | one session: 5,482 × "message thread not found" in ~3.8 days, still going |
| Telegram's chat-wide hold is **forgotten at a restart**, which then calls at once into the hold | `_floodParking` (memory) | — |

## 2. Guarantees

1. **A message the service accepted is delivered** — through 5xx answers, timeouts, holds of any length, hot swaps, quits,
   crashes and (within 250 ms) power cuts. The one exception is a message Telegram refuses PERMANENTLY in every form
   (§7): it is kept in `dead.jsonl` and reported, never silently dropped. A session the user CLOSES takes its topic and
   everything for it (Telegram deletes a topic with its messages): §8.6.
2. **Order per topic**: a topic's messages reach Telegram in the order they were queued — a reply never lands above its
   prompt — across retries and restarts.
3. **At-least-once** (D-O2): an item whose outcome is unknown is sent again. Edits, deletes, reactions, pins and renames
   repeat harmlessly; a new message can, rarely, show twice; a topic is never created twice (§8.4).
4. **Crash-safe at once, power-safe within 250 ms** (D-O3).
5. **The chat's budget holds** (MESSAGING.md §21.9 / §21.14): retries are paced like every call, a hold survives a
   restart, and a Telegram outage is probed, not hammered.
6. **Cheap**: microseconds on the queuing thread, no flush there, bounded disk, a start in milliseconds.
7. **No new dependency**: plain Win32 files and `Json.h`.

## 3. The model

An **item** is one Bot API call plus what the outbox needs to keep it:

| Field | Meaning |
|---|---|
| `id` | a monotonic 64-bit number, never reused (the journal's key) |
| `lane` | the ordering domain (below) |
| `prio`, `seq`, `sub` | today's order: `SendPriority`, then the queue sequence, then the part / fallback slot |
| `cls` | `d` durable · `l` latest (keyed) · `e` ephemeral (never journaled) |
| `key` | for `l`: a newer item with the same key replaces a queued one (today's `OutItem::key`) |
| `method`, `params` | the call; `message_thread_id` is set AT SEND TIME from the lane's current thread (§8) |
| `blob` | an upload's bytes in `outbox\blobs\<id>` (documents, pictures), deleted once sent |
| `fb` | the fallback form (rich → HTML → plain text, photo → document), so the SENDER can fall back with no closure |
| `ref` | dependencies resolved at send time: `{"reply": <item id>}` = reply to the message that item became |
| `then` | a NAMED completion with its data (`{"kind":"card-posted","topic":"t7","hash":123}`), run when the item's outcome is known — in this run or after a restart (§9) |

**Lanes.** `topic:<uid>` — a session's topic (`uid` is new, persisted on `TopicRecord`, kept through a Rekey and a
re-creation); `search` — the 🔎 Search topic; `main` — the main view ("All"); `nt:<thread>` — a new thread's wizard;
`k:<key>` — keyed edits that wait for nothing (card and prompt-status edits, reactions, the directory). Inside a lane the
order is strict `(seq, sub)`; across lanes the sender picks by `(prio, seq, sub)` as today. A card RE-POST goes in its
topic's lane (it must land below the content queued before it); a card EDIT goes in its `k:` lane.

| Calls | Class | Lane |
|---|---|---|
| prompt lines, final replies, remarks, work-log creates, show-only questions / plans, answers, interrupts, API errors, slash-command lines, history replay, change lines, idle notices, command replies, the start note, Search answers and cards, secretary sends | `d` | the topic's / `search` / `main` |
| documents and photos (`/screenimage`, `/transcript`, `/download`'s Send as file, All Screen parts) | `d` + blob | the topic's |
| a card's re-post (the old card's delete, then the new card) | `l` | the topic's — in order, below the content queued before it |
| card / work-log / prompt-status edits, the 🖼️ card picture upload, the directory's post / edit / pin, reactions, deletes of Telegram's notices | `l` | its `k:` lane |
| answerCallbackQuery, sendChatAction | `e` | — (memory only: worthless after ~15 s) |

Topic calls (`createForumTopic`, `editForumTopic`, `deleteForumTopic`) stay the sync's direct calls (state-driven, planned
again every tick), with two additions: a create is journaled (§8.4), and an idle delete waits for its lane (§8.6).

## 4. The journal on disk

- **Where:** `<profile>\messaging\telegram\outbox\` — `j-<first id, 16 digits>.jsonl` segments, `blobs\`, `dead.jsonl`,
  and `.lock` (§5.5).
- **A record** is ONE line: a UTF-8 JSON object, `"op"` first, `"crc"` last (CRC-32, 8 hex digits, of the line's bytes
  before `,"crc":`), ending in `\n`, at most 1 MB (a bigger payload goes to a blob).
- **A segment** starts with `{"op":"seg","v":1,"first":N,"at":…,"writer":"0.8.0.13 release"}`, grows by appends only,
  and is sealed at 8 MB.

| Record | Written | Example |
|---|---|---|
| `put` | an item is queued (a GROUP of puts is one write, §5.3) | `{"op":"put","id":812,"g":810,"gn":3,"lane":"topic:t7","prio":0,"seq":5521,"sub":0,"cls":"d","method":"sendRichMessage","params":{…},"fb":{…},"ref":{"reply":809},"then":{"kind":"final-reply","topic":"t7"},"at":1762770000123,"crc":"9a1b2c3d"}` |
| `try` | right before the call | `{"op":"try","id":812,"n":1,"at":…}` |
| `ok` | Telegram took it | `{"op":"ok","id":812,"mid":90311,"res":{…the facts `then` needs…}}` |
| `retry` | a transient failure, with the back-off's next moment | `{"op":"retry","id":812,"n":1,"why":"502 Bad Gateway","nb":1762770002123}` |
| `drop` | superseded, its target gone, the session closed, or dead (also copied to `dead.jsonl`) | `{"op":"drop","id":812,"why":"superseded"}` |
| `hold` / `lane` | a lane held (its topic missing, a 403) / released onto a (new) thread | `{"op":"lane","lane":"topic:t7","thread":46012}` |
| `flood` | Telegram's hold, in wall-clock time | `{"op":"flood","until":1762791900000,"method":"*"}` |
| `cursor` | the mirror's transcript position, in the SAME group as the messages it derived (§9.2) | `{"op":"cursor","topic":"t7","pos":7340032}` |
| `create` / `created` | a topic create about to go / answered | `{"op":"create","topic":"t7","name":"✍️ 🟦 phone","at":…}` |

Measured on this PC's C: (Corsair MP600 PRO NVMe, the profile's drive) with 2 KB lines
([`tools/telegram-outbox-bench/journal_bench.py`](../../tools/telegram-outbox-bench/journal_bench.py)):

| Step | Cost |
|---|---|
| one append (one `WriteFile` on a `FILE_APPEND_DATA` handle), no flush | **7.6 µs** |
| one `FlushFileBuffers` | 5.3 ms |
| 32 appends per flush | 172.8 µs per record |
| start: fold a 10 MB journal (5,000 puts + 4,000 outcomes + a torn last line) | 85 ms (Python; C++ faster) |
| a second handle tailing the file while it grows | every line, in order, 0.54 ms median lag (p99 1.65 ms) |
| an append while another thread flushes (§5.5) | 39 µs median through a second flush handle; 1.3 ms median through the same handle |

Both scripts live in `tools/telegram-outbox-bench/` (`python -I <script> <a scratch folder on the profile's drive>`; they
write only there) and ran from a scratch folder on C: on 2026-11-10; the numbers include Python's own overhead, so a C++
append is cheaper still — the RATIOS are what the design relies on.

## 5. Concurrency — what protects it (the owner's question, D-O1)

**The actors:** producers on ANY thread (the tick, the poll thread, the send loop's own completions, workers for pictures,
`/status`, voice, census, and window replies on a UI thread); ONE sender; ONE flusher; the rotation and checkpoint; the
recovery at a start; outside readers (a person, a script); and, in theory, a second process on the same profile.

### 5.1 One leaf lock orders the file and the queue together
- The outbox is its own class with its own mutex. Under it, and only under it: an id and a seq are taken, the line is
  written with ONE `WriteFile`, and the in-memory queue is changed. The file and the queue therefore change in the same
  order; the queue never holds an item whose line is not in the OS cache; an `ok` can never precede its `put`.
- It is a **leaf**: code holding it calls nothing else (no callback, no log line, no other lock). Callers may hold the
  service's `_mtx` when they queue (`_mtx` → outbox lock is allowed; the reverse never happens), so no lock order can
  deadlock. MESSAGING.md §21.7 found exactly that bug once (a reply sent under `_mtx` that took `_mtx` again).
- Held for microseconds (a few milliseconds for the ~2 % of appends that meet a flush, §5.5): the JSON of `params` is
  built BEFORE the lock (the expensive part), and the same serialized bytes become the HTTP body when the item goes
  (serialized once). The append stays inside the lock on purpose: D-O3 wants the line in the OS cache before the queuing
  call returns, and a hand-off to a writer thread would open a window in which a kill loses an accepted message.

### 5.2 One write per record, a line is whole or absent
- Every record (or group, §5.3) goes out in ONE `WriteFile` that ends in `\n`. Two producers can never interleave
  bytes: the lock serializes them, and there is one handle.
- A reader takes only `\n`-terminated lines; a tail without its `\n` is "not written yet" (a live writer) or torn (after a
  crash — §6 cuts it off). The CRC catches a line that a power cut filled with garbage but that still parses.

### 5.3 Groups are all-or-nothing
- A message's parts, its prompt line + its quote, and the mirror's messages + its `cursor` are queued in ONE call:
  consecutive seqs, one buffer, one `WriteFile`. Every `put` carries `g` (the group's first id) and `gn` (its size);
  recovery accepts a group only when all its lines are there, so a torn tail can never leave half a message queued.

### 5.4 The sender's in-flight protocol
1. Under the lock: pick the next item (§7), mark it in flight, append `try`.
2. Without any lock: the HTTP call (seconds, maybe). Producers keep queuing meanwhile.
3. Under the lock: append the outcome (`ok` / `retry` / `drop` / `hold`), update the queue.
4. Without the lock: the in-memory continuations (taps, today's `onDone`) and the `then` completion, which may queue new
   items (the lock is free, so no self-deadlock).
- A newer keyed item for an item IN FLIGHT cannot cancel the call: it waits in its lane and goes after the answer — at
  Telegram the newest still wins.

### 5.5 The flusher, rotation and other processes
- **The flusher** (its own thread) flushes at most every 250 ms when something was written (D-O3), never under the lock
  and through a handle of ITS OWN to the segment: Windows serializes every call on one synchronous handle (its file
  object), so a `FlushFileBuffers` on the writer's handle stalls the next append, while a flush through a second handle
  writes the file's cached data (whoever wrote it). Measured (`flush_contention.py`, an append every ~1 ms against a
  flusher looping every 2 ms — far harder than 4 flushes a second): appends alone median 25 µs / p99 0.2 ms; flushing
  through the SAME handle median 1.3 ms / p99 8.8 ms; through a SECOND handle median 39 µs / p90 0.7 ms / p99 2.4 ms — an
  append that meets a flush can still wait a few ms (NTFS locks the file briefly), which at ≤ 4 flushes a second is ~2 %
  of appends. The app's last-window close and `Stop()` flush at once.
- **Rotation** happens under the lock (a new segment, its `seg` line, the active segment swapped). A segment object holds
  both handles and lives in a shared pointer, so the old one closes after the flusher's last flush of it. A sealed segment
  is deleted (by the flusher) once none of its items is live and none of its `ok` lines waits for its `then` (§9.1).
- **Checkpoint**: when the journal passes 64 MB, or a sealed segment is kept only by a few old items, the live items are
  written again into a fresh segment (same ids, `"re":1`), flushed, and the older segments deleted. A crash midway leaves
  the same id in two segments, which the fold reads once (§6).
- **One writer per profile**: at a start the outbox opens `.lock` with no sharing and `FILE_FLAG_DELETE_ON_CLOSE` — a
  second process cannot open it (the bot then does not start, and its status line says so), and a dead process leaves no
  stale lock (Windows closes its handles). The segments are opened with read, write and delete sharing (the flusher's own
  handle needs the write share), so anyone may read or tail them (`Get-Content -Wait`); `.lock` is what keeps every other
  WRITER out. The bot's own one-poller-per-token mutex (M7) already keeps a second bot off the same token.

### 5.6 A kill at every point

| Killed … | After the start |
|---|---|
| before the producer's `WriteFile` | not queued — the caller never got its "queued" |
| after the `WriteFile`, before a flush | in the OS cache: survives a process kill; a power cut loses at most the last 250 ms |
| after `try`, before the call | sent (it never went) |
| during the call, or before `ok` was written | sent again: at-least-once — Telegram may show it twice (D-O2) |
| after `ok`, before its `then` reached state.json | `then` runs again at the start (idempotent, §9.1) |
| mid-rotation | an empty new segment: harmless |
| mid-checkpoint | an id in two segments: folded once |
| mid-write by a power cut | the torn or zero-filled tail is cut at the last whole line; an incomplete group is discarded whole |

## 6. Recovery (at `_start`, before any thread runs)

1. Take `.lock` (§5.5).
2. Read the segments in order. A line without `\n`, with a wrong CRC or that does not parse: at the END of the last
   segment → the file is cut there (`SetEndOfFile`, logged); anywhere else → skipped, counted, logged — never a reason not
   to start.
3. Fold: a complete group's `put`s become items (a repeated id keeps its newest line); `try` marks an attempt; `ok`
   removes the item and records `id → message id` (for `ref`) and, when newer than state.json's `outboxApplied`, its
   `then` for step 5; `retry` keeps the attempt count and the back-off moment; `drop` removes; `hold` / `lane` set lane
   states; `flood` restores Telegram's hold if it is still in the future; `cursor` keeps the newest per topic;
   `create` / `created` restore pending creates (§8.4).
4. Keyed items: only the newest live `put` per key stays.
5. Replay the `then` completions of the `ok`s that state.json had not saved yet (§9.1).
6. An item with a `try` and no outcome is ambiguous → it is sent again (D-O2; counted as `resent-after-crash`).
7. The mirror's cursor per topic = the newer of state.json's and the journal's (§9.2).
8. The threads start; the sender begins with the recovered queue, inside any restored hold.

## 7. The sender

**Picking** (one call at a time, as today): skip a lane that is held, in back-off, or whose head is in flight; respect
the 1.05 s gap, the chat's hold, the call window for background items (history only under 18 calls a minute; renames,
re-sorts and form upgrades as in §21.9 – §21.15); then the lowest `(prio, seq, sub)` among the lane heads.

**The answer decides** (one pure function, harness-tested):

| Telegram's answer | Verdict |
|---|---|
| ok; "message is not modified"; `TOPIC_NOT_MODIFIED` | `ok` |
| 429 `retry_after` | a CHAT-wide hold until then (journaled `flood`; measured: one hold covered every method); the item stays first in its lane; no attempt counted |
| 5xx (500 / 502 / 503 / 504) or no HTTP answer (DNS, TLS, timeout, cut) | `retry`: back-off 1, 2, 4, 8, 16, 32, 60 s, then every 60 s, ±20 % jitter; never given up (§2's first guarantee); the circuit breaker counts it |
| "can't parse entities", a refused rich part, a refused photo | its `fb` becomes the item, in the same place (`sub` + 1), no attempt counted |
| "replied message not found" (a quote) | the quote removed, sent at once |
| "message thread not found" / `TOPIC_ID_INVALID` | the lane is HELD and §8 starts |
| "message to edit not found" / "message can't be edited" (an edit) | `drop` (its target is gone) + `then` (a card is posted again) |
| "message to delete not found" / "can't be deleted for everyone" | `ok` (nothing more to do; the second is logged once per run) |
| any other 400 | dead: `drop` + `dead.jsonl` + the census; one silent line in 🔎 Search at most hourly ("2 messages for “🟦 phone” could not be delivered: …") |
| 401 | the bot stops (poll and sender); the journal waits for a new token |
| 403 (the user blocked the bot) | every lane held; probed every 10 minutes; released at the first success |

**The circuit breaker:** 8 transient failures in a row across all lanes = Telegram unreachable: nothing is sent for 15 s,
then ONE item probes; a success closes it, a failure doubles the pause (up to 5 minutes). The status line says so
("Telegram unreachable since 11:33 — 214 messages kept, sent when it answers").

## 8. A topic that is gone is made again — found by its thread, never its title (D-O4)

### 8.1 The owner's rule and caution
"Make the topic again" — "but make sure to not fail to find it, sometimes its titled differently as we lose title sync".
So:
- **A topic is identified by its THREAD ID only**, never by its name. A title drifts whenever a rename was lost (a 429
  before §21.14, an answer that never came before §21.29, an old record), so a name is never a key.
- **"Gone" is confirmed, never inferred from one refused send** — Telegram has answered transient nonsense before
  (`BOT_TOPIC_WRONG_HOME` for 7 minutes right after the 6-hour hold).

### 8.2 Confirming
1. The refused item stays at its lane's head; the lane is held (`hold`).
2. 10 s later the sync PROBES the thread: `editForumTopic(thread, the session's CURRENT decorated name, its state icon)`.
   - ok or `TOPIC_NOT_MODIFIED` → the topic exists: the lane is released. The probe has also put the topic's title back in
     sync with the session (the drift the owner noticed) — and costs nothing when the title was right.
   - "thread not found" / `TOPIC_ID_INVALID` → a second probe 30 s later; two in a row = gone.
   - anything else (5xx, no answer, 429) → probed again on the back-off; the lane stays held, nothing is lost.

### 8.3 Making it again
1. `createForumTopic` with the session's current title and state icon (journaled `create`, §8.4) → `created` →
   `{"op":"lane","lane":"topic:t7","thread":<new>}` → the lane is released onto the new thread.
2. The topic's card goes first, with one line: "This topic was made again — the old one was deleted in Telegram." Then the
   held items, in order (their `message_thread_id` is the lane's new thread). The history replay (§21.9) runs for the
   new topic as for any new topic, so the conversation is complete again.
3. The record keeps its `uid`, its session, its mirror cursor; only `threadId` (and the card's message id) change.

### 8.4 A create is never made twice
- `create` (with the exact name sent) is journaled before the call and `created` after the answer.
- When the answer is lost, Telegram still sends the bot its OWN `forum_topic_created` service message through
  `getUpdates` (seen since 2026-10-16, §21.4). The pending create whose name matches, within 2 minutes, takes that thread
  — no second create. The name sent a moment ago is exact (drift needs time and a lost rename); two pending creates with
  one name are matched in order.
- After a restart the pending create is in the journal and the echo is still unconfirmed in `getUpdates` (its offset is
  saved within 2 s), so the same match works. Past 2 minutes with no echo, the create is made again (logged).

### 8.5 A Rekey or a recreation keeps the lane
`/clear`, `/resume` and `/handover-here` move the topic to the tab's next conversation (§21.6's Rekey): the `uid`, and so
the lane and everything queued in it, moves with the record.

### 8.6 Idle and closed
- **Idle** (the grace ran out): the topic's delete becomes the LAST item of its lane — what waits is delivered first, then
  the topic goes. Today's purge (`_deleteTopic` erasing the queued items) goes.
- **Closed** (the user closed the tab): Telegram deletes the topic WITH its messages, so posting into it a second before
  achieves nothing: the lane's items are dropped with the reason `session closed` (logged, never silent).

## 9. Completions and the mirror

### 9.1 `then`: completions that survive a restart
Today 65 callbacks (`[self …]` lambdas) carry what a send's answer changes. Each becomes a named, IDEMPOTENT completion
that works from the item's data, so it runs the same whether the item was queued in this run or recovered:
`card-posted` (the card's message id, form, picture), `card-edited`, `worklog-created`, `prompt-line-sent` (the turn's
prompt id for the reply's quote), `history-commit` (the replay's committed position, §21.9), `start-note`, `directory-posted`
/ `-edited` / `-pinned`, `question-card`, `menu-card`, `picker-posted`, `secretary-verdict`, … state.json saves
`outboxApplied` — the newest `ok` whose `then` it contains — so a start replays only the newer ones. What lives only for
the run (tap holds, a window's toast) stays an in-memory attachment of the item, as `onDone` / `tapHolds` are today.

### 9.2 The transcript cursor rides with its messages
The mirror derives messages from transcript bytes `[cursor, end)` and queues them as ONE group ending with
`{"op":"cursor","topic":…,"pos":end}`. A crash can therefore neither lose them (they are in the cursor's own group) nor
derive them twice (the restored cursor is past them). state.json's cursor stays as a hint; the journal's wins when newer.

## 10. Into today's code

- **`MessagingOutbox.h`** (pure, harness-tested): the records, the fold, the group rule, the CRC, the answer's verdict,
  the back-off, lane eligibility, the topic-gone state machine, the echo match. **`MessagingOutbox.cpp`**: the files,
  `.lock`, the flusher thread, rotation, checkpoint.
- `_enqueue` → `Outbox::Put(item)` / `PutGroup(items)`; `_sendLoop` keeps its pacing and calls `Outbox::Next()` /
  `Outbox::Settle(id, result)`; `_start` recovers instead of `_outbox.clear()`; `_deleteTopic` stops purging (§8.6).
- `_htmlItems`' and `_sendRich`'s fallback lambdas become the item's `fb`; `OutItem::beforeSend` + `PromptRef` become
  `ref.reply`.
- **One Telegram client per thread** (poll, send, tick, voice): `TelegramApi` has a single abort slot, so `Stop()` can miss
  the long poll today (a cog Save that stops the bot can freeze every window up to ~40 s). With its own client the sender's
  call in flight is aborted at `Stop()` and its item, left with a `try`, is sent again at the next start.
- The 10-minute census adds `queued`, `retrying`, `held lanes`, `dead`, `journal MB`, `oldest age`; the cog's status line
  and the start note name a backlog ("214 messages waited for Telegram since 11:33").

## 11. Efficiency

- **Queuing**: microseconds under the lock (§5.1); the JSON was being built anyway, and is built once.
- **Flushing**: at most 4 per second, ~1 per second at the chat's pace, off the queuing threads.
- **Disk**: the chat's ceiling is ~20 calls a minute (≈ 1,200 an hour); at ~2 KB a line that is ≤ 2.5 MB an hour, in 8 MB
  segments deleted as they drain; a checkpoint keeps the journal under 64 MB.
- **Start**: one sequential read of a few MB (85 ms for 10 MB in Python).
- **Memory**: the live items, and the last 10,000 `id → message id` pairs for `ref`.

## 12. Phases

| Phase | What | Its effect alone |
|---|---|---|
| P1 | lanes, the answer table, retries with back-off, the circuit breaker, the hold persisted, the topic-gone probe + recreate (§8.2 – §8.3) | a running bot stops losing messages to 502s; zombie topics end |
| P2 | the journal: `put` / `try` / `ok` / `retry` / `drop`, groups, CRC, `.lock`, the flusher, recovery, segment deletion — for `d` items without a completion (most replies, remarks, finals, command replies) | a restart, a swap or a crash loses nothing queued |
| P3 | `then` (the 65 callbacks by kind), `outboxApplied`, `l` items journaled | cards, work logs, history commits survive a restart exactly |
| P4 | the transcript cursor in the journal (§9.2) | the mirror can no longer skip or repeat a turn across a crash |
| P5 | `create` / `created` + the echo match, topic `uid`, lane retargeting, idle deletes after the drain | no duplicate topics, no purge |
| P6 | one client per thread, the census, the status line, `dead.jsonl` + its note, the checkpoint | clean stops; visible health |

**Tests** (the m5 harness, pure core): the fold over every record kind, complete and incomplete groups, a torn and a
zero-filled tail, a CRC failure, an id in two segments, keyed superseding, the answer table, the back-off, lane picking,
§5.6's kill table through an injected file layer that fails at step N, the topic-gone state machine, the echo match.
**A soak test**: a scripted fake Bot API (502s, timeouts, 429s, "thread not found") and a service killed at random
points: every durable item delivered at least once, each topic in order, nothing lost.

## 13. Open

- **O1 — the backlog after a long hold.** Six hours of a busy fleet can queue thousands of messages; at ~20 calls a minute
  the drain takes hours (live content first, history last, stale card edits collapsed to the newest). Whether content
  older than N hours should be folded into one digest per topic is the owner's call.
- **O2 — a re-sync of every topic's title**: the probe heals a drifted title only when its topic is in doubt. A
  `/resynctitles` (two calls per topic, ~1 hour for 566 topics at the budget) is possible; not decided.
- **O3 — "can't be deleted for everyone"** — answered (2026-11-11, [MESSAGING.md](MESSAGING.md) §21.51): not the rename notices
  (their own refusal line never fired). The ~500 refusals were the old CARD's delete when a card moved down: Telegram lets a
  bot delete its message only within 48 h (TDLib's `revoke_pm_time_limit` for a bot; a user's is unlimited), and 486 of them
  came from one whole-list re-sort (2026-11-04, §21.52). A card that old is no longer deleted at all: it stays, its buttons
  working.
- **O4 — disk full**: a refused append fails the queuing call; the in-memory item still goes, the census shows `journal
  write failed`, and the next successful append resumes durability. Never a reason to stop sending.

## 14. As built (2026-11-10, the owner's "Implement"; NOT BUILT into any install yet)

What exists, from design phases P1, P2, part of P5 and part of P6 ([MESSAGING.md](MESSAGING.md) §21.50 describes it as it works).
The places below where the code differs from §3 – §11 are deliberate cuts for this first part.

**The code.** [`MessagingOutbox.h`](../../src/cascadia/TerminalApp/AgentMaster/MessagingOutbox.h) (pure, header-only: UTF-8,
CRC-32, sealed lines, every record, the fold, the answer table, the item class and retry policy, the back-off, the breaker, the
lanes, the topic probe's state machine, the plain-text fallback) ·
[`MessagingOutboxJournal.{h,cpp}`](../../src/cascadia/TerminalApp/AgentMaster/MessagingOutboxJournal.h) (the files, `.lock`,
the flusher, the compaction) · `Messaging.{h,cpp}` (`_enqueue`, `_sendLoop`, `_outboxRecover`, `_tendHeldThreads`,
`_settleGoneThread`, `_outboxCheckpointIfDue`, `_queueRecoveredFallback`, `_outboxCensusText`; `_deleteTopic`'s idle wait) ·
`TelegramApi` (`Abort` ends every request, `Shutdown`) · m5 `TestMessagingOutbox`.

| Design | As built |
|---|---|
| §3 classes `d` / `l` / `e`, lanes `topic:<uid>` / `search` / `main` / `nt:` / `k:` | **Journaled = the plain posts** of the conversation (sendMessage / sendRichMessage with no key, no buttons, not history, not as the user, no upload — `OutboxItemIsDurable`). Keyed items and the rest stay in memory; their state machines rebuild them. **A lane is a THREAD id**: the posts of one thread keep their order behind a waiting post; there is no topic `uid` (a Rekey keeps the thread, so the lane follows the topic anyway) |
| §3 `fb`, `ref`, `then` | `fb` is built (a rich item's classic HTML). **No `ref` and no `then`**: a recovered item goes without its callback — no quote unless its put carried one, no completion. In-run items keep their callbacks (`onDone`, `beforeSend`) as before |
| §4 segments `j-<id>.jsonl` (8 MB), `blobs\` | **One file, `outbox.jsonl`**, rewritten compact at every start and past 16 MB (temp + rename); no blobs (uploads are not journaled). Records `hdr` (the bot's token hash and the owner's chat: another bot's or owner's journal is discarded), `put`, `try`, `ok`, `retry`, `drop`, `lane`, `flood`; no `hold`, `cursor`, `create`, `created` |
| §5 the outbox's own leaf lock | **The service's `_mtx`** orders the queue and the put (a put is appended in the same lock as the queue's change); the journal's own mutexes are leaves under it. Every append happens under `_mtx` or on the send thread for the item it holds, which the in-run compaction (send thread, under `_mtx`) relies on. Groups (`g` / `gn`) are not built: a message's chunks are separate puts |
| §5.5 the flusher's own handle, `.lock` | As designed: a second handle, ≤ 250 ms; `.lock` with no sharing and delete-on-close (a held lock = memory only for that run, logged) |
| §6 recovery | As designed, minus groups, `then` replay and the cursor: torn tail and damaged lines skipped and counted, `try`-without-outcome items sent again (counted `resent`), the back-off and the redirects kept, the flood hold restored in wall time |
| §7 the answer table and the breaker | As designed for ok / not modified / 429 / 5xx and no answer / parse and rich refusals / thread not found / other refusals (dead letters in `dead.jsonl`). **Not built**: the quote-refused row, the edit-target-gone row's re-post (edits are not journaled), 401 / 403 handling, the hourly Search note about dead letters, the status line. A **short 429** puts the item back in its place instead of calling again inline |
| §7 retries | Durable and keyed: until Telegram answers. **Bounded** (3 attempts) for the rest; **never** for createForumTopic, tap answers, chat actions, sends as the user, the secretary's sends and uploads |
| §8.2 confirming | As designed (two `editForumTopic` probes with the record's name and icon; a thread no record names is probed with no change), plus: no verdict waits 30 s doubling to 5 min; a thread the probes find 3 times within 10 min while it keeps refusing posts counts as gone. **Only posts hold a thread** (a topic call's or a delete's "not found" stays its answer) |
| §8.3 making it again | **Through the topic sync**: the gone topic's record is forgotten, so the sync makes the topic again like a new one, with its history replayed (§21.9) — the queued mirror and replay items are DROPPED (the replay sends them again), the rest moves to the new thread (`lane`), a silent "♻️ This topic was made again" line goes first, the verbose mode carries over. No new topic within 15 min → the items are given up (their callbacks hear "message thread not found") |
| §8.4 create / created + the echo | **Not built**: topic creates stay the sync's direct calls, as before |
| §8.6 idle and closed | As designed: an idle delete waits while durable items for its thread are queued; any other delete drops them, journaled with the reason |
| §9.2 the cursor in the journal | **Not built**: the mirror's cursor stays in `state.json` only, so a kill within `kStateProgressSaveGapMs` after a mirror pass shows that pass twice (at-least-once) |
| §10 one Telegram client per thread | **Instead**: one client whose `Abort` ends every request in flight and whose `Shutdown` refuses new ones (`Stop` uses it) — the stop no longer misses the long poll |
| §10 the census and the status line | The census line's `outbox(…)` part is built; the cog's status line and the start note's backlog are not |
