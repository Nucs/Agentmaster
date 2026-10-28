# Agentmaster — voice notes: a Telegram voice note becomes the session's prompt

> **Status (2026-10-27; the integration in dev since 0.0.1.38 and in production since 0.8.0.1, both 2026-10-28): the POC and the integration are WRITTEN.** The POC (`tools/voice-poc/`, Python)
> runs on this PC and measured the engines below. On the owner's three recordings the full pipeline — Whisper
> large-v3-turbo with the session's glossary, a second engine's hearing (Parakeet, FUSION), the dictionary pass and the
> CORRECTION by Claude Sonnet 5.5 — came out **word for word: 0.0 % WER, every term with its exact spelling, 3 runs of 3**
> (§4.4). The app integration (MESSAGING.md §21.43) is in the tree: the messaging service's voice path, the session
> vocabulary in C++ and the correction layer (pinned to the POC by goldens: 253 checks pass standalone), the voice worker
> protocol (with the fusion's `alternatives`), the endpoint engine, the settings and the cog's VOICE NOTES section. A
> build over older intermediates must PURGE (SessionModels.h changed; dev 0.0.1.38's was a clean build). The bundled native worker (`agentmaster-voice.exe`, §6.4) is designed,
> not written: until it ships, the local engine is any worker command — the POC's Python worker on this PC (§6.2 gives the
> measured command).
>
> The owner's recordings of the evaluation set: §7.1. Their results: §4.4.
>
> **2026-10-28, in dev since 0.0.1.39, not in production:** a note in a NEW thread (writing in "All" opens one) starts its
> session like a typed message there and becomes its first prompt (§3). Production 0.8.0.1 still answers such a note "this
> thread has no session".

## 1. The ask and the shape of the answer

**The ask (2026-10-27, from the Telegram topic "fluidvoice telegram"):** look into FluidVoice; let a Telegram voice note in
a session's topic be transcribed into that session's prompt; three technical sentences, read by the owner, must come out
close to perfect; the evaluation is a test that runs locally (and later in e2e); a separate POC, this design, and the full
integration in Agentmaster.

**The answer in one paragraph.** A voice note in a session's topic is downloaded and handed to a speech engine together
with the session's VOCABULARY — the names of its project (its folder, its CLAUDE.md), a developer glossary and the owner's
own dictionary. The engine's text then goes through a DICTIONARY PASS that respells split or misheard names ("Con PTY",
"Agent Master", "Jason L" → ConPTY, Agentmaster, JSONL) and never touches an everyday word, and then through the
CORRECTION: one hidden `claude -p` (Sonnet 5.5) gets the glossary and every engine's hearing of the note and returns the
same words with what was misheard fixed ("Becode a" → "Decode the" where a second engine heard it right). The result is
posted under the note (🎤) and read exactly like a typed message: a queued prompt, or the answer to an open question card.
This is FluidVoice's design — word boost + custom dictionary + AI enhancement — with the vocabulary taken from where the
prompt goes instead of a list the user maintains, and the enhancement given more than one hearing.

## 2. FluidVoice — what it is, and what this design took from it

