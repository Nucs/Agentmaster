// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — VoiceHttp: the "openai" voice engine (VOICE.md §6.3) — ONE multipart POST to an OpenAI-compatible
// /v1/audio/transcriptions endpoint: OpenAI (whisper-1, gpt-4o-transcribe), Groq (whisper-large-v3-turbo), or any local
// server that speaks the same API. The voice note goes up as it came from Telegram (Opus in Ogg: every one of these
// accepts .ogg), with the session vocabulary as `prompt`. Plain C++ + WinHTTP (no WinRT, no PCH).
//
// ⚠ PRIVACY: this engine SENDS the owner's voice to the endpoint. It is never the default (VoiceEngineConfig::engine is
// "worker"); the cog says so beside the choice.
//
// The API key lives in Windows Credential Manager (target "Agentmaster:voice:api-key:<profile leaf>"), never in
// settings.json, a log line or an error text — like the bot token (MESSAGING.md M4).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Agentmaster::Voice
{
    /// <summary>One endpoint call's outcome.</summary>
    struct VoiceHttpResult
    {
        bool ok{ false };
        std::wstring text; // the transcript ("text" of the JSON answer)
        std::wstring error; // why it failed: the endpoint's own message when it sent one, else the transport's
        int httpStatus{ 0 };
        int64_t ms{ 0 }; // the round trip
    };

    /// <summary>
    /// Transcribes one audio file with an OpenAI-compatible endpoint. Blocks for the round trip; call it on the voice thread.
    /// </summary>
    /// <param name="url">The full URL ("https://api.openai.com/v1/audio/transcriptions"); http:// is allowed for a local server.</param>
    /// <param name="apiKey">The bearer key ("" sends no Authorization header: a local server).</param>
    /// <param name="model">The model field ("whisper-1").</param>
    /// <param name="language">The language field ("" = omitted: the endpoint detects it).</param>
    /// <param name="prompt">The prompt field: the session vocabulary ("" = omitted).</param>
    /// <param name="fileName">The file name the form carries ("voice.ogg": the endpoint picks its decoder by the extension).</param>
    /// <param name="audio">The file's bytes.</param>
    /// <param name="timeoutMs">The receive timeout.</param>
    /// <returns>The outcome; never throws.</returns>
    VoiceHttpResult TranscribeWithEndpoint(std::wstring_view url, std::wstring_view apiKey, std::wstring_view model, std::wstring_view language,
                                           std::wstring_view prompt, std::wstring_view fileName, const std::string& audio, uint32_t timeoutMs) noexcept;

    /// <summary>The Credential Manager target of a profile's voice API key: "Agentmaster:voice:api-key:&lt;profile leaf&gt;".</summary>
    /// <param name="profileDir">The profile folder.</param>
    /// <returns>The target.</returns>
    /// <exception cref="std::bad_alloc">If the target cannot be allocated.</exception>
    std::wstring VoiceApiKeyTarget(std::wstring_view profileDir);

    /// <summary>Reads the profile's voice API key ("" when none is stored or the store refuses).</summary>
    /// <param name="profileDir">The profile folder.</param>
    /// <returns>The key; never logged.</returns>
    std::wstring ReadVoiceApiKey(std::wstring_view profileDir) noexcept;

    /// <summary>Stores (replaces) the profile's voice API key; an all-blank key DELETES the stored one.</summary>
    /// <param name="profileDir">The profile folder.</param>
    /// <param name="key">The key.</param>
    /// <returns>True when stored (or deleted); false when Credential Manager refused (logged without the key).</returns>
    bool StoreVoiceApiKey(std::wstring_view profileDir, std::wstring_view key) noexcept;
}
