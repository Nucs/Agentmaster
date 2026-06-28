// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster engine TU. ProcessInspect CONTENT: conversation-text read, the session-end.js summary analyzer (AnalyzeSessionTranscript / lineage / duration / plan-file + Se* noise) and Codex (Phase C2) rollout-tail state. Partial TU of ProcessInspect.cpp.
#include "ProcessInspect.h"

#include <windows.h>
#include <appmodel.h> // GetPackageFamilyName (host terminal: real WT vs Agentmaster vs Dev)
#include <tlhelp32.h> // CreateToolhelp32Snapshot

// Bring Window To Front (BringClaudeWindowToFront): COM + UI Automation client for the WT tab
// pick. Raw COM via WRL ComPtr — still no WinRT, still standalone-harness friendly. The explicit
// objbase/oleauto includes keep this TU independent of WIN32_LEAN_AND_MEAN trimming windows.h.
#include <objbase.h> // CoInitializeEx / CoCreateInstance
#include <oleauto.h> // SysStringLen / SysFreeString (UIA names are BSTRs)
#include <UIAutomation.h> // IUIAutomation* (tab enumeration + SelectionItem.Select)
#include <wrl/client.h> // Microsoft::WRL::ComPtr

#include <algorithm>
#include <cwctype> // towlower (tab-name heuristics)
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "ClaudeSpawn.h" // ClaudeProjectsDir() — the live Claude transcript root
#include "Json.h" // transcript line parsing (ReadTranscriptInfo)
#include "TranscriptStore.h" // IsNoiseUserPrompt + PickDisplayTitle (the shared prompt-noise + title-precedence rules)
#include "ProcessInspect.Internal.h" // the shared file-local PEB/string/file primitives

