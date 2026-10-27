// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — VoiceTranscriber: the app's VoiceBridge (VOICE.md §6.1) — for each voice note: the session vocabulary
// (VoiceVocabulary.h, asked through Windows' spell checker, VoiceSpellWords.h), the engine (a voice worker process,
// VoiceWorker.h, or an OpenAI-compatible endpoint, VoiceHttp.h), then the dictionary pass over the engine's text.
// Plain C++ + Win32 + COM (no WinRT, no PCH). Created by the factory TerminalPage.AgentEngine.cpp hands the messaging
// service (MessagingService::SetVoiceFactory); Engine.cpp never names it.

#pragma once

#include <memory>

#include "VoiceBridge.h"

namespace Agentmaster::Voice
{
    /// <summary>
    /// Makes the app's transcriber. It starts nothing: the first note starts the worker (or calls the endpoint).
    /// </summary>
    /// <returns>The bridge; never null.</returns>
    /// <exception cref="std::bad_alloc">If it cannot be allocated.</exception>
    std::unique_ptr<VoiceBridge> MakeVoiceTranscriber();

    /// <summary>
    /// The bundled worker's path: agentmaster-voice.exe beside the running Agentmaster.exe, or "" when the install carries
    /// none (VOICE.md §6.4: built only when its native runtime is present).
    /// </summary>
    /// <returns>The full path, or "".</returns>
    std::wstring BundledVoiceWorkerPath() noexcept;
}
