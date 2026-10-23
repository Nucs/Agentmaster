# 22 · The `agentmaster` CLI and the session database (CLI, SDB)

> Conventions and fixtures: [README.md](README.md). Spec: CLI.md (§5 the verbs, §7 targeting, §11 the known gaps);
> SESSIONS_DB.md §16 (what is built), §16.6 (running it by hand), §17 (the user's requirements it does not meet yet).
> Needs F-CORP; the CLI runs in the guest against the test profile (`--profile <dir>` or `AGENTMASTER_PROFILE`), with
> the app running and with it closed.

**What shipped.** The CLI's read-only verbs predate 0.7.7 (`show`, `list`, `sessions`, `tabs`, `windows`, `external`,
`--self`, `--json`); it works with the app up or down. 0.8.0: the **session database** preview — Off by default;
**Shadow** builds it and compares; **On** runs the Sessions page's content search on it and gives the AI search ten
database tools from the next start; the CLI verbs `search`, `sql`, `sessdb`; a stdio MCP server registered by hand.

## Lifecycle model

```text
 CLI: agentmaster <verb> ─► the alias shim (console) ─► agentmaster-cli.exe ─► reads sessions.json · windows/*.json · PEB facts ·
      transcripts · presence heartbeats ─► text or {schemaVersion: 2, profile, …}; exit 0 ok · 1 not found / ambiguous · 2 usage
 DB:  Off ─► Shadow (built in the background; [sessdb-diff] beside today's search) ─► On (the page's content phase; the AI search's tools at the next start)
      agentmaster-sessiondb.exe per profile (pipe; idle exit 10 min) ◄─ the app's refresh tick (5 s: changed sessions upserted)
                                                                      ◄─ agentmaster search / sql / sessdb ◄─ an MCP client (`… mcp`)
```

---

### CLI-01 · The fleet from a shell, app up or down

> **Since** ≤ 0.7.7 · **P1** · **Needs** mock, F-S2, F-W2 · **Spec** CLI.md §5, §6

**Phases**
1. **App up.** *Do:* `agentmaster sessions --json`, `tabs --json`, `windows`, `list`, `show <title> --tail 4`. *Expect:* exit 0;
   the envelope `{schemaVersion: 2, profile, …}`; every open session with its derived state, folder, pid; `tabs` in strip
   order per window; `show` with identity, state, the last 4 text blocks, the queue and the runner mode.
2. **`--self`.** *Do:* from a shell tab inside Agentmaster (`agentmaster show --self`) and from a Claude session's Bash tool
   (`[[mock bash "agentmaster show --self --json"; reply "x"]]`). *Expect:* the calling tab's own session.
3. **App down.** *Do:* quit; repeat phase 1. *Expect:* the same sessions (from disk), states derived from transcripts and
   heartbeats; exit 0.
4. **Errors.** *Expect:* an unknown ref → exit 1; an ambiguous ref → exit 1 listing the candidates; a bad flag → exit 2.

**Edge cases**
- **E1 `agentmaster --help` as the first token** → forwarded to the GUI (a message box): put a verb first.
- **E2 A session whose claude is busy with a background shell** → reads Idle (known gap, CLI.md §11).
- **E3 "Waiting for you"** → reads Idle in the CLI (known gap).
- **E4 Hebrew titles** → printed correctly in a UTF-8 console and in `--json`.

---

### CLI-02 · The CLI targets the right install

> **Since** ≤ 0.7.7 · **P2** · **Needs** the release and dev identities both installed (or two profiles) · **Spec** CLI.md §7, §11

**Phases**
1. **By alias.** `agentmaster sessions` → the release profile; `agentmasterdev sessions` → the dev profile.
2. **Cross-target.** `agentmaster sessions --instance dev` → the dev profile.

**Known defects (expected failures until fixed)**
- **CLI-K1 The dev alias fails on every real verb** (`0xC0000409` in `exit()`, the Debug build's Control Flow Guard table;
  CLI.md §11, 2026-10-09). Expected: exit 0 with the output.
