# Agentmaster — voice notes: a Telegram voice note becomes the session's prompt

> **Status (2026-10-27): the POC and the integration are WRITTEN; NOT BUILT.** The POC (`tools/voice-poc/`, Python)
> runs on this PC and measured the engines below. The app integration (MESSAGING.md §21.43) is in the tree: the
> messaging service's voice path, the session vocabulary in C++ (pinned to the POC by goldens: 190 checks pass
> standalone), the voice worker protocol, the endpoint engine, the settings and the cog's VOICE NOTES section. The
> next build must PURGE (SessionModels.h changed). The bundled native worker (`agentmaster-voice.exe`, §6.4) is designed,
> not written: until it ships, the local engine is any worker command — the POC's Python worker on this PC.
>
> The owner's recordings of the evaluation set: §7.1. Their results: §4.4.

## 1. The ask and the shape of the answer

**The ask (2026-10-27, from the Telegram topic "fluidvoice telegram"):** look into FluidVoice; let a Telegram voice note in
a session's topic be transcribed into that session's prompt; three technical sentences, read by the owner, must come out
close to perfect; the evaluation is a test that runs locally (and later in e2e); a separate POC, this design, and the full
integration in Agentmaster.

**The answer in one paragraph.** A voice note in a session's topic is downloaded and handed to a speech engine together
with the session's VOCABULARY — the names of its project (its folder, its CLAUDE.md), a developer glossary and the owner's
own dictionary. The engine's text then goes through a DICTIONARY PASS that respells split or misheard names ("Con PTY",
"Agent Master", "Jason L" → ConPTY, Agentmaster, JSONL) and never touches an everyday word. The result is posted under the
note (🎤) and read exactly like a typed message: a queued prompt, or the answer to an open question card. This is
FluidVoice's design — word boost + custom dictionary (+ optional AI enhancement) — with the vocabulary taken from where the
prompt goes instead of a list the user maintains.

## 2. FluidVoice — what it is, and what this design took from it

FluidVoice ([altic-dev/FluidVoice](https://github.com/altic-dev/FluidVoice), GPLv3 since 2026-02-23, Apache-2.0 before;
~12.5k stars) is a local dictation app for macOS 15+ (a Windows pre-release exists). Read 2026-10-27 at its `main`:

| Layer | FluidVoice | Here |
|---|---|---|
| Speech model | Parakeet TDT v3 by default (also v2, Nemotron Speech 3.5, Parakeet Flash, Cohere Transcribe, Whisper Tiny–Large, Apple Speech), run on the Apple Neural Engine through **FluidAudio** (CoreML) | The same models on Windows: Parakeet TDT 0.6B v2/v3 through **sherpa-onnx** (ONNX Runtime, CPU), Whisper large-v3 through **faster-whisper** (CTranslate2, CUDA) |
| Word boost | `ParakeetVocabularyStore`: up to 256 terms with aliases and weights (`parakeet_custom_vocabulary.json`: alpha 2.8, minCtcScore −2.2, minSimilarity 0.72, minCombinedConfidence 0.64, minTermLength 3) — FluidAudio's CTC keyword spotting | Whisper's `initial_prompt` (the glossary line), or sherpa-onnx per-stream **hotwords** (modified beam search + an Aho-Corasick context graph) |
| Custom Dictionary | trigger spellings → replacement, after recognition; built-in entries `claud, clawed → Claude`, `post hoc → PostHog`, `hugging face → Hugging Face`, `fluid voice → FluidVoice`; replacement protection rules | The **dictionary pass** (§5.4): curated triggers (the same idea, `clawed → Claude` included) + spellings GENERATED from every vocabulary term, guarded by an everyday-English test |
| Text formatting | filler-word removal, spoken punctuation ("comma", "new line"), dictation literals | Not taken: a prompt to Claude Code tolerates both; the engines punctuate |
| AI Enhancement | optional post-processing by OpenAI / Groq / a custom provider, or the local ~3.5 GB "Fluid Intelligence" (not GPL) | Optional layer 3: one headless `claude -p` correction with the glossary (POC: `llm_fix.py`; app: §10, not built) |
| Where the words come from | the user's word list | **the session**: its folder name, the hard names of its CLAUDE.md, the developer glossary, the owner's dictionary (§5.1) |

Nothing of FluidVoice's code is copied (GPLv3 vs this repository's AGPL is compatible, but no copy was needed): the
studied files are `ParakeetVocabularyStore.swift`, `BuiltInDictionary.swift`, `ASRService.swift` (the pipeline: filler
removal → custom dictionary → word-boost hit tracking), `Package.swift` (FluidAudio, transcribe-cpp-swift).

