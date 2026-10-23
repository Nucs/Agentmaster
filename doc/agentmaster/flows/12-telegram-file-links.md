# 12 · Telegram: file links (TGF)

> Conventions and fixtures: [README.md](README.md). Spec: MESSAGING.md §21.32.4 (as built). Needs **F-TG**, an HTTP
> client on the test machine standing in for the phone, and (hosted mode) the `cc.sysd.cc` route — or a local
> stand-in for it.

**What shipped (0.8.0, off by default).** With **Settings → Messaging → FILE LINKS** on, every `[name](file:///…)`
link in Claude's own text to an existing, allowed file becomes a download link served by a small server inside
Agentmaster — on the home network (private addresses only) or through a hosted HTTPS relay. A link opens only a file
its session linked, for 30 days after its last mention; secrets and excluded folders are never linked; `/download
<path>` links any file of the session, with **📤 Send as file** (≤ 50 MB).

## Lifecycle model

```text
 Claude's text mentions [x](file:///C:/w/a.txt) ─► rule checks (exists · not a folder · not secret · not excluded · path legal)
     ├─ pass ─► link <base>/station-<station>/<session key>/<encoded path>; the file joins the session's LINKED SET (30 days)
     └─ fail ─► left exactly as Claude wrote it
 a GET on that link ─► peer private? ─► station ours? ─► key one of ours? ─► path in that session's set, not expired? ─►
     re-check names + FINAL path (symlinks) ─► 200 / 206 (Range) as an attachment          otherwise 403 / 404 / 405 / 503
```

---

### TGF-01 · A file Claude links becomes a download link

> **Since** 0.8.0 · **P1** · **Needs** F-TG, mock, LAN mode · **Spec** MESSAGING.md §21.32.4

**Phases**
1. **Turn on.** *Do:* cog → Messaging → FILE LINKS: On the home network (LAN base from the dropdown). *Expect:* the
   status line says the server listens (release port 32594 / dev 32595); [LOG] the listen line.
2. **Mention.** *Do:* `[[mock write C:\e2e\w\t1\report.md "# r"; reply "See [report](file:///C:/e2e/w/t1/report.md)."]]`.
   *Expect:* the topic's reply shows a normal link whose URL is
   `http://<lan>:<port>/station-<station>/<64 hex>/C/e2e/w/t1/report.md`.
3. **Download.** *Do:* GET the URL from the test machine (a private address). *Expect:* 200, `Content-Disposition:
   attachment` with the exact name, `no-store`, `nosniff`; the bytes equal the file; HEAD works; a `Range` → 206; an
   impossible range → 416. *Check:* [LOG] `[file-link] 200 GET <station> <key8> "report.md" …`.
4. **Restart.** *Expect:* the same link still works (the linked set persists in `filelinks.json`).

**Edge cases**
- **E1 A secret-looking file** (`.env`, `id_rsa`, `*.pem`, `.claude.json`, under `.ssh`) → left as written.
- **E2 An excluded folder** (cog list) → left as written; excluding it later makes an existing link 404.
- **E3 A folder link, a UNC share, a relative path, `..`, a stream name, `nul.txt`, a trailing dot / space** → left as written.
- **E4 A link inside a code span or block** → untouched.
- **E5 The user's own prompt** holding a file URL → never rewritten.
- **E6 The history replay** → links rewritten there too.

---

### TGF-02 · The server refuses everything it should

> **Since** 0.8.0 · **P1** · **Needs** LAN mode, an HTTP client · **Spec** MESSAGING.md §21.32.4

**Phases** (each a request against the running server)
1. A well-formed URL with a WRONG key → 404.
2. Another session's key with this file's path (not in ITS set) → 404.
3. A path never linked → 404; an expired link (> 30 days since the last mention; time-shifted test) → 404.
4. `POST` / `PUT` → 405; any other path shape → 404; `/healthz` → 200.
5. A public (non-private) peer address → refused.
6. A symlink linked under an allowed name that points at a secret → refused at the FINAL path check.
7. 25 parallel connections → the 25th gets 503.

---

### TGF-03 · /download and Send as file

> **Since** 0.8.0 · **P1** · **Needs** F-TG, mock · **Spec** MESSAGING.md §21.32.4

**Phases**
1. `/download report.md` (relative to the session's folder) → the name and size, ⬇ Download link, the path, and
   [📤 Send as file].
2. Tap 📤 → the file is read again (still there, allowed, ≤ 50 MB) and uploaded as a document; the button busy until
   it is in.
3. `/download` with no path, a folder, a secret, an excluded or missing file → the reason; in 🔎 Search → "works in a
   session's topic".
4. A 60 MB file → refused for Send as file (the link still offered).

---

### TGF-04 · Hosted mode and the off switch

> **Since** 0.8.0 · **P2** · **Needs** the relay or a stand-in · **Spec** MESSAGING.md §21.32.4

**Phases**
1. **Hosted.** Mode "Through cc.sysd.cc" → links use the hosted base; a request through the relay reaches the station.
2. **Off.** Mode Off → no rewrite; the server stops; existing links stop working.
3. **Void every link.** Delete the credential `Agentmaster:messaging:telegram-filelinks:<profile leaf>`, restart the bot →
   a new secret; old links 404.

**Known open item** (MESSAGING.md §21.32.4): whether Android's DownloadManager passes the post-quantum guard — a manual
check on a phone, out of the automated suite.
