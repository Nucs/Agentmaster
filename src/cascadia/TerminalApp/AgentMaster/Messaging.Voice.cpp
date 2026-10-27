// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the Telegram service's VOICE NOTES (MESSAGING.md §21.43, VOICE.md §6): a voice note in a session's topic is
// downloaded, transcribed and read like the text it says. See Messaging.h's §21.43 section for the shape (the voice thread,
// the lanes, the leaf lock); VoiceBridge.h for the seam to the transcriber. Plain C++ (no WinRT, no PCH).
//
// The flow of one note:
//   poll thread  _voiceTake: 👀 on the note, a VoiceJob queued, a pending item at the end of its thread's lane
//   voice thread _voiceLoop → _voiceRun: getFile, the download, the transcriber (vocabulary → engine → dictionary pass)
//                → the lane item marked done, the tick woken
//   tick thread  _voiceTick → _voiceDeliver: the transcript echoed under the note ("🎤 …"), then the note read as that text by
//                _handleTopicMessage — the same path as a typed message (a queued prompt with its reactions on the NOTE, an
//                answer to an open question card, a backlog question for a note sent while Agentmaster was closed)
// Messages of the same thread that arrive while a note is pending wait in its lane and are handed to _handleMessage again in
// arrival order, so "voice, then a typed follow-up" reaches the session in that order.

#include "Messaging.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "ClaudeSpawn.h" // AppendStateLog, LogNav, ShortId, LogSwallowedException
#include "SessionRegistry.h"

namespace Agentmaster::Msg
{
    namespace
    {
        /// <summary>The most bytes a voice note may have: the Bot API serves files up to 20 MB through getFile.</summary>
        constexpr size_t kVoiceMaxBytes = 20u << 20;
        /// <summary>The echo's transcript is cut here (a Telegram message holds 4,096 characters; the prompt keeps every word).</summary>
        constexpr size_t kVoiceEchoMaxChars = 3500;
        /// <summary>The reaction a note wears while it is downloaded and transcribed (one of the reactions a bot may set).</summary>
        constexpr const wchar_t* kVoiceHeardReaction = L"\U0001F440"; // 👀
        /// <summary>How often the idle voice thread wakes to let the transcriber end an idle worker.</summary>
        constexpr int64_t kVoiceIdleTickMs = 30 * 1000;

        /// <summary>Now, as Unix ms.</summary>
        /// <returns>The time.</returns>
        int64_t WallMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        /// <summary>The media object of a voice-like message (voice, audio, round video note) and its kind, or null.</summary>
        /// <param name="msg">The message.</param>
        /// <param name="kind">Receives "voice" / "audio" / "video_note".</param>
        /// <returns>The media object, or null for any other message.</returns>
        const json::Value* VoiceMedia(const json::Value& msg, std::wstring& kind)
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
    }

    // ===== the factory and the status line ===================================================================================

    void MessagingService::SetVoiceFactory(Voice::VoiceBridgeFactory factory) noexcept
    {
        try
        {
            std::lock_guard lk{ _voiceMtx };
            _voiceFactory = std::move(factory);
        }
        catch (...)
        {
            // Rule #18. Recovery: no factory — each voice note is answered "not available in this build".
            LogSwallowedException(L"MessagingService::SetVoiceFactory");
        }
    }

    std::wstring MessagingService::VoiceStatusLine() const noexcept
    {
        try
        {
            std::shared_ptr<Voice::VoiceBridge> voice;
            bool on = true;
            bool hasFactory = false;
            {
                std::lock_guard lk{ _voiceMtx };
                voice = _voice;
                on = _voiceOn;
                hasFactory = static_cast<bool>(_voiceFactory);
            }
            if (!on)
            {
                return L"Voice notes: off.";
            }
            if (!hasFactory)
            {
                return L"Voice notes: not available in this build.";
            }
            // Describe outside the lock: it takes the transcriber's own lock (the leaf rule of `_voiceMtx`).
            return voice ? voice->Describe() : std::wstring{ L"Voice notes: ready when the bot runs." };
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::VoiceStatusLine");
            return L"Voice notes: (status unavailable)";
        }
    }