- **CLI-K2 An unpackaged release CLI with no inherited `AGENTMASTER_PROFILE` reads the DEV profile** (the project defines
  `AGENTMASTER_DEV` for every configuration; CLI.md §11) — so a per-user install's `agentmaster-cli.exe` run from a plain
  shell answers from `~/.agentmaster-dev`, and creates it. Expected: the install's own profile.

---

### SDB-01 · Shadow builds the database and compares

> **Since** 0.8.0 · **P2** · **Needs** F-CORP (≈ 300 sessions, so a cold build takes minutes, not the 37 min of 11 GB) · **Spec** SESSIONS_DB.md §16

**Phases**
1. **Off by default.** *Expect:* no `agentmaster-sessiondb.exe` process; no `<profile>\sessiondb\sessions.duckdb`.
2. **Shadow.** *Do:* ⚙ → Sessions → SESSION DATABASE → Shadow; Save. *Expect:* the service copied to
   `<profile>\sessiondb\bin` (with `duckdb.dll`) and started; the cold build runs in the background (the UI stays
   responsive); [LOG] `<profile>\sessiondb\sessiondb.log` `[sessdb]` progress.
3. **Compare.** *Do:* search the Sessions page for 10 words. *Expect:* the page's results exactly as with Off; hooks.log
   `[sessdb-diff]` lines with no missing or extra session (or each difference explained).

---

### SDB-02 · On: the page and the AI search use the database

> **Since** 0.8.0 · **P2** · **Needs** SDB-01's database, mock (for the AI search) · **Spec** SESSIONS_DB.md §16.3

**Phases**
1. **On.** *Do:* switch to On; Save; restart (the AI search takes the mode at start). *Expect:* the page's content phase
   answered by the database (faster on a large corpus); the fast phase unchanged.
2. **The AI search's tools.** *Expect:* [MOCK] the harness's tool list holds the ten `mcp__agentmaster__*` tools (not the page
   search's); a scripted `mcp__agentmaster__search` call returns rows; `show` resolves the database's short ids.

**Edge cases**
- **E1 The service missing beside the app** (a layout without `duckdb.dll`) → the AI search falls back to the page search
  (logged); the page keeps its own search.

---

### SDB-03 · The database from a shell, and staying current

> **Since** 0.8.0 · **P2** · **Needs** SDB-01's database, mock · **Spec** SESSIONS_DB.md §16.6; CLI.md §5

**Phases**
1. **Search.** `agentmaster search tab drag crash` → ranked sessions with snippets.
2. **SQL.** `agentmaster sql "SELECT count(*) FROM v_sessions"` → the count; a statement over the 2.5 s deadline → cancelled
   with a message.
3. **Status.** `agentmaster sessdb status` → the version, the file, the refresh state; `resync` → a sweep; `stop` → the
   service exits; the next `search` starts it again.
4. **Fresh text.** *Do:* a new turn whose reply holds a unique word (`[[mock reply "zebracornet"]]`). *Expect:* `agentmaster
   search zebracornet` finds the session within the refresh period + a merge (≤ ~10 s).
5. **Idle exit.** *Expect:* with no client for 10 minutes the service exits.

**Edge cases**
- **E1 A transcript deleted from disk** → still searchable, marked deleted (`transcript_present` false).
- **E2 The one-shot modes against a profile whose service runs** → never (they open the file themselves); the CLI goes through
  the service.

---

### SDB-04 · The MCP server

> **Since** 0.8.0 · **P2** · **Needs** SDB-01's database, mock · **Spec** SESSIONS_DB.md §16.2 (D6)

**Phases**
1. **Register by hand.** `claude mcp add --scope user agentmaster -- "<profile>\sessiondb\bin\agentmaster-sessiondb.exe" mcp`.
2. **Use it.** *Do:* in a session `[[mock tool mcp__agentmaster__search {"query": "tab drag crash"}; reply "found"]]`. *Expect:*
   the tool result lists sessions ([MOCK] `expect`).
