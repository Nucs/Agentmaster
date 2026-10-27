# Telegram voice notes — the POC

The proof of concept behind **VOICE.md** (`doc/agentmaster/VOICE.md`): can a Telegram voice note of a developer's
spoken prompt come out as a transcript that is close to perfect, technical words included — and with which engine and
which correction layers? It is separate from the app: Python, a venv on Q:, the models on Q:. The app's integration
(MESSAGING.md §21.43) ports its vocabulary rules to C++ (`VoiceVocabulary.h`, pinned to this folder's `vocab.py` by
goldens) and can run its engines through `voice_worker.py`.

## What is here

| File | What it does |
|---|---|
| `references.json` | The evaluation set: three technical sentences, and the terms whose spelling must survive. |
| `common.py` | Where things live (`Q:\agentmaster-voice`), audio decoding (PyAV), WER + term scoring. |
| `vocab.py` | **The reference** for the session vocabulary: the glossary, curated triggers, the plain-English test (Windows' spell checker), the context mining, Whisper's prompt, Parakeet's hotwords, the dictionary pass. |
| `engines.py` | Parakeet TDT 0.6B v2/v3 (sherpa-onnx, CPU; greedy or beam + hotwords) and Whisper large-v3 / turbo (faster-whisper, CUDA; with or without the prompt). |
| `llm_fix.py` | The optional third layer (FluidVoice's "AI Enhancement"): one headless `claude -p` correction with the glossary. |
| `eval_voice.py` | The evaluation: engine × vocabulary context × layer, scored; with `--expect` it is a TEST (exit 1 below the bar). |
| `fetch_tg_voice.py` | Gets the owner's voice notes out of a session's Telegram topic (forward → getFile → download → delete the forward), never touching `getUpdates`. |
| `synth_fixtures.py` | Windows TTS renditions of the sentences as Ogg/Opus voice notes: a pipeline smoke test, not the evaluation. |
| `voice_worker.py` | A voice worker (VOICE.md §6.2's protocol over stdin/stdout) for these engines: the app's `voiceWorkerCommand` can point at it. |

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
$PY fetch_tg_voice.py fetch --session ba1e0620 --after 14016 --before 14030 --ids s1,s2,s3   # the owner's recordings
$PY eval_voice.py                                       # the matrix on the recordings
$PY eval_voice.py --llm --engines whisper-large-v3+prompt,parakeet-v2-greedy   # + the Claude correction layer
# the TEST (the local evaluation of VOICE.md §7.3):
$PY eval_voice.py --engines whisper-large-v3+prompt --contexts session \
    --expect "whisper-large-v3+prompt | ctx=session | dict" --max-wer 0.05 --min-cased 1.0
```

Results are written to `Q:\agentmaster-voice\results\eval-<time>-<fixtures>.json`.

⚠ The recordings of the owner's voice stay on Q: (`Q:\agentmaster-voice\recordings`): the repository is PUBLIC.