    // ===== settings, start, stop ===========================================================================================

    void MessagingService::_voiceApply(const AppSettings& settings) noexcept
    {
        try
        {
            Voice::VoiceEngineConfig cfg;
            cfg.engine = settings.voiceEngine;
            cfg.workerCommand = settings.voiceWorkerCommand;
            cfg.endpoint = settings.voiceEndpoint.empty() ? std::wstring{ L"https://api.openai.com/v1/audio/transcriptions" } : settings.voiceEndpoint;
            cfg.model = settings.voiceModel;
            cfg.language = settings.voiceLanguage;
            cfg.useSessionContext = settings.voiceUseSessionContext;
            cfg.dictionary = settings.voiceDictionary;
            cfg.idleMinutes = ClampVoiceWorkerIdleMinutes(settings.voiceWorkerIdleMinutes);
            cfg.profileDir = _profile;
            // "off" or "" = no correction (VoiceTranscriber reads both); any other value is handed to claude's --model as is, so
            // a model id typed into settings.json works too.
            cfg.correctionModel = settings.voiceCorrectionModel;
            std::shared_ptr<Voice::VoiceBridge> voice;
            {
                std::lock_guard lk{ _voiceMtx };
                _voiceOn = settings.messagingVoiceEnabled;
                _voiceEcho = settings.voiceEchoTranscript;
                _voiceConfig = cfg;
                voice = _voice;
            }
            // Outside the leaf lock: Configure takes the transcriber's lock, and it never waits for a note in flight.
            if (voice)
            {
                voice->Configure(cfg);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the previous voice settings stay in force until the next Save.
            LogSwallowedException(L"MessagingService::_voiceApply");
        }
    }

    void MessagingService::_voiceStart() noexcept
    {
        try
        {
            {
                std::lock_guard lk{ _voiceMtx };
                _voiceStopping = false;
                _voiceJobs.clear();
                _voiceLanes.clear();
            }
            // The transcriber itself is made by the FIRST note (_voiceRun), not here: the app layer may set the factory after the
            // engine's first Apply started the bot (the Search agent's factory has the same order), and a note must still find it.
            _voiceThread = std::thread([this]() noexcept { _voiceLoop(); });
        }
        catch (...)
        {
            // Rule #18. Recovery: no voice thread this run — a voice note waits in its lane until the next start drops it (logged).
            LogSwallowedException(L"MessagingService::_voiceStart");
        }
    }

    void MessagingService::_voiceStop() noexcept
    {
        try
        {
            std::shared_ptr<Voice::VoiceBridge> voice;
            {
                std::lock_guard lk{ _voiceMtx };
                _voiceStopping = true;
                voice = _voice;
            }
            // Kill the engine FIRST: a note in flight then fails fast, and the join below does not wait for a transcription.
            if (voice)
            {
                voice->Shutdown();
            }
            _voiceCv.notify_all();
            if (_voiceThread.joinable())
            {
                _voiceThread.join();
            }
            size_t notes = 0;
            size_t held = 0;
            {
                std::lock_guard lk{ _voiceMtx };
                for (const auto& [thread, lane] : _voiceLanes)
                {
                    (void)thread;
                    for (const auto& item : lane)
                    {
                        (item.isVoice ? notes : held)++;
                    }
                }
                _voiceJobs.clear();
                _voiceLanes.clear();
                // The transcriber goes with the bot: its worker (and its model's memory) must not outlive a stopped bot. The last
                // reference is dropped outside the lock below (its destructor joins the worker's readers).
                voice = std::move(_voice);
            }
            voice.reset();
            if (notes || held)
            {
                _log(L"[msg] voice: the bot stopped with " + std::to_wstring(notes) + L" voice note(s) not transcribed and " + std::to_wstring(held) +
                     L" message(s) waiting behind them; they are not read (send them again)");
            }
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_voiceStop");
        }
    }

    // ===== the poll thread: take a note, hold what follows it ==============================================================

    bool MessagingService::_voiceTake(const json::Value& msg, const std::wstring& sessionId, int64_t threadId, bool bindHandingOver, bool edited)
    {
        std::wstring kind;
        const json::Value* media = VoiceMedia(msg, kind);
        const int64_t messageId = msg.I64At(L"message_id", 0);
        if (!media)
        {
            // Not a note: held when its thread's lane holds anything (it must not overtake a note still being transcribed).
            bool heldNow = false;
            {
                std::lock_guard lk{ _voiceMtx };
                const auto it = _voiceLanes.find(threadId);
                if (threadId != 0 && it != _voiceLanes.end())
                {
                    if (edited)
                    {
                        // An edit of a message the lane still holds replaces it there; an edit of one already read goes on to
                        // the usual edit handling (a queued prompt's text is replaced, a sent one is answered).
                        for (auto& item : it->second)
                        {
                            if (!item.isVoice && item.msg.I64At(L"message_id", 0) == messageId)
                            {
                                item.msg = msg;
                                return true;
                            }
                        }
                        return false;
                    }
                    VoiceLaneItem item;
                    item.seq = ++_voiceSeq;
                    item.isVoice = false;
                    item.sessionId = sessionId;
                    item.msg = msg;
                    it->second.push_back(std::move(item));
                    heldNow = true;
                }
            }
            if (heldNow)
            {
                // The tick hands it over as soon as the notes ahead of it are done (maybe already: wake it).
                {
                    std::lock_guard lk{ _mtx };
                    _tickRequested = true;
                }
                _tickCv.notify_all();
            }
            return heldNow;
        }
        if (edited)
        {
            return true; // an edited note (its caption): nothing to read again
        }
        // A note where no session reads it: answered, never queued anywhere.
        if (sessionId.empty())
        {
            _reply(threadId, L"\U0001F3A4 Voice notes are read in a session's topic (this thread has no session).", false, json::Value{}, messageId);
            return true;
        }
        if (bindHandingOver)
        {
            // §21.17: the thread is being bound to its new session this very moment; its held messages are handed over first.
            _reply(threadId, L"\U0001F3A4 This topic's session is still starting \x2014 send the voice note again in a moment.", false, json::Value{}, messageId);
            return true;
        }
        VoiceJob job;
        job.sessionId = sessionId;
        job.threadId = threadId;
        job.messageId = messageId;
        job.fileId = media->StrAt(L"file_id");
        job.durationSec = media->I64At(L"duration", 0);
        job.fileSize = media->I64At(L"file_size", 0);
        job.kind = kind;
        std::wstring refusal;
        {
            std::lock_guard lk{ _voiceMtx };
            if (!_voiceOn)
            {
                refusal = L"\U0001F3A4 Voice notes are off (\x2699 Settings \x203A Messaging \x203A Voice notes). Send it as text.";
            }
            else if (!_voiceFactory)
            {
                refusal = L"\U0001F3A4 Voice notes are not available in this build. Send it as text.";
            }
            else if (_voiceStopping)
            {
                refusal = L"\U0001F3A4 The bot is stopping \x2014 send the voice note again after it starts.";
            }
            else if (job.fileId.empty())
            {
                refusal = L"\U0001F3A4 This voice note carries no file. Send it again.";
            }
            else if (job.fileSize > static_cast<int64_t>(kVoiceMaxBytes))
            {
                refusal = L"\U0001F3A4 This recording is larger than the bot may download (20 MB). Send a shorter one.";
            }
            else
            {
                VoiceLaneItem item;
                item.seq = ++_voiceSeq;
                item.isVoice = true;
                item.sessionId = sessionId;
                item.msg = msg;
                // D2 is judged at ARRIVAL: a note read live stays live however long its transcription takes (a first worker start
                // loads a model; a slow CPU engine on a long note).
                item.liveAtArrival = !IsBacklogUpdate(msg.I64At(L"date", 0) * 1000, WallMs());
                job.seq = item.seq;
                _voiceLanes[threadId].push_back(std::move(item));
                _voiceJobs.push_back(job);
            }
        }
        if (!refusal.empty())
        {
            _reply(threadId, refusal, false, json::Value{}, messageId);
            LogNav(L"remote telegram voice " + ShortId(sessionId) + L" -> refused");
            return true;
        }
        _voiceCv.notify_all();
        // The note is heard: 👀 until its transcript is read (then the queued prompt's own reactions take over, §7.1).
        _react(messageId, kVoiceHeardReaction);
        _registry->UpdateQuiet(sessionId, [](SessionInfo& s) { s.readUnixMs = WallMs(); });
        _log(L"[msg] " + ShortId(sessionId) + L" voice note queued for transcription (message " + std::to_wstring(messageId) + L", " + kind + L", " +
             std::to_wstring(job.durationSec) + L" s)");
        LogNav(L"remote telegram voice " + ShortId(sessionId) + L" seconds=" + std::to_wstring(job.durationSec));
        return true;
    }

    // ===== the voice thread ================================================================================================

    void MessagingService::_voiceLoop() noexcept
    {
        // The spell checker the transcriber asks (VoiceSpellWords.h) is COM: this thread lives in the multithreaded apartment
        // for its whole life, and leaves it only after the last job.
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;)
        {
            VoiceJob job;
            std::shared_ptr<Voice::VoiceBridge> voice;
            {
                std::unique_lock lk{ _voiceMtx };
                _voiceCv.wait_for(lk, std::chrono::milliseconds(kVoiceIdleTickMs), [this] { return _voiceStopping || !_voiceJobs.empty(); });
                if (_voiceStopping)
                {
                    break;
                }
                voice = _voice;
                if (_voiceJobs.empty())
                {
                    lk.unlock();
                    if (voice)
                    {
                        voice->Tick(); // an idle worker ends here
                    }
                    continue;
                }
                job = std::move(_voiceJobs.front());
                _voiceJobs.pop_front();
            }
            Voice::VoiceTranscript result = _voiceRun(job);
            {
                std::lock_guard lk{ _voiceMtx };
                if (const auto it = _voiceLanes.find(job.threadId); it != _voiceLanes.end())
                {
                    for (auto& item : it->second)
                    {
                        if (item.seq == job.seq)
                        {
                            item.done = true;
                            item.result = std::move(result);
                            break;
                        }
                    }
                }
            }
            {
                std::lock_guard lk{ _mtx };
                _tickRequested = true;
            }
            _tickCv.notify_all();
            if (voice)
            {
                voice->Tick();
            }
        }
        // Every COM object this thread made goes BEFORE CoUninitialize (the transcriber's spell checker): released after it, it
        // would call into an unloaded server at the thread's exit. `_voice` is still set here (_voiceStop drops it after the join).
        std::shared_ptr<Voice::VoiceBridge> last;
        {
            std::lock_guard lk{ _voiceMtx };
            last = _voice;
        }
        if (last)
        {
            last->ThreadEnding();
        }
        if (SUCCEEDED(com))
        {
            ::CoUninitialize();
        }
    }

