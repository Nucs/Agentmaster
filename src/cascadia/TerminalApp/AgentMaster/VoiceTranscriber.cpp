// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — VoiceTranscriber (see VoiceTranscriber.h): one voice note → the session vocabulary → the engine → the
// dictionary pass → the correction (VoiceCorrect.h) → the prompt text.

#include "VoiceTranscriber.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <vector>

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException
#include "ProfileBootstrap.h" // Profiles::detail::Utf8ToWide
#include "VoiceCorrect.h"
#include "VoiceHttp.h"
#include "VoiceProtocol.h"
#include "VoiceSpellWords.h"
#include "VoiceVocabulary.h"
#include "VoiceWorker.h"

namespace Agentmaster::Voice
{
    namespace
    {
        /// <summary>A CLAUDE.md larger than this is read only up to it (the mined words of the first 2 MB are plenty).</summary>
        constexpr size_t kMaxContextBytes = 2u << 20;
        /// <summary>The OpenAI transcription endpoint's own upload limit (25 MB): a larger note is refused before the upload.</summary>
        constexpr size_t kMaxEndpointBytes = 25u << 20;
        /// <summary>
        /// The correction's whole budget (claude's start, the model call, its retries): Sonnet 5.5 answered the owner's 9–14 s
        /// notes in 3–11 s (the POC's runs of 2026-10-27); a call past this is ended and the note goes on with the dictionary's
        /// text. The voice thread waits for it, so the session's next message waits too (Messaging.Voice.cpp's lanes) — this
        /// caps that wait.
        /// </summary>
        constexpr uint32_t kCorrectionTimeoutMs = 60000;

        /// <summary>One line into the voice log.</summary>
        /// <param name="line">The line.</param>
        void VoiceLog(const std::wstring& line) noexcept
        {
            try
            {
                AppendStateLog(L"voice.log", line + L"\n");
            }
            catch (...)
            {
                // ⚠ DELIBERATELY BARE: the logger itself failed; nothing left to report through.
            }
        }

