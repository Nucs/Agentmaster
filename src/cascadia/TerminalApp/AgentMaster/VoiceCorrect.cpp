// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — VoiceCorrect (see VoiceCorrect.h): the OS half of the voice notes' correction layer — the system prompt file
// and one hidden `claude -p` per note, through ClaudeStatus.h's RunClaudeCapture.

#include "VoiceCorrect.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>

#include "ClaudeSpawn.h" // AppendStateLog, LogSwallowedException
#include "ClaudeStatus.h" // RunClaudeCapture
#include "ProfileBootstrap.h" // Profiles::detail::WideToUtf8

namespace Agentmaster::Voice
{
    namespace
    {
        /// <summary>One line into the voice log.</summary>
        /// <param name="line">The line.</param>
        void CorrectLog(const std::wstring& line) noexcept
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

        /// <summary>
        /// The system prompt's file, written when missing or different (byte compare): claude reads it at every call, and a
        /// prompt changed by an update must reach the next call. One writer at a time (a static mutex): the voice thread is the
        /// only caller today, the lock keeps a second one safe.
        /// </summary>
        /// <param name="profileDir">The active profile.</param>
        /// <returns>The file's full path, or "" when it could not be written.</returns>
        std::wstring EnsureSystemPromptFile(const std::wstring& profileDir)
        {
            static std::mutex m;
            std::lock_guard lk{ m };
            const std::filesystem::path dir = std::filesystem::path{ profileDir } / L"voice";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const std::filesystem::path file = dir / L"correct-system.txt";
            const std::string want = Profiles::detail::WideToUtf8(CorrectionSystemPrompt());
            {
                std::ifstream in(file, std::ios::binary);
                const std::string have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (have == want)
                {
                    return file.wstring();
                }
            }
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(want.data(), static_cast<std::streamsize>(want.size()));
            return out ? file.wstring() : std::wstring{};
        }
    }

    CorrectionResult CorrectTranscript(const std::wstring& claudeExe, const std::wstring& profileDir, const std::wstring& model, const std::wstring& effort,
                                       const std::wstring& text, const std::wstring& glossary, const std::vector<std::wstring>& alternatives,
                                       uint32_t timeoutMs) noexcept
    {
        CorrectionResult r;
        try
        {
            if (claudeExe.empty())
            {
                r.error = L"Claude Code was not found on this PC";
                return r;
            }
            const std::wstring systemFile = EnsureSystemPromptFile(profileDir);
            if (systemFile.empty())
            {
                r.error = L"the correction's system prompt could not be written";
                return r;
            }
            const std::wstring args = BuildCorrectionArgs(model, effort.empty() ? std::wstring{ L"low" } : effort, systemFile,
                                                          BuildCorrectionUserPrompt(text, glossary, alternatives));
            // The working folder is the profile's voice folder: no CLAUDE.md there (and --setting-sources "" loads none anyway),
            // nothing of the session's repository reaches the model besides the glossary.
            const ClaudeCaptureResult run = RunClaudeCapture(claudeExe, args, (std::filesystem::path{ profileDir } / L"voice").wstring(), timeoutMs);
            r.ms = run.tookMs;
            if (!run.started)
            {
                r.error = L"claude could not start: " + run.error;
                return r;
            }
            if (run.timedOut)
            {
                r.error = L"the correction did not answer within " + std::to_wstring(timeoutMs / 1000) + L" s";
                return r;
            }
            const CorrectionOutput out = ParseCorrectionOutput(run.out);
            if (!out.ok)
            {
                r.error = out.error + (run.exitCode ? L" (exit code " + std::to_wstring(run.exitCode) + L")" : std::wstring{});
                return r;
            }
            if (!AcceptCorrection(text, out.text))
            {
                // The model answered instead of correcting (or said nothing): the transcript stands; the log keeps the start of
                // what it said, for the record.
                r.error = L"the correction was not a correction (kept the transcript)";
                CorrectLog(L"[voice] correction refused by the guard: " + out.text.substr(0, 200));
                return r;
            }
            r.text = out.text;
            r.ok = true;
            return r;
        }
        catch (...)
        {
            LogSwallowedException(L"Voice::CorrectTranscript");
            r.ok = false;
            r.error = L"the correction failed (an exception; see hooks.log)";
            return r;
        }
    }
}