    Voice::VoiceTranscript MessagingService::_voiceRun(const VoiceJob& job) noexcept
    {
        Voice::VoiceTranscript out;
        std::wstring path;
        try
        {
            // The transcriber, made by the first note of a bot run (from the factory the app layer set; _voiceStart explains why
            // not earlier) and configured with the settings in force; later notes reuse it.
            std::shared_ptr<Voice::VoiceBridge> voice;
            bool made = false;
            Voice::VoiceEngineConfig cfg;
            Voice::VoiceBridgeFactory factory;
            {
                std::lock_guard lk{ _voiceMtx };
                voice = _voice;
                if (!voice)
                {
                    factory = _voiceFactory;
                }
                cfg = _voiceConfig;
            }
            if (!voice && factory)
            {
                // Made outside the leaf lock (the factory allocates; it starts no process), then published unless a concurrent
                // stop came first.
                std::shared_ptr<Voice::VoiceBridge> fresh{ factory().release() };
                std::lock_guard lk{ _voiceMtx };
                if (!_voiceStopping)
                {
                    if (!_voice)
                    {
                        _voice = fresh;
                        made = true;
                    }
                    voice = _voice;
                }
            }
            if (!voice)
            {
                out.error = L"the transcriber is not available";
                return out;
            }
            if (made)
            {
                voice->Configure(cfg);
            }
            // 1. getFile names the download (paced like every direct call: the chat's ~1 call/s budget is shared).
            if (!_awaitCallSlot(L"getFile"))
            {
                out.error = L"the bot is stopping";
                return out;
            }
            auto params = json::Value::MkObj();
            params.Set(L"file_id", json::Value::MkStr(job.fileId));
            const TgResult file = _api->Call(L"getFile", params, 30000);
            const std::wstring filePath = file.ok ? file.result.StrAt(L"file_path") : std::wstring{};
            if (filePath.empty())
            {
                out.error = L"Telegram did not give the file (" + (file.description.empty() ? std::wstring{ L"no path" } : file.description) + L")";
                return out;
            }
            // 2. The bytes, into the profile (a packaged app's %LOCALAPPDATA% is virtualized; a worker outside the package must
            //    read the same file): <profile>\messaging\telegram\voice\<message id>.<ext>.
            std::string bytes;
            const TgResult dl = _api->DownloadFile(filePath, kVoiceMaxBytes, bytes, 120000);
            if (!dl.ok || bytes.empty())
            {
                out.error = L"the voice note could not be downloaded (" + (dl.description.empty() ? std::wstring{ L"empty" } : dl.description) + L")";
                return out;
            }
            const std::filesystem::path dir = std::filesystem::path{ _profile } / L"messaging" / L"telegram" / L"voice";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const size_t dot = filePath.rfind(L'.');
            const std::wstring ext = dot == std::wstring::npos || filePath.size() - dot > 6 ? std::wstring{ L".oga" } : filePath.substr(dot);
            path = (dir / (std::to_wstring(job.messageId) + ext)).wstring();
            {
                std::ofstream f(std::filesystem::path{ path }, std::ios::binary | std::ios::trunc);
                f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (!f)
                {
                    out.error = L"the voice note could not be saved for the engine";
                    path.clear();
                    return out;
                }
            }
            // 3. The transcriber, with the session the note is for (its folder and title shape the vocabulary).
            Voice::VoiceNote note;
            note.audioPath = path;
            note.durationSec = job.durationSec;
            note.sessionId = job.sessionId;
            if (const auto s = _session(job.sessionId))
            {
                // The folder claude runs in (where Claude Code reads its CLAUDE.md), not the inferred one: the vocabulary is
                // the project's the session works in.
                note.workdir = s->workingDir;
                note.title = s->title;
            }
            // The claude the correction runs, read for EACH note: a claude installed or re-pointed while the bot runs is used
            // by the next note ("" = none found: the note keeps the dictionary's text and the echo says why).
            note.claudeExe = _hooks.claudeExe ? _hooks.claudeExe() : std::wstring{};
            out = voice->Transcribe(note);
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_voiceRun");
            out.ok = false;
            if (out.error.empty())
            {
                out.error = L"the transcription failed (an exception; see hooks.log)";
            }
        }
        // 4. The audio is the owner's voice: deleted as soon as it is transcribed, whatever the outcome (VOICE.md §8).
        if (!path.empty())
        {
            ::DeleteFileW(path.c_str());
        }
        return out;
    }

