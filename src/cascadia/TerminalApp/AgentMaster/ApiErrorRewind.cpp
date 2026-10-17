// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the API-ERROR REWIND (HOOKS.md "API-error turns"). See ApiErrorRewind.h for the
// incident, Claude Code's resume rule this works with, and the timing rule it must keep. Plain C++ +
// Win32 (no WinRT, no PCH): TerminalAppLib and the m5 harness compile this same file.

#include "ApiErrorRewind.h"

#include "ClaudeSpawn.h" // ResolveClaudeTranscriptPath, ShortId, LogSwallowedException
#include "Json.h"
#include "TranscriptStore.h" // ReadSessionPresence, PresenceRowMatchesProcess

#include <windows.h>

#include <algorithm>
#include <climits> // INT_MAX (the Win32 UTF conversions take int lengths)
#include <unordered_map>
#include <unordered_set>

namespace Agentmaster
{
    namespace
    {
        // Claude Code's own continuation prompt (2.1.291: `Hie`). Its resume path appends it as a META user
        // row before it re-sends an interrupted turn, so a resume that died AGAIN leaves it between the real
        // tool result and the new error. It is Claude Code's text, not the user's: the walk steps over it
        // (otherwise the rewind would land ON it, and Claude Code never continues a turn whose tail is a meta
        // row). The CLAUDE_CODE_RESUME_PROMPT override is not followed (not set on this machine).
        constexpr std::wstring_view kClaudeResumePrompt = L"Continue from where you left off.";

        // The byte pattern every API-error row carries (Claude Code writes compact JSON). A window without it
        // cannot end in an API error, which keeps the common case — a restart of a healthy session — at one
        // read and one scan, with no JSON parsing at all.
        constexpr std::string_view kApiErrorNeedle = "\"isApiErrorMessage\":true";

        // Read windows, in order: most error tails fit the first; a huge tool result between the error and
        // its user row needs the second; the third is the whole file, capped so a pathological transcript
        // cannot make one restart read gigabytes (the planner then reports NeedMoreHistory as Skipped).
        constexpr uint64_t kReadWindows[] = { 256ull * 1024, 16ull * 1024 * 1024, 256ull * 1024 * 1024 };

        // UTF-8 -> UTF-16. Claude transcripts are UTF-8 JSON lines. Empty on empty input or a conversion
        // failure (invalid sequences are replaced, so a failure means only an absurd length).
        std::wstring Utf8ToWide(std::string_view s)
        {
            if (s.empty() || s.size() > static_cast<size_t>(INT_MAX))
            {
                return {};
            }
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            if (n <= 0)
            {
                return {};
            }
            std::wstring w(static_cast<size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }

        // UTF-16 -> UTF-8 for the marker line. A lone surrogate (only possible in the copied lastPrompt
        // display text) becomes U+FFFD, which keeps the line valid JSON.
        std::string WideToUtf8(std::wstring_view s)
        {
            if (s.empty() || s.size() > static_cast<size_t>(INT_MAX))
            {
                return {};
            }
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
            if (n <= 0)
            {
                return {};
            }
            std::string out(static_cast<size_t>(n), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
            return out;
        }

        // FILETIME -> Unix ms, truncating exactly like ProcessInspect's FileTimeToUnixMs (whose copy is
        // private to the ProcessInspect TUs). It must match bit for bit: PresenceRowMatchesProcess compares
        // the result with the heartbeat's procStart, decoded by that same truncation, within 1 ms.
        int64_t FileTimeToUnixMsLocal(const FILETIME& ft) noexcept
        {
            ULARGE_INTEGER u;
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;
            constexpr uint64_t kEpochDiff = 116444736000000000ull; // 1601-01-01 -> 1970-01-01 in 100 ns
            if (u.QuadPart < kEpochDiff)
            {
                return 0;
            }
            return static_cast<int64_t>((u.QuadPart - kEpochDiff) / 10000ull);
        }

        // Session ids are lowercase GUIDs everywhere Claude Code writes them, but the registry and the
        // heartbeat are separate producers: compare ASCII case-insensitively so a case difference can never
        // hide a live holder (a missed holder is the one failure here that loses the rewind).
        bool SessionIdEq(std::wstring_view a, std::wstring_view b) noexcept
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i)
            {
                wchar_t x = a[i], y = b[i];
                if (x >= L'A' && x <= L'Z')
                {
                    x = static_cast<wchar_t>(x - L'A' + L'a');
                }
                if (y >= L'A' && y <= L'Z')
                {
                    y = static_cast<wchar_t>(y - L'A' + L'a');
                }
                if (x != y)
                {
                    return false;
                }
            }
            return true;
        }

