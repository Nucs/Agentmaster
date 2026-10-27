// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — VoiceBridge: the seam between the Telegram service and speech-to-text (VOICE.md §6.1). Pure (std only).
//
// The messaging service (Messaging.Voice.cpp) knows only this interface: it hands a downloaded voice note and the session
// it is for, and gets the transcript back — already corrected by the session's vocabulary. Behind it the app wires
// VoiceTranscriber (VoiceTranscriber.{h,cpp}: the vocabulary, the engines, the dictionary pass) through
// MessagingService::SetVoiceFactory, from TerminalPage.AgentEngine.cpp, exactly like the Search topic's agent
// (SetSearchFactory): Engine.cpp never names it, and the m5 harness drives the service with a fake bridge instead.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Agentmaster::Voice
{
    /// <summary>
    /// How voice notes are transcribed (the cog's VOICE NOTES section; AppSettings::voice*). Copied into the bridge by
    /// Configure on every settings apply; the bridge applies a changed engine or worker command at the next note.
    /// </summary>
    struct VoiceEngineConfig
    {
        // "worker" (a local voice worker process: VoiceProtocol.h), "openai" (an OpenAI-compatible
        // /v1/audio/transcriptions endpoint: OpenAI, Groq, a local server) or "off". Any other word reads as off.
        std::wstring engine{ L"worker" };
        // The worker's whole command line ("" = the bundled agentmaster-voice.exe beside the app, when it is there).
        std::wstring workerCommand;
        std::wstring endpoint; // the "openai" engine's URL (https://api.openai.com/v1/audio/transcriptions)
        std::wstring model; // the "openai" engine's model ("whisper-1", "gpt-4o-transcribe", "whisper-large-v3-turbo")
        std::wstring language{ L"en" }; // "" = the engine detects it
        bool useSessionContext{ true }; // the session's folder name + CLAUDE.md join the vocabulary
        std::wstring dictionary; // the owner's dictionary text (VoiceVocabulary.h ParseDictionary)
        uint32_t idleMinutes{ 10 }; // the worker process ends after this long without a note (0 = after each note)
        std::wstring profileDir; // the active profile (the API key's Credential Manager target, voice.log)
        // The CORRECTION layer (VoiceCorrect.h): the model that fixes what the engines misheard, given the glossary and every
        // engine's hearing ("" or "off" = no correction). Sonnet 5.5 is what transcribed the owner's recordings word for word.
        std::wstring correctionModel{ L"claude-sonnet-5-5" };
        std::wstring correctionEffort{ L"low" };
    };

    /// <summary>One voice note to transcribe, and the session it is for.</summary>
    struct VoiceNote
    {
        std::wstring audioPath; // the downloaded file (Opus in Ogg for a voice note)
        int64_t durationSec{ 0 }; // Telegram's duration (sizes the engine's timeout); 0 = unknown
        std::wstring sessionId;
        std::wstring workdir; // the session's effective working folder (its CLAUDE.md and its name join the vocabulary)
        std::wstring title; // the session title (its hard words join the vocabulary)
        std::wstring claudeExe; // the engine's claude.exe, resolved for THIS note (the correction runs it; "" = none found)
    };

    /// <summary>One respelling the dictionary pass made (VoiceVocabulary.h's VoiceReplacement, repeated so this header stays
    /// free of the vocabulary's).</summary>
    struct VoiceFix
    {
        std::wstring from;
        std::wstring to;
    };

    /// <summary>What a transcription gives back.</summary>
    struct VoiceTranscript
    {
        bool ok{ false };
        std::wstring text; // the prompt: the engine's text after the dictionary pass, white space folded
        std::wstring raw; // the engine's own text (the log, the evaluation)
        std::wstring error; // why it failed, in words the topic can show (ok == false)
        std::wstring engine; // the engine's name ("whisper-large-v3+prompt", "openai:whisper-1")
        int64_t audioMs{ 0 }; // the audio's length, when the engine said
        int64_t engineMs{ 0 }; // the engine's own time
        int64_t totalMs{ 0 }; // the bridge's whole time (worker start, vocabulary, engine, dictionary)
        std::vector<VoiceFix> fixes; // the dictionary pass's respellings
        size_t vocabularyTerms{ 0 }; // how many terms biased the engine (the log)
        std::vector<std::wstring> alternatives; // other engines' hearings (a fusion worker), after their dictionary pass
        bool corrected{ false }; // the correction layer's text replaced the engine's
        int64_t correctionMs{ 0 }; // the correction's wall time (0 = none ran)
        std::wstring correctionError; // why a correction that ran did not replace the text ("" = it did, or none ran)
    };

    /// <summary>
    /// The transcriber as the service sees it. Every call comes from the service's ONE voice thread (Messaging.Voice.cpp),
    /// so an implementation needs no lock of its own for its engine; Configure may come from another thread (a cog Save)
    /// and must be synchronized against Transcribe.
    /// </summary>
    struct VoiceBridge
    {
        virtual ~VoiceBridge() = default;
        /// <summary>Transcribes one note. Blocks for the whole job (a worker's first start loads its model: up to minutes).</summary>
        /// <param name="note">The note.</param>
        /// <returns>The transcript, or a failure with its reason; never throws.</returns>
        virtual VoiceTranscript Transcribe(const VoiceNote& note) noexcept = 0;
        /// <summary>Takes new settings (any thread). A changed engine or worker command ends the running worker.</summary>
        /// <param name="config">The settings.</param>
        virtual void Configure(const VoiceEngineConfig& config) noexcept = 0;
        /// <summary>Ends an idle worker whose idle time passed (called by the voice thread between jobs).</summary>
        virtual void Tick() noexcept = 0;
        /// <summary>Ends the worker now (the bot stops, the app closes).</summary>
        virtual void Shutdown() noexcept = 0;
        /// <summary>
        /// The voice thread is about to leave COM (CoUninitialize): release every COM object made on it (the spell checker).
        /// ⚠ Called on that thread, last thing before CoUninitialize — a COM object released after it calls into an unloaded
        /// server (an access violation at thread exit).
        /// </summary>
        virtual void ThreadEnding() noexcept = 0;
        /// <summary>One line for the cog's status and /status: the engine, whether it runs, the last error.</summary>
        /// <returns>The line.</returns>
        virtual std::wstring Describe() const = 0;
    };

    /// <summary>Makes the app's bridge (MessagingService::SetVoiceFactory).</summary>
    using VoiceBridgeFactory = std::function<std::unique_ptr<VoiceBridge>()>;
}
