// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — where a Telegram VOICE NOTE goes (MESSAGING.md §21.43, VOICE.md §3): the pure half of the service's voice entry
// (MessagingService::_voiceTake, Messaging.Voice.cpp). No Telegram call, no service, no lock: the m5 suite pins every rule
// (tests_voice.cpp, TestVoiceNoteRoute).
//
// A voice note goes where a TYPED message of the same thread goes:
//   - a session's topic: transcribed now, with that session's vocabulary, and read as the text it says;
//   - a thread no session owns yet — one the user opened (writing in the chat's "All" view opens one with that very message),
//     or one whose session closed while the thread stayed: HELD by the thread's new-thread wizard (§21.17 / §21.25) like a
//     typed first message, and transcribed only when the session is up, so the new session's own folder and CLAUDE.md shape
//     the vocabulary; its transcript is that session's first prompt;
//   - a session's topic whose new-thread bind still hands its held messages over: held by the bind, behind them;
//   - and refused where a typed message is not a prompt either: the chat's main view, the 🔎 Search topic (a search is typed),
//     a recreated topic's old thread.
// Until 2026-10-28 every thread with no session refused the note ("this thread has no session"): a note sent in "All" never
// started the session a typed message there starts.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Json.h"

namespace Agentmaster::Msg
{
    /// <summary>
    /// The most bytes a voice note may have: the Bot API serves files up to 20 MB through getFile. A larger note is refused when
    /// it ARRIVES (VoiceNoteUnreadableText), never after a whole new-thread wizard was walked for it.
    /// </summary>
    inline constexpr size_t kVoiceMaxBytes = 20u << 20;

    /// <summary>
    /// The media object of a voice-like message — a voice note, an audio file, a round video note — and its kind, or null for
    /// any other message. A message carries at most one of the three; the first found wins.
    /// </summary>
    /// <param name="msg">The Telegram message.</param>
    /// <param name="kind">Receives "voice" / "audio" / "video_note" (left alone for any other message).</param>
    /// <returns>The media object (it lives in `msg`: valid while `msg` is), or null.</returns>
    inline const json::Value* VoiceNoteMedia(const json::Value& msg, std::wstring& kind)
    {
        for (const wchar_t* k : { L"voice", L"audio", L"video_note" })
        {
            if (const auto* m = msg.Find(k); m && m->type == json::Value::Type::Obj)
            {
                kind = k;
                return m;
            }
        }
        return nullptr;
    }

    /// <summary>Whether a message is a voice-like note (VoiceNoteMedia finds its media).</summary>
    /// <param name="msg">The Telegram message.</param>
    /// <returns>True for a voice note, an audio file or a round video note.</returns>
    inline bool IsVoiceNoteMessage(const json::Value& msg)
    {
        std::wstring kind;
        return VoiceNoteMedia(msg, kind) != nullptr;
    }

    /// <summary>Where a voice note goes (DecideVoiceNoteRoute) — the same place a typed message of its thread goes.</summary>
    enum class VoiceNoteRoute
    {
        Transcribe, // a session's topic: the voice thread and the thread's lane, now
        HoldForNewThread, // a thread no session owns yet: held by its new-thread wizard, transcribed once its session is up
        HoldInBind, // a session's topic whose new-thread bind still hands messages over: held by the bind, behind them
        RefuseMainView, // the chat's main view (thread 0): a typed message gets the help line there, never a session
        RefuseSearchTopic, // the 🔎 Search topic: a search is typed (VOICE.md §10)
        RefuseOrphanTopic, // a recreated topic's old thread, about to be deleted: the note belongs in the new topic
    };