        // The text of a message's content: a plain string, or its text blocks concatenated.
        std::wstring ContentText(const json::Value* content)
        {
            if (!content)
            {
                return {};
            }
            if (content->type == json::Value::Type::Str)
            {
                return content->str;
            }
            std::wstring text;
            if (content->type == json::Value::Type::Arr)
            {
                for (const auto& blk : content->arr)
                {
                    if (blk.type == json::Value::Type::Obj && blk.StrAt(L"type") == L"text")
                    {
                        text += blk.StrAt(L"text");
                    }
                }
            }
            return text;
        }

        // The first line of `s`, trimmed, at most `cap` characters (an ellipsis marks a cut). Log text only.
        std::wstring FirstLineCapped(std::wstring_view s, size_t cap)
        {
            size_t b = 0;
            while (b < s.size() && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n'))
            {
                ++b;
            }
            size_t e = s.find_first_of(L"\r\n", b);
            if (e == std::wstring_view::npos)
            {
                e = s.size();
            }
            std::wstring line{ s.substr(b, e - b) };
            if (line.size() > cap)
            {
                line.resize(cap);
                line += L"\x2026";
            }
            return line;
        }

        // One transcript row (a user / assistant / system / attachment / progress line with a uuid), reduced
        // to the facts the planner reads. `order` is the line's index in the window (file order).
        struct Row
        {
            size_t order{ 0 };
            std::wstring type;
            std::wstring uuid;
            std::wstring parent; // "" for a root (null / absent parentUuid)
            std::wstring sessionId;
            std::wstring timestamp;
            std::wstring messageId; // assistant: message.id — the rows of ONE API response share it
            std::wstring text; // user: the content text; assistant: its text blocks
            std::wstring errorKind; // the API-error row's `error` field
            bool apiError{ false }; // top-level isApiErrorMessage
            bool refusalClass{ false }; // an API-error row Claude Code counts as COMPLETE (refusal / DLP / safety block)
            bool isMeta{ false };
            bool compactSummary{ false };
            bool toolResult{ false }; // user: a tool_result block first, or a toolUseResult payload
            bool forkBriefing{ false }; // attachment of type fork_briefing — never the loader's "last row"
        };

        bool IsBookkeepingType(std::wstring_view type) noexcept
        {
            return type == L"system" || type == L"attachment" || type == L"progress";
        }

        ApiErrorRewindPlan Verdict(ApiErrorRewindPlan plan, ApiErrorRewindVerdict v, std::wstring reason)
        {
            plan.verdict = v;
            plan.reason = std::move(reason);
            return plan;
        }

        ApiErrorRewindPlan PlanImpl(std::string_view tailUtf8, bool windowIsWholeFile, std::wstring_view sessionId)
        {
            ApiErrorRewindPlan plan;
            if (tailUtf8.empty())
            {
                return plan; // an empty transcript ends in nothing
            }
            // The fast exit FIRST: a window with no API-error row at all cannot end in one, whatever else
            // it holds — and that is every healthy session's restart. (Checked before the torn-tail test so
            // a healthy session caught mid-write is not reported as "torn" by a feature it never needed.)
            if (tailUtf8.find(kApiErrorNeedle) == std::string_view::npos)
            {
                return plan;
            }
            // Claude Code ends every line with "\n" (append-only JSONL). A last byte that is not one means a
            // writer is in the middle of a line, or died there: appending would glue the marker onto it.
            if (tailUtf8.back() != '\n')
            {
                return Verdict(std::move(plan), ApiErrorRewindVerdict::TornTail, L"the transcript does not end with a newline (a write is in progress, or one was cut off)");
            }

            const std::wstring text = Utf8ToWide(tailUtf8);
            std::vector<std::wstring_view> lines;
            for (size_t start = 0;;)
            {
                const size_t nl = text.find(L'\n', start);
                if (nl == std::wstring::npos)
                {
                    break;
                }
                std::wstring_view line{ text.data() + start, nl - start };
                while (!line.empty() && line.back() == L'\r')
                {
                    line.remove_suffix(1);
                }
                lines.push_back(line);
                start = nl + 1;
            }
            // A window that does not start at byte 0 may begin mid-line: drop that first line (when it was
            // whole after all, the cost is one row the walk can still ask for with a larger window).
            if (!windowIsWholeFile && !lines.empty())
            {
                lines.erase(lines.begin());
            }

            std::vector<Row> rows;
            std::unordered_map<std::wstring, size_t> byUuid; // uuid -> index in rows; LAST occurrence wins, as in Claude Code's loader map
            std::wstring lastPromptText;
            bool haveLastPrompt = false;
            // Claude Code's loader state for the active leaf, replayed in file order (ApiErrorRewind.h):
            //   leafMarker   — Yn: the last last-prompt leafUuid (a null-explicit "cleared" marker empties it);
            //   explicitLeaf — io: that marker was explicit (or re-stamped the same leaf right after an
            //                  explicit one); ANY later transcript row resets it;
            //   cleared      — bo: a null-explicit marker with no leaf marker or row after it (an empty
            //                  conversation on load).
            std::wstring leafMarker;
            size_t leafMarkerOrder = 0;
            bool explicitLeaf = false;
            bool cleared = false;

            for (size_t i = 0; i < lines.size(); ++i)
            {
                const auto& line = lines[i];
                if (line.empty())
                {
                    continue;
                }
                const auto parsed = json::Parse(line);
                if (!parsed || parsed->type != json::Value::Type::Obj)
                {
                    continue; // not a JSON object line — Claude Code's loader skips it too
                }
                const auto& o = *parsed;
                const std::wstring type = o.StrAt(L"type");
                if (type == L"last-prompt")
                {
                    if (const auto* lp = o.Find(L"lastPrompt"); lp && lp->type == json::Value::Type::Str)
                    {
                        lastPromptText = lp->str; // the newest display text, copied into our marker
                        haveLastPrompt = true;
                    }
                    const auto* leaf = o.Find(L"leafUuid");
                    if (!leaf)
                    {
                        continue; // a marker without leafUuid carries no leaf (it moves nothing)
                    }
                    if (leaf->type == json::Value::Type::Str && !leaf->str.empty())
                    {
                        explicitLeaf = o.BoolAt(L"explicit") || (explicitLeaf && leaf->str == leafMarker);
                        leafMarker = leaf->str;
                        leafMarkerOrder = i;
                        cleared = false;
                    }
                    else if (leaf->type == json::Value::Type::Null && o.BoolAt(L"explicit"))
                    {
                        leafMarker.clear();
                        explicitLeaf = false;
                        cleared = true;
                    }
                    continue;
                }
                if (type != L"user" && type != L"assistant" && !IsBookkeepingType(type))
                {
                    continue; // cost-state, mode, file-history-*, queue-operation, … carry no conversation
                }
                if (o.BoolAt(L"isSidechain"))
                {
                    continue; // a subagent's row: never on the main branch
                }
                Row r;
                r.uuid = o.StrAt(L"uuid");
                if (r.uuid.empty())
                {
                    continue;
                }
                r.order = i;
                r.type = type;
                r.parent = o.StrAt(L"parentUuid");
                r.sessionId = o.StrAt(L"sessionId");
                r.timestamp = o.StrAt(L"timestamp");
                r.isMeta = o.BoolAt(L"isMeta");
                r.compactSummary = o.BoolAt(L"isCompactSummary");
                if (type == L"attachment")
                {
                    if (const auto* att = o.Find(L"attachment"); att && att->type == json::Value::Type::Obj)
                    {
                        r.forkBriefing = att->StrAt(L"type") == L"fork_briefing";
                    }
                }
                const auto* msg = o.Find(L"message");
                const json::Value* content = (msg && msg->type == json::Value::Type::Obj) ? msg->Find(L"content") : nullptr;
                if (type == L"assistant")
                {
                    r.apiError = o.BoolAt(L"isApiErrorMessage");
                    r.errorKind = o.StrAt(L"error");
                    if (msg && msg->type == json::Value::Type::Obj)
                    {
                        r.messageId = msg->StrAt(L"id");
                        // Claude Code 2.1.291's xfo(): these API-error rows are a COMPLETED turn, never a
                        // transient failure — its classifier does not skip them, and neither do we.
                        const std::wstring apiErr = o.StrAt(L"apiError");
                        r.refusalClass = r.apiError &&
                                         (msg->StrAt(L"stop_reason") == L"refusal" ||
                                          apiErr == L"dlp_request_denied" ||
                                          apiErr == L"safety_monitor_blocked");
                    }
                    r.text = ContentText(content);
                }
                else if (type == L"user")
                {
                    r.text = ContentText(content);
                    if (content && content->type == json::Value::Type::Arr && !content->arr.empty() &&
                        content->arr.front().type == json::Value::Type::Obj &&
                        content->arr.front().StrAt(L"type") == L"tool_result")
                    {
                        r.toolResult = true;
                    }
                    if (const auto* tur = o.Find(L"toolUseResult"); tur && tur->type != json::Value::Type::Null)
                    {
                        r.toolResult = true; // Claude Code's t$(): a toolUseResult payload marks a tool turn too
                    }
                }
                if (!r.forkBriefing)
                {
                    explicitLeaf = false; // a later transcript row ends an explicit marker's hold (the loader's io=false)
                    cleared = false;
                }
                byUuid[r.uuid] = rows.size();
                rows.push_back(std::move(r));
            }

            const Row* lastRow = nullptr; // the loader's `wn`: the last main-branch row, a fork briefing excepted
            for (auto it = rows.rbegin(); it != rows.rend(); ++it)
            {
                if (!it->forkBriefing)
                {
                    lastRow = &*it;
                    break;
                }
            }
            if (!lastRow)
            {
                return Verdict(std::move(plan), ApiErrorRewindVerdict::NotApplicable, L"no conversation rows in the bytes read");
            }
            if (cleared)
            {
                return Verdict(std::move(plan), ApiErrorRewindVerdict::NotApplicable, L"the conversation was cleared to empty");
            }

            // Does `row` descend from `ancestorUuid` (Claude Code's FCn with direct=true)? Bounded walk: a
            // parent cycle or a parent outside the window ends it with "no".
            const auto descendsFrom = [&](const Row* row, const std::wstring& ancestorUuid) {
                for (size_t guard = 0; row && guard <= rows.size(); ++guard)
                {
                    if (row->uuid == ancestorUuid)
                    {
                        return true;
                    }
                    const auto it = byUuid.find(row->parent);
                    row = (it == byUuid.end()) ? nullptr : &rows[it->second];
                }
                return false;
            };

            // The active leaf, as Claude Code's loader picks it (see ApiErrorRewind.h).
            std::wstring leaf = lastRow->uuid;
            if (!leafMarker.empty())
            {
                const auto it = byUuid.find(leafMarker);
                if (it == byUuid.end())
                {
                    if (!windowIsWholeFile)
                    {
                        return Verdict(std::move(plan), ApiErrorRewindVerdict::NeedMoreHistory, L"the active-leaf marker names a row older than the bytes read");
                    }
                    // Not in the whole file either: the loader falls back to the last row (leaf stays).
                }
                else
                {
                    const bool explicitAtEnd = explicitLeaf && lastRow->order < leafMarkerOrder;
                    if (explicitAtEnd || lastRow->uuid == leafMarker || !descendsFrom(lastRow, leafMarker))
                    {
                        leaf = leafMarker;
                    }
                    // else: the last row continues the marked leaf, so the loader takes the last row.
                }
            }

            // Up through system / attachment / progress rows to the leaf's MESSAGE (the first user / assistant).
            // `leaf` is always a key of byUuid here (the last row, or a marker leaf found above).
            const Row* e = &rows[byUuid.at(leaf)];
            for (size_t guard = 0; IsBookkeepingType(e->type); ++guard)
            {
                const auto it = byUuid.find(e->parent);
                if (e->parent.empty() || guard > rows.size())
                {
                    return Verdict(std::move(plan), ApiErrorRewindVerdict::NotApplicable, L"no message on the active branch");
                }
                if (it == byUuid.end())
                {
                    return Verdict(std::move(plan), windowIsWholeFile ? ApiErrorRewindVerdict::NotApplicable : ApiErrorRewindVerdict::NeedMoreHistory, L"the active branch leaves the bytes read before a message");
                }
                e = &rows[it->second];
            }
            if (e->type != L"assistant" || !e->apiError)
            {
                return plan; // NotApplicable: the active branch ends in a real message (the usual case)
            }
            plan.errorUuid = e->uuid;
            plan.errorText = FirstLineCapped(e->text, 160);
            plan.errorKind = e->errorKind;
            plan.errorTimestamp = e->timestamp;
            if (e->refusalClass)
            {
                return Verdict(std::move(plan), ApiErrorRewindVerdict::RefusalError, L"a refusal / DLP / safety block is not a transient failure \x2014 it is never resent");
            }
            for (const auto& r : rows)
            {
                if (r.order > e->order)
                {
                    if (r.type == L"user" || r.type == L"assistant")
                    {
                        return Verdict(std::move(plan), ApiErrorRewindVerdict::NotApplicable, L"the conversation went on after the error");
                    }
                    ++plan.rowsAfterError;
                }
            }

            // Up from the error to the user row below the failed response, stepping over that response's own
            // rows (one message id), bookkeeping rows, earlier error rows of the same failure, and Claude
            // Code's own "Continue from where you left off." rows.
            std::wstring partialMessageId;
            bool sawPartial = false;
            const Row* cur = e;
            const Row* target = nullptr;
            for (size_t guard = 0; guard <= rows.size() && !target; ++guard)
            {
                if (cur->parent.empty())
                {
                    return Verdict(std::move(plan), ApiErrorRewindVerdict::UnknownShape, L"no user row above the error");
                }
                const auto it = byUuid.find(cur->parent);
                if (it == byUuid.end())
                {
                    return Verdict(std::move(plan), windowIsWholeFile ? ApiErrorRewindVerdict::UnknownShape : ApiErrorRewindVerdict::NeedMoreHistory, L"the rows above the error leave the bytes read before a user row");
                }
                cur = &rows[it->second];
                if (cur->type == L"assistant")
                {
                    if (cur->apiError)
                    {
                        continue; // an earlier error row of the same failure
                    }
                    if (sawPartial && !partialMessageId.empty() && !cur->messageId.empty() && cur->messageId != partialMessageId)
                    {
                        return Verdict(std::move(plan), ApiErrorRewindVerdict::UnknownShape, L"two different responses sit above the error with no user row between them");
                    }
                    if (!sawPartial || partialMessageId.empty())
                    {
                        partialMessageId = cur->messageId;
                    }
                    sawPartial = true;
                    ++plan.partialRows;
                    continue;
                }
                if (cur->type == L"user")
                {
                    if (cur->isMeta && cur->text == kClaudeResumePrompt)
                    {
                        ++plan.resumePromptRows; // a resume that died again: rewind past Claude Code's own continuation too
                        continue;
                    }
                    target = cur;
                }
                // system / attachment / progress between the user row and the response: stepped over
            }
            if (!target)
            {
                return Verdict(std::move(plan), ApiErrorRewindVerdict::UnknownShape, L"the walk above the error did not end (a parent cycle)");
            }

            plan.targetUuid = target->uuid;
            plan.targetKind = target->toolResult ? L"tool result" : (target->compactSummary ? L"compact summary" : (target->isMeta ? L"meta" : L"prompt"));

            // The marker: Claude Code's own rewind line (its key order: type, lastPrompt, leafUuid, explicit,
            // sessionId), WITHOUT "rewound" — that flag would make the next resume treat the leaf as a
            // user's deliberate rewind and refuse to continue the turn.
            auto marker = json::Value::MkObj();
            marker.Set(L"type", json::Value::MkStr(L"last-prompt"));
            if (haveLastPrompt)
            {
                marker.Set(L"lastPrompt", json::Value::MkStr(lastPromptText));
            }
            marker.Set(L"leafUuid", json::Value::MkStr(target->uuid));
            marker.Set(L"explicit", json::Value::MkBool(true));
            const std::wstring sid = !sessionId.empty() ? std::wstring{ sessionId } : e->sessionId;
            if (!sid.empty())
            {
                marker.Set(L"sessionId", json::Value::MkStr(sid));
            }
            plan.markerLine = WideToUtf8(json::Dump(marker));
            if (plan.markerLine.empty())
            {
                return Verdict(std::move(plan), ApiErrorRewindVerdict::UnknownShape, L"the marker line could not be encoded");
            }
            plan.verdict = ApiErrorRewindVerdict::Rewind;
            return plan;
        }

        // Read the last `window` bytes of `path` (all of it when smaller). `fileSize` is the size at the
        // read — the append later refuses unless the file still has exactly that size. Returns false when
        // the file cannot be opened or read.
        bool ReadTail(const std::wstring& path, uint64_t window, std::string& out, uint64_t& fileSize, bool& wholeFile)
        {
            const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            LARGE_INTEGER sz{};
            if (!::GetFileSizeEx(h, &sz))
            {
                ::CloseHandle(h);
                return false;
            }
            fileSize = static_cast<uint64_t>(sz.QuadPart);
            const uint64_t start = fileSize > window ? fileSize - window : 0;
            wholeFile = start == 0;
            LARGE_INTEGER li{};
            li.QuadPart = static_cast<LONGLONG>(start);
            if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
            {
                ::CloseHandle(h);
                return false;
            }
            out.assign(static_cast<size_t>(fileSize - start), '\0');
            size_t got = 0;
            while (got < out.size())
            {
                DWORD n = 0;
                const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(out.size() - got, 1u << 20));
                if (!::ReadFile(h, out.data() + got, chunk, &n, nullptr) || n == 0)
                {
                    break;
                }
                got += n;
            }
            ::CloseHandle(h);
            // A short read means the file shrank under us; plan on what was read — the size check before
            // the append then refuses (the file is no longer what it was).
            out.resize(got);
            return true;
        }

