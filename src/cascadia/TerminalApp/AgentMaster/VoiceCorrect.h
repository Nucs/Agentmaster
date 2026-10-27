// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — VoiceCorrect: the voice notes' CORRECTION layer (VOICE.md §5.5) — FluidVoice's "AI Enhancement", as one
// headless Claude Code call per note. The model gets the session glossary and the transcript — or every engine's hearing of
// the same recording (multi-hypothesis FUSION) — and may fix only recognition errors: a misheard term, a word that makes no
// sense in a developer's prompt, casing, a stray filler or article, punctuation. Measured on the owner's recordings
// (2026-10-27): the engines alone 5.6–12.7 % WER; with Sonnet 5.5 correcting Whisper-turbo + Parakeet's second hearing,
// 0.0 % on three runs out of three, every technical term exact.
//
// This header is the PURE half (std + Json.h; harness-tested against tools\voice-poc\llm_fix.py through the goldens): the
// system prompt verbatim, the user prompt, the command line, the output reader, the plausibility guard. The OS half —
// writing the system prompt file and running claude.exe hidden — is VoiceCorrect.cpp (RunClaudeCapture of ClaudeStatus.h).
// ⚠ llm_fix.py is the reference: a change of a prompt is made there first and the goldens regenerated.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Json.h"