FluidVoice ([altic-dev/FluidVoice](https://github.com/altic-dev/FluidVoice), GPLv3 since 2026-02-23, Apache-2.0 before;
~12.5k stars) is a local dictation app for macOS 15+ (a Windows pre-release exists). Read 2026-10-27 at its `main`:

| Layer | FluidVoice | Here |
|---|---|---|
| Speech model | Parakeet TDT v3 by default (also v2, Nemotron Speech 3.5, Parakeet Flash, Cohere Transcribe, Whisper Tiny–Large, Apple Speech), run on the Apple Neural Engine through **FluidAudio** (CoreML) | The same models on Windows: Parakeet TDT 0.6B v2/v3 through **sherpa-onnx** (ONNX Runtime, CPU), Whisper large-v3 through **faster-whisper** (CTranslate2, CUDA) |
| Word boost | `ParakeetVocabularyStore`: up to 256 terms with aliases and weights (`parakeet_custom_vocabulary.json`: alpha 2.8, minCtcScore −2.2, minSimilarity 0.72, minCombinedConfidence 0.64, minTermLength 3) — FluidAudio's CTC keyword spotting | Whisper's `initial_prompt` (the glossary line), or sherpa-onnx per-stream **hotwords** (modified beam search + an Aho-Corasick context graph) |
| Custom Dictionary | trigger spellings → replacement, after recognition; built-in entries `claud, clawed → Claude`, `post hoc → PostHog`, `hugging face → Hugging Face`, `fluid voice → FluidVoice`; replacement protection rules | The **dictionary pass** (§5.4): curated triggers (the same idea, `clawed → Claude` included) + spellings GENERATED from every vocabulary term, guarded by an everyday-English test |
| Text formatting | filler-word removal, spoken punctuation ("comma", "new line"), dictation literals | Not taken: a prompt to Claude Code tolerates both; the engines punctuate |
| AI Enhancement | optional post-processing by OpenAI / Groq / a custom provider, or the local ~3.5 GB "Fluid Intelligence" (not GPL) | The **correction** (§5.5), on by default: one hidden `claude -p` with the glossary and every engine's hearing (multi-hypothesis FUSION), on the PC's own Claude Code login (POC: `llm_fix.py`; app: `VoiceCorrect.h`) |
| Where the words come from | the user's word list | **the session**: its folder name, the hard names of its CLAUDE.md, the developer glossary, the owner's dictionary (§5.1) |

Nothing of FluidVoice's code is copied (GPLv3 vs this repository's AGPL is compatible, but no copy was needed): the
studied files are `ParakeetVocabularyStore.swift`, `BuiltInDictionary.swift`, `ASRService.swift` (the pipeline: filler
removal → custom dictionary → word-boost hit tracking), `Package.swift` (FluidAudio, transcribe-cpp-swift).

## 3. In Telegram

1. The owner holds the mic in a session's topic and talks. The note gets 👀 (heard).
2. The voice thread downloads it, transcribes it with the session's vocabulary, runs the dictionary pass and the
   correction (§5.5).
3. The transcript is posted silently under the note: `🎤 Check why the ConPTY host in Agentmaster drops …` (italic; the
   echo can be switched off, §9). A correction that ran and failed adds a line under it — `⚠️ not corrected: <why>`
   (Claude Code not found, no answer within 60 s, an answer that was no correction) — and the note goes on with the
   engine's words.
4. The note is read as that text, through the same function as a typed message (`_handleTopicMessage`): queued as the next
   prompt with its reactions (✍ → 👨‍💻 → …) on the NOTE; or taken as the answer by an open question card (§21.30); or, for a
   note sent while Agentmaster was closed, asked about first (D2 — judged at the note's ARRIVAL, so a slow transcription
   never turns a live note into a backlog question).
5. A message the owner sends right after the note WAITS behind it (the thread's lane, §6.5), so "voice, then a typed
   follow-up" reaches the session in that order.

**A note in a NEW thread starts the session, like a typed message there** (MESSAGING.md §21.17 / §21.25). Writing in the
chat's "All" view opens a new thread with that very message; a thread whose session closed while the thread stayed is one
too. The note gets 👀 and is HELD by the thread's new-thread wizard — the folder picker goes up with it ("Your message waits
here and becomes the session's first prompt") — untranscribed: the session it is for does not exist yet, and ITS folder and
CLAUDE.md are the vocabulary the note needs. When the pick has launched the session and the thread becomes its topic, the
hand-over (`_adoptBoundThreads`) sends the held note to the voice thread with that session: it is transcribed, echoed (🎤) and
queued as the session's first prompt, read as a held message (no backlog question however long the pick took, no question
card's). Messages typed after it in the thread were held behind it and follow it in order (they wait in the lane behind
the note). A note sent while the thread's session is being bound waits in the bind the same way. The routing is one pure
rule, `DecideVoiceNoteRoute` (`MessagingVoice.h`): a note goes where a typed message of its thread goes.