        // Read + plan with the widening windows. `fileSize` is the size of the LAST read.
        ApiErrorRewindPlan ReadAndPlan(const std::wstring& path, std::wstring_view sessionId, uint64_t& fileSize, bool& readOk)
        {
            ApiErrorRewindPlan plan;
            readOk = true;
            for (const uint64_t window : kReadWindows)
            {
                std::string bytes;
                bool whole = false;
                if (!ReadTail(path, window, bytes, fileSize, whole))
                {
                    readOk = false;
                    return plan;
                }
                plan = PlanApiErrorRewind(bytes, whole, sessionId);
                if (plan.verdict != ApiErrorRewindVerdict::NeedMoreHistory || whole)
                {
                    break;
                }
            }
            return plan;
        }

        std::wstring JoinPids(const std::vector<uint32_t>& pids)
        {
            std::wstring s;
            for (const auto pid : pids)
            {
                if (!s.empty())
                {
                    s += L", ";
                }
                s += std::to_wstring(pid);
            }
            return s;
        }
    }

    ApiErrorRewindPlan PlanApiErrorRewind(std::string_view tailUtf8, bool windowIsWholeFile, std::wstring_view sessionId) noexcept
    {
        try
        {
            return PlanImpl(tailUtf8, windowIsWholeFile, sessionId);
        }
        catch (...)
        {
            // A planner failure must never cost the restart: report "nothing to do" and keep the trace
            // (Rule #18). Allocation failures on a huge window are the realistic cause.
            LogSwallowedException(L"PlanApiErrorRewind");
            ApiErrorRewindPlan plan;
            plan.reason = L"planner failed";
            return plan;
        }
    }