    // ===== the tick: hand the lanes over ===================================================================================

    void MessagingService::_voiceTick() noexcept
    {
        try
        {
            // One item at a time, taken under the leaf lock and delivered outside it. A lane stays in the map while it is being
            // handed over (it is erased only when found empty at the next look), so a message the poll thread reads meanwhile
            // still joins the lane BEHIND the items being delivered and never overtakes them.
            for (int guard = 0; guard < 256; ++guard)
            {
                int64_t threadId = 0;
                VoiceLaneItem item;
                bool got = false;
                {
                    std::lock_guard lk{ _voiceMtx };
                    for (auto it = _voiceLanes.begin(); it != _voiceLanes.end();)
                    {
                        if (it->second.empty())
                        {
                            it = _voiceLanes.erase(it);
                            continue;
                        }
                        auto& front = it->second.front();
                        if (front.isVoice && !front.done)
                        {
                            ++it; // its note is still being transcribed: the whole lane waits
                            continue;
                        }
                        threadId = it->first;
                        item = std::move(front);
                        it->second.pop_front();
                        got = true;
                        break;
                    }
                }
                if (!got)
                {
                    return;
                }
                if (item.isVoice)
                {
                    _voiceDeliver(threadId, item);
                }
                else
                {
                    try
                    {
                        _handleMessage(item.msg, item.edited, true);
                    }
                    catch (...)
                    {
                        // Rule #18. Recovery: this held message is dropped (logged); the lane goes on with the next one.
                        LogSwallowedException(L"MessagingService::_voiceTick (a held message)");
                    }
                }
            }
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_voiceTick");
        }
    }

