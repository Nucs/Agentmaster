# Telegram voice notes — the POC

The proof of concept behind **VOICE.md** (`doc/agentmaster/VOICE.md`): can a Telegram voice note of a developer's
spoken prompt come out as a transcript that is close to perfect, technical words included — and with which engine and
which correction layers? It is separate from the app: Python, a venv on Q:, the models on Q:. The app's integration
(MESSAGING.md §21.43) ports its vocabulary rules and its correction layer to C++ (`VoiceVocabulary.h` and
`VoiceCorrect.h`, pinned to this folder's `vocab.py` and `llm_fix.py` by goldens) and can run its engines through
`voice_worker.py`.

**The answer, on the owner's three recordings** (VOICE.md §4.4): Whisper large-v3-turbo with the session's glossary,
Parakeet v2 as a second hearing, the dictionary pass and Claude Sonnet 5.5's correction of both hearings — **0.0 % WER,
every term spelled exactly, 3 runs of 3**. The engines alone (the first matrix): 12.7–18.3 %.

## What is here

| File | What it does |
|---|---|
| `references.json` | The evaluation set: three technical sentences, and the terms whose spelling must survive. |
| `common.py` | Where things live (`Q:\agentmaster-voice`), audio decoding (PyAV), WER + term scoring, `apply_said` (a recording scored against what its reader SAID, `said.json` beside the recordings). |
| `vocab.py` | **The reference** for the session vocabulary: the glossary (in priority order), curated triggers, the plain-English test (Windows' spell checker), the context mining (speakable names only), Whisper's prompt, Parakeet's hotwords, the dictionary pass, the correction's glossary line. |
| `engines.py` | Parakeet TDT 0.6B v2/v3 (sherpa-onnx, CPU; greedy or beam + hotwords) and Whisper large-v3 / turbo (faster-whisper, CUDA; with or without the prompt). |
| `llm_fix.py` | **The reference** for the CORRECTION layer (FluidVoice's "AI Enhancement", VOICE.md §5.5): one headless `claude -p` with the glossary and every engine's hearing (fusion), its system prompt, user prompt and length guard. |
| `eval_voice.py` | The evaluation: engine × vocabulary context × layer (`raw`, `dict`, `dict+llm`, `dict+llm+fuse`), scored; with `--expect` it is a TEST (exit 1 below the bar). |
| `fetch_tg_voice.py` | Gets the owner's voice notes out of a session's Telegram topic (forward → getFile → download → delete the forward), never touching `getUpdates`. |
| `synth_fixtures.py` | Windows TTS renditions of the sentences as Ogg/Opus voice notes: a pipeline smoke test, not the evaluation. |
| `voice_worker.py` | A voice worker (VOICE.md §6.2's protocol over stdin/stdout) for these engines: the app's `voiceWorkerCommand` can point at it; `--fuse <engine>` adds a second engine's hearing as `alternatives` for the correction. |

## Setup (once)

```bash
# the venv on Q: (K: is nearly full); it reuses the base Python's torch (CUDA) and openai-whisper
/c/Users/ELI/.claude/python/python -m venv --system-site-packages /q/agentmaster-voice/venv
/q/agentmaster-voice/venv/Scripts/python.exe -m pip install sherpa-onnx faster-whisper jiwer wordfreq comtypes
# Parakeet TDT 0.6B v2 / v3, int8 ONNX (sherpa-onnx exports; ~650 MB each) into Q:\agentmaster-voice\models
for v in v2 v3; do d=/q/agentmaster-voice/models/parakeet-tdt-0.6b-$v-int8; mkdir -p $d
  for f in encoder.int8.onnx decoder.int8.onnx joiner.int8.onnx tokens.txt; do
    curl -sL -o $d/$f https://huggingface.co/csukuangfj/sherpa-onnx-nemo-parakeet-tdt-0.6b-$v-int8/resolve/main/$f; done; done
```

Whisper large-v3 comes from the Hugging Face cache (`Systran/faster-whisper-large-v3`), turbo is downloaded on first use.

## Run

```bash
cd tools/voice-poc
PY=/q/agentmaster-voice/venv/Scripts/python.exe
$PY synth_fixtures.py                                   # TTS fixtures -> Q:\agentmaster-voice\synthetic
$PY fetch_tg_voice.py fetch --session ba1e0620 --after 14016 --before 14033 --ids s1,s2,s3   # the owner's recordings
$PY eval_voice.py                                       # the matrix on the recordings
$PY eval_voice.py --llm --engines whisper-large-v3-turbo+prompt,parakeet-v2-greedy   # + the correction (Haiku 5.5 by default)
# the TEST (the local evaluation of VOICE.md §7.3) - the app's whole pipeline, three Sonnet 5.5 calls:
$PY eval_voice.py --engines whisper-large-v3-turbo+prompt --contexts session --llm --llm-model claude-sonnet-5-5 \
    --fuse parakeet-v2-beam+hotwords \
    --expect "whisper-large-v3-turbo+prompt | ctx=session | dict+llm+fuse" --max-wer 0.05 --min-cased 1.0
# the worker the app runs (its voiceWorkerCommand):
"Q:\agentmaster-voice\venv\Scripts\python.exe" "K:\source\Agentmaster\tools\voice-poc\voice_worker.py" --engine whisper-large-v3-turbo+prompt --fuse parakeet-v2-beam+hotwords
```

Results are written to `Q:\agentmaster-voice\results\eval-<time>-<fixtures>.json`. After a change to `vocab.py` or
`llm_fix.py`, run `src\cascadia\TerminalApp\AgentMaster\tests\voice\make_goldens.py` and port the change to
`VoiceVocabulary.h` / `VoiceCorrect.h` until `TestVoiceVocabulary` / `TestVoiceCorrection` pass again.

⚠ The recordings of the owner's voice stay on Q: (`Q:\agentmaster-voice\recordings`): the repository is PUBLIC.