    ApiErrorRewindResult RewindApiErrorTail(const std::wstring& transcriptPath, std::wstring_view sessionId) noexcept
    {
        ApiErrorRewindResult r;
        try
        {
            uint64_t sizeAtRead = 0;
            bool readOk = true;
            r.plan = ReadAndPlan(transcriptPath, sessionId, sizeAtRead, readOk);
            if (!readOk)
            {
                r.outcome = ApiErrorRewindOutcome::Failed;
                r.detail = L"the transcript could not be read (error " + std::to_wstring(::GetLastError()) + L")";
                return r;
            }
            if (r.plan.verdict == ApiErrorRewindVerdict::NotApplicable)
            {
                return r; // NotNeeded
            }
            if (r.plan.verdict != ApiErrorRewindVerdict::Rewind)
            {
                r.outcome = ApiErrorRewindOutcome::Skipped;
                r.detail = r.plan.reason;
                return r;
            }
            // Nobody may hold the conversation: a live claude that ran the failed turn re-stamps the error
            // branch as its leaf at exit and would silently undo the line (ApiErrorRewind.h, TIMING). The
            // seams that end a claude wait for its exit first; this is the belt for every caller.
            if (const auto holders = LiveClaudeProcessesForSession(sessionId); !holders.empty())
            {
                r.outcome = ApiErrorRewindOutcome::Skipped;
                r.detail = L"claude pid " + JoinPids(holders) + L" still runs this conversation (its exit would put the error back as the active leaf)";
                return r;
            }

            // FILE_APPEND_DATA without FILE_WRITE_DATA: every write lands at the end of the file whatever
            // the file pointer — the same append-only write claude itself makes. FILE_READ_ATTRIBUTES is for
            // the size check below.
            const HANDLE h = ::CreateFileW(transcriptPath.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                r.outcome = ApiErrorRewindOutcome::Failed;
                r.detail = L"the transcript could not be opened for append (error " + std::to_wstring(::GetLastError()) + L")";
                return r;
            }
            LARGE_INTEGER sz{};
            const bool sized = ::GetFileSizeEx(h, &sz) != FALSE;
            if (!sized || static_cast<uint64_t>(sz.QuadPart) != sizeAtRead)
            {
                // The file changed between the read and now (a writer the holder check could not see, e.g.
                // a claude that started this very moment): the plan no longer describes the tail.
                ::CloseHandle(h);
                r.outcome = ApiErrorRewindOutcome::Skipped;
                r.detail = sized ? (L"the transcript changed while it was planned (" + std::to_wstring(sizeAtRead) + L" -> " + std::to_wstring(sz.QuadPart) + L" bytes)") :
                                   L"the transcript's size could not be read before the append";
                return r;
            }
            const std::string line = r.plan.markerLine + "\n";
            DWORD written = 0;
            const BOOL ok = ::WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            const DWORD writeError = ok ? 0 : ::GetLastError();
            ::CloseHandle(h);
            if (!ok || written != line.size())
            {
                // A short write leaves a torn last line: Claude Code's loader skips a line it cannot parse,
                // and the next plan here reports TornTail instead of appending onto it.
                r.outcome = ApiErrorRewindOutcome::Failed;
                r.detail = L"the append failed (error " + std::to_wstring(writeError) + L", " + std::to_wstring(written) + L" of " + std::to_wstring(line.size()) + L" bytes written)";
                return r;
            }
            r.outcome = ApiErrorRewindOutcome::Appended;
            r.appendedAtByte = sizeAtRead;
        }
        catch (...)
        {
            LogSwallowedException(L"RewindApiErrorTail");
            r.outcome = ApiErrorRewindOutcome::Failed;
            r.detail = L"an exception was thrown (see the [exc] line above)";
        }
        return r;
    }