namespace Agentmaster::Voice
{
    /// <summary>
    /// The correction's system prompt — llm_fix.py's SYSTEM_PROMPT, byte for byte (the goldens compare them). ⚠ Its examples
    /// name no word of the evaluation sentences: a prompt that quoted the test's own terms would score itself.
    /// </summary>
    /// <returns>The prompt (no trailing line break).</returns>
    inline const std::wstring& CorrectionSystemPrompt()
    {
        static const std::wstring k = LR"PROMPT(You fix speech-recognition errors in a software developer's dictated message to an AI coding agent.
The message was spoken aloud and transcribed by speech models; the glossary lists the technical terms of the
developer's project and tools. The speaker may have an accent, so a word can come out as a similar-sounding word
(for example "dirty" for "thirty", "seat" for "sheet", "kube control" for "kubectl").

Rules:
- Keep the speaker's words, their order and their meaning. Never add content, answer, summarize or rephrase.
- Fix only recognition errors: a word that SOUNDS like a glossary term becomes that term with the glossary's exact
  spelling (for example "duck DB" -> "DuckDB", "win RT" -> "WinRT"); a word that makes no sense in a developer's prompt
  becomes the similar-sounding word that does; identifiers and acronyms get their usual casing; a stray filler or
  article the grammar rejects is dropped; numbers stay as the transcript writes them; punctuation is fixed.
- When SEVERAL transcripts of the same recording are given, they are independent hearings by different engines: where
  they agree, keep it; where they differ, choose for each disputed word the reading the audio most likely held - the
  one that fits the sentence, a developer's prompt and the glossary. Output ONE message.
- If nothing needs fixing, return the transcript unchanged.
- Output ONLY the corrected message, with no quotes, labels or commentary.)PROMPT";
        return k;
    }

    /// <summary>
    /// llm_fix.py's build_user_prompt: the glossary, then the transcript — or, with other engines' hearings, every transcript
    /// of the same recording labeled A, B, … (the first is the primary engine's; a hearing equal to it, or empty, is dropped).
    /// </summary>
    /// <param name="text">The primary transcript (after the dictionary pass).</param>
    /// <param name="glossary">The glossary line (VoiceVocabulary::GlossaryLine).</param>
    /// <param name="alternatives">Other engines' transcripts (after their own dictionary pass).</param>
    /// <returns>The user prompt.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildCorrectionUserPrompt(std::wstring_view text, std::wstring_view glossary, const std::vector<std::wstring>& alternatives)
    {
        std::vector<std::wstring> others;
        for (const auto& a : alternatives)
        {
            if (!a.empty() && a != text)
            {
                others.push_back(a);
            }
        }
        if (others.empty())
        {
            return L"Glossary: " + std::wstring{ glossary } + L"\n\nTranscript:\n" + std::wstring{ text } + L"\n";
        }
        std::wstring out = L"Glossary: " + std::wstring{ glossary } + L"\n\nTranscripts of the same recording by different speech engines:\n";
        out += L"A: " + std::wstring{ text };
        for (size_t i = 0; i < others.size(); ++i)
        {
            out += L"\n";
            out += static_cast<wchar_t>(L'B' + i);
            out += L": " + others[i];
        }
        out += L"\n";
        return out;
    }

    /// <summary>
    /// llm_fix.py's accept_correction: a correction replaces the transcript only when it is not blank and its length is within
    /// 0.6×–1.5× of the transcript's (plus slack for a short note). A correction never adds or drops content, so a much longer
    /// or shorter text means the model ANSWERED the prompt instead of fixing it — the transcript then stands.
    /// </summary>
    /// <param name="before">The transcript the model was given.</param>
    /// <param name="after">The model's text.</param>
    /// <returns>True to use the correction.</returns>
    inline bool AcceptCorrection(std::wstring_view before, std::wstring_view after) noexcept
    {
        size_t a = 0;
        size_t b = after.size();
        while (a < b && (after[a] == L' ' || after[a] == L'\t' || after[a] == L'\r' || after[a] == L'\n'))
        {
            ++a;
        }
        while (b > a && (after[b - 1] == L' ' || after[b - 1] == L'\t' || after[b - 1] == L'\r' || after[b - 1] == L'\n'))
        {
            --b;
        }
        const double len = static_cast<double>(b - a);
        if (len <= 0)
        {
            return false;
        }
        const double n = static_cast<double>(before.size());
        return 0.6 * n - 10 <= len && len <= 1.5 * n + 20;
    }

    /// <summary>
    /// Quotes one argument for a Windows command line the way CommandLineToArgvW (and the MSVC CRT, and Node / Bun) read it
    /// back: a bare argument when it holds no space, tab, line break or quote; else in quotes, with every backslash run before
    /// a quote (or before the closing quote) doubled and each quote escaped. An empty argument becomes "".
    /// </summary>
    /// <param name="arg">The argument.</param>
    /// <returns>The quoted argument.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring QuoteCommandLineArg(std::wstring_view arg)
    {
        if (!arg.empty() && arg.find_first_of(L" \t\n\v\r\"") == std::wstring_view::npos)
        {
            return std::wstring{ arg };
        }
        std::wstring out = L"\"";
        for (size_t i = 0;; ++i)
        {
            size_t backslashes = 0;
            while (i < arg.size() && arg[i] == L'\\')
            {
                ++i;
                ++backslashes;
            }
            if (i == arg.size())
            {
                out.append(backslashes * 2, L'\\'); // they now precede the closing quote: each is doubled
                break;
            }
            if (arg[i] == L'"')
            {
                out.append(backslashes * 2 + 1, L'\\'); // doubled, plus one that escapes the quote itself
                out += L'"';
            }
            else
            {
                out.append(backslashes, L'\\'); // not before a quote: taken literally
                out += arg[i];
            }
        }
        out += L'"';
        return out;
    }

    /// <summary>
    /// The `claude -p` arguments of one correction (quoted; the user prompt last, as the positional prompt — the runner's
    /// standard input is NUL). The verified clean recipe of this PC (memory "claude-p-headless-on-this-machine"): no settings
    /// sources (no CLAUDE.md, no hooks, no user settings), no tools, no MCP server, no session file, two API retries at most,
    /// the effort through the settings' environment (the effort variable beats --effort).
    /// </summary>
    /// <param name="model">The model id ("claude-sonnet-5-5").</param>
    /// <param name="effort">The effort level ("low").</param>
    /// <param name="systemPromptFile">The file holding CorrectionSystemPrompt().</param>
    /// <param name="userPrompt">BuildCorrectionUserPrompt's text.</param>
    /// <returns>The argument string (without the executable).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring BuildCorrectionArgs(std::wstring_view model, std::wstring_view effort, std::wstring_view systemPromptFile, std::wstring_view userPrompt)
    {
        const std::wstring settings = L"{\"verbose\":false,\"env\":{\"CLAUDE_CODE_MAX_RETRIES\":\"2\",\"CLAUDE_CODE_RETRY_WATCHDOG\":\"0\",\"CLAUDE_CODE_EFFORT_LEVEL\":\"" +
                                      std::wstring{ effort } + L"\"}}";
        std::wstring args = L"-p --model " + QuoteCommandLineArg(model);
        args += L" --no-session-persistence --output-format json --strict-mcp-config --mcp-config " + QuoteCommandLineArg(L"{\"mcpServers\":{}}");
        args += L" --system-prompt-file " + QuoteCommandLineArg(systemPromptFile);
        args += L" --tools " + QuoteCommandLineArg(L"") + L" --permission-mode dontAsk --setting-sources " + QuoteCommandLineArg(L"");
        args += L" --settings " + QuoteCommandLineArg(settings);
        args += L" " + QuoteCommandLineArg(userPrompt);
        return args;
    }

    /// <summary>One correction's outcome (CorrectTranscript).</summary>
    struct CorrectionResult
    {
        bool ok{ false }; // the call answered and its text passed AcceptCorrection: `text` replaces the transcript
        std::wstring text; // the corrected transcript (ok == true)
        std::wstring error; // why the transcript stands as it was ("" when ok)
        int64_t ms{ 0 }; // the call's wall time
    };

    /// <summary>
    /// Corrects one transcript with a hidden `claude -p` (VoiceCorrect.cpp; the OS half of this header): writes the system
    /// prompt to &lt;profile&gt;\voice\correct-system.txt (when missing or changed), runs claude.exe through RunClaudeCapture
    /// (ClaudeStatus.h: a scrubbed environment, NUL standard input, a kill-on-close job) in that folder, reads its output and
    /// applies the plausibility guard. Blocks for the call (~3–8 s with Sonnet 5.5); call it on the voice thread.
    /// </summary>
    /// <param name="claudeExe">The engine's claude.exe ("" = no correction: a failure "Claude Code was not found").</param>
    /// <param name="profileDir">The active profile.</param>
    /// <param name="model">The model id ("claude-sonnet-5-5").</param>
    /// <param name="effort">The effort level ("low").</param>
    /// <param name="text">The primary transcript (after the dictionary pass).</param>
    /// <param name="glossary">The glossary line.</param>
    /// <param name="alternatives">Other engines' hearings (after their own dictionary pass).</param>
    /// <param name="timeoutMs">How long the call may take.</param>
    /// <returns>The outcome; never throws (a failure keeps the transcript, with its reason).</returns>
    CorrectionResult CorrectTranscript(const std::wstring& claudeExe, const std::wstring& profileDir, const std::wstring& model, const std::wstring& effort,
                                       const std::wstring& text, const std::wstring& glossary, const std::vector<std::wstring>& alternatives,
                                       uint32_t timeoutMs) noexcept;

    /// <summary>What one correction call printed, read.</summary>
    struct CorrectionOutput
    {
        bool ok{ false };
        std::wstring text; // the model's text ("result")
        std::wstring error; // why there is none
    };

    /// <summary>
    /// Reads `claude -p --output-format json`'s standard output: one result object ({"result": …, "is_error": …}), or — when a
    /// verbose setting leaked in — an array of every message whose LAST object is the result.
    /// </summary>
    /// <param name="stdoutText">The output.</param>
    /// <returns>The text, or the failure.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline CorrectionOutput ParseCorrectionOutput(std::wstring_view stdoutText)
    {
        CorrectionOutput r;
        const auto doc = json::Parse(stdoutText);
        if (!doc)
        {
            r.error = L"the correction's output was not JSON";
            return r;
        }
        const json::Value* result = &*doc;
        if (doc->type == json::Value::Type::Arr)
        {
            if (doc->arr.empty())
            {
                r.error = L"the correction printed no result";
                return r;
            }
            result = &doc->arr.back();
        }
        if (result->type != json::Value::Type::Obj)
        {
            r.error = L"the correction's result was not an object";
            return r;
        }
        if (result->BoolAt(L"is_error", false))
        {
            r.error = L"the correction failed: " + result->StrAt(L"result").substr(0, 300);
            return r;
        }
        r.text = result->StrAt(L"result");
        r.ok = true;
        return r;
    }
}