    void MessagingService::_voiceDeliver(int64_t threadId, VoiceLaneItem& item) noexcept
    {
        try
        {
            const int64_t messageId = item.msg.I64At(L"message_id", 0);
            const Voice::VoiceTranscript& r = item.result;
            if (!r.ok)
            {
                _react(messageId, L"");
                _reply(threadId, L"\U0001F3A4 I could not transcribe this voice note: " + TgEscapeHtml(r.error) + L". Send it again, or as text.", false,
                       json::Value{}, messageId);
                _log(L"[msg] " + ShortId(item.sessionId) + L" voice note not transcribed (message " + std::to_wstring(messageId) + L"): " + r.error);
                LogNav(L"remote telegram voice " + ShortId(item.sessionId) + L" -> failed");
                return;
            }
            bool echo = true;
            {
                std::lock_guard lk{ _voiceMtx };
                echo = _voiceEcho;
            }
            // The text the note says, with the caption (a recording sent with words typed under it) after it.
            std::wstring text = r.text;
            if (const std::wstring caption = item.msg.StrAt(L"caption"); !caption.empty())
            {
                text += L"\n\n" + caption;
            }
            if (echo)
            {
                // Silent, quoting the note: the owner sees what the session will get before it gets it. A correction that ran and
                // failed says so under it: the text is the engine's, so a misheard word is more likely, and the reason tells
                // the owner what to fix (Claude Code missing, a timeout).
                const std::wstring shown = r.text.size() > kVoiceEchoMaxChars ? r.text.substr(0, kVoiceEchoMaxChars) + L"\x2026" : r.text;
                std::wstring echoText = L"\U0001F3A4 <i>" + TgEscapeHtml(shown) + L"</i>";
                if (!r.correctionError.empty())
                {
                    echoText += L"\n\x26A0\xFE0F not corrected: " + TgEscapeHtml(r.correctionError);
                }
                _reply(threadId, echoText, false, json::Value{}, messageId);
            }
            const std::wstring correction = !r.correctionError.empty() ? L", correction failed: " + r.correctionError
                                            : r.correctionMs > 0       ? (r.corrected ? L", corrected in " : L", correction kept it, ") +
                                                                       std::to_wstring(r.correctionMs) + L" ms"
                                                                       : std::wstring{};
            _log(L"[msg] " + ShortId(item.sessionId) + L" voice note transcribed (message " + std::to_wstring(messageId) + L", " + std::to_wstring(r.text.size()) +
                 L" chars, " + r.engine + L", " + std::to_wstring(r.totalMs) + L" ms, " + std::to_wstring(r.fixes.size()) + L" vocabulary fix(es)" + correction +
                 L")");
            // The note, read as the text it says: every rule of a typed message applies (a question card takes it as its answer,
            // a note sent while Agentmaster was closed is asked about, a queued prompt's reactions land on the note itself).
            json::Value said = item.msg;
            said.Set(L"text", json::Value::MkStr(text));
            if (item.liveAtArrival)
            {
                // D2 judged at arrival (_voiceTake): _handleTopicMessage skips its backlog question for a note that came live.
                said.Set(L"am_voice_live", json::Value::MkBool(true));
            }
            _react(messageId, L"");
            _handleTopicMessage(item.sessionId, said, threadId);
            LogNav(L"remote telegram voice " + ShortId(item.sessionId) + L" -> read chars=" + std::to_wstring(text.size()));
        }
        catch (...)
        {
            // Rule #18. Recovery: this note's transcript is not read (logged); the owner sees no answer and can send it again.
            LogSwallowedException(L"MessagingService::_voiceDeliver");
        }
    }
}