## 3. In Telegram

1. The owner holds the mic in a session's topic and talks. The note gets 👀 (heard).
2. The voice thread downloads it, transcribes it with the session's vocabulary, runs the dictionary pass.
3. The transcript is posted silently under the note: `🎤 Check why the ConPTY host in Agentmaster drops …` (italic; the
   echo can be switched off, §9).
4. The note is read as that text, through the same function as a typed message (`_handleTopicMessage`): queued as the next
   prompt with its reactions (✍ → 👨‍💻 → …) on the NOTE; or taken as the answer by an open question card (§21.30); or, for a
   note sent while Agentmaster was closed, asked about first (D2 — judged at the note's ARRIVAL, so a slow transcription
   never turns a live note into a backlog question).
5. A message the owner sends right after the note WAITS behind it (the thread's lane, §6.5), so "voice, then a typed
   follow-up" reaches the session in that order.

A note that cannot be read is answered and queued nowhere: no session in the thread, voice notes off, no engine set, a file
over 20 MB (the Bot API's getFile limit), a failed transcription ("🎤 I could not transcribe this voice note: <why>").

## 4. The POC (`tools/voice-poc/`)

### 4.1 Layout

Python 3.12 in a venv on Q: (`Q:\agentmaster-voice\venv`, `--system-site-packages` for the base interpreter's CUDA torch);
models, recordings and results on Q: too (K: is nearly full; the repository is public, so the owner's voice never enters
it). The files and how to run them: [`tools/voice-poc/README.md`](../../tools/voice-poc/README.md).

### 4.2 Engines (this PC: i9-13900K, RTX 4060 Ti 16 GB)

| Engine | Runtime | Size | Speed (RTF, measured) | Biasing |
|---|---|---|---|---|
| Parakeet TDT 0.6B **v2** (English) | sherpa-onnx 2.0.1, CPU, 8 threads, int8 | 650 MB | ~0.07 (a 10 s note in 0.7 s) | hotwords (beam search) |
| Parakeet TDT 0.6B **v3** (25 European languages) | same | 670 MB | ~0.07 | hotwords |
| Whisper **large-v3** | faster-whisper 1.2.1 / CTranslate2 4.8.4, CUDA fp16, beam 5 | 3 GB | ~0.09 | `initial_prompt` |
| Whisper **large-v3-turbo** | same | 1.6 GB | ~0.04 | `initial_prompt` |

Notes. CTranslate2 needs cuDNN 9 + cuBLAS 12: the POC adds torch's own `lib` folder to the DLL path
(`common.add_cuda_dll_dirs`), because this PC's PATH reaches an older torch with cuDNN 8 first. sherpa-onnx's Parakeet
models ship no `bpe.vocab`; hotwords given as words need one, derived from `tokens.txt` with equal scores (sherpa-onnx's own
example does this). Parakeet does not take Hebrew; Whisper does (§11).

### 4.3 Scoring

* **WER** after Whisper's English normalizer (case, punctuation, "forty" = 40, British spellings) plus a unit map
  ("16 kHz" = "sixteen kilohertz"): did the words come through.
* **Term rate**: each technical term of the sentence present as one exact token sequence ("conpty", not "con pty").
* **Cased rate**: each term with its canonical spelling in the raw text ("ConPTY", "JSONL"; an all-lowercase term like
  "named pipe" only as the same words).

### 4.4 Results

**Synthetic voice notes** (Windows TTS "David" → Ogg/Opus 32 kbit/s; a pipeline check — a synthetic voice reads "Ogg"
as "Ag", which no person does):

| Configuration | WER | Terms | Cased | RTF |
|---|---|---|---|---|
| whisper-large-v3 + prompt, session vocabulary, + dictionary pass | **0.0 %** | 100 % | 100 % | 0.09 |
| whisper-large-v3-turbo + prompt (session or builtin), ± dictionary | 2.8 % | 93 % | 93 % | 0.04 |
| parakeet-v2 / v3 greedy + session dictionary pass | 2.8 % | 93 % | 86 % | 0.07 |
| parakeet-v2 beam + session hotwords + dictionary | 2.8 % | 93 % | 93 % | 0.08 |
| whisper-large-v3 alone (no vocabulary) | 7.0 % | 79 % | 64 % | 0.09 |
| parakeet-v2 greedy alone | 11.3 % | 71 % | 64 % | 0.07 |

What the synthetic set taught the design (each one changed a rule, §5):
* Mining every capitalized word of CLAUDE.md made the dictionary pass rewrite "on" → "ON", "and" → "AND" and
  "session state" → "SessionState": only HARD names are mined now, and an everyday word is never respelled.
* ~480 hotwords made Parakeet WORSE than none, and a beam search with hotwords invented "ASCII" from "a session": only hard
  terms become hotwords, with gentle scores; greedy + the dictionary pass is the safer Parakeet setting.
* The spell checker knows "Pty" (as in "Pty Ltd"): an all-capital part is now judged by its lowercase spelling only.

**The owner's recordings** (§7.1): pending — the topic got the sentences as message 14016 on 2026-10-27; this section gets
the table when the notes are in.

## 5. The session vocabulary

The reference is `tools/voice-poc/vocab.py`; `VoiceVocabulary.h` is its line-for-line C++ port, held to it by goldens
(§7.5). A rule changes in `vocab.py` first.

### 5.1 Where the words come from

| Source | Weight | Respell from everyday words? |
|---|---|---|
| The built-in developer glossary (`BUILTIN_GLOSSARY`: formats, protocols, audio, hardware, Windows, languages, tools, databases, AI models — ~170 terms) | 1 | yes (curated) |
| The session's folder name (`Agentmaster`) | 3 | yes ("agent master" → Agentmaster) |
| The hard names of the folder's CLAUDE.md (§5.2), at most 60, most frequent first | 2 | no ("session state" stays) |
| The session title's hard words | 2 | no |
| The owner's dictionary (`voiceDictionary`, §5.4) | 3 | yes, plus its explicit triggers |

### 5.2 Hard words, and the everyday-English test

A term is **hard** when a speech model is likely to get it wrong: it holds a digit or `+`/`#`, or one of its case-split parts
is not everyday English (`ConPTY` → Con + **PTY**; `JSONL`; `Agentmaster`). `SessionState` is not hard: "session state"
comes out right, only its spelling differs. A term is **distinctive** (the dictionary pass may respell INTO it) when it has
an inner capital, is an acronym, holds a digit or symbol, or is a word that is not everyday English.

**Everyday English** is one question, asked of Windows' own en-US spell checker (`ISpellChecker`, spellcheck.h; in the box
since Windows 8 — the POC asks the same API through comtypes, so both agree): an ALL-CAPITAL word of 2+ letters is a word
only when its lowercase spelling is known ("ON", "AND"; not "PTY", "DB", "UI"); any other word when its lowercase OR its
title-case spelling is known (proper nouns are capitalized in the dictionary: "English", "Claude", "Kubernetes"). With no
dictionary (an N edition without the language), every word reads as known: the safe answer, nothing is respelled.

**Mining CLAUDE.md** keeps a hard, distinctive word seen at least twice, and drops code-only spellings nobody dictates (an
underscore, `::`, a file name, a cmdlet like `Get-AppxPackage`, an error code like `C2039`), shouted English ("NOT",
"BEFORE", a long all-caps name), two-letter acronyms seen fewer than 8 times, and compounds of everyday words. On this
repository's CLAUDE.md (at `a935ca604`) that yields Agentmaster, Claude, XAML, ConPTY, MSIX, SDK, DuckDB, x64, … (60).

### 5.3 The three uses

1. **Whisper's prompt** — `A developer dictates a prompt to Claude Code. Glossary: <terms>.`, highest weight LAST (Whisper
   keeps the last ~224 tokens and weighs the end most), cut from the start at ~700 characters.
2. **Parakeet's hotwords** — `Agentmaster :3.0/ConPTY :2.5/JSONL :2.0/…`: hard terms only, scores 2.0 / 2.5 / 3.0 by weight,
   at most 96.
3. **The dictionary pass** (§5.4) on whatever text the engine wrote — every engine, the endpoint's included.

### 5.4 The dictionary pass

Windows of 1–4 words, longest first, left to right; a window is SQUASHED (lowercase, letters and digits, `+`/`#` kept) and
looked up among: the curated triggers (explicit), the canonical spellings and the GENERATED spoken forms of every
distinctive term (its case-split parts "con pty", its acronyms spelled out "con p t y", a long name's two-part splits
"agent master"). Guards for a generated form: a single everyday word is never rewritten ("go", "on", "rejects"); several
everyday words respell only into a curated term (folder name, glossary, the owner's dictionary — never a mined identifier).
A curated trigger is a decision someone made and passes the guards ("clawed" → Claude, "jason l" → JSONL). Punctuation around
a window is kept; a canonical window is left alone (the pass is idempotent).

The owner's dictionary (`voiceDictionary`): one entry per line — `Word` (boost it, respell its generated forms) or
`spoken form, other form => Word` (FluidVoice's trigger → replacement, explicit); `#` starts a comment.

## 6. The integration

### 6.1 The seam

`MessagingService` (engine code, also in the m5 harness) knows only `Voice::VoiceBridge` (`VoiceBridge.h`): `Transcribe(note)`
→ the corrected transcript, `Configure(settings)`, `Tick()` (the idle end), `Shutdown()`, `Describe()`. The app layer sets the
factory (`TerminalPage.AgentEngine.cpp`, beside the Search agent's): `Voice::MakeVoiceTranscriber()` (`VoiceTranscriber.cpp`)
— for each note it builds the session vocabulary (cached while the session's folder, title, CLAUDE.md stamp and the
dictionary are unchanged), runs the engine, runs the dictionary pass and folds the text into one paragraph. Engine.cpp never
names it; the harness has no factory and answers "not available in this build".

### 6.2 The voice worker protocol (v1)

A VOICE WORKER is any program that speaks JSON lines on stdin / stdout (`VoiceProtocol.h`): `hello` (load the engine; say
whether it takes a `prompt` and `hotwords`), `transcribe {path, language, prompt, hotwords}` → `{ok, text, audio_ms,
decode_ms, engine}`, `quit`. Rules: stdout carries only protocol lines (anything else is logged and skipped); every request
gets exactly one answer with its id; a failure is `{"ok":false,"error":…}`, never an exit. The app sends both `prompt` and
`hotwords`: the worker uses what its engine takes. The vocabulary and the dictionary pass stay in the app, so every engine
gets the same corrections.

`VoiceWorker.cpp` runs ONE worker per process: started hidden by the first note (`CREATE_SUSPENDED`, put into a kill-on-close
job, then resumed — nothing it starts outlives Agentmaster), the three pipes passed by `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`
only, its stderr into `voice.log` line by line, `PYTHONUNBUFFERED=1`, `PYTHONIOENCODING=utf-8` and `AM_HARNESS=voice-worker`
in its environment. A transcription's deadline is 60 s + 4 s per second of audio (at most 15 min); a missed deadline ENDS the
worker (the next note starts a fresh one). It ends after `voiceWorkerIdleMinutes` without a note. A cog Save that changes
the engine KILLS the running worker from the UI thread without waiting (a note in flight then fails fast); its cleanup runs
on the voice thread.

`tools/voice-poc/voice_worker.py` is a worker for the POC's engines:

```
"Q:\agentmaster-voice\venv\Scripts\python.exe" "K:\source\Agentmaster\tools\voice-poc\voice_worker.py" --engine whisper-large-v3+prompt
```

### 6.3 The endpoint engine

`voiceEngine = "openai"`: one multipart POST per note to an OpenAI-compatible `/v1/audio/transcriptions` URL (OpenAI
`whisper-1` / `gpt-4o-transcribe`, Groq `whisper-large-v3-turbo`, a local server) with `model`, `language`, `prompt` (the
glossary line), `response_format=json`, `temperature=0`, the note as `voice.ogg` (`VoiceHttp.cpp`, WinHTTP). The key is in
Credential Manager (`Agentmaster:voice:api-key:<profile leaf>`), wiped from the request headers' string after the send.
⚠ A cloud endpoint receives the owner's voice; never the default.

### 6.4 The bundled worker — `agentmaster-voice.exe` (DESIGN, not written)

A worker that ships with Agentmaster, so a fresh install reads voice notes with nothing to set up, the way FluidVoice does:
* **Engine:** Parakeet TDT 0.6B v2 int8 (English; v3 for 25 European languages) through the sherpa-onnx **C API**
  (`sherpa-onnx-c-api.dll` + `onnxruntime.dll`, Apache-2.0 / MIT), CPU — ~0.07 RTF here, no GPU needed. Greedy decoding +
  the app's dictionary pass (§4.4: the beam search with hotwords invents words).
* **Audio:** a minimal Ogg demuxer + **libopus** (vcpkg `opus`, BSD) → 48 kHz float → a polyphase FIR to 16 kHz.
* **Native runtime:** pinned by SHA-256 like DuckDB (`tools\Get-SherpaOnnx.ps1` + `voice\sherpa-onnx.props`, the release
  archive `sherpa-onnx-v2.0.1-win-x64-shared.tar.bz2`), copied into the layout beside `agentmaster-voice.exe`;
  `.am-manifest`, `release.yml` and `Export-AgentmasterSymbols.ps1` list it.
* **Model:** not in the MSIX (650 MB): downloaded on first use into `%USERPROFILE%\.agentmaster-models\parakeet-tdt-0.6b-v2-int8\`
  (shared by every profile; never under the virtualized `%LOCALAPPDATA%`), each file checked against its Hugging Face LFS
  SHA-256 (`encoder.int8.onnx` a32b12d1…, `decoder.int8.onnx` b6bb6496…, `joiner.int8.onnx` 79461643…). The first note
  waits for the download, and the topic says so ("⬇ downloading the speech model, 650 MB").

### 6.5 Threads and order

* **Poll thread** — `_voiceTake`: a note becomes a job (FIFO across every topic: one engine, one note at a time) and a
  pending item at the end of its thread's LANE; any other message of a thread whose lane holds something joins the lane
  (an edit replaces the held message it edits).
* **Voice thread** — `_voiceLoop` (COM multithreaded apartment, for the spell checker): `getFile` (paced by the chat's call
  slot), the download (`TelegramApi::DownloadFile`, 20 MB cap) into `<profile>\messaging\telegram\voice\<message id>.oga`,
  the transcriber, the file DELETED, the lane item marked done, the tick woken; between jobs the transcriber's idle end.
* **Tick thread** — `_voiceTick`: takes each lane's ready items in order (a done note → `_voiceDeliver`: the echo, then
  `_handleTopicMessage` with the text; a held message → `_handleMessage(…, fromVoiceLane=true)`), one at a time under the
  leaf lock `_voiceMtx`, delivered outside it. A lane leaves the map only when found empty, so a message read meanwhile
  joins it BEHIND what is being delivered.
* **Stop** — `_voiceStop` runs first: it kills the engine (a note in flight fails fast), joins the voice thread, and drops
  the queue and the lanes (logged). ⚠ Their notes and held messages are not read: the owner sends them again.

### 6.6 Files and logs

| What | Where |
|---|---|
| A downloaded note | `<profile>\messaging\telegram\voice\<message id>.oga`, deleted right after its transcription |
| The worker's stderr, its start / stop / idle end, each transcription's timings and respellings | `<profile>\voice.log` |
| The service's lines | `hooks.log`: `[msg] <sid> voice note queued for transcription …`, `… transcribed (… chars, <engine>, … ms, N vocabulary fix(es))`, `… not transcribed …`; `[nav] remote telegram voice …` |

## 7. Evaluation and tests

### 7.1 The owner's recordings

The three sentences of `tools/voice-poc/references.json` were posted in the topic (message 14016). The owner reads each as one
voice note and sends "done"; `fetch_tg_voice.py fetch --after 14016 --before <done's id> --ids s1,s2,s3` recovers them
through the bot without `getUpdates` (a silent forward gives the note's `file_id`, the forward is deleted at once, `getFile`
+ the download) into `Q:\agentmaster-voice\recordings\` with a `manifest.json`. They stay on Q: (the repository is public).

### 7.2 Synthetic fixtures

`synth_fixtures.py` (Windows SAPI → Ogg/Opus like a Telegram client records) gives the pipeline a check without the owner;
it is never the evaluation.

### 7.3 The local test

```
python eval_voice.py --engines whisper-large-v3+prompt --contexts session \
    --expect "whisper-large-v3+prompt | ctx=session | dict" --max-wer 0.05 --min-cased 1.0
```

exits 1 when the configuration misses the bar (corpus WER ≤ 5 %, every term with its exact spelling), 2 when the recordings
are missing. The session context is CLAUDE.md pinned at `a935ca604` (`--claude-md-rev`), the repository before this feature
documented itself, so the test cannot grow easier as VOICE.md's words land in CLAUDE.md.

### 7.4 Generality

The three sentences and the glossary have one author: the glossary is a general developer list and names no sentence's
phrase, but the honest test is a NEW set — the next owner session reads three new sentences, unseen by the rules, and they
join `references.json` (`s4`…).

### 7.5 C++ tests (the m5 harness)

* `TestVoiceVocabulary` — `VoiceVocabulary.h` against `tests\voice\vocab_goldens.json`, which
  `tests\voice\make_goldens.py` writes by running `vocab.py`: 33 terms' rules, the context terms of
  `tests\voice\context_sample.md`, three vocabularies (terms, weights, sources, respell flags; Whisper's prompt at two
  budgets; the hotwords at two limits; the dictionary pass over 18 transcripts, text + respellings, idempotent), the owner's
  dictionary text. The word test answers from `tests\voice\plain_words.txt`: the spell checker's answers the reference
  recorded, so the suite does not depend on the machine's dictionary; a word the port asks that the reference never did
  fails the suite (parity drift).
* `TestVoiceProtocol` — the request lines (one line each, escaped), the answer reader, the prompt text.

Both pass standalone (190 checks; `cl` of `tests_voice.cpp` alone). The service's voice path needs Telegram: e2e.

### 7.6 E2E (later)

A fake Bot API in the e2e host (`getUpdates` serving one `message` with a `voice` of a fixture `.oga`, `getFile`, `/file/bot…/…`
serving the fixture, `sendMessage` / `setMessageReaction` recorded) and a fake worker (`voice_worker.py` with a canned
engine, or the real one on the guest's CPU with Parakeet) drive: the echo's text, the queued prompt's text and its
`remoteSource` (the note's id), a typed message sent after the note queued AFTER it, an open question card answered by
voice, a failing worker answered in the topic, a note with no session answered and nothing queued.

## 8. Privacy and security

* A local worker keeps the audio on this PC; the downloaded file is deleted after its transcription.
* The endpoint engine sends the audio to its URL — off unless chosen; the cog's choice says so.
* Keys (the endpoint's) live in Credential Manager per profile; never in settings.json, a log line or an error text.
* The worker runs as the user, hidden, in a job that ends with Agentmaster; its command line is the owner's setting.
* Only the paired owner's notes are read (the sender check comes first, H3).

## 9. Settings

| Setting | Default | Meaning |
|---|---|---|
| `messagingVoiceEnabled` | on | Voice notes are read. |
| `voiceEngine` | `worker` | `worker` · `openai` · `off`. |
| `voiceWorkerCommand` | "" | The worker's command line; "" = the bundled worker when present. |
| `voiceEndpoint` / `voiceModel` | "" / "" | The endpoint's URL (OpenAI's) and model (`whisper-1`). Its key: Credential Manager. |
| `voiceLanguage` | `en` | "" = the engine detects it. |
| `voiceUseSessionContext` | on | The folder's name and CLAUDE.md join the vocabulary. |
| `voiceDictionary` | "" | The owner's dictionary (§5.4). |
| `voiceWorkerIdleMinutes` | 10 | 0–1440; the worker ends after this long without a note. |
| `voiceEchoTranscript` | on | The transcript is posted under the note before it is read. |

## 10. Open items

1. **The owner's recordings** — fetch, run the matrix, fill §4.4, set the test's configuration and bar from them.
2. **The bundled worker** (§6.4) — the native exe, the pinned runtime, the model download.
3. **AI enhancement in the app** — a hidden `claude -p` correction (the `ClaudeStatusProbe` seam) behind a setting, if the
   recordings show errors the dictionary pass cannot fix.
4. **Voice in the 🔎 Search topic** — a spoken search request (today: "voice notes are read in a session's topic").
5. **Hebrew** — Whisper large-v3 (or an ivrit.ai fine-tune) with `voiceLanguage = he`; Parakeet has no Hebrew.
6. **Spoken commands** — "slash compact" → `/compact` (today a transcript is always prompt text).
7. **E2E** (§7.6).