A note that cannot be read is answered and queued nowhere: the chat's main view (a typed message gets the help line there),
the 🔎 Search topic (a search is typed), a recreated topic's old thread, voice notes off, no engine set, a file over 20 MB
(the Bot API's getFile limit) — each of these at the note's ARRIVAL, a held one too, so it never waits through a folder
pick to fail — and a failed transcription ("🎤 I could not transcribe this voice note: <why>").

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

**The owner's recordings** (§7.1: three Telegram voice notes of 9, 14 and 14 s, 71 words, 14 terms, read by the owner on
2026-10-27). Scored against what the owner SAID (`said.json`): every one of the eight engine configurations heard "then use
Parakeet" where the sentence says "run" — a reading change, not a recognition error. The pipeline the app runs, layer by
layer (runs `212550`, `212658`, `212744` in `Q:\agentmaster-voice\results\`):

| Layer | WER | Terms | Cased |
|---|---|---|---|
| Whisper large-v3-turbo + the session glossary (Whisper's prompt) | 5.6 % | 100 % | 92.9 % |
| + the dictionary pass | 5.6 % | 100 % | 100 % |
| + the correction, Claude Sonnet 5.5, the transcript alone | 4.2–5.6 % | 100 % | 100 % |
| + the correction, Claude Sonnet 5.5, with Parakeet's hearing too (FUSION) | **0.0 %, 3 runs of 3** | 100 % | 100 % |

Every other configuration measured on the recordings:

| Configuration | WER | Terms | Cased | Runs |
|---|---|---|---|---|
| Parakeet v2 greedy + dictionary + Sonnet 5.5 with the beam + hotwords hearing (fusion; CPU only) | 0.0 % | 100 % | 100 % | 1 |
| Whisper turbo + glossary + dictionary + Haiku 5.5 (fusion) | 2.8 % | 100 % | 100 % | 1 |
| Whisper turbo + glossary + dictionary + Haiku 5.5 (the transcript alone) | 2.8–4.2 % | 100 % | 100 % | 2 |
| Parakeet v2 greedy + dictionary + a correction (Haiku or Sonnet, the transcript alone) | 2.8–4.2 % | 85.7–100 % | 78.6–100 % | 3 |
| Parakeet v2 greedy + dictionary | 9.9 % | 78.6 % | 57.1 % | 3 |
| Whisper large-v3 + glossary + dictionary | 11.3 % | 78.6 % | 78.6 % | 1 |
| Parakeet v2 beam + hotwords + dictionary | 12.7 % | 71.4 % | 71.4 % | 1 |
| The first matrix, before the speakable filter and against the intended sentence: Whisper turbo + glossary + dictionary (its best) | 8.5 % | 92.9 % | 92.9 % | 1 |
| … the same matrix, engines alone: Whisper turbo · Parakeet v2 / v3 · Whisper large-v3 | 12.7 · 14.1 · 14.1 · 18.3 % | 64–79 % | 43–57 % | 1 |

Time: the engines take 0.03–0.08 of the note's length; the correction adds 3–11 s with Sonnet 5.5 (2–5 s with Haiku 5.5),
so in those runs a note was text 4–12 s after its file was on disk.

What the recordings taught the design:
* **Real speech ranks the engines differently.** The synthetic voice favored Whisper large-v3; the owner's voice favors
  large-v3-turbo (5.6 % against 11.3 %, with the same glossary). The worker's default is turbo.
* **Whisper's prompt was crowded with names nobody says.** The glossary held `lowerCamelCase` identifiers, three-part
  compounds and two-letter versions (`v9`); the SPEAKABLE filter (§5.2) and a glossary in priority order (§5.1) took turbo
  from 8.5 % to 5.6 % with every term right — before any model call.
* **The correction needs a second hearing.** "Becode a Telegram voice note" (s2) is fixed only when Parakeet's "Decode the"
  is in front of the model; given the transcript alone, Sonnet kept 4.2–5.6 %. Where two engines disagree, the audio was
  unclear, and the model picks what fits the sentence and the glossary (§5.5).
* **Sonnet, not Haiku.** Same prompt, same inputs: Haiku 5.5 moved between 2.8 % and 4.2 % from run to run; Sonnet 5.5 held
  0.0 %. Sonnet is the default; Haiku stays a choice (faster, cheaper).
* **The correction's prompt names no test term.** A first draft gave ConPTY and JSONL as examples; they were replaced with
  terms that appear in no sentence, so the score is the method's and not the prompt's.

## 5. The session vocabulary

The reference is `tools/voice-poc/vocab.py`; `VoiceVocabulary.h` is its line-for-line C++ port, held to it by goldens
(§7.5). A rule changes in `vocab.py` first.

### 5.1 Where the words come from

| Source | Weight | Respell from everyday words? |
|---|---|---|
| The built-in developer glossary (`BUILTIN_GLOSSARY`, ~170 terms, in PRIORITY order: AI and speech, audio and media, hardware and OS, data formats, languages, tools, databases, generic words — every use that truncates cuts its END first, so Whisper's prompt keeps the AI and speech names a prompt to Claude Code says most) | 1 | yes (curated) |
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
"BEFORE", a long all-caps name), two-letter acronyms seen fewer than 8 times, and compounds of everyday words. It keeps
only SPEAKABLE names, too: no `lowerCamelCase` identifier (`tabToken`, `kStateSettleMs`), nothing of three or more word
parts (`WriteAllUtf8`, `SHGetKnownFolderPath`), no two-character token that is not an acronym (`v9`, `m5`). On the owner's
recordings those identifiers had filled Whisper's prompt and pushed out the glossary words a note did hold (§4.4). On this
repository's CLAUDE.md (at `a935ca604`) the mining yields Agentmaster, Claude, XAML, ConPTY, MSIX, SDK, DuckDB, x64, …

### 5.3 The four uses

1. **Whisper's prompt** — `A developer dictates a prompt to Claude Code. Glossary: <terms>.`, highest weight LAST (Whisper
   keeps the last ~224 tokens and weighs the end most), cut from the start at ~700 characters.
2. **Parakeet's hotwords** — `Agentmaster :3.0/ConPTY :2.5/JSONL :2.0/…`: hard terms only, scores 2.0 / 2.5 / 3.0 by weight,
   at most 96.
3. **The dictionary pass** (§5.4) on whatever text the engine wrote — every engine, the endpoint's included.
4. **The correction's glossary** (§5.5) — `Agentmaster, ConPTY, …`: the terms highest weight first, at most 160
   (`GlossaryLine`): a model reads far more than Whisper's prompt holds.

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

### 5.5 The correction (fusion)

The reference is `tools/voice-poc/llm_fix.py`; `VoiceCorrect.h` is its C++ port (the prompt text byte for byte, the user
prompt, the guard — held to it by goldens, §7.5), `VoiceCorrect.cpp` runs the call.

**What the model gets.** A system prompt (written to `<profile>\voice\correct-system.txt` when it changed, read by claude
through `--system-prompt-file`) and one user message:

```
Glossary: Agentmaster, ConPTY, … (§5.3, at most 160 terms)

Transcripts of the same recording by different speech engines:
A: <the primary engine's text, after the dictionary pass>
B: <the second engine's text, after the dictionary pass>
```

— or `Transcript:\n<text>` when there is one hearing (a worker without fusion, the endpoint engine, or a second hearing
equal to the first: an empty or identical alternative is dropped). The system prompt's rules: keep the speaker's words,
their order and their meaning — never add, answer, summarize or rephrase; fix only recognition errors: a word that SOUNDS
like a glossary term becomes that term with the glossary's spelling ("duck DB" → DuckDB, "win RT" → WinRT), a word that
makes no sense in a developer's prompt becomes the similar-sounding word that does (an accent turns "thirty" into
"dirty", "sheet" into "seat", kubectl into "kube control"), identifiers and acronyms get their usual casing, a stray filler
or article the grammar rejects goes, numbers stay as written, punctuation is fixed; several transcripts are independent
hearings — keep what they agree on, and for each disputed word choose the reading that fits the sentence, a developer's
prompt and the glossary; nothing to fix → the transcript unchanged; the corrected message only. Its examples name no term
of the evaluation's sentences.

**The call** — the clean headless recipe (`BuildCorrectionArgs`): `claude.exe -p --model <model> --no-session-persistence
--output-format json --strict-mcp-config --mcp-config {"mcpServers":{}} --system-prompt-file <file> --tools ""
--permission-mode dontAsk --setting-sources "" --settings {verbose:false, env: CLAUDE_CODE_MAX_RETRIES=2,
CLAUDE_CODE_RETRY_WATCHDOG=0, CLAUDE_CODE_EFFORT_LEVEL=low} <user message>`, through ClaudeStatus.h's `RunClaudeCapture`
(hidden, in a kill-on-close job, NUL stdin, Agentmaster's environment minus every variable of a parent Claude Code session
or tab, plus `AM_HARNESS=claude-status` so the Fleet Observer and the CLI skip it and `DISABLE_AUTOUPDATER=1`; the working
folder `<profile>\voice`). No tools, no MCP server, no settings, hooks or CLAUDE.md, no session saved: the model sees the
glossary and the hearings, nothing of the session's repository. The claude is the engine's (`Engine::claudeExePath`),
read for each note.

**The guard** (`AcceptCorrection`): the answer replaces the text only when it is not blank and its length is within
`0.6 × before − 10` … `1.5 × before + 20` characters: a model that ANSWERED the prompt instead of correcting it is caught
by its length. A refusal is logged to `voice.log` with the answer's first 200 characters.

**Failure keeps the note.** No claude, a start failure, 60 s without an answer (`kCorrectionTimeoutMs`), an error result or
a refused answer: the note goes on with the dictionary pass's text, the echo says `⚠️ not corrected: <why>`, and voice.log
and hooks.log say why. An empty hearing skips the call.

**Fusion.** A worker that runs two engines returns the second one's text as `alternatives` (§6.2); the app gives each the
dictionary pass and hands them to the model as B (C, …). The POC worker's `--fuse <engine>` does it; on this PC Whisper
large-v3-turbo on the GPU + Parakeet v2 (beam search + the session's hotwords) on the CPU, which costs the second engine's
~1 s. The measurement (§4.4): fusion is what turns the correction from 4.2–5.6 % into 0.0 %.

**Cost and privacy.** One model call per note on the PC's own Claude Code login (its plan or API key); Sonnet 5.5 by
default (`voiceCorrectionModel`), Haiku 5.5 or off in the cog. The TEXT reaches Anthropic — the same text then becomes the
session's prompt, which reaches Anthropic anyway; the audio never does.

## 6. The integration

### 6.1 The seam

`MessagingService` (engine code, also in the m5 harness) knows only `Voice::VoiceBridge` (`VoiceBridge.h`): `Transcribe(note)`
→ the corrected transcript, `Configure(settings)`, `Tick()` (the idle end), `Shutdown()`, `Describe()`. The app layer sets the
factory (`TerminalPage.AgentEngine.cpp`, beside the Search agent's): `Voice::MakeVoiceTranscriber()` (`VoiceTranscriber.cpp`)
— for each note it builds the session vocabulary (cached while the session's folder, title, CLAUDE.md stamp and the
dictionary are unchanged), runs the engine, runs the dictionary pass on every hearing, runs the correction (§5.5, with the
note's `claudeExe`, which the service reads from `MessagingHooks::claudeExe` for each note) and folds the text into one
paragraph. The result carries the engine's own text (`raw`), the respellings, the other hearings, whether the correction
changed the text, its time and its failure. Engine.cpp never names it; the harness has no factory and answers "not
available in this build".

### 6.2 The voice worker protocol (v1)

A VOICE WORKER is any program that speaks JSON lines on stdin / stdout (`VoiceProtocol.h`): `hello` (load the engine; say
whether it takes a `prompt` and `hotwords`), `transcribe {path, language, prompt, hotwords}` → `{ok, text, audio_ms,
decode_ms, engine, alternatives?}`, `quit`. Rules: stdout carries only protocol lines (anything else is logged and skipped);
every request gets exactly one answer with its id; a failure is `{"ok":false,"error":…}`, never an exit. The app sends both
`prompt` and `hotwords`: the worker uses what its engine takes. `alternatives` (optional, v1-compatible: a reader that does
not know it skips it) is a worker's OTHER engines' hearings of the same note, for the correction's fusion (§5.5); empty
strings are dropped. The vocabulary, the dictionary pass and the correction stay in the app, so every engine gets the same
corrections.

`VoiceWorker.cpp` runs ONE worker per process: started hidden by the first note (`CREATE_SUSPENDED`, put into a kill-on-close
job, then resumed — nothing it starts outlives Agentmaster), the three pipes passed by `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`
only, its stderr into `voice.log` line by line, `PYTHONUNBUFFERED=1`, `PYTHONIOENCODING=utf-8` and `AM_HARNESS=voice-worker`
in its environment. A transcription's deadline is 60 s + 4 s per second of audio (at most 15 min); a missed deadline ENDS the
worker (the next note starts a fresh one). It ends after `voiceWorkerIdleMinutes` without a note. A cog Save that changes
the engine KILLS the running worker from the UI thread without waiting (a note in flight then fails fast); its cleanup runs
on the voice thread.

`tools/voice-poc/voice_worker.py` is a worker for the POC's engines. The command measured best on the owner's recordings
(§4.4) — Whisper large-v3-turbo on the GPU, with Parakeet v2 (beam + hotwords) on the CPU as the second hearing:

```
"Q:\agentmaster-voice\venv\Scripts\python.exe" "K:\source\Agentmaster\tools\voice-poc\voice_worker.py" --engine whisper-large-v3-turbo+prompt --fuse parakeet-v2-beam+hotwords
```

Without `--fuse` the worker returns one hearing and the correction works from it alone (4.2–5.6 % on the recordings).

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

* **Poll thread** — `_voiceTake`: a note goes where a typed message of its thread goes (`DecideVoiceNoteRoute`). In a
  session's topic it becomes a job (FIFO across every topic: one engine, one note at a time) and a pending item at the end of
  its thread's LANE; any other message of a thread whose lane holds something joins the lane (an edit replaces the held
  message it edits). In a thread no session owns yet, or a topic whose bind still hands over, it is held there
  (`_holdForNewThread`), after the checks that would refuse it anyway (voice off, no transcriber, no file, over 20 MB).
* **Tick thread, the hand-over** — `_adoptBoundThreads`: each held message goes through `_voiceTake(…, heldByNewThread)` first:
  a held note becomes a job with the new session (its lane item marked `held`), and a held message behind it joins the lane
  marked `held` too; a held message no lane holds goes straight to the topic as before.
* **Voice thread** — `_voiceLoop` (COM multithreaded apartment, for the spell checker): `getFile` (paced by the chat's call
  slot), the download (`TelegramApi::DownloadFile`, 20 MB cap) into `<profile>\messaging\telegram\voice\<message id>.oga`,
  the transcriber, the file DELETED, the lane item marked done, the tick woken; between jobs the transcriber's idle end.
* **Tick thread** — `_voiceTick`: takes each lane's ready items in order (a done note → `_voiceDeliver`: the echo, then
  `_handleTopicMessage` with the text — `held = true` for a note a new thread held; a held message → `_handleMessage(…,
  fromVoiceLane=true)`, or, one a new thread held, `_handleTopicMessage(…, held = true)` like the hand-over's own), one at a
  time under the leaf lock `_voiceMtx`, delivered outside it. A lane leaves the map only when found empty, so a message read
  meanwhile joins it BEHIND what is being delivered.
* **Stop** — `_voiceStop` runs first: it kills the engine (a note in flight fails fast), joins the voice thread, and drops
  the queue and the lanes (logged). ⚠ Their notes and held messages are not read: the owner sends them again.

### 6.6 Files and logs

| What | Where |
|---|---|
| A downloaded note | `<profile>\messaging\telegram\voice\<message id>.oga`, deleted right after its transcription |
| The correction's system prompt | `<profile>\voice\correct-system.txt`, rewritten when an update changes the prompt; `<profile>\voice\` is also the correction's working folder |
| The worker's stderr, its start / stop / idle end, each transcription's timings, other hearings, respellings and correction (`, correction N ms (changed)` / `(unchanged)` / `FAILED after N ms: <why>`), a correction the guard refused | `<profile>\voice.log` |
| The service's lines | `hooks.log`: `[msg] <sid> voice note queued for transcription …`, `… transcribed (… chars, <engine>, … ms, N vocabulary fix(es)[, corrected in N ms \| , correction kept it, N ms \| , correction failed: <why>])`, `… not transcribed …`; `[nav] remote telegram voice …` |

## 7. Evaluation and tests

### 7.1 The owner's recordings

The three sentences of `tools/voice-poc/references.json` were posted in the topic (message 14016). The owner read each as one
voice note (messages 14025, 14028, 14031: 9, 14 and 14 s) and sent "done"; `fetch_tg_voice.py fetch --after 14016 --before
<done's id> --ids s1,s2,s3` recovered them through the bot without `getUpdates` (a silent forward gives the note's `file_id`,
the forward is deleted at once, `getFile` + the download) into `Q:\agentmaster-voice\recordings\` with a `manifest.json`.
They stay on Q: (the repository is public). `said.json` beside them records where the reading differs from the intended
sentence (s2: "use Parakeet" for "run Parakeet" — all eight engine configurations heard "use"); the evaluation scores
against it (`common.apply_said`).

### 7.2 Synthetic fixtures

`synth_fixtures.py` (Windows SAPI → Ogg/Opus like a Telegram client records) gives the pipeline a check without the owner;
it is never the evaluation.

### 7.3 The local test

```
python eval_voice.py --engines whisper-large-v3-turbo+prompt --contexts session --llm --llm-model claude-sonnet-5-5 \
    --fuse parakeet-v2-beam+hotwords \
    --expect "whisper-large-v3-turbo+prompt | ctx=session | dict+llm+fuse" --max-wer 0.05 --min-cased 1.0
