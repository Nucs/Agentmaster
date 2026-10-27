// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — VoiceProtocol: the voice worker protocol v1 (VOICE.md §6.2), pure (std + Json.h), harness-tested.
//
// A VOICE WORKER is any program that turns an audio file into text behind this protocol: the bundled
// agentmaster-voice.exe (VOICE.md §6.4), the POC's Python worker (tools\voice-poc\voice_worker.py: Whisper on the GPU,
// Parakeet on the CPU), or anything the owner writes. Agentmaster starts ONE per process (AppSettings::voiceWorkerCommand),
// keeps it while voice notes keep coming, and ends it after AppSettings::voiceWorkerIdleMinutes. JSON lines over the
// worker's stdin / stdout, one request at a time:
//
//   -> {"v":1,"id":1,"op":"hello"}
//   <- {"v":1,"id":1,"ok":true,"engine":"whisper-large-v3","languages":["en"],"prompt":true,"hotwords":false}
//   -> {"v":1,"id":2,"op":"transcribe","path":"C:\\…\\14021.oga","language":"en","prompt":"…","hotwords":"…"}
//   <- {"v":1,"id":2,"ok":true,"text":"…","audio_ms":7480,"decode_ms":512,"engine":"…"}
//   -> {"v":1,"id":3,"op":"quit"}
//
// The contract the app relies on: stdout carries ONLY protocol lines (UTF-8, one object per line; anything else is logged
// and skipped); every request gets exactly one answer with its id; a failure is {"ok":false,"error":"…"}, never an exit.
// The worker TRANSCRIBES; the app owns the vocabulary (it sends `prompt` and `hotwords`, VoiceVocabulary.h) and the
// dictionary pass (it runs on the returned text), so every engine — local or cloud — gets the same correction layer.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "Json.h"

namespace Agentmaster::Voice
{
    /// <summary>The protocol version this app speaks (the "v" of every request; an answer without one reads as v1).</summary>
    inline constexpr int kVoiceProtocolVersion = 1;

    /// <summary>What one transcription asks of a worker.</summary>
    struct VoiceWorkerRequest
    {
        std::wstring path; // the audio file (a Telegram voice note's .oga, Opus in Ogg); the worker only reads it
        std::wstring language; // "en", "he", … ; "" = let the engine detect it
        std::wstring prompt; // Whisper's initial prompt (VoiceVocabulary::WhisperPrompt); "" = none
        std::wstring hotwords; // sherpa-onnx's hotwords (VoiceVocabulary::Hotwords); "" = none
    };

    /// <summary>One answer line of a worker, read.</summary>
    struct VoiceWorkerAnswer
    {
        int64_t id{ 0 }; // the request's id (0 when the line carried none: an answer to an unreadable request)
        bool ok{ false };
        std::wstring text; // the transcript (transcribe)
        std::wstring error; // why it failed (ok == false)
        std::wstring engine; // the engine's name, for the log and the cog ("whisper-large-v3+prompt")
        int64_t audioMs{ 0 }; // the audio's length the worker decoded
        int64_t decodeMs{ 0 }; // the transcription's wall time in the worker
        bool takesPrompt{ false }; // hello: the engine uses `prompt`
        bool takesHotwords{ false }; // hello: the engine uses `hotwords`
    };

    namespace detail
    {
        /// <summary>A request object with the protocol's common fields.</summary>
        /// <param name="id">The request id.</param>
        /// <param name="op">The operation.</param>
        /// <returns>The object.</returns>
        /// <exception cref="std::bad_alloc">If the object cannot be allocated.</exception>
        inline json::Value VoiceRequestObject(int64_t id, const wchar_t* op)
        {
            auto o = json::Value::MkObj();
            o.Set(L"v", json::Value::MkNum(kVoiceProtocolVersion));
            o.Set(L"id", json::Value::MkNum(static_cast<double>(id)));
            o.Set(L"op", json::Value::MkStr(op));
            return o;
        }
    }

    /// <summary>The "hello" line: the worker loads its engine and says what it takes.</summary>
    /// <param name="id">The request id.</param>
    /// <returns>One line (no line break; the caller adds "\n" and encodes UTF-8).</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::wstring BuildHelloRequest(int64_t id)
    {
        return json::Dump(detail::VoiceRequestObject(id, L"hello"));
    }

    /// <summary>The "quit" line: the worker answers and exits (the app still ends the process if it lingers).</summary>
    /// <param name="id">The request id.</param>
    /// <returns>One line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::wstring BuildQuitRequest(int64_t id)
    {
        return json::Dump(detail::VoiceRequestObject(id, L"quit"));
    }

    /// <summary>The "transcribe" line. Json::Dump escapes the path's backslashes and every control character, so the
    /// line never breaks inside a value (a prompt with a line break stays one protocol line).</summary>
    /// <param name="id">The request id.</param>
    /// <param name="request">What to transcribe.</param>
    /// <returns>One line.</returns>
    /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
    inline std::wstring BuildTranscribeRequest(int64_t id, const VoiceWorkerRequest& request)
    {
        auto o = detail::VoiceRequestObject(id, L"transcribe");
        o.Set(L"path", json::Value::MkStr(request.path));
        o.Set(L"language", json::Value::MkStr(request.language));
        o.Set(L"prompt", json::Value::MkStr(request.prompt));
        o.Set(L"hotwords", json::Value::MkStr(request.hotwords));
        return json::Dump(o);
    }

    /// <summary>
    /// Reads one line a worker wrote. A line that is not a JSON object (a library that printed to stdout after all, a
    /// progress bar) is not an answer: nullopt, and the reader skips it.
    /// </summary>
    /// <param name="line">The line (no line break).</param>
    /// <returns>The answer, or nullopt for a line that is not one.</returns>
    /// <exception cref="std::bad_alloc">If the answer cannot be allocated.</exception>
    inline std::optional<VoiceWorkerAnswer> ParseWorkerAnswer(std::wstring_view line)
    {
        const auto doc = json::Parse(line);
        if (!doc || doc->type != json::Value::Type::Obj || !doc->Find(L"ok"))
        {
            return std::nullopt;
        }
        VoiceWorkerAnswer a;
        a.id = doc->I64At(L"id", 0);
        a.ok = doc->BoolAt(L"ok", false);
        a.text = doc->StrAt(L"text");
        a.error = doc->StrAt(L"error");
        a.engine = doc->StrAt(L"engine");
        a.audioMs = doc->I64At(L"audio_ms", 0);
        a.decodeMs = doc->I64At(L"decode_ms", 0);
        a.takesPrompt = doc->BoolAt(L"prompt", false);
        a.takesHotwords = doc->BoolAt(L"hotwords", false);
        if (!a.ok && a.error.empty())
        {
            a.error = L"the worker answered a failure without saying why";
        }
        return a;
    }

    /// <summary>
    /// The text a transcript becomes as a prompt: trimmed, inner runs of white space folded to one space (an engine joins
    /// its segments with stray spaces or line breaks; a dictated prompt is one paragraph). Empty when nothing was heard.
    /// </summary>
    /// <param name="text">The engine's text (after the dictionary pass).</param>
    /// <returns>The prompt text.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring NormalizeTranscriptForPrompt(std::wstring_view text)
    {
        std::wstring out;
        out.reserve(text.size());
        bool pendingSpace = false;
        for (const wchar_t c : text)
        {
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x00A0)
            {
                pendingSpace = !out.empty();
                continue;
            }
            if (pendingSpace)
            {
                out += L' ';
                pendingSpace = false;
            }
            out += c;
        }
        return out;
    }
}