    ApiErrorRewindResult RewindApiErrorTailForSession(std::wstring_view sessionId) noexcept
    {
        try
        {
            const std::wstring path = ResolveClaudeTranscriptPath(sessionId);
            if (path.empty())
            {
                return {}; // NotNeeded: never prompted, nothing to resume
            }
            return RewindApiErrorTail(path, sessionId);
        }
        catch (...)
        {
            LogSwallowedException(L"RewindApiErrorTailForSession");
            ApiErrorRewindResult r;
            r.outcome = ApiErrorRewindOutcome::Failed;
            r.detail = L"an exception was thrown (see the [exc] line above)";
            return r;
        }
    }

    ApiErrorRewindPlan PlanApiErrorRewindForSession(std::wstring_view sessionId) noexcept
    {
        try
        {
            const std::wstring path = ResolveClaudeTranscriptPath(sessionId);
            if (path.empty())
            {
                return {};
            }
            uint64_t size = 0;
            bool readOk = true;
            auto plan = ReadAndPlan(path, sessionId, size, readOk);
            return readOk ? plan : ApiErrorRewindPlan{};
        }
        catch (...)
        {
            LogSwallowedException(L"PlanApiErrorRewindForSession");
            return {};
        }
    }

    std::wstring DescribeApiErrorRewind(const ApiErrorRewindResult& r, std::wstring_view sessionId, std::wstring_view seam)
    {
        std::wstring s = L"[api-error-rewind] " + ShortId(std::wstring{ sessionId }) + L" " + std::wstring{ seam } + L": ";
        const auto& p = r.plan;
        std::wstring error;
        if (!p.errorUuid.empty())
        {
            error = L"\"" + p.errorText + L"\" (" + (p.errorKind.empty() ? std::wstring{ L"no kind" } : p.errorKind) + L", " + p.errorTimestamp + L")";
        }
        switch (r.outcome)
        {
        case ApiErrorRewindOutcome::Appended:
            s += L"rewound past " + error + L" \x2014 " + std::to_wstring(p.partialRows) + L" half-response row(s), " +
                 std::to_wstring(p.resumePromptRows) + L" earlier continue row(s), " + std::to_wstring(p.rowsAfterError) +
                 L" row(s) after it \x2014 to " + ShortId(p.targetUuid) + L" (" + p.targetKind + L"); line at byte " + std::to_wstring(r.appendedAtByte) +
                 ((p.targetKind == L"tool result" || p.targetKind == L"prompt") ? L"; the resumed claude continues the turn" : L"; Claude Code will NOT continue a turn that stops at this row \x2014 type to go on");
            break;
        case ApiErrorRewindOutcome::NotNeeded:
            s += L"no API error at the end of the conversation";
            break;
        case ApiErrorRewindOutcome::Skipped:
            s += L"NOT rewound \x2014 " + r.detail + (error.empty() ? std::wstring{} : (L" " + error));
            break;
        case ApiErrorRewindOutcome::Failed:
            s += L"FAILED \x2014 " + r.detail + (error.empty() ? std::wstring{} : (L" " + error));
            break;
        }
        return s;
    }