namespace Agentmaster
{
    static std::wstring ReadConversationTextImpl(std::wstring_view transcriptPath, bool codex, size_t maxBytes);
    // Agentmaster (extra-safe): never let a transcript-read throw escape into a background coroutine (the
    // copy-transcript action) -- contain it and return empty. See AnalyzeSessionTranscript.
    std::wstring ReadConversationText(std::wstring_view transcriptPath, bool codex, size_t maxBytes)
    {
        try
        {
            return ReadConversationTextImpl(transcriptPath, codex, maxBytes);
        }
        catch (...)
        {
            OutputDebugStringW(L"[Agentmaster] ReadConversationText: swallowed parse exception (no crash)\n");
            return {};
        }
    }
    static std::wstring ReadConversationTextImpl(std::wstring_view transcriptPath, bool codex, size_t maxBytes)
    {
        if (transcriptPath.empty())
        {
            return {};
        }
        const std::wstring path{ transcriptPath };
        const std::string bytes = ReadFileHead(path, maxBytes);
        if (bytes.empty())
        {
            return {};
        }
        const bool truncated = (maxBytes != 0); // a head read may end mid-line -> skip the last segment
        const std::wstring wide = Utf8ToWide(bytes);

        // Agentmaster (revert-aware DISPLAY): the COPIED transcript must be the LIVE conversation only —
        // the chain from the current leaf to root — so a double-ESC rewind's abandoned (interleaved)
        // branch is excluded. Claude only: a Codex rollout is linear (no parentUuid tree), and
        // ActiveBranchUuids returns empty for it anyway. Empty on a truncated head read (no tail leaf
        // marker). SEARCH is a separate path (it keeps every line). See TranscriptStore::ActiveBranchUuids.
        const std::unordered_set<std::wstring> activeBranch = (!codex && !truncated) ? ActiveBranchUuids(wide) : std::unordered_set<std::wstring>{};

        std::wstring out;
        const auto emit = [&out](const wchar_t* who, std::wstring t) {
            // Trim surrounding whitespace/newlines so blocks pack cleanly.
            while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r' || t.back() == L' ' || t.back() == L'\t'))
            {
                t.pop_back();
            }
            size_t b = 0;
            while (b < t.size() && (t[b] == L'\n' || t[b] == L'\r' || t[b] == L' ' || t[b] == L'\t'))
            {
                ++b;
            }
            if (b)
            {
                t.erase(0, b);
            }
            if (t.empty())
            {
                return;
            }
            if (!out.empty())
            {
                out += L"\n\n";
            }
            out += who;
            out += L":\n";
            out += t;
        };

        size_t start = 0;
        for (size_t i = 0; i <= wide.size(); ++i)
        {
            if (i < wide.size() && wide[i] != L'\n')
            {
                continue;
            }
            if (i == wide.size() && truncated)
            {
                break;
            }
            std::wstring_view line(wide.data() + start, i - start);
            start = i + 1;
            while (!line.empty() && line.back() == L'\r')
            {
                line.remove_suffix(1);
            }
            if (line.empty())
            {
                continue;
            }
            const auto parsed = json::Parse(line);
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                continue;
            }
            const auto& obj = *parsed;

            if (codex)
            {
                // Codex rollout: the CLEAN visible messages are event_msg/{user_message,agent_message};
                // reasoning, tool calls, response_item context blobs, etc. are all skipped.
                if (obj.StrAt(L"type") != L"event_msg")
                {
                    continue;
                }
                const auto* pl = obj.Find(L"payload");
                if (!pl || pl->type != json::Value::Type::Obj)
                {
                    continue;
                }
                const std::wstring pt = pl->StrAt(L"type");
                if (pt == L"user_message")
                {
                    const std::wstring msg = pl->StrAt(L"message");
                    if (!msg.empty() && !IsNoiseUserPrompt(msg))
                    {
                        emit(L"User", msg);
                    }
                }
                else if (pt == L"agent_message")
                {
                    emit(L"Assistant", pl->StrAt(L"message"));
                }
                continue;
            }

            // Agentmaster (revert-aware): skip a Claude line on a rewound-away branch (its uuid isn't on
            // the active leaf->root chain), so the copied transcript reflects the live conversation only.
            // Empty activeBranch (head read / no leaf marker) => keep all.
            if (!activeBranch.empty())
            {
                if (const std::wstring uuid = obj.StrAt(L"uuid"); !uuid.empty() && activeBranch.count(uuid) == 0)
                {
                    continue;
                }
            }
            // Claude transcript: type user/assistant only; drop meta/compact/sidechain turns.
            const std::wstring lineType = obj.StrAt(L"type");
            const bool isUser = (lineType == L"user");
            const bool isAssistant = (lineType == L"assistant");
            if ((!isUser && !isAssistant) || obj.BoolAt(L"isMeta") || obj.BoolAt(L"isCompactSummary") || obj.BoolAt(L"isSidechain"))
            {
                continue;
            }
            const auto* msg = obj.Find(L"message");
            if (!msg || msg->type != json::Value::Type::Obj)
            {
                continue;
            }
            const auto* content = msg->Find(L"content");
            if (!content)
            {
                continue;
            }
            std::wstring text;
            bool hasToolResult = false;
            if (content->type == json::Value::Type::Str)
            {
                text = content->str;
            }
            else if (content->type == json::Value::Type::Arr)
            {
                for (const auto& blk : content->arr)
                {
                    if (blk.type != json::Value::Type::Obj)
                    {
                        continue;
                    }
                    const std::wstring bt = blk.StrAt(L"type");
                    if (bt == L"tool_result")
                    {
                        hasToolResult = true; // a tool-result turn (user role), not a human message
                        break;
                    }
                    if (bt == L"text")
                    {
                        if (!text.empty())
                        {
                            text += L"\n";
                        }
                        text += blk.StrAt(L"text");
                    }
                    // tool_use / thinking / image / etc. -> skipped (only visible TEXT is kept)
                }
            }
            if (isUser)
            {
                if (hasToolResult || text.empty() || IsNoiseUserPrompt(text))
                {
                    continue; // tool-result turn, empty, or a control marker — not a human message
                }
                emit(L"User", text);
            }
            else // assistant
            {
                if (text.empty())
                {
                    continue; // a pure tool_use / thinking turn — no visible assistant text
                }
                emit(L"Assistant", text);
            }
        }
        return out;
    }

    // --- session-end.js port helpers (TAB_OVERLAY.md summary panel) ---
    static std::wstring SeBasename(const std::wstring& fp)
    {
        const auto pos = fp.find_last_of(L"/\\");
        return pos == std::wstring::npos ? fp : fp.substr(pos + 1);
    }
    static bool SeIsPlansPath(const std::wstring& fp)
    {
        std::wstring n = fp; // normalize separators + case, then look for a /plans/ segment (JS: /[/\\]plans[/\\]/i)
        for (auto& c : n)
        {
            if (c == L'\\')
                c = L'/';
            else if (c >= L'A' && c <= L'Z')
                c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        return n.find(L"/plans/") != std::wstring::npos;
    }
    // Agentmaster (summary): the inner text of the FIRST <tag>...</tag> in c, trimmed of surrounding
    // whitespace. Empty if the tag (or its closer) is absent. Used to rebuild a slash-command prompt
    // from its wrapper tags.
    static std::wstring SeExtractTagText(const std::wstring& c, const wchar_t* tag)
    {
        const std::wstring open = std::wstring(L"<") + tag + L">";
        const std::wstring close = std::wstring(L"</") + tag + L">";
        const auto a = c.find(open);
        if (a == std::wstring::npos)
        {
            return {};
        }
        const auto s = a + open.size();
        const auto e = c.find(close, s);
        if (e == std::wstring::npos)
        {
            return {};
        }
        std::wstring inner = c.substr(s, e - s);
        size_t b = 0, en = inner.size();
        while (b < en && (inner[b] == L' ' || inner[b] == L'\t' || inner[b] == L'\r' || inner[b] == L'\n'))
        {
            ++b;
        }
        while (en > b && (inner[en - 1] == L' ' || inner[en - 1] == L'\t' || inner[en - 1] == L'\r' || inner[en - 1] == L'\n'))
        {
            --en;
        }
        return inner.substr(b, en - b);
    }

    // Agentmaster (summary): reconstruct the prompt the user actually typed for a slash command —
    // "/name args" from <command-name>/name</command-name> + <command-args>args</command-args>, with
    // the wrapper tags stripped. Empty when there's no <command-name> (the caller then leaves the
    // content for the noise filter). The bare word in <command-message> is redundant with /name, so
    // it's unused. Args may legitimately contain '<'/'>' (real prompt text) — only the WRAPPER tags
    // are removed, never angle brackets inside the user's own args.
    static std::wstring SeReconstructCommandPrompt(const std::wstring& c)
    {
        std::wstring name = SeExtractTagText(c, L"command-name");
        if (name.empty())
        {
            return {};
        }
        const std::wstring args = SeExtractTagText(c, L"command-args");
        return args.empty() ? name : (name + L" " + args);
    }

    // True iff `c` is a teammate (multi-agent) PROTOCOL message. Claude Code auto-injects an
    // "Another Claude session sent a message:" wrapper around a <teammate-message ...> block whenever a
    // PEER session signals this one. When the block's PAYLOAD is a JSON machine envelope
    // ({"type":"<x>_notification","from":...,"timestamp":...} — on disk today only idle_notification, but
    // the SHAPE generalizes to the whole family: started / completed / error / ...), it is pure signaling,
    // NOT a human prompt, so it must not be numbered in the summary. A FREE-TEXT teammate message (a real
    // request, or a delivered REPORT — e.g. summary="Full CLA audit report") has a PROSE payload (it does
    // NOT begin with {"type":"), so it stays VISIBLE — preserving the deliberate "show real teammate
    // content" decision. Detect by SHAPE (robust to any future protocol `type`, zero false-positive on a
    // prose report that merely mentions a notification): the first non-space char run after the
    // <teammate-message ...> open tag is the JSON envelope `{"type":"`. Pure.
    static bool SeIsTeammateProtocol(const std::wstring& c)
    {
        const size_t tag = c.find(L"<teammate-message");
        if (tag == std::wstring::npos)
        {
            return false;
        }
        const size_t gt = c.find(L'>', tag); // end of the <teammate-message ...> open tag
        if (gt == std::wstring::npos)
        {
            return false;
        }
        size_t p = gt + 1;
        while (p < c.size() && (c[p] == L' ' || c[p] == L'\t' || c[p] == L'\r' || c[p] == L'\n'))
        {
            ++p;
        }
        return c.compare(p, 9, L"{\"type\":\"") == 0; // a JSON machine envelope (protocol) vs a prose message/report
    }

    bool SeIsCommandNoise(const std::wstring& c)
    {
        const auto has = [&](const wchar_t* s) { return c.find(s) != std::wstring::npos; };
        const auto starts = [&](const wchar_t* p) { return c.rfind(p, 0) == 0; };
        // Agentmaster (summary fix): a message PASTED verbatim from Claude Code's own rendered output
        // begins with a TUI marker glyph that NEVER starts a typed HUMAN prompt — ● U+25CF (the
        // assistant/tool bullet), ⏺ U+23FA, or ⎿ U+23BF (the tool-result branch). It only appears when the
        // user pastes assistant/tool output back in, so it is NOT a real user message and must not be
        // numbered in the summary (the "why does my summary list assistant messages" report). Compared by
        // CODE POINT, not a literal, so the source stays pure-ASCII regardless of the compiler's /utf-8 flag.
        // A full-corpus scan (4489 ext-user msgs) found ● dominant; ⏺/⎿ are included defensively.
        if (const size_t nb = c.find_first_not_of(L" \t\r\n"); nb != std::wstring::npos)
        {
            const wchar_t f = c[nb];
            if (f == 0x25CF || f == 0x23FA || f == 0x23BF)
            {
                return true;
            }
        }
        return has(L"<command-message>") || has(L"<command-name>") || has(L"<command-args>") || has(L"<local-command-") ||
               has(L"<bash-input>") || has(L"<bash-stdout>") || has(L"<bash-stderr>") ||
               // Agentmaster (summary refinement): system-injected execution noise that arrives as a
               // "user" message but is NOT a human prompt. (a) background task-finished notices
               // <task-notification> (carry <task-id>/<tool-use-id>/<output-file>); (b) finished
               // background-command results (<output-file>, or a <status>...</status> + <summary>...
               // block); (c) subagent token/timing telemetry footers (<usage>/<subagent_tokens>).
               // Teammate (multi-agent) PROTOCOL signaling — an "Another Claude session sent a message:"
               // wrapper whose <teammate-message> payload is a JSON machine envelope (idle_notification &
               // the rest of the {"type":...,"from":...} family) — is dropped (SeIsTeammateProtocol). A
               // real teammate message / delivered REPORT (prose payload) is intentionally STILL KEPT.
               SeIsTeammateProtocol(c) ||
               has(L"<task-notification>") ||
               has(L"<output-file>") || (has(L"<status>") && has(L"<summary>")) ||
               has(L"<usage>") || has(L"<subagent_tokens>") ||
               // Agentmaster (summary refinement 2 — chosen from a full-corpus jq scan): the bash
               // analog of task-notification (a finished BACKGROUND bash command + its shell id /
               // persisted output — mostly already inside a <bash-stdout> block, but explicit here
               // closes the gap for an output-less command), the system echo of input forwarded to a
               // background task, and injected system reminders.
               has(L"<bash-notification>") || has(L"<shell-id>") || has(L"<persisted-output>") ||
               has(L"<background-task-input>") || has(L"<system-reminder>") ||
               // Agentmaster (summary fix): session-end.js also skips startsWith('\n'), but a full-corpus
               // scan (2765 transcripts / 15314 ext-user msgs) found that rule to be a 100% false positive
               // — all 24 matches were REAL user content (terminal-screen pastes / multi-line prompts that
               // merely begin with a newline), so it only ever emptied the summary. It is dropped here; the
               // caller (AnalyzeSessionTranscript) trims leading whitespace before this check, so these
               // startsWith prefixes still catch a "\nCaveat:"/"\n[Request interrupted"-style noise line.
               starts(L"Caveat:") || starts(L"Overview:") || starts(L"[Request interrupted");
    }
    static bool SeAllWhitespace(const std::wstring& s)
    {
        for (const wchar_t c : s)
        {
            if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n')
            {
                return false;
            }
        }
        return true;
    }
    static bool SeParseIso(std::wstring_view iso, FILETIME& outUtc)
    {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        if (::swscanf_s(std::wstring{ iso }.c_str(), L"%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6)
        {
            return false;
        }
        SYSTEMTIME st{};
        st.wYear = static_cast<WORD>(y);
        st.wMonth = static_cast<WORD>(mo);
        st.wDay = static_cast<WORD>(d);
        st.wHour = static_cast<WORD>(h);
        st.wMinute = static_cast<WORD>(mi);
        st.wSecond = static_cast<WORD>(s);
        return ::SystemTimeToFileTime(&st, &outUtc) != FALSE;
    }

    // Agentmaster: the line-derived LAST-ACTIVITY ms from a transcript chunk — the NEWEST `timestamp`
    // among REAL conversation lines (type "user"/"assistant", non-meta/compact/sidechain). WHY this
    // exists vs the file mtime (TranscriptTimes): `claude --resume`, a /model or permission-mode change,
    // and a shell-cwd reset all APPEND UNTIMESTAMPED state lines (`last-prompt`/`mode`/`permission-mode`/
    // `summary`) to the tail — so the file MTIME jumps to resume-time while the conversation did nothing.
    // A restored session focused after a restart would otherwise read "active just now" (it only resumed;
    // measured live: such trailer blocks sit 7–32 h after the last real line). Those lines carry no
    // `timestamp` AND aren't user/assistant, so this skips them — matching TranscriptStore::QuickRowFacts
    // (the Sessions browser's line-derived last-activity) + AnalyzeSessionTranscript's lastTs. "max, not
    // last-seen": a fork's copied tail carries OLD stamps, so the newest among the chunk wins. Pure +
    // total; tolerates a partial leading line (a tail read can start mid-line — it fails json::Parse and
    // is skipped). 0 when the chunk holds no timestamped conversation line.
    int64_t LastActivityMsFromTranscriptChunk(std::wstring_view chunk)
    {
        int64_t newest = 0;
        size_t start = 0;
        for (size_t i = 0; i <= chunk.size(); ++i)
        {
            if (i < chunk.size() && chunk[i] != L'\n')
            {
                continue;
            }
            std::wstring_view line(chunk.data() + start, i - start);
            start = i + 1;
            while (!line.empty() && line.back() == L'\r')
            {
                line.remove_suffix(1);
            }
            if (line.empty())
            {
                continue;
            }
            const auto parsed = json::Parse(line);
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                continue; // garbage / a partial leading line — skip
            }
            const auto& obj = *parsed;
            // The same real-conversation-line predicate ReadTranscriptInfo / ParseTranscriptDelta use:
            // type user/assistant only; drop meta / compact-summary / sidechain (subagent) turns. The
            // untimestamped trailer/state lines (last-prompt/mode/permission-mode) and the away_summary
            // recap (type "system") are excluded here, so neither inflates "last activity".
            const std::wstring type = obj.StrAt(L"type");
            if ((type != L"user" && type != L"assistant") || obj.BoolAt(L"isMeta") || obj.BoolAt(L"isCompactSummary") || obj.BoolAt(L"isSidechain"))
            {
                continue;
            }
            const std::wstring ts = obj.StrAt(L"timestamp");
            if (ts.empty())
            {
                continue;
            }
            FILETIME ft{};
            if (SeParseIso(ts, ft))
            {
                const int64_t ms = FileTimeToUnixMs(ft);
                if (ms > newest)
                {
                    newest = ms;
                }
            }
        }
        return newest;
    }

    // Agentmaster: the line-derived last-activity read from the transcript TAIL — the cheap,
    // mtime-gateable form of TranscriptInfo.lastTs that the Fleet Observer feeds into
    // SessionInfo.convLastActivityUnixMs INSTEAD of the lying file mtime (see
    // LastActivityMsFromTranscriptChunk for WHY mtime lies). Grows the tail window (the literal tail is
    // usually untimestamped state lines, and a single assistant line can exceed 1 MiB) until a
    // timestamped conversation line lands or the cap is hit. Folds in SUBAGENT side-file activity — a
    // running Task/Agent subagent keeps the PARENT transcript quiescent, so its work lives in the side
    // files (the same fold TranscriptTimesIn does), and those files are short-lived/never-resumed so
    // their mtime is honest. 0 when the file is absent / has no timestamped conversation line in the
    // window (the caller then falls back to the mtime). Filesystem only.
    int64_t ReadTranscriptLastActivityTailIn(std::wstring_view projectsDir, std::wstring_view cwd, std::wstring_view sessionId)
    {
        if (projectsDir.empty() || cwd.empty() || sessionId.empty())
        {
            return 0;
        }
        const std::wstring path = std::wstring{ projectsDir } + L"\\" + EncodeCwdToProjectDir(cwd) + L"\\" + std::wstring{ sessionId } + L".jsonl";
        int64_t lineMs = 0;
        for (const size_t window : { size_t{ 256u << 10 }, size_t{ 2u << 20 }, size_t{ 16u << 20 } })
        {
            const std::string bytes = ReadFileTail(path, window);
            if (bytes.empty())
            {
                break; // absent / unreadable / empty
            }
            lineMs = LastActivityMsFromTranscriptChunk(Utf8ToWide(bytes));
            if (lineMs != 0 || bytes.size() < window)
            {
                break; // found a conversation timestamp, or the whole file already fit this window
            }
        }
        const int64_t subMs = SubagentActivityUnixMs(path); // running-subagent activity (parent quiescent)
        return (std::max<int64_t>)(lineMs, subMs); // explicit template arg + parens dodge the windows.h max() macro
    }

    int64_t ReadTranscriptLastActivityTail(std::wstring_view cwd, std::wstring_view sessionId)
    {
        return ReadTranscriptLastActivityTailIn(ClaudeProjectsDir(), cwd, sessionId);
    }

    // Agentmaster (conversation lineage): the real human-prompt TEXT from a user line, or "" when it is
    // NOT one (a tool-result turn, a slash-command/bash/notification noise marker, or whitespace-only).
    // Mirrors the externalUser extraction in AnalyzeSessionTranscriptImpl (first text block / string
    // content -> slash-command reconstruct -> strip leading whitespace -> SeIsCommandNoise), so the
    // conversation-SEGMENT collector below can never drift from the live Messages list.
    static std::wstring SeExtractRealUserPrompt(const json::Value& obj)
    {
        // isCompactSummary == the synthetic "This session is being continued… Summary: …" bridge a
        // /compact writes; it is NOT a human prompt (session-end.js treats it as meta). It rides a
        // userType:"external" user line that SeIsCommandNoise doesn't catch, so guard it explicitly.
        if (obj.BoolAt(L"isCompactSummary") || obj.BoolAt(L"isMeta"))
        {
            return {};
        }
        const auto* msg = obj.Find(L"message");
        if (!msg || msg->type != json::Value::Type::Obj)
        {
            return {};
        }
        std::wstring content;
        if (const auto* c = msg->Find(L"content"))
        {
            if (c->type == json::Value::Type::Str)
            {
                content = c->str;
            }
            else if (c->type == json::Value::Type::Arr)
            {
                for (const auto& blk : c->arr)
                {
                    if (blk.type == json::Value::Type::Obj && blk.StrAt(L"type") == L"text")
                    {
                        content = blk.StrAt(L"text");
                        break;
                    }
                }
            }
        }
        if (!content.empty() && content.find(L"<command-name>") != std::wstring::npos)
        {
            if (std::wstring cmd = SeReconstructCommandPrompt(content); !cmd.empty())
            {
                content = std::move(cmd);
            }
        }
        if (const size_t nb = content.find_first_not_of(L" \t\r\n"); nb == std::wstring::npos)
        {
            content.clear();
        }
        else if (nb > 0)
        {
            content.erase(0, nb);
        }
        if (content.empty() || SeAllWhitespace(content) || SeIsCommandNoise(content))
        {
            return {};
        }
        return content;
    }

    // Agentmaster (conversation lineage): a compact token count for a segment label (409797 -> "409k").
    static std::wstring SeFormatTokens(int64_t n)
    {
        if (n <= 0)
        {
            return {};
        }
        if (n >= 1000000)
        {
            return std::to_wstring(n / 1000000) + L"M";
        }
        if (n >= 1000)
        {
            return std::to_wstring(n / 1000) + L"k";
        }
        return std::to_wstring(n);
    }

    // Agentmaster (conversation lineage): a human label for a compaction boundary's compactMetadata, e.g.
    // "compacted · manual · 409k→5k" (trigger + pre/post token counts when present).
    static std::wstring SeFormatCompactionLabel(const json::Value* md)
    {
        std::wstring label = L"compacted";
        if (md && md->type == json::Value::Type::Obj)
        {
            if (const std::wstring trig = md->StrAt(L"trigger"); !trig.empty())
            {
                label += L" · " + trig;
            }
            const std::wstring pre = SeFormatTokens(md->I64At(L"preTokens"));
            const std::wstring post = SeFormatTokens(md->I64At(L"postTokens"));
            if (!pre.empty() && !post.empty())
            {
                label += L" · " + pre + L"→" + post;
            }
        }
        return label;
    }

    std::vector<ConversationSegment> CollectConversationSegments(std::wstring_view transcriptText)
    {
        // Split a transcript's COMPLETE text into segments at each system/compact_boundary, collecting
        // each segment's REAL user prompts (deduped WITHIN the segment, like the live Messages list).
        // The boundary that ENDS a segment supplies its label. Segments are in file order — the LAST is
        // the current/active conversation; the earlier ones are the "previous session(s)" /compact
        // summarized away. By POSITION (not the leaf chain — a boundary's parentUuid is null, so the
        // pre-compaction turns are unreachable by a leaf walk). Pure.
        std::vector<ConversationSegment> segs(1);
        std::unordered_set<std::wstring> seen; // dedup within the current segment; resets at each boundary
        size_t start = 0;
        for (size_t i = 0; i <= transcriptText.size(); ++i)
        {
            if (i < transcriptText.size() && transcriptText[i] != L'\n')
            {
                continue;
            }
            std::wstring_view line = transcriptText.substr(start, i - start);
            start = i + 1;
            while (!line.empty() && line.back() == L'\r')
            {
                line.remove_suffix(1);
            }
            if (line.empty())
            {
                continue;
            }
            const auto parsed = json::Parse(line);
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                continue;
            }
            const auto& obj = *parsed;
            const std::wstring type = obj.StrAt(L"type");
            if (type == L"system" && obj.StrAt(L"subtype") == L"compact_boundary")
            {
                segs.back().label = SeFormatCompactionLabel(obj.Find(L"compactMetadata"));
                segs.emplace_back();
                seen.clear();
                continue;
            }
            if (type == L"user" && obj.StrAt(L"userType") == L"external")
            {
                if (std::wstring p = SeExtractRealUserPrompt(obj); !p.empty() && seen.insert(p).second)
                {
                    segs.back().userMsgs.push_back(std::move(p));
                }
            }
        }
        return segs;
    }

    static SessionSummary AnalyzeSessionTranscriptImpl(std::wstring_view transcriptPath, size_t maxBytes);
    // Agentmaster (extra-safe): a transcript parser must NEVER throw into its caller. Most callers run on
    // a BACKGROUND thread inside a fire_and_forget coroutine (the summary panel, alt-nav, the Sessions
    // browser) or the scanner thread, where an uncaught exception -- a malformed/partial .jsonl, an
    // unguarded substr, a std::bad_alloc on a huge file -- would unwind with no frame to catch it and
    // std::terminate the whole app, taking every session with it. This thin wrapper contains any throw
    // and returns an empty result (== the existing "not found" path); the real work is the Impl below.
    // OutputDebugString can't throw and needs no profile/logging dependency, so the engine stays pure for
    // the test harness + CLI while a genuine parse bug stays discoverable (DebugView / a debugger).
    SessionSummary AnalyzeSessionTranscript(std::wstring_view transcriptPath, size_t maxBytes)
    {
        try
        {
            return AnalyzeSessionTranscriptImpl(transcriptPath, maxBytes);
        }
        catch (...)
        {
            OutputDebugStringW(L"[Agentmaster] AnalyzeSessionTranscript: swallowed parse exception (no crash)\n");
            return {};
        }
    }

    std::vector<ConversationSegment> CollectConversationLineage(const std::wstring& sessionId,
                                                                const std::wstring& cwd,
                                                                int maxDepth)
    {
        std::vector<ConversationSegment> lineage;
        if (sessionId.empty())
        {
            return lineage;
        }
        // Cycle-safe: every id we touch (the start + each resolved parent) goes in `seen`, so a
        // pathological loop (a self-pointing parentSessionId, a continuation graph cycle) can't spin.
        std::unordered_set<std::wstring> seen;
        seen.insert(sessionId);

        // State carried hop-to-hop. The FIRST hop needs the CURRENT session's parentSessionId (the
        // plan-restart "read the full transcript at: <parent>.jsonl" link) — read it once here (whole
        // file: the marker rides the first external-user message, but that message can be a large plan
        // paste, so don't risk truncating it). Each later hop gets its parentSessionId from the same
        // read that yields its segments (no extra IO).
        std::wstring curId = sessionId;
        std::wstring curCwd = cwd;
        std::wstring curParentId;
        {
            const std::wstring curPath = ResolveClaudeTranscriptPath(curId);
            if (!curPath.empty())
            {
                curParentId = AnalyzeSessionTranscript(curPath, 0 /* whole file */).parentSessionId;
            }
        }

        for (int depth = 0; depth < maxDepth; ++depth)
        {
            // Resolve cur's predecessor: ONLY a plan-restart PARENT — an EXPLICIT cross-file link the child
            // transcript itself carries ("read the full transcript at: <parent>.jsonl"). The former /clear
            // continuation predecessor (a same-cwd session that merely started shortly before) was REMOVED:
            // there is no solid on-disk signal for a /clear successor (/clear leaves no link; /compact is
            // IN-PLACE in the same file), so the timing heuristic merged unrelated conversations into a
            // false lineage. Plan-restart, by contrast, is a real reference and stays. [Agentmaster]
            std::wstring predId;
            std::wstring predCwd;
            if (!curParentId.empty() && curParentId != curId && !seen.count(curParentId) &&
                !ResolveClaudeTranscriptPath(curParentId).empty())
            {
                predId = curParentId; // plan-restart parent (solid, explicit)
            }
            if (predId.empty())
            {
                break; // no explicit cross-file parent — the lineage ends here
            }
            seen.insert(predId);

            const std::wstring predPath = ResolveClaudeTranscriptPath(predId);
            if (predPath.empty())
            {
                break; // the parent's transcript is gone — stop rather than guess past it
            }
            const auto pa = AnalyzeSessionTranscript(predPath, 0 /* whole file */);

            // The predecessor contributes, OLDEST FIRST: its OWN in-file /compact history, then a
            // segment for its active (leaf) messages — the part its own file did NOT summarize away.
            // So the panel's "previous session N" list spans files seamlessly (a parent that was itself
            // /compact'ed surfaces as several numbered sessions).
            std::vector<ConversationSegment> contribution = pa.previousSegments;
            if (!pa.userMsgs.empty())
            {
                ConversationSegment leaf;
                leaf.userMsgs = pa.userMsgs; // a plain cross-file join => no compaction label (header reads "Previous session N")
                contribution.push_back(std::move(leaf));
            }
            // Prepend the whole contribution BEFORE everything gathered so far — this parent is older.
            lineage.insert(lineage.begin(), contribution.begin(), contribution.end());

            // Advance: the predecessor becomes `cur`. Its parentSessionId comes from the SAME read. Its
            // cwd advances to the predecessor's REAL dir — a plan-parent hop may land in a DIFFERENT dir,
            // so the NEXT hop's plan-parent resolution + segment read look in the right place. One cheap
            // head read; plan hops are rare.
            curId = predId;
            curParentId = pa.parentSessionId;
            if (predCwd.empty())
            {
                predCwd = ReadTranscriptQuickFacts(predPath, 0).cwd;
            }
            if (!predCwd.empty())
            {
                curCwd = predCwd;
            }
        }
        return lineage;
    }

    static SessionSummary AnalyzeSessionTranscriptImpl(std::wstring_view transcriptPath, size_t maxBytes)
    {
        SessionSummary out;
        if (transcriptPath.empty())
        {
            return out;
        }
        const std::string bytes = ReadFileHead(std::wstring{ transcriptPath }, maxBytes);
        if (bytes.empty())
        {
            return out;
        }
        out.found = true;
        const bool truncated = (maxBytes != 0);
        const std::wstring wide = Utf8ToWide(bytes);

        // Agentmaster (revert-aware DISPLAY): a Claude double-ESC rewind orphans the abandoned
        // branch's message lines — they stay in the file, INTERLEAVED with the live ones — so the
        // summary panel must show ONLY the live branch (the chain from the current leaf to root).
        // Build that uuid set ONCE and skip any node not on it. Computed on a FULL read only: a
        // truncated head read's tail leaf marker is absent (and an EARLY in-window `last-prompt`
        // marker would name a stale leaf), so `truncated` forces the empty set == keep-all. An
        // empty set also covers pre-marker strata. SEARCH/index deliberately does NOT filter (a
        // reverted message stays findable) — see TranscriptStore::ActiveBranchUuids.
        const std::unordered_set<std::wstring> activeBranch = truncated ? std::unordered_set<std::wstring>{} : ActiveBranchUuids(wide);

        std::unordered_set<std::wstring> seenMsgs, seenRead, seenEdit, seenCreated;
        // A Write's created-vs-overwrote verdict is in its tool_result ("File created successfully at:"
        // for a NEW file, "...has been updated successfully" otherwise), which arrives in a later user
        // message — so defer Write classification: map the Write's tool_use id -> its file_path here,
        // resolve it when the matching tool_result is seen, and fall back to "edited" for any with no
        // result by end-of-transcript (a truncated tail).
        std::unordered_map<std::wstring, std::wstring> pendingWrites;
        // Agentmaster: tool_use ids of interactive (AskUserQuestion) blocks seen so far — a later user
        // tool_result answering one is a REAL user interaction (the user chose an answer), so it must
        // advance "last user msg" (out.lastUserTs) like a typed prompt. The interactive-tool set mirrors
        // SessionScanner.h's IsInteractiveTool (today: AskUserQuestion).
        std::unordered_set<std::wstring> interactiveAskIds;
        json::Value lastTodos;
        bool haveTodos = false;
        bool isFirstUser = true;

        size_t start = 0;
        for (size_t i = 0; i <= wide.size(); ++i)
        {
            if (i < wide.size() && wide[i] != L'\n')
            {
                continue;
            }
            if (i == wide.size() && truncated)
            {
                break;
            }
            std::wstring_view line(wide.data() + start, i - start);
            start = i + 1;
            while (!line.empty() && line.back() == L'\r')
            {
                line.remove_suffix(1);
            }
            if (line.empty())
            {
                continue;
            }
            const auto parsed = json::Parse(line);
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                continue;
            }
            const auto& obj = *parsed;

            // Agentmaster (revert-aware): the per-message "IsActiveLeaf" property — false when this line
            // sits on a branch a double-ESC rewind abandoned (its uuid isn't on the current leaf->root
            // chain). uuid-less state/marker lines (mode / permission-mode / last-prompt / snapshot) and
            // the keep-all case (empty set — a head read / no marker) are active. Decided BEFORE the
            // timestamp capture so a discarded turn never sets first/last activity.
            const std::wstring lineUuid = obj.StrAt(L"uuid");
            const bool onActiveBranch = activeBranch.empty() || lineUuid.empty() || activeBranch.count(lineUuid) != 0;
            if (!onActiveBranch)
            {
                continue; // not part of the live conversation
            }

            const std::wstring ts = obj.StrAt(L"timestamp");
            if (!ts.empty())
            {
                if (out.firstTs.empty())
                {
                    out.firstTs = ts;
                }
                out.lastTs = ts;
            }
            if (out.branch.empty())
            {
                const std::wstring gb = obj.StrAt(L"gitBranch");
                if (!gb.empty())
                {
                    out.branch = gb;
                }
            }

            const std::wstring type = obj.StrAt(L"type");
            const bool externalUser = (type == L"user" && obj.StrAt(L"userType") == L"external");

            if (externalUser && isFirstUser)
            {
                isFirstUser = false;
                if (const auto* pc = obj.Find(L"planContent");
                    pc && pc->type != json::Value::Type::Null &&
                    !(pc->type == json::Value::Type::Bool && !pc->boolean) &&
                    !(pc->type == json::Value::Type::Str && pc->str.empty()))
                {
                    out.hasPlanContent = true;
                }
                // Parent: "read the full transcript at: <...>.jsonl" in the (string) content.
                const auto* msg = obj.Find(L"message");
                std::wstring contentStr;
                if (msg && msg->type == json::Value::Type::Obj)
                {
                    if (const auto* c = msg->Find(L"content"); c && c->type == json::Value::Type::Str)
                    {
                        contentStr = c->str;
                    }
                }
                if (!contentStr.empty())
                {
                    std::wstring lc = contentStr;
                    for (auto& ch : lc)
                    {
                        if (ch >= L'A' && ch <= L'Z')
                        {
                            ch = static_cast<wchar_t>(ch - L'A' + L'a');
                        }
                    }
                    const auto mk = lc.find(L"read the full transcript at:");
                    if (mk != std::wstring::npos)
                    {
                        size_t p = mk + 28; // == len("read the full transcript at:")
                        while (p < contentStr.size() && (contentStr[p] == L' ' || contentStr[p] == L'\t'))
                        {
                            ++p;
                        }
                        size_t e = p;
                        while (e < contentStr.size() && contentStr[e] != L' ' && contentStr[e] != L'\t' && contentStr[e] != L'\r' && contentStr[e] != L'\n')
                        {
                            ++e;
                        }
                        const std::wstring tok = contentStr.substr(p, e - p);
                        if (tok.size() >= 6)
                        {
                            std::wstring tl = tok;
                            for (auto& ch : tl)
                            {
                                if (ch >= L'A' && ch <= L'Z')
                                {
                                    ch = static_cast<wchar_t>(ch - L'A' + L'a');
                                }
                            }
                            if (tl.rfind(L".jsonl") == tl.size() - 6)
                            {
                                std::wstring stem = SeBasename(tok);
                                stem.resize(stem.size() - 6); // drop ".jsonl"
                                out.parentSessionId = stem;
                            }
                        }
                    }
                }
            }

            if (externalUser)
            {
                const auto* msg = obj.Find(L"message");
                if (msg && msg->type == json::Value::Type::Obj)
                {
                    const auto* c = msg->Find(L"content");
                    std::wstring content;
                    if (c)
                    {
                        if (c->type == json::Value::Type::Str)
                        {
                            content = c->str;
                        }
                        else if (c->type == json::Value::Type::Arr)
                        {
                            for (const auto& blk : c->arr)
                            {
                                if (blk.type == json::Value::Type::Obj && blk.StrAt(L"type") == L"text")
                                {
                                    content = blk.StrAt(L"text");
                                    break;
                                }
                            }
                        }
                    }
                    // Agentmaster: a slash command (/clear, /compact, /<custom>, ...) arrives as a user
                    // message wrapped in <command-name>/x</command-name> + <command-args>...</command-args>.
                    // Reconstruct the prompt the user actually typed — "/x args", wrapper tags stripped —
                    // so it reads as a real numbered Message instead of being dropped as noise below. The
                    // reconstructed text has no <command-*> wrappers, so it sails through SeIsCommandNoise;
                    // a malformed wrapper (no reconstruction) keeps the original and is filtered as before.
                    if (!content.empty() && content.find(L"<command-name>") != std::wstring::npos)
                    {
                        if (std::wstring cmd = SeReconstructCommandPrompt(content); !cmd.empty())
                        {
                            content = std::move(cmd);
                        }
                    }
                    // Agentmaster (summary fix): strip leading whitespace/newlines BEFORE the noise
                    // checks AND before storing. A pasted prompt (e.g. a terminal-screen capture) often
                    // begins with a newline — left in place it (a) defeats SeIsCommandNoise's startsWith
                    // prefixes (a "\nCaveat:" line would slip through) and (b) used to get the WHOLE
                    // message dropped by the old startsWith('\n') rule (a 100% false positive, now gone),
                    // leaving the summary panel empty for a session whose only human input is a paste.
                    // A whitespace-only message trims to empty and is dropped by the !content.empty() gate.
                    if (const size_t nb = content.find_first_not_of(L" \t\r\n"); nb == std::wstring::npos)
                    {
                        content.clear();
                    }
                    else if (nb > 0)
                    {
                        content.erase(0, nb);
                    }
                    // isCompactSummary == the synthetic "This session is being continued… Summary: …"
                    // bridge a /compact writes. It rides a userType:"external" user line and is NOT
                    // caught by SeIsCommandNoise, so without this guard it leaked into the Messages list
                    // as a fake "1st message" (session-end.js treats it as meta). The compaction is
                    // surfaced instead by the `compacted` flag + the previous-session segments.
                    if (!content.empty() && !SeAllWhitespace(content) && !SeIsCommandNoise(content) && !obj.BoolAt(L"isCompactSummary") && !obj.BoolAt(L"isMeta"))
                    {
                        // Track the LAST real user prompt's time (for the "last user msg" ago), even if
                        // the text dedups against an earlier identical prompt — recency is what matters.
                        out.lastUserTs = ts;
                        if (seenMsgs.insert(content).second)
                        {
                            out.userMsgs.push_back(content);
                        }
                    }
                    // Agentmaster: answering an AskUserQuestion is a real user interaction, so it advances
                    // "last user msg" too — but the answer's content is a tool_result block (not a typed
                    // text prompt), so the text path above skips it. Detect the answer by correlating the
                    // tool_result's tool_use_id back to a prior interactive (AskUserQuestion) tool_use
                    // (interactiveAskIds, populated in the assistant branch below). Advance the TIMESTAMP
                    // only — the synthetic "User has answered your questions…" text stays OUT of the
                    // Messages list (userMsgs is real typed prompts).
                    else if (c && c->type == json::Value::Type::Arr && !interactiveAskIds.empty())
                    {
                        for (const auto& blk : c->arr)
                        {
                            if (blk.type == json::Value::Type::Obj && blk.StrAt(L"type") == L"tool_result" &&
                                interactiveAskIds.count(blk.StrAt(L"tool_use_id")) != 0)
                            {
                                out.lastUserTs = ts;
                                break;
                            }
                        }
                    }
                }
            }

            // Agentmaster: the Claude Code idle RECAP (a {"type":"system","subtype":"away_summary"}
            // line written when the session sits idle >5 min — a one-paragraph "what we did / what's
            // next"). The LAST one wins (newer recaps supersede); the "(disable recaps in /config)" UI
            // hint is stripped (NormalizeRecapText). Empty content is ignored so a malformed line never
            // clears a good recap.
            if (type == L"system" && obj.StrAt(L"subtype") == L"away_summary")
            {
                if (std::wstring r = NormalizeRecapText(obj.StrAt(L"content")); !r.empty())
                {
                    out.awaySummary = std::move(r);
                }
            }

            if (type == L"assistant")
            {
                const auto* msg = obj.Find(L"message");
                if (msg && msg->type == json::Value::Type::Obj)
                {
                    if (const auto* c = msg->Find(L"content"); c && c->type == json::Value::Type::Arr)
                    {
                        for (const auto& blk : c->arr)
                        {
                            if (blk.type != json::Value::Type::Obj || blk.StrAt(L"type") != L"tool_use")
                            {
                                continue;
                            }
                            const std::wstring name = blk.StrAt(L"name");
                            const auto* input = blk.Find(L"input");
                            std::wstring fp;
                            if (input && input->type == json::Value::Type::Obj)
                            {
                                fp = input->StrAt(L"file_path");
                            }
                            if (!fp.empty())
                            {
                                const std::wstring base = SeBasename(fp);
                                if (name == L"Read")
                                {
                                    if (seenRead.insert(base).second)
                                    {
                                        out.filesRead.push_back(base);
                                    }
                                    if (SeIsPlansPath(fp))
                                    {
                                        out.planFilesRead.push_back(fp);
                                    }
                                }
                                else if (name == L"Edit")
                                {
                                    // Edit always targets an EXISTING file (Claude requires a prior Read).
                                    if (seenCreated.find(base) == seenCreated.end() && seenEdit.insert(base).second)
                                    {
                                        out.filesEdited.push_back(base);
                                    }
                                }
                                else if (name == L"Write")
                                {
                                    // Defer: created (new file) vs edited (overwrite) is decided by the
                                    // tool_result text, resolved below. Keep the plan-file capture here.
                                    if (const std::wstring id = blk.StrAt(L"id"); !id.empty())
                                    {
                                        pendingWrites[id] = fp;
                                    }
                                    if (SeIsPlansPath(fp))
                                    {
                                        out.planFilePath = fp;
                                    }
                                }
                            }
                            if (name == L"TodoWrite" && input && input->type == json::Value::Type::Obj)
                            {
                                if (const auto* todos = input->Find(L"todos"); todos && todos->type == json::Value::Type::Arr)
                                {
                                    lastTodos = *todos;
                                    haveTodos = true;
                                }
                            }
                            if (name == L"ExitPlanMode")
                            {
                                out.hasExitPlanMode = true;
                            }
                            // Agentmaster: remember each interactive (AskUserQuestion) tool_use id so the
                            // user's later tool_result answering it advances "last user msg" (see the
                            // externalUser branch above). Mirrors SessionScanner.h's IsInteractiveTool.
                            if (name == L"AskUserQuestion")
                            {
                                if (const std::wstring askId = blk.StrAt(L"id"); !askId.empty())
                                {
                                    interactiveAskIds.insert(askId);
                                }
                            }
                        }
                    }
                }
            }

            // tool_result blocks (carried in user messages) resolve a deferred Write: "File created
            // successfully at:" => a NEW file (Files Created), anything else (an overwrite) => Files
            // Edited. Created wins over Edited for the same basename.
            if (type == L"user" && !pendingWrites.empty())
            {
                const auto* msg = obj.Find(L"message");
                if (msg && msg->type == json::Value::Type::Obj)
                {
                    if (const auto* c = msg->Find(L"content"); c && c->type == json::Value::Type::Arr)
                    {
                        for (const auto& blk : c->arr)
                        {
                            if (blk.type != json::Value::Type::Obj || blk.StrAt(L"type") != L"tool_result")
                            {
                                continue;
                            }
                            const auto pw = pendingWrites.find(blk.StrAt(L"tool_use_id"));
                            if (pw == pendingWrites.end())
                            {
                                continue;
                            }
                            const std::wstring base = SeBasename(pw->second);
                            pendingWrites.erase(pw);
                            std::wstring res; // the tool_result text (a string, or {type:text} blocks)
                            if (const auto* rc = blk.Find(L"content"))
                            {
                                if (rc->type == json::Value::Type::Str)
                                {
                                    res = rc->str;
                                }
                                else if (rc->type == json::Value::Type::Arr)
                                {
                                    for (const auto& rb : rc->arr)
                                    {
                                        if (rb.type == json::Value::Type::Obj && rb.StrAt(L"type") == L"text")
                                        {
                                            res += rb.StrAt(L"text");
                                        }
                                    }
                                }
                            }
                            if (res.find(L"File created successfully at:") != std::wstring::npos)
                            {
                                if (seenCreated.insert(base).second)
                                {
                                    out.filesCreated.push_back(base);
                                }
                            }
                            else if (seenCreated.find(base) == seenCreated.end() && seenEdit.insert(base).second)
                            {
                                out.filesEdited.push_back(base);
                            }
                        }
                    }
                }
            }
        }

        // Any Write whose tool_result never arrived (a truncated tail) falls back to "edited".
        for (const auto& [id, fp] : pendingWrites)
        {
            const std::wstring base = SeBasename(fp);
            if (seenCreated.find(base) == seenCreated.end() && seenEdit.insert(base).second)
            {
                out.filesEdited.push_back(base);
            }
        }

        std::sort(out.filesRead.begin(), out.filesRead.end());
        std::sort(out.filesCreated.begin(), out.filesCreated.end());
        std::sort(out.filesEdited.begin(), out.filesEdited.end());
        // A file shown under Files Created / Files Edited is already accounted for there — drop it
        // from Files Read so the same basename is never listed twice (working on a file is the
        // meaningful line; the read of it is implied). Read keeps only files that were ONLY read.
        if (!out.filesRead.empty() && (!out.filesEdited.empty() || !out.filesCreated.empty()))
        {
            std::unordered_set<std::wstring> written(out.filesEdited.begin(), out.filesEdited.end());
            written.insert(out.filesCreated.begin(), out.filesCreated.end());
            out.filesRead.erase(std::remove_if(out.filesRead.begin(), out.filesRead.end(),
                                                [&written](const std::wstring& f) { return written.count(f) != 0; }),
                                out.filesRead.end());
        }
        if (haveTodos)
        {
            for (const auto& t : lastTodos.arr)
            {
                if (t.type != json::Value::Type::Obj)
                {
                    continue;
                }
                const std::wstring st = t.StrAt(L"status");
                if (st == L"completed")
                {
                    ++out.tasksCompleted;
                }
                else if (st == L"pending" || st == L"in_progress")
                {
                    ++out.tasksPending;
                }
            }
        }

        // Agentmaster (conversation lineage): split by in-file compaction boundaries. The LAST segment
        // is the active conversation (== out.userMsgs, leaf-filtered above); the EARLIER ones are the
        // "previous session(s)" /compact summarized away — surfaced (oldest first) by the summary panel's
        // previous-sessions toggle. Skipped on a truncated head read (a boundary may be cut off). Only
        // non-empty previous segments are kept (an empty one — e.g. back-to-back compactions — is noise).
        if (!truncated)
        {
            std::vector<ConversationSegment> segs = CollectConversationSegments(wide);
            if (segs.size() > 1)
            {
                out.compacted = true;
                segs.pop_back(); // drop the current/active segment (already in out.userMsgs)
                for (auto& s : segs)
                {
                    if (!s.userMsgs.empty())
                    {
                        out.previousSegments.push_back(std::move(s));
                    }
                }
            }
        }
        return out;
    }

    std::wstring FormatSessionDuration(std::wstring_view startIso, std::wstring_view endIso)
    {
        if (startIso.empty() || endIso.empty())
        {
            return {};
        }
        FILETIME a{}, b{};
        if (!SeParseIso(startIso, a) || !SeParseIso(endIso, b))
        {
            return {};
        }
        ULARGE_INTEGER ua{}, ub{};
        ua.LowPart = a.dwLowDateTime;
        ua.HighPart = a.dwHighDateTime;
        ub.LowPart = b.dwLowDateTime;
        ub.HighPart = b.dwHighDateTime;
        const long long diffSec = ub.QuadPart >= ua.QuadPart ? static_cast<long long>((ub.QuadPart - ua.QuadPart) / 10000000ULL) : 0;
        const long long h = diffSec / 3600, m = (diffSec % 3600) / 60, s = diffSec % 60;
        wchar_t dur[48];
        if (h > 0)
        {
            ::swprintf(dur, 48, L"%lldh %lldm", h, m);
        }
        else if (m > 0)
        {
            ::swprintf(dur, 48, L"%lldm %llds", m, s);
        }
        else
        {
            ::swprintf(dur, 48, L"%llds", s);
        }
        const auto hhmm = [](const FILETIME& utc) -> std::wstring {
            FILETIME lf{};
            SYSTEMTIME st{};
            if (::FileTimeToLocalFileTime(&utc, &lf) && ::FileTimeToSystemTime(&lf, &st))
            {
                wchar_t t[8];
                ::swprintf(t, 8, L"%02d:%02d", st.wHour, st.wMinute);
                return t;
            }
            return L"--:--";
        };
        return std::wstring{ dur } + L" (" + hhmm(a) + L" -> " + hhmm(b) + L")";
    }

    std::wstring FindPlanFileInTranscript(std::wstring_view transcriptPath)
    {
        if (transcriptPath.empty())
        {
            return {};
        }
        const std::string bytes = ReadFileHead(std::wstring{ transcriptPath }, 0);
        if (bytes.empty())
        {
            return {};
        }
        const std::wstring wide = Utf8ToWide(bytes);
        std::wstring planFile;
        size_t start = 0;
        for (size_t i = 0; i <= wide.size(); ++i)
        {
            if (i < wide.size() && wide[i] != L'\n')
            {
                continue;
            }
            std::wstring_view line(wide.data() + start, i - start);
            start = i + 1;
            while (!line.empty() && line.back() == L'\r')
            {
                line.remove_suffix(1);
            }
            if (line.empty())
            {
                continue;
            }
            const auto parsed = json::Parse(line);
            if (!parsed || parsed->type != json::Value::Type::Obj || parsed->StrAt(L"type") != L"assistant")
            {
                continue;
            }
            const auto* msg = parsed->Find(L"message");
            if (!msg || msg->type != json::Value::Type::Obj)
            {
                continue;
            }
            const auto* c = msg->Find(L"content");
            if (!c || c->type != json::Value::Type::Arr)
            {
                continue;
            }
            for (const auto& blk : c->arr)
            {
                if (blk.type != json::Value::Type::Obj || blk.StrAt(L"type") != L"tool_use" || blk.StrAt(L"name") != L"Write")
                {
                    continue;
                }
                if (const auto* input = blk.Find(L"input"); input && input->type == json::Value::Type::Obj)
                {
                    const std::wstring fp = input->StrAt(L"file_path");
                    if (!fp.empty() && SeIsPlansPath(fp))
                    {
                        planFile = fp; // keep the LAST
                    }
                }
            }
        }
        return planFile;
    }

    // ===== Codex turn-state (Phase C2): rollout-tail -> Running / Waiting / Idle ===============

    CodexBoundary ClassifyCodexLine(std::wstring_view jsonLine)
    {
        CodexBoundary b;
        while (!jsonLine.empty() && (jsonLine.back() == L'\r' || jsonLine.back() == L'\n'))
        {
            jsonLine.remove_suffix(1);
        }
        if (jsonLine.empty())
        {
            return b;
        }
        const auto parsed = json::Parse(jsonLine);
        if (!parsed || parsed->type != json::Value::Type::Obj)
        {
            return b;
        }
        const auto& obj = *parsed;
        if (obj.StrAt(L"type") != L"event_msg") // only event_msg payloads carry the turn lifecycle
        {
            return b;
        }
        const auto* pl = obj.Find(L"payload");
        if (!pl || pl->type != json::Value::Type::Obj)
        {
            return b;
        }
        const std::wstring pt = pl->StrAt(L"type");
        if (pt == L"task_started")
        {
            b.isBoundary = true;
            b.state = CodexState::Running; // a turn opened -> the agent is working
        }
        else if (pt == L"task_complete")
        {
            b.isBoundary = true;
            b.state = CodexState::Waiting; // turn done -> waiting for the user
            b.lastAgentMessage = pl->StrAt(L"last_agent_message");
        }
        else if (pt == L"turn_aborted" || pt == L"thread_rolled_back")
        {
            b.isBoundary = true;
            b.state = CodexState::Waiting; // user-interrupted / rolled back -> the turn ended; waiting
        }
        return b;
    }

    CodexState ReadCodexStateDelta(std::wstring_view rolloutPath, int64_t& offsetInOut, CodexState prior, std::wstring* lastAgentMessageOut)
    {
        if (rolloutPath.empty())
        {
            return prior;
        }
        // The last bytes that always contain the most-recent boundary (task_complete ends every
        // at-rest session; turns are frequent enough that ~1 MiB covers several on the heaviest
        // sessions measured). First sight / fall-behind seeks here; a steady forward delta is tiny.
        constexpr int64_t kCodexTailWindowBytes = 1 << 20; // 1 MiB
        constexpr int64_t kCodexMaxCatchupBytes = 4 << 20; // a forward delta beyond this -> tail-seek instead

        const std::wstring path{ rolloutPath };
        const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return prior;
        }
        LARGE_INTEGER szli{};
        if (!::GetFileSizeEx(h, &szli) || szli.QuadPart <= 0)
        {
            ::CloseHandle(h);
            return prior;
        }
        const int64_t size = szli.QuadPart;

        int64_t start = offsetInOut;
        bool skipPartialLead = false;
        if (start <= 0 || start > size || (size - start) > kCodexMaxCatchupBytes)
        {
            start = (size > kCodexTailWindowBytes) ? (size - kCodexTailWindowBytes) : 0;
            skipPartialLead = (start > 0); // a tail seek lands mid-line — drop the partial leading line
        }
        if (start >= size) // nothing new since the last read
        {
            ::CloseHandle(h);
            return prior;
        }

        LARGE_INTEGER mv{};
        mv.QuadPart = start;
        if (!::SetFilePointerEx(h, mv, nullptr, FILE_BEGIN))
        {
            ::CloseHandle(h);
            return prior;
        }
        std::string buf(static_cast<size_t>(size - start), '\0');
        size_t got = 0;
        while (got < buf.size())
        {
            DWORD rd = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(buf.size() - got, 1u << 20));
            if (!::ReadFile(h, buf.data() + got, chunk, &rd, nullptr) || rd == 0)
            {
                break;
            }
            got += rd;
        }
        buf.resize(got);
        ::CloseHandle(h);

        const size_t lastNl = buf.rfind('\n');
        if (lastNl == std::string::npos)
        {
            return prior; // no complete line yet (a partial append, or a line longer than the window) — don't advance
        }
        size_t lineStart = 0;
        if (skipPartialLead)
        {
            const size_t firstNl = buf.find('\n');
            lineStart = (firstNl == std::string::npos) ? buf.size() : firstNl + 1;
        }

        CodexState result = prior;
        bool saw = false;
        std::wstring lastMsg;
        size_t ls = lineStart;
        for (size_t i = lineStart; i <= lastNl; ++i)
        {
            if (buf[i] != '\n')
            {
                continue;
            }
            if (i > ls)
            {
                const CodexBoundary b = ClassifyCodexLine(Utf8ToWide(buf.substr(ls, i - ls)));
                if (b.isBoundary)
                {
                    result = b.state;
                    saw = true;
                    if (!b.lastAgentMessage.empty())
                    {
                        lastMsg = b.lastAgentMessage;
                    }
                }
            }
            ls = i + 1;
        }
        offsetInOut = start + static_cast<int64_t>(lastNl) + 1; // byte cursor past the last consumed newline
        if (saw && lastAgentMessageOut && !lastMsg.empty())
        {
            *lastAgentMessageOut = lastMsg;
        }
        return saw ? result : prior;
    }
}