```

exits 1 when the configuration misses the bar (corpus WER ≤ 5 %, every term with its exact spelling), 2 when the recordings
are missing; it passed 3 runs of 3 at 0.0 % (§4.4). It is the app's pipeline: the same vocabulary, Whisper's prompt, the
dictionary pass, the second hearing and the correction's prompt and guard (`llm_fix.py`, which `VoiceCorrect.h` matches by
goldens). It calls Sonnet 5.5 three times (one per note) on this PC's Claude Code login. The session context is CLAUDE.md
pinned at `a935ca604` (`--claude-md-rev`), the repository before this feature documented itself, so the test cannot grow
easier as VOICE.md's words land in CLAUDE.md.

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
* `TestVoiceProtocol` — the request lines (one line each, escaped), the answer reader (`alternatives` included: strings
  kept, empty ones dropped), the prompt text.
* `TestVoiceCorrection` — `VoiceCorrect.h` against the goldens' `correction` section, which `make_goldens.py` writes from
  `llm_fix.py`: the system prompt byte for byte, the glossary line at 160 and 5 terms, the user prompt with one hearing,
  with two and three hearings and with a duplicate hearing; the guard's cases (a correction, a blank answer, an answer to
  the prompt, a cut); the command line's quoting round-tripped through `CommandLineToArgvW` (quotes, backslashes, spaces,
  line breaks, an empty argument) and the whole command (21 arguments, the JSON `--settings` intact, the user prompt last);
  the JSON answer reader (an object, the last item of an array, `is_error`, not JSON, an empty array).
* `TestVoiceNoteRoute` — `MessagingVoice.h`: a note goes where a typed message of its thread goes (a new thread's wizard
  holds it — the owner's note in "All" of 2026-10-28 —, a binding topic's bind holds it, a session's topic transcribes it,
  the main view / the Search topic / a recreated topic's old thread refuse), the refusal texts (each says where a note IS
  read; nothing HTML would read as markup), why a note cannot be read and in which order, and which messages are notes.

All four pass standalone (253 checks; `cl` of `tests_voice.cpp` alone) and in the whole isolated suite. No test calls a
model (the correction's call, `VoiceCorrect.cpp`, is not linked into the harness): §7.3 is the test that does. The
service's voice path needs Telegram: e2e.

### 7.6 E2E (later)

A fake Bot API in the e2e host (`getUpdates` serving one `message` with a `voice` of a fixture `.oga`, `getFile`, `/file/bot…/…`
serving the fixture, `sendMessage` / `setMessageReaction` recorded) and a fake worker (`voice_worker.py` with a canned
engine, or the real one on the guest's CPU with Parakeet) drive: the echo's text, the queued prompt's text and its
`remoteSource` (the note's id), a typed message sent after the note queued AFTER it, an open question card answered by
voice, a failing worker answered in the topic, a note in the main view or the Search topic answered and nothing queued, and
a note that OPENS a new thread: held with the folder picker, then transcribed and queued as the new session's first prompt
once the pick launched it, a typed message sent after the note queued after it.

## 8. Privacy and security

* A local worker keeps the audio on this PC; the downloaded file is deleted after its transcription.
* The endpoint engine sends the audio to its URL — off unless chosen; the cog's choice says so.
* The correction (§5.5, on by default) sends the TEXT and the session's glossary to Anthropic through the PC's own Claude
  Code login — the text that then becomes the session's prompt, which goes there anyway. It reads no file of the session;
  its claude runs with no tools, no MCP server and no settings. `voiceCorrectionModel = off` stops it.
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
| `voiceCorrectionModel` | `claude-sonnet-5-5` | The correction's model (§5.5): `claude-sonnet-5-5` · `claude-haiku-5-5` · `off` (or ""); any other id typed into settings.json is passed to claude's `--model` as is (the cog shows it as "Another model" and keeps it). |

## 10. Open items

1. **The bundled worker** (§6.4) — the native exe, the pinned runtime, the model download — with a SECOND HEARING for the
   correction: Parakeet v2 greedy with a beam + hotwords pass of the same model as `alternatives` (one model, two decodings,
   CPU only) reached 0.0 % with Sonnet 5.5 in its one run (§4.4); Whisper turbo when a GPU is there.
2. **A new evaluation set** (§7.4) — three new sentences, unseen by the rules and the prompt, to confirm the 0.0 % is the
   method's.
3. **The correction's latency** — 3–11 s per note with Sonnet 5.5; a resident `claude` (stream-json, the agent harness's
   process) would save claude's start on each call (not measured), if notes feel slow.
4. **Voice in the 🔎 Search topic** — a spoken search request (today: "voice notes are read in a session's topic").
5. **Hebrew** — Whisper large-v3 (or an ivrit.ai fine-tune) with `voiceLanguage = he`; Parakeet has no Hebrew; the
   correction's prompt is English-only.
6. **Spoken commands** — "slash compact" → `/compact` (today a transcript is always prompt text).
7. **E2E** (§7.6) — with a fake worker that returns `alternatives`, and `voiceCorrectionModel = off` (no model in e2e) plus
   one case with a fake claude for the correction's failure echo.