    /// <summary>
    /// Where a voice note goes, in the order _handleMessage routes a typed message: a session's topic (or its bind while that
    /// hands over), the main view, the Search topic, a recreation's old thread, else a thread whose wizard holds it.
    /// ⚠ Keep it in step with _handleMessage: a thread where a typed message starts a session must hold a note too, or a note
    /// sent in "All" is refused while a typed one there works (the bug of 2026-10-28).
    /// </summary>
    /// <param name="hasSession">The thread is a session's topic.</param>
    /// <param name="bindHandingOver">That topic's new-thread bind still hands its held messages over (§21.17).</param>
    /// <param name="threadId">The note's thread (0 = the chat's main view).</param>
    /// <param name="searchThread">The 🔎 Search topic's thread (0 = there is none).</param>
    /// <param name="orphan">The thread is a recreated topic's old thread (MessagingState::orphanThreads).</param>
    /// <returns>The route.</returns>
    inline VoiceNoteRoute DecideVoiceNoteRoute(bool hasSession, bool bindHandingOver, int64_t threadId, int64_t searchThread, bool orphan) noexcept
    {
        if (hasSession)
        {
            return bindHandingOver ? VoiceNoteRoute::HoldInBind : VoiceNoteRoute::Transcribe;
        }
        if (threadId == 0)
        {
            return VoiceNoteRoute::RefuseMainView;
        }
        if (threadId == searchThread)
        {
            return VoiceNoteRoute::RefuseSearchTopic; // searchThread 0 never gets here: thread 0 is the main view, above
        }
        if (orphan)
        {
            return VoiceNoteRoute::RefuseOrphanTopic;
        }
        return VoiceNoteRoute::HoldForNewThread;
    }

    /// <summary>
    /// The answer to a note its route refuses (posted under the note); "" for a route that takes the note. Each says where a note
    /// IS read, so the owner knows what to do with it.
    /// </summary>
    /// <param name="route">The route (DecideVoiceNoteRoute).</param>
    /// <returns>The text (HTML-safe: no markup characters), or "".</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring VoiceNoteRouteRefusalText(VoiceNoteRoute route)
    {
        switch (route)
        {
        case VoiceNoteRoute::RefuseMainView:
            return L"\U0001F3A4 Voice notes are read in a session's topic, or in a new thread, which starts a new session.";
        case VoiceNoteRoute::RefuseSearchTopic:
            return L"\U0001F3A4 The \U0001F50E Search topic reads typed searches only \x2014 type it, or send the voice note in a session's topic.";
        case VoiceNoteRoute::RefuseOrphanTopic:
            return L"\U0001F3A4 This topic is being made again and goes away in a moment; send the voice note in its new topic.";
        case VoiceNoteRoute::Transcribe:
        case VoiceNoteRoute::HoldForNewThread:
        case VoiceNoteRoute::HoldInBind:
        default:
            return {};
        }
    }

    /// <summary>
    /// Why a note cannot be read at all, whatever its thread — asked when it ARRIVES, before a wizard or a bind holds it (a note
    /// that would fail after the folder pick must say so now), and again when it is transcribed (the settings may have changed
    /// meanwhile). "" = it can be read.
    /// </summary>
    /// <param name="voiceOn">AppSettings::messagingVoiceEnabled.</param>
    /// <param name="hasTranscriber">The app layer set the transcriber's factory (the m5 harness never does).</param>
    /// <param name="stopping">The bot is stopping (its voice thread is going away).</param>
    /// <param name="hasFileId">The note's media carries a file_id (getFile needs it).</param>
    /// <param name="fileSize">Telegram's file_size (0 = unknown: allowed, the download is capped anyway).</param>
    /// <returns>The answer posted under the note, or "".</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring VoiceNoteUnreadableText(bool voiceOn, bool hasTranscriber, bool stopping, bool hasFileId, int64_t fileSize)
    {
        if (!voiceOn)
        {
            return L"\U0001F3A4 Voice notes are off (\x2699 Settings \x203A Messaging \x203A Voice notes). Send it as text.";
        }
        if (!hasTranscriber)
        {
            return L"\U0001F3A4 Voice notes are not available in this build. Send it as text.";
        }
        if (stopping)
        {
            return L"\U0001F3A4 The bot is stopping \x2014 send the voice note again after it starts.";
        }
        if (!hasFileId)
        {
            return L"\U0001F3A4 This voice note carries no file. Send it again.";
        }
        if (fileSize > static_cast<int64_t>(kVoiceMaxBytes))
        {
            return L"\U0001F3A4 This recording is larger than the bot may download (20 MB). Send a shorter one.";
        }
        return {};
    }
}