        /// <summary>A file's bytes, up to `cap` ("" when it cannot be read).</summary>
        /// <param name="path">The file.</param>
        /// <param name="cap">The most bytes read.</param>
        /// <returns>The bytes.</returns>
        std::string ReadBytes(const std::wstring& path, size_t cap)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f)
            {
                return {};
            }
            std::string bytes;
            bytes.resize(cap);
            f.read(bytes.data(), static_cast<std::streamsize>(cap));
            bytes.resize(static_cast<size_t>((std::max)(std::streamsize{ 0 }, f.gcount())));
            return bytes;
        }

        /// <summary>A file's last-write time and size (0, 0 when it does not exist): the context cache's key.</summary>
        /// <param name="path">The file.</param>
        /// <returns>(time, size).</returns>
        std::pair<uint64_t, uint64_t> FileStamp(const std::wstring& path) noexcept
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            {
                return { 0, 0 };
            }
            return { (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime,
                     (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow };
        }

        /// <summary>
        /// The engine's timeout for a note: a minute, plus four seconds per second of audio (a CPU engine on a long note),
        /// capped at 15 minutes. A note with no known duration gets five minutes.
        /// </summary>
        /// <param name="durationSec">Telegram's duration.</param>
        /// <returns>The timeout in milliseconds.</returns>
        uint32_t EngineTimeoutMs(int64_t durationSec) noexcept
        {
            if (durationSec <= 0)
            {
                return 5u * 60u * 1000u;
            }
            const int64_t ms = 60000 + durationSec * 4000;
            return static_cast<uint32_t>((std::min)(ms, int64_t{ 15 * 60 * 1000 }));
        }

        /// <summary>The VoiceBridge of the app.</summary>
        class VoiceTranscriber final : public VoiceBridge
        {
        public:
            /// <summary>
            /// VoiceBridge::Transcribe — on the voice thread: the session vocabulary, the engine, the dictionary pass. A failure
            /// is remembered for Describe (the cog's status line) until the next success.
            /// </summary>
            /// <param name="note">The note.</param>
            /// <returns>The transcript or the failure; never throws.</returns>
            VoiceTranscript Transcribe(const VoiceNote& note) noexcept override
            {
                VoiceTranscript out = _transcribe(note);
                try
                {
                    std::lock_guard lk{ _mtx };
                    _lastError = out.ok ? std::wstring{} : out.error;
                }
                catch (...)
                {
                    LogSwallowedException(L"VoiceTranscriber::Transcribe (last error)");
                }
                if (!out.ok)
                {
                    VoiceLog(L"[voice] " + note.sessionId.substr(0, 8) + L" failed: " + out.error);
                }
                return out;
            }

            /// <summary>The whole job behind Transcribe (every early return is a failure with its reason).</summary>
            /// <param name="note">The note.</param>
            /// <returns>The transcript or the failure; never throws.</returns>
            VoiceTranscript _transcribe(const VoiceNote& note) noexcept
            {
                VoiceTranscript out;
                const ULONGLONG started = ::GetTickCount64();
                try
                {
                    VoiceEngineConfig cfg;
                    {
                        std::lock_guard lk{ _mtx };
                        cfg = _config;
                    }
                    if (cfg.engine != L"worker" && cfg.engine != L"openai")
                    {
                        out.error = L"voice notes are off (\x2699 Settings \x203A Messaging \x203A Voice notes)";
                        return out;
                    }
                    // 1. The vocabulary of THIS session (cached while its inputs are unchanged: a burst of notes to one
                    //    session builds it once).
                    const VoiceVocabulary& vocab = _vocabularyFor(cfg, note);
                    out.vocabularyTerms = vocab.Terms().size();
                    const std::wstring prompt = vocab.WhisperPrompt();
                    // 2. The engine.
                    std::wstring raw;
                    if (cfg.engine == L"worker")
                    {
                        std::wstring command = cfg.workerCommand;
                        if (command.empty())
                        {
                            const std::wstring bundled = BundledVoiceWorkerPath();
                            if (bundled.empty())
                            {
                                out.error = L"no voice engine is set: \x2699 Settings \x203A Messaging \x203A Voice notes \x203A worker command (VOICE.md \x00A7" L"6)";
                                return out;
                            }
                            command = L"\"" + bundled + L"\"";
                        }
                        // The worker is SHARED with Configure / Shutdown (other threads): the copy taken here keeps it alive for
                        // this call even if a Save retires it meanwhile (it is then killed, and this call fails fast).
                        std::shared_ptr<VoiceWorkerProcess> worker;
                        {
                            std::lock_guard lk{ _mtx };
                            if (!_worker || _worker->CommandLine() != command)
                            {
                                if (_worker)
                                {
                                    _retired.push_back(std::move(_worker));
                                }
                                _worker = std::make_shared<VoiceWorkerProcess>(command, cfg.profileDir);
                            }
                            worker = _worker;
                        }
                        _stopRetired(); // on this (the voice) thread: Stop joins the old worker's readers
                        VoiceWorkerRequest request;
                        request.path = note.audioPath;
                        request.language = cfg.language;
                        request.prompt = prompt;
                        request.hotwords = vocab.Hotwords();
                        std::wstring error;
                        const auto answer = worker->Transcribe(request, EngineTimeoutMs(note.durationSec), error);
                        if (!answer)
                        {
                            out.error = error;
                            return out;
                        }
                        if (!answer->ok)
                        {
                            out.error = L"the voice worker failed: " + answer->error;
                            return out;
                        }
                        raw = answer->text;
                        out.engine = answer->engine;
                        out.audioMs = answer->audioMs;
                        out.engineMs = answer->decodeMs;
                        // A fusion worker's other hearings: each gets the same dictionary pass, then they go to the correction.
                        for (const auto& alt : answer->alternatives)
                        {
                            out.alternatives.push_back(vocab.Apply(alt));
                        }
                    }
                    else
                    {
                        const std::string audio = ReadBytes(note.audioPath, kMaxEndpointBytes + 1);
                        if (audio.empty())
                        {
                            out.error = L"the voice note's file could not be read";
                            return out;
                        }
                        if (audio.size() > kMaxEndpointBytes)
                        {
                            out.error = L"the voice note is larger than the endpoint takes (25 MB)";
                            return out;
                        }
                        const std::wstring key = ReadVoiceApiKey(cfg.profileDir);
                        const auto r = TranscribeWithEndpoint(cfg.endpoint, key, cfg.model.empty() ? std::wstring{ L"whisper-1" } : cfg.model, cfg.language, prompt,
                                                              L"voice.ogg", audio, EngineTimeoutMs(note.durationSec));
                        if (!r.ok)
                        {
                            out.error = r.error;
                            return out;
                        }
                        raw = r.text;
                        out.engine = L"openai:" + (cfg.model.empty() ? std::wstring{ L"whisper-1" } : cfg.model);
                        out.engineMs = r.ms;
                    }
                    // 3. The dictionary pass.
                    std::vector<VoiceReplacement> made;
                    std::wstring fixed = vocab.Apply(raw, &made);
                    out.raw = raw;
                    for (const auto& m : made)
                    {
                        out.fixes.push_back({ m.from, m.to });
                    }
                    // 4. The CORRECTION (VOICE.md §5.5): one hidden claude call with the glossary and every engine's hearing.
                    //    A failure or an implausible answer keeps the dictionary's text — a note is never lost to it, only
                    //    less polished (the reason goes to voice.log and the result). An empty hearing skips it: there is
                    //    nothing to correct, and the model would only invent.
                    bool correctionRan = false;
                    if (!fixed.empty() && !cfg.correctionModel.empty() && cfg.correctionModel != L"off")
                    {
                        correctionRan = true;
                        const CorrectionResult c = CorrectTranscript(note.claudeExe, cfg.profileDir, cfg.correctionModel, cfg.correctionEffort, fixed,
                                                                     vocab.GlossaryLine(160), out.alternatives, kCorrectionTimeoutMs);
                        out.correctionMs = c.ms;
                        if (c.ok)
                        {
                            out.corrected = c.text != fixed;
                            fixed = c.text;
                        }
                        else
                        {
                            out.correctionError = c.error;
                        }
                    }
                    // 5. One paragraph.
                    out.text = NormalizeTranscriptForPrompt(fixed);
                    out.ok = !out.text.empty();
                    if (!out.ok)
                    {
                        out.error = L"nothing was heard in the voice note";
                    }
                    out.totalMs = static_cast<int64_t>(::GetTickCount64() - started);
                    std::wstring fixesText;
                    for (const auto& f : out.fixes)
                    {
                        fixesText += L" [" + f.from + L" \x2192 " + f.to + L"]";
                    }
                    // The correction's outcome, in the same line: what a slow or unpolished note cost, and why.
                    std::wstring correctionText;
                    if (correctionRan)
                    {
                        correctionText = out.correctionError.empty()
                                             ? L", correction " + std::to_wstring(out.correctionMs) + L" ms" + (out.corrected ? L" (changed)" : L" (unchanged)")
                                             : L", correction FAILED after " + std::to_wstring(out.correctionMs) + L" ms: " + out.correctionError;
                    }
                    VoiceLog(L"[voice] " + note.sessionId.substr(0, 8) + L" " + out.engine + L": " + std::to_wstring(out.audioMs) + L" ms of audio, engine " +
                             std::to_wstring(out.engineMs) + L" ms, total " + std::to_wstring(out.totalMs) + L" ms, " + std::to_wstring(out.vocabularyTerms) +
                             L" terms, " + std::to_wstring(out.alternatives.size()) + L" other hearing(s), " + std::to_wstring(out.fixes.size()) + L" fix(es)" +
                             fixesText + correctionText);
                    return out;
                }
                catch (...)
                {
                    LogSwallowedException(L"VoiceTranscriber::_transcribe");
                    out.ok = false;
                    out.error = L"the transcription failed (an exception; see hooks.log)";
                    return out;
                }
            }

            /// <summary>
            /// VoiceBridge::Configure — takes new settings on ANY thread (the cog's Save) and never blocks on a note in flight:
            /// a changed engine or worker command RETIRES the running worker (killed at once, so a note in flight fails fast with
            /// "the voice worker exited"); its handles and reader threads are cleaned up on the voice thread (_stopRetired).
            /// </summary>
            /// <param name="config">The settings.</param>
            void Configure(const VoiceEngineConfig& config) noexcept override
            {
                try
                {
                    std::lock_guard lk{ _mtx };
                    const bool engineChanged = config.engine != _config.engine || config.workerCommand != _config.workerCommand;
                    _config = config;
                    if (engineChanged && _worker)
                    {
                        _worker->Kill();
                        _retired.push_back(std::move(_worker));
                    }
                }
                catch (...)
                {
                    LogSwallowedException(L"VoiceTranscriber::Configure");
                }
            }

            /// <summary>
            /// VoiceBridge::Tick — on the voice thread between notes: cleans up retired workers, and ends the live one when it
            /// has been idle for the configured minutes (its model leaves memory; the next note starts it again).
            /// </summary>
            void Tick() noexcept override
            {
                try
                {
                    _stopRetired();
                    std::shared_ptr<VoiceWorkerProcess> idle;
                    {
                        std::lock_guard lk{ _mtx };
                        if (!_worker || !_worker->Running())
                        {
                            return;
                        }
                        const uint64_t idleMs = ::GetTickCount64() - _worker->LastActiveTick();
                        if (idleMs < static_cast<uint64_t>(_config.idleMinutes) * 60000ull)
                        {
                            return;
                        }
                        VoiceLog(L"[voice] the worker was idle for " + std::to_wstring(idleMs / 60000) + L" min: it is ended (the next note starts it again)");
                        idle = _worker; // stopped outside the lock: Stop waits up to 2 s for a polite exit
                    }
                    idle->Stop();
                }
                catch (...)
                {
                    LogSwallowedException(L"VoiceTranscriber::Tick");
                }
            }

            /// <summary>
            /// VoiceBridge::Shutdown — from the thread that stops the service: KILLS the workers (no waiting for a note in flight,
            /// which then fails fast) and retires them; the destructor (after the voice thread is joined) cleans them up.
            /// </summary>
            void Shutdown() noexcept override
            {
                try
                {
                    std::lock_guard lk{ _mtx };
                    if (_worker)
                    {
                        _worker->Kill();
                        _retired.push_back(std::move(_worker));
                    }
                    for (const auto& w : _retired)
                    {
                        w->Kill();
                    }
                }
                catch (...)
                {
                    LogSwallowedException(L"VoiceTranscriber::Shutdown");
                }
            }

            /// <summary>
            /// VoiceBridge::ThreadEnding — on the voice thread, before its CoUninitialize: the thread's spell checker goes now (its
            /// thread_local would release it after COM is gone). The cached vocabulary keeps no COM pointer (its word test reads
            /// the thread's checker at each call), so it stays valid.
            /// </summary>
            void ThreadEnding() noexcept override
            {
                ReleaseThreadSpellChecker();
            }

            /// <summary>VoiceBridge::Describe — the engine in a few words, whether its worker runs, and the last failure.</summary>
            /// <returns>The line.</returns>
            /// <exception cref="std::bad_alloc">If the line cannot be allocated.</exception>
            std::wstring Describe() const override
            {
                std::lock_guard lk{ _mtx };
                std::wstring line;
                if (_config.engine == L"openai")
                {
                    line = L"Voice notes: " + (_config.model.empty() ? std::wstring{ L"whisper-1" } : _config.model) + L" at " + _config.endpoint;
                }
                else if (_config.engine != L"worker")
                {
                    return L"Voice notes: off";
                }
                else if ((_config.workerCommand.empty() ? BundledVoiceWorkerPath() : _config.workerCommand).empty())
                {
                    return L"Voice notes: no engine set";
                }
                else
                {
                    line = std::wstring{ L"Voice notes: local worker, " } + (_worker && _worker->Running() ? L"running" : L"starts with the next note");
                }
                return _lastError.empty() ? line : line + L" (last note failed: " + _lastError + L")";
            }

        private:
            /// <summary>
            /// Stops (joins and closes) every retired worker, on the voice thread: a retired worker was killed already, so its
            /// Stop returns at once; doing it here keeps Configure and Shutdown free of any wait.
            /// </summary>
            void _stopRetired() noexcept
            {
                std::vector<std::shared_ptr<VoiceWorkerProcess>> retired;
                {
                    std::lock_guard lk{ _mtx };
                    retired.swap(_retired);
                }
                for (const auto& w : retired)
                {
                    w->Stop();
                }
            }

            /// <summary>
            /// The session's vocabulary, rebuilt only when its inputs changed (the session's folder, title, CLAUDE.md stamp,
            /// the dictionary, the context switch — every one of them is in the key). Runs on the voice thread only; the cache
            /// is read and written there and nowhere else.
            /// </summary>
            /// <param name="cfg">The settings in force.</param>
            /// <param name="note">The note (its session's folder and title).</param>
            /// <returns>The vocabulary (owned by the transcriber; valid until the next call).</returns>
            const VoiceVocabulary& _vocabularyFor(const VoiceEngineConfig& cfg, const VoiceNote& note)
            {
                std::wstring workdir = cfg.useSessionContext ? note.workdir : std::wstring{};
                std::wstring title = cfg.useSessionContext ? note.title : std::wstring{};
                std::wstring claudeMdPath;
                std::pair<uint64_t, uint64_t> stamp{ 0, 0 };
                // The NEAREST CLAUDE.md: the session's folder, else up to three folders above it (a session in a repository's
                // subfolder speaks that repository's words; Claude Code itself loads the CLAUDE.md files of the parents too).
                std::wstring dir = workdir;
                for (int level = 0; level < 4 && !dir.empty(); ++level)
                {
                    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
                    {
                        dir.pop_back();
                    }
                    if (dir.empty() || (dir.size() == 2 && dir[1] == L':'))
                    {
                        break; // a drive root: no project there
                    }
                    const std::wstring candidate = dir + L"\\CLAUDE.md";
                    stamp = FileStamp(candidate);
                    if (stamp.second > 0)
                    {
                        claudeMdPath = candidate;
                        break;
                    }
                    const size_t slash = dir.find_last_of(L"\\/");
                    dir = slash == std::wstring::npos ? std::wstring{} : dir.substr(0, slash);
                }
                if (claudeMdPath.empty())
                {
                    stamp = { 0, 0 }; // no CLAUDE.md found: the key must not carry the last miss's stamp
                }
                const std::wstring key = workdir + L"\n" + title + L"\n" + claudeMdPath + L"\n" + std::to_wstring(stamp.first) + L":" +
                                         std::to_wstring(stamp.second) + L"\n" + cfg.dictionary;
                if (_vocab && key == _vocabKey)
                {
                    return *_vocab;
                }
                std::wstring claudeMd;
                if (stamp.second > 0)
                {
                    claudeMd = Profiles::detail::Utf8ToWide(ReadBytes(claudeMdPath, kMaxContextBytes));
                }
                _vocab = std::make_unique<VoiceVocabulary>(SessionVocabulary(SpellCheckerWordTest(), workdir, title, claudeMd, ParseDictionary(cfg.dictionary)));
                _vocabKey = key;
                return *_vocab;
            }

            mutable std::mutex _mtx; // guards _config, _worker (the pointer), _retired, _lastError
            VoiceEngineConfig _config;
            std::shared_ptr<VoiceWorkerProcess> _worker; // the live worker (shared with the call in flight)
            std::vector<std::shared_ptr<VoiceWorkerProcess>> _retired; // killed, waiting for their cleanup on the voice thread
            std::wstring _lastError;
            std::unique_ptr<VoiceVocabulary> _vocab; // the voice thread's vocabulary cache (voice thread only)
            std::wstring _vocabKey; // its inputs (voice thread only)
        };
    }

    std::unique_ptr<VoiceBridge> MakeVoiceTranscriber()
    {
        return std::make_unique<VoiceTranscriber>();
    }

    std::wstring BundledVoiceWorkerPath() noexcept
    {
        try
        {
            wchar_t buffer[MAX_PATH * 4]{};
            const DWORD n = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
            if (n == 0 || n >= std::size(buffer))
            {
                return {};
            }
            std::wstring path{ buffer, n };
            const size_t slash = path.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
            {
                return {};
            }
            path = path.substr(0, slash + 1) + L"agentmaster-voice.exe";
            return ::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ? std::wstring{} : path;
        }
        catch (...)
        {
            LogSwallowedException(L"BundledVoiceWorkerPath");
            return {};
        }
    }
}