    ProcessExitWait::~ProcessExitWait()
    {
        _CloseAll();
    }

    ProcessExitWait::ProcessExitWait(ProcessExitWait&& other) noexcept :
        _procs(std::move(other._procs))
    {
        other._procs.clear();
    }

    ProcessExitWait& ProcessExitWait::operator=(ProcessExitWait&& other) noexcept
    {
        if (this != &other)
        {
            _CloseAll();
            _procs = std::move(other._procs);
            other._procs.clear();
        }
        return *this;
    }

    void ProcessExitWait::_CloseAll() noexcept
    {
        for (auto& [pid, h] : _procs)
        {
            if (h)
            {
                ::CloseHandle(static_cast<HANDLE>(h));
            }
        }
        _procs.clear();
    }

    ProcessExitWait ProcessExitWait::ForSession(std::wstring_view sessionId) noexcept
    {
        ProcessExitWait w;
        if (sessionId.empty())
        {
            return w;
        }
        try
        {
            std::unordered_set<uint32_t> seen;
            for (const auto& row : ReadSessionPresence())
            {
                if (row.pid == 0 || !SessionIdEq(row.sessionId, sessionId) || !seen.insert(row.pid).second)
                {
                    continue;
                }
                // SYNCHRONIZE for the wait, limited query for the creation time. A process we may not open
                // (gone already, or elevated beyond our reach) cannot be waited on and is left out.
                const HANDLE h = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, row.pid);
                if (!h)
                {
                    continue;
                }
                if (::WaitForSingleObject(h, 0) != WAIT_TIMEOUT)
                {
                    ::CloseHandle(h); // exited already (its heartbeat file lingers after a crash)
                    continue;
                }
                // The PID-reuse guard, on the HANDLE we will wait on: the heartbeat must be this process's
                // (procStart == its creation time), else a recycled pid would make us wait on a stranger.
                FILETIME created{}, exited{}, kernel{}, user{};
                const int64_t startMs = ::GetProcessTimes(h, &created, &exited, &kernel, &user) ? FileTimeToUnixMsLocal(created) : 0;
                if (!PresenceRowMatchesProcess(row, startMs))
                {
                    ::CloseHandle(h);
                    continue;
                }
                w._procs.emplace_back(row.pid, h);
            }
        }
        catch (...)
        {
            LogSwallowedException(L"ProcessExitWait::ForSession");
        }
        return w;
    }

    bool ProcessExitWait::WaitAll(uint32_t timeoutMs) noexcept
    {
        // One deadline for all of them: the processes exit concurrently, so the total wait is the slowest
        // one, never the sum.
        const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
        for (const auto& [pid, h] : _procs)
        {
            const ULONGLONG now = ::GetTickCount64();
            const DWORD left = now >= deadline ? 0 : static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, INFINITE - 1));
            if (::WaitForSingleObject(static_cast<HANDLE>(h), left) != WAIT_OBJECT_0)
            {
                return false;
            }
        }
        return true;
    }

    std::vector<uint32_t> ProcessExitWait::Pids() const
    {
        std::vector<uint32_t> pids;
        pids.reserve(_procs.size());
        for (const auto& [pid, h] : _procs)
        {
            pids.push_back(pid);
        }
        return pids;
    }

    std::vector<uint32_t> LiveClaudeProcessesForSession(std::wstring_view sessionId) noexcept
    {
        try
        {
            return ProcessExitWait::ForSession(sessionId).Pids();
        }
        catch (...)
        {
            LogSwallowedException(L"LiveClaudeProcessesForSession");
            return {};
        }
    }
}
