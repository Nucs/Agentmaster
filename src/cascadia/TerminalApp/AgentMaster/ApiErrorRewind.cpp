// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the API-ERROR REWIND (HOOKS.md "API-error turns"), the USER-INTERRUPT REWIND and the resume
// classifier (HOOKS.md "Interrupted turns"). See ApiErrorRewind.h for the incidents, Claude Code's resume rule
// these work with, and the timing rule they must keep. Plain C++ + Win32 (no WinRT, no PCH): TerminalAppLib and
// the m5 harness compile this same file.
//
// One parse serves all three (ParseBranch): the transcript's rows, Claude Code's active leaf and the message at
// the end of the active branch, exactly as the API-error planner always computed them. Each planner then walks
// from that message in its own way, and both rewinds build the same marker line and append it the same way.

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

        // Claude Code's two USER-interrupt rows (2.1.291: `VS` and `gu`), written as the text of a user row under
        // the reply an Esc cut off. The second one marks an Esc DURING a tool call (its batch's results above it
        // are the "doesn't want to proceed" rejections Claude Code writes for the cut calls).
        constexpr std::wstring_view kInterruptMarker = L"[Request interrupted by user]";
        constexpr std::wstring_view kInterruptMarkerToolUse = L"[Request interrupted by user for tool use]";
        // The bytes both interrupt rows share (no JSON escaping applies to them). A window without them cannot
        // end in a user interrupt: the interrupt planner's fast exit, like kApiErrorNeedle's.
        constexpr std::string_view kInterruptNeedle = "[Request interrupted by user";

        // The start of the tool-result text Claude Code writes for a tool call the user rejected or an Esc cut
        // ("The user doesn't want to proceed with this tool use. …" and "The user doesn't want to take this action
        // right now. …"). Claude Code's classifier never continues a turn whose tail results are ALL such texts
        // (2.1.291's SMt), so neither a rewind nor a fork prediction may count on it.
        constexpr std::wstring_view kRejectedToolResultPrefix = L"The user doesn't want to";

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
            // ---- the interrupt planner's and the resume classifier's facts (HOOKS.md "Interrupted turns") ----
            bool interruptedByShutdown{ false }; // user: Claude Code wrote this interrupt row at a SHUTDOWN (it continues those turns)
            bool toolDenialEndsTurn{ false }; // user tool result: top-level toolDenialEndsTurn (2.1.291's bMt — never continued)
            bool backgroundedByTurnAbort{ false }; // user tool result: toolUseResult.backgroundedByTurnAbort (wMt — never continued)
            bool allResultsRejected{ false }; // user: every tool_result block's text is a rejection (SMt — never continued)
            bool localCommand{ false }; // user: the echo of a local slash command or a `!` shell line (never continued)
            bool queuedTaskNotification{ false }; // user: a queued task notification kept for the transcript only (eV — skipped from the tail)
            bool taskNotification{ false }; // user: origin.kind == "task-notification" (tgn — continued as a turn)
            bool hasToolUse{ false }; // assistant: at least one tool_use block
            bool thinkingOnly{ false }; // assistant: nothing but thinking / redacted_thinking blocks
            std::wstring toolUseName; // assistant: the first tool_use block's name (the fork question names it)
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

        // What ParseBranch found (ApiErrorRewind.cpp's header comment): the rows of the bytes read, Claude Code's
        // active leaf, and the MESSAGE at the end of the active branch — or why there is none.
        struct BranchParse
        {
            // Why the parse stopped. Every planner maps these onto its own verdicts (the API-error planner keeps
            // the reasons it always logged).
            enum class Status
            {
                Ok, // messageIndex / leafIndex name the active branch's message and leaf
                NoRows, // no conversation rows in the bytes read
                Cleared, // the conversation was cleared to empty (a null explicit last-prompt at the end)
                MarkerOutsideWindow, // the active-leaf marker names a row older than the bytes read (window not whole)
                NoMessage, // the active branch has no user / assistant row (a root bookkeeping row, a cycle)
                LeftWindow, // the active branch leaves the bytes read before a message
            };
            Status status{ Status::Ok };
            std::vector<Row> rows; // the main-branch rows of the window, file order
            std::unordered_map<std::wstring, size_t> byUuid; // uuid -> index in rows; LAST occurrence wins, as in Claude Code's loader map
            std::wstring lastPromptText; // the newest last-prompt display text (copied into a marker line)
            bool haveLastPrompt{ false };
            size_t leafIndex{ 0 }; // Ok: the active leaf (Claude Code's loader rule)
            size_t messageIndex{ 0 }; // Ok: the first user / assistant row at or above the leaf
            // Ok: the leaf is the last last-prompt marker's leaf, and that marker said "rewound":true — Claude
            // Code's own rewind (double Esc), after which a resume continues nothing.
            bool leafRewound{ false };
        };

        // Split the window into rows and replay Claude Code's loader: the active leaf, then up through bookkeeping
        // rows to the branch's message. The API-error planner's parse, moved here unchanged so the interrupt
        // planner and the resume classifier see exactly the branch it sees.
        //   tailUtf8          — the bytes read (complete lines; a partial LAST line has no "\n" and is not a line).
        //   windowIsWholeFile — the bytes start at byte 0 (else the first line may be partial and is dropped).
        //   out               — receives the parse (its status says how far it got).
        // Throws std::bad_alloc only (callers wrap it).
        void ParseBranch(std::string_view tailUtf8, bool windowIsWholeFile, BranchParse& out)
        {
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

            auto& rows = out.rows;
            auto& byUuid = out.byUuid; // uuid -> index in rows; LAST occurrence wins, as in Claude Code's loader map
            // Claude Code's loader state for the active leaf, replayed in file order (ApiErrorRewind.h):
            //   leafMarker   — Yn: the last last-prompt leafUuid (a null-explicit "cleared" marker empties it);
            //   explicitLeaf — io: that marker was explicit (or re-stamped the same leaf right after an
            //                  explicit one); ANY later transcript row resets it;
            //   cleared      — bo: a null-explicit marker with no leaf marker or row after it (an empty
            //                  conversation on load).
            //   markerRewound — that marker said "rewound":true (Claude Code's own double-Esc rewind), carried
            //                  across a re-stamp of the same leaf like explicitLeaf (the interrupt work's addition:
            //                  the resume classifier must know that such a leaf is never continued).
            std::wstring leafMarker;
            size_t leafMarkerOrder = 0;
            bool explicitLeaf = false;
            bool cleared = false;
            bool markerRewound = false;

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
                        out.lastPromptText = lp->str; // the newest display text, copied into our marker
                        out.haveLastPrompt = true;
                    }
                    const auto* leaf = o.Find(L"leafUuid");
                    if (!leaf)
                    {
                        continue; // a marker without leafUuid carries no leaf (it moves nothing)
                    }
                    if (leaf->type == json::Value::Type::Str && !leaf->str.empty())
                    {
                        const bool sameLeaf = leaf->str == leafMarker;
                        explicitLeaf = o.BoolAt(L"explicit") || (explicitLeaf && sameLeaf);
                        markerRewound = o.BoolAt(L"rewound") || (markerRewound && sameLeaf);
                        leafMarker = leaf->str;
                        leafMarkerOrder = i;
                        cleared = false;
                    }
                    else if (leaf->type == json::Value::Type::Null && o.BoolAt(L"explicit"))
                    {
                        leafMarker.clear();
                        explicitLeaf = false;
                        markerRewound = false;
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
                    // The resume classifier's facts: a tool_use block makes the row a CALL (dropped by Claude
                    // Code's loader while it has no result), and a row of nothing but thinking is the orphan
                    // that goes with a dropped call.
                    if (content && content->type == json::Value::Type::Arr && !content->arr.empty())
                    {
                        bool onlyThinking = true;
                        for (const auto& blk : content->arr)
                        {
                            if (blk.type != json::Value::Type::Obj)
                            {
                                onlyThinking = false;
                                continue;
                            }
                            const std::wstring bt = blk.StrAt(L"type");
                            if (bt == L"tool_use")
                            {
                                if (!r.hasToolUse)
                                {
                                    r.toolUseName = blk.StrAt(L"name");
                                }
                                r.hasToolUse = true;
                            }
                            if (bt != L"thinking" && bt != L"redacted_thinking")
                            {
                                onlyThinking = false;
                            }
                        }
                        r.thinkingOnly = onlyThinking;
                    }
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
                        if (tur->type == json::Value::Type::Obj)
                        {
                            r.backgroundedByTurnAbort = tur->BoolAt(L"backgroundedByTurnAbort");
                        }
                    }
                    r.interruptedByShutdown = o.BoolAt(L"interruptedByShutdown");
                    r.toolDenialEndsTurn = o.BoolAt(L"toolDenialEndsTurn");
                    // SMt: EVERY block a tool_result whose text is a rejection. A plain-string content or a mixed
                    // block list is no rejection row.
                    if (content && content->type == json::Value::Type::Arr && !content->arr.empty())
                    {
                        bool allRejected = true;
                        for (const auto& blk : content->arr)
                        {
                            if (blk.type != json::Value::Type::Obj || blk.StrAt(L"type") != L"tool_result")
                            {
                                allRejected = false;
                                break;
                            }
                            const std::wstring resultText = ContentText(blk.Find(L"content"));
                            if (resultText.rfind(kRejectedToolResultPrefix, 0) != 0)
                            {
                                allRejected = false;
                                break;
                            }
                        }
                        r.allResultsRejected = allRejected;
                    }
                    // A local slash command's echo (`<command-name>…`, its stdout / stderr / caveat) or a `!` shell
                    // line (`<bash-input>`, `<bash-stdout>`): Claude Code's resume continues none of them (Z2).
                    {
                        size_t lead = 0;
                        while (lead < r.text.size() && (r.text[lead] == L' ' || r.text[lead] == L'\n' || r.text[lead] == L'\r' || r.text[lead] == L'\t'))
                        {
                            ++lead;
                        }
                        const std::wstring_view head = std::wstring_view{ r.text }.substr(lead);
                        r.localCommand = head.rfind(L"<command-", 0) == 0 || head.rfind(L"<local-command-", 0) == 0 || head.rfind(L"<bash-", 0) == 0;
                    }
                    if (const auto* origin = o.Find(L"origin"); origin && origin->type == json::Value::Type::Obj)
                    {
                        r.taskNotification = origin->StrAt(L"kind") == L"task-notification";
                    }
                    r.queuedTaskNotification = r.taskNotification && o.BoolAt(L"queueTranscriptOnly");
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
                out.status = BranchParse::Status::NoRows;
                return;
            }
            if (cleared)
            {
                out.status = BranchParse::Status::Cleared;
                return;
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
                        out.status = BranchParse::Status::MarkerOutsideWindow;
                        return;
                    }
                    // Not in the whole file either: the loader falls back to the last row (leaf stays).
                }
                else
                {
                    const bool explicitAtEnd = explicitLeaf && lastRow->order < leafMarkerOrder;
                    if (explicitAtEnd || lastRow->uuid == leafMarker || !descendsFrom(lastRow, leafMarker))
                    {
                        leaf = leafMarker;
                        out.leafRewound = markerRewound;
                    }
                    // else: the last row continues the marked leaf, so the loader takes the last row.
                }
            }

            // Up through system / attachment / progress rows to the leaf's MESSAGE (the first user / assistant).
            // `leaf` is always a key of byUuid here (the last row, or a marker leaf found above).
            out.leafIndex = byUuid.at(leaf);
            size_t at = out.leafIndex;
            for (size_t guard = 0; IsBookkeepingType(rows[at].type); ++guard)
            {
                const Row& e = rows[at];
                const auto it = byUuid.find(e.parent);
                if (e.parent.empty() || guard > rows.size())
                {
                    out.status = BranchParse::Status::NoMessage;
                    return;
                }
                if (it == byUuid.end())
                {
                    out.status = BranchParse::Status::LeftWindow;
                    return;
                }
                at = it->second;
            }
            out.messageIndex = at;
            out.status = BranchParse::Status::Ok;
        }

        // The rewind's marker: Claude Code's own rewind line (its key order: type, lastPrompt, leafUuid,
        // explicit, sessionId), WITHOUT "rewound" — that flag would make the next resume treat the leaf as a
        // user's deliberate rewind and refuse to continue the turn. Shared by both rewinds so their lines can
        // never differ.
        //   b                 — the parse (its newest lastPrompt text is copied when it has one).
        //   targetUuid        — the new active leaf.
        //   sessionId         — the conversation id for the line; empty takes `fallbackSessionId`.
        //   fallbackSessionId — the id the deciding row itself carries.
        // Returns the UTF-8 line without its newline; empty when it could not be encoded.
        // Throws std::bad_alloc only.
        std::string BuildRewindMarkerLine(const BranchParse& b, const std::wstring& targetUuid, std::wstring_view sessionId, const std::wstring& fallbackSessionId)
        {
            auto marker = json::Value::MkObj();
            marker.Set(L"type", json::Value::MkStr(L"last-prompt"));
            if (b.haveLastPrompt)
            {
                marker.Set(L"lastPrompt", json::Value::MkStr(b.lastPromptText));
            }
            marker.Set(L"leafUuid", json::Value::MkStr(targetUuid));
            marker.Set(L"explicit", json::Value::MkBool(true));
            const std::wstring sid = !sessionId.empty() ? std::wstring{ sessionId } : fallbackSessionId;
            if (!sid.empty())
            {
                marker.Set(L"sessionId", json::Value::MkStr(sid));
            }
            return WideToUtf8(json::Dump(marker));
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

            BranchParse branch;
            ParseBranch(tailUtf8, windowIsWholeFile, branch);
            switch (branch.status)
            {
            case BranchParse::Status::Ok:
                break;
            case BranchParse::Status::NoRows:
                return Verdict(std::move(plan), ApiErrorRewindVerdict::NotApplicable, L"no conversation rows in the bytes read");
            case BranchParse::Status::Cleared:
                return Verdict(std::move(plan), ApiErrorRewindVerdict::NotApplicable, L"the conversation was cleared to empty");
            case BranchParse::Status::MarkerOutsideWindow:
                return Verdict(std::move(plan), ApiErrorRewindVerdict::NeedMoreHistory, L"the active-leaf marker names a row older than the bytes read");
            case BranchParse::Status::NoMessage:
                return Verdict(std::move(plan), ApiErrorRewindVerdict::NotApplicable, L"no message on the active branch");
            case BranchParse::Status::LeftWindow:
                return Verdict(std::move(plan), windowIsWholeFile ? ApiErrorRewindVerdict::NotApplicable : ApiErrorRewindVerdict::NeedMoreHistory, L"the active branch leaves the bytes read before a message");
            }
            const auto& rows = branch.rows;
            const auto& byUuid = branch.byUuid;
            const Row* e = &rows[branch.messageIndex];
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
            // user's deliberate rewind and refuse to continue the turn. (Built by the helper both rewinds share.)
            plan.markerLine = BuildRewindMarkerLine(branch, target->uuid, sessionId, e->sessionId);
            if (plan.markerLine.empty())
            {
                return Verdict(std::move(plan), ApiErrorRewindVerdict::UnknownShape, L"the marker line could not be encoded");
            }
            plan.verdict = ApiErrorRewindVerdict::Rewind;
            return plan;
        }

        // ---- the USER-INTERRUPT planner (HOOKS.md "Interrupted turns") ---------------------------------------

        InterruptRewindPlan InterruptVerdict(InterruptRewindPlan plan, InterruptRewindVerdict v, std::wstring reason)
        {
            plan.verdict = v;
            plan.reason = std::move(reason);
            return plan;
        }

        // Is `row` a user interrupt row (its whole text one of Claude Code's two interrupt markers)?
        bool IsInterruptRow(const Row& row) noexcept
        {
            return row.type == L"user" && (row.text == kInterruptMarker || row.text == kInterruptMarkerToolUse);
        }

        // Would Claude Code's classifier CONTINUE a turn whose tail is the tool-result row `target`? Not when every
        // result is a rejection (SMt), when the abort backgrounded the tool (wMt), or when a tool result of the same
        // batch — between `target` and the reply that made the calls — carries toolDenialEndsTurn (bMt walks from
        // the tail up to the previous assistant row).
        //   rows / byUuid — the parse.
        //   target        — the tool-result row.
        //   why           — receives the reason when the answer is no.
        // Returns true when the turn would be continued.
        bool ToolResultContinues(const std::vector<Row>& rows, const std::unordered_map<std::wstring, size_t>& byUuid, const Row& target, std::wstring& why)
        {
            if (target.allResultsRejected)
            {
                why = L"denied tool result";
                return false;
            }
            if (target.backgroundedByTurnAbort)
            {
                why = L"backgrounded tool result";
                return false;
            }
            const Row* cur = &target;
            for (size_t guard = 0; cur && guard <= rows.size(); ++guard)
            {
                if (cur->type == L"assistant")
                {
                    break; // the reply that made the calls: the batch ends here
                }
                if (cur->type == L"user" && cur->toolResult && cur->toolDenialEndsTurn)
                {
                    why = L"denied tool result";
                    return false;
                }
                const auto it = byUuid.find(cur->parent);
                cur = (it == byUuid.end()) ? nullptr : &rows[it->second];
            }
            return true;
        }

        InterruptRewindPlan PlanInterruptImpl(std::string_view tailUtf8, bool windowIsWholeFile, std::wstring_view sessionId)
        {
            InterruptRewindPlan plan;
            if (tailUtf8.empty())
            {
                return plan; // an empty transcript ends in nothing
            }
            // The fast exit FIRST, like the API-error planner's: a window holding no interrupt row cannot end in
            // one — the common restart (a session at rest after a finished turn) stops at one byte scan.
            if (tailUtf8.find(kInterruptNeedle) == std::string_view::npos)
            {
                return plan;
            }
            if (tailUtf8.back() != '\n')
            {
                return InterruptVerdict(std::move(plan), InterruptRewindVerdict::TornTail, L"the transcript does not end with a newline (a write is in progress, or one was cut off)");
            }

            BranchParse branch;
            ParseBranch(tailUtf8, windowIsWholeFile, branch);
            switch (branch.status)
            {
            case BranchParse::Status::Ok:
                break;
            case BranchParse::Status::NoRows:
                return InterruptVerdict(std::move(plan), InterruptRewindVerdict::NotApplicable, L"no conversation rows in the bytes read");
            case BranchParse::Status::Cleared:
                return InterruptVerdict(std::move(plan), InterruptRewindVerdict::NotApplicable, L"the conversation was cleared to empty");
            case BranchParse::Status::MarkerOutsideWindow:
                return InterruptVerdict(std::move(plan), InterruptRewindVerdict::NeedMoreHistory, L"the active-leaf marker names a row older than the bytes read");
            case BranchParse::Status::NoMessage:
                return InterruptVerdict(std::move(plan), InterruptRewindVerdict::NotApplicable, L"no message on the active branch");
            case BranchParse::Status::LeftWindow:
                return InterruptVerdict(std::move(plan), windowIsWholeFile ? InterruptRewindVerdict::NotApplicable : InterruptRewindVerdict::NeedMoreHistory, L"the active branch leaves the bytes read before a message");
            }
            const auto& rows = branch.rows;
            const auto& byUuid = branch.byUuid;
            const Row* e = &rows[branch.messageIndex];
            if (!IsInterruptRow(*e))
            {
                return plan; // NotApplicable: the active branch ends in something else (the usual case)
            }
            if (e->interruptedByShutdown)
            {
                // Claude Code wrote this interrupt itself while shutting down (a Restart, an app close): its resume
                // CONTINUES such a turn by itself (2.1.291's _Mt) — nothing to take back, nothing to ask.
                return InterruptVerdict(std::move(plan), InterruptRewindVerdict::NotApplicable, L"a shutdown interrupted the turn, not the user \x2014 Claude Code continues it by itself");
            }
            plan.interruptUuid = e->uuid;
            plan.interruptTimestamp = e->timestamp;
            plan.duringToolUse = e->text == kInterruptMarkerToolUse;
            for (const auto& r : rows)
            {
                if (r.order > e->order)
                {
                    if (r.type == L"user" || r.type == L"assistant")
                    {
                        return InterruptVerdict(std::move(plan), InterruptRewindVerdict::NotApplicable, L"the conversation went on after the interrupt");
                    }
                    ++plan.rowsAfterInterrupt;
                }
            }

            // Up from the interrupt row to the user row below the interrupted reply (the probe-measured shapes,
            // HOOKS.md "Interrupted turns"): the reply's own rows (one message id), bookkeeping rows, transient
            // API-error rows, Claude Code's own "Continue from where you left off." rows and — for an Esc during
            // a tool call — the cut batch's results between the interrupt and the reply.
            std::wstring partialMessageId;
            bool sawPartial = false;
            const Row* cur = e;
            const Row* target = nullptr;
            for (size_t guard = 0; guard <= rows.size() && !target; ++guard)
            {
                if (cur->parent.empty())
                {
                    return InterruptVerdict(std::move(plan), InterruptRewindVerdict::UnknownShape, L"no user row above the interrupt");
                }
                const auto it = byUuid.find(cur->parent);
                if (it == byUuid.end())
                {
                    return InterruptVerdict(std::move(plan), windowIsWholeFile ? InterruptRewindVerdict::UnknownShape : InterruptRewindVerdict::NeedMoreHistory, L"the rows above the interrupt leave the bytes read before a user row");
                }
                cur = &rows[it->second];
                if (IsBookkeepingType(cur->type))
                {
                    continue; // hook attachments, the prompt snapshot, turn bookkeeping
                }
                if (cur->type == L"assistant")
                {
                    if (cur->apiError)
                    {
                        if (cur->refusalClass)
                        {
                            // A refused request is a FINISHED turn to Claude Code; resending it would repeat
                            // what Anthropic refused (the API-error rewind's rule).
                            return InterruptVerdict(std::move(plan), InterruptRewindVerdict::UnknownShape, L"a refused response sits above the interrupt");
                        }
                        ++plan.errorRows; // a retried request the Esc ended
                        continue;
                    }
                    if (sawPartial && !partialMessageId.empty() && !cur->messageId.empty() && cur->messageId != partialMessageId)
                    {
                        return InterruptVerdict(std::move(plan), InterruptRewindVerdict::UnknownShape, L"two different replies sit above the interrupt with no user row between them");
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
                        // A resume's (or a fork's) own continuation that the Esc then stopped: rewind past it, so
                        // the next resume makes a fresh one instead of resting on a meta row it never continues.
                        ++plan.resumePromptRows;
                        continue;
                    }
                    if (plan.duringToolUse && !sawPartial && cur->toolResult)
                    {
                        // The results Claude Code wrote for the calls the Esc cut ("doesn't want to proceed"), and
                        // any call of the same batch that finished: the rewind re-does the whole step.
                        ++plan.discardedResults;
                        continue;
                    }
                    if (IsInterruptRow(*cur))
                    {
                        return InterruptVerdict(std::move(plan), InterruptRewindVerdict::UnknownShape, L"an earlier interrupt sits above this one");
                    }
                    target = cur;
                }
            }
            if (!target)
            {
                return InterruptVerdict(std::move(plan), InterruptRewindVerdict::UnknownShape, L"the walk above the interrupt did not end (a parent cycle)");
            }

            plan.targetUuid = target->uuid;
            bool continues = false;
            if (target->toolResult)
            {
                std::wstring why;
                continues = ToolResultContinues(rows, byUuid, *target, why);
                plan.targetKind = continues ? std::wstring{ L"tool result" } : why;
            }
            else if (target->compactSummary)
            {
                plan.targetKind = L"compact summary";
            }
            else if (target->isMeta)
            {
                plan.targetKind = L"meta";
            }
            else if (target->localCommand)
            {
                plan.targetKind = L"local command";
            }
            else
            {
                plan.targetKind = L"prompt";
                plan.targetText = FirstLineCapped(target->text, 160);
                continues = true;
            }
            if (!continues)
            {
                return InterruptVerdict(std::move(plan), InterruptRewindVerdict::WouldNotContinue, L"Claude Code does not continue a turn that stops at a " + plan.targetKind + L" \x2014 a rewind would only hide the interrupted reply");
            }

            plan.markerLine = BuildRewindMarkerLine(branch, target->uuid, sessionId, e->sessionId);
            if (plan.markerLine.empty())
            {
                return InterruptVerdict(std::move(plan), InterruptRewindVerdict::UnknownShape, L"the marker line could not be encoded");
            }
            plan.verdict = InterruptRewindVerdict::Rewind;
            return plan;
        }

        // ---- the RESUME CLASSIFIER (2.1.291's bfo, mirrored — ApiErrorRewind.h) --------------------------------

        ResumeContinuationPlan ContinuationVerdict(ResumeContinuation v, std::wstring tailKind, std::wstring reason)
        {
            ResumeContinuationPlan plan;
            plan.verdict = v;
            plan.tailKind = std::move(tailKind);
            plan.reason = std::move(reason);
            return plan;
        }

        // The verdict for a USER row reached from the tail (bfo's `S` user branch when `viaAttachment` is false,
        // its attachment branch's user checks when true — the two differ only for a meta row and a plain prompt).
        //   rows / byUuid  — the parse.
        //   s              — the user row.
        //   viaAttachment  — the tail was an attachment above which this row was found.
        //   skippedQueued  — a queued task notification was stepped over at the tail (bfo's `h`).
        ResumeContinuationPlan ClassifyUserTail(const std::vector<Row>& rows, const std::unordered_map<std::wstring, size_t>& byUuid, const Row& s, bool viaAttachment, bool skippedQueued)
        {
            if (s.compactSummary)
            {
                return ContinuationVerdict(ResumeContinuation::None, L"compact summary", L"the branch ends at a compaction summary");
            }
            if (IsInterruptRow(s))
            {
                return s.interruptedByShutdown ? ContinuationVerdict(ResumeContinuation::ContinuesTurn, L"interrupt", {}) :
                                                 ContinuationVerdict(ResumeContinuation::None, L"interrupt", L"the user interrupted the turn");
            }
            if (s.localCommand)
            {
                return ContinuationVerdict(ResumeContinuation::None, L"local command", L"the branch ends at a local command");
            }
            if (s.toolResult)
            {
                std::wstring why;
                if (!ToolResultContinues(rows, byUuid, s, why))
                {
                    return ContinuationVerdict(ResumeContinuation::None, why, L"the branch ends at a " + why);
                }
                return ContinuationVerdict(ResumeContinuation::ContinuesTurn, L"tool result", {});
            }
            if (viaAttachment)
            {
                // bfo's attachment branch has no meta / prompt case: anything else above an attachment tail is
                // continued as a turn.
                return ContinuationVerdict(ResumeContinuation::ContinuesTurn, s.isMeta ? L"meta" : L"prompt", {});
            }
            if (s.isMeta)
            {
                return ContinuationVerdict(ResumeContinuation::None, L"meta", L"the branch ends at a meta row");
            }
            if (s.taskNotification)
            {
                return ContinuationVerdict(ResumeContinuation::ContinuesTurn, L"task notification", {});
            }
            if (skippedQueued)
            {
                return ContinuationVerdict(ResumeContinuation::ContinuesTurn, L"prompt", {});
            }
            auto plan = ContinuationVerdict(ResumeContinuation::ResendsPrompt, L"prompt", {});
            plan.promptText = FirstLineCapped(s.text, 160);
            return plan;
        }

        ResumeContinuationPlan ClassifyImpl(std::string_view tailUtf8, bool windowIsWholeFile)
        {
            if (tailUtf8.empty())
            {
                return ContinuationVerdict(ResumeContinuation::None, L"empty", L"an empty transcript");
            }
            BranchParse branch;
            ParseBranch(tailUtf8, windowIsWholeFile, branch);
            switch (branch.status)
            {
            case BranchParse::Status::Ok:
                break;
            case BranchParse::Status::NoRows:
                return ContinuationVerdict(ResumeContinuation::None, L"empty", L"no conversation rows in the bytes read");
            case BranchParse::Status::Cleared:
                return ContinuationVerdict(ResumeContinuation::None, L"empty", L"the conversation was cleared to empty");
            case BranchParse::Status::MarkerOutsideWindow:
                return ContinuationVerdict(ResumeContinuation::NeedMoreHistory, {}, L"the active-leaf marker names a row older than the bytes read");
            case BranchParse::Status::NoMessage:
                return ContinuationVerdict(ResumeContinuation::None, L"empty", L"no message on the active branch");
            case BranchParse::Status::LeftWindow:
                return windowIsWholeFile ? ContinuationVerdict(ResumeContinuation::None, L"empty", L"the active branch has no message") :
                                           ContinuationVerdict(ResumeContinuation::NeedMoreHistory, {}, L"the active branch leaves the bytes read before a message");
            }
            if (branch.leafRewound)
            {
                return ContinuationVerdict(ResumeContinuation::None, L"rewound", L"the user rewound the conversation here (Claude Code continues nothing after its own rewind)");
            }
            const auto& rows = branch.rows;
            const auto& byUuid = branch.byUuid;
            const auto parentOf = [&](const Row* r) -> const Row* {
                const auto it = byUuid.find(r->parent);
                return (r->parent.empty() || it == byUuid.end()) ? nullptr : &rows[it->second];
            };

            // From the active LEAF (not the message — an attachment tail takes bfo's other branch): step over what
            // Claude Code's tail walk steps over, and over a call in flight, which its loader drops.
            const Row* s = &rows[branch.leafIndex];
            std::wstring droppedTool;
            bool dropped = false;
            bool skippedQueued = false;
            for (size_t guard = 0; s && guard <= rows.size(); ++guard)
            {
                if (s->type == L"system" || s->type == L"progress")
                {
                    s = parentOf(s);
                    continue;
                }
                if (s->type == L"user" && s->queuedTaskNotification)
                {
                    skippedQueued = true;
                    s = parentOf(s);
                    continue;
                }
                if (s->type == L"assistant")
                {
                    if (s->apiError && !s->refusalClass)
                    {
                        s = parentOf(s); // a transient API error (EMt): stepped over
                        continue;
                    }
                    if (s->hasToolUse && !s->apiError)
                    {
                        // A call with no result yet (nothing below it on the branch): the loader drops it and the
                        // turn continues from the row before — the fork runs the call AGAIN.
                        if (droppedTool.empty())
                        {
                            droppedTool = s->toolUseName;
                        }
                        dropped = true;
                        s = parentOf(s);
                        continue;
                    }
                    if (dropped && s->thinkingOnly)
                    {
                        s = parentOf(s); // the dropped call's own thinking: an orphan without it
                        continue;
                    }
                }
                break;
            }
            if (!s)
            {
                return windowIsWholeFile ? ContinuationVerdict(ResumeContinuation::None, L"empty", L"nothing on the branch above the tail") :
                                           ContinuationVerdict(ResumeContinuation::NeedMoreHistory, {}, L"the tail's walk left the bytes read");
            }
            ResumeContinuationPlan plan;
            if (s->type == L"assistant")
            {
                plan = ContinuationVerdict(ResumeContinuation::None, L"reply", L"the turn ended with a reply");
            }
            else if (s->type == L"user")
            {
                plan = ClassifyUserTail(rows, byUuid, *s, false, skippedQueued);
            }
            else
            {
                // An attachment (or any other bookkeeping) tail: bfo's attachment branch walks up past bookkeeping
                // to the first message. A reply there means the turn had finished (an API-error reply included:
                // a refusal is complete, and a transient error above an attachment tail is a finished retry).
                const Row* y = parentOf(s);
                bool yDropped = false;
                for (size_t guard = 0; y && guard <= rows.size(); ++guard)
                {
                    if (IsBookkeepingType(y->type))
                    {
                        y = parentOf(y);
                        continue;
                    }
                    if (y->type == L"assistant" && y->apiError && !y->refusalClass)
                    {
                        y = parentOf(y);
                        continue;
                    }
                    if (y->type == L"assistant" && y->hasToolUse)
                    {
                        if (droppedTool.empty())
                        {
                            droppedTool = y->toolUseName;
                        }
                        yDropped = true;
                        y = parentOf(y);
                        continue;
                    }
                    if (y->type == L"assistant" && yDropped && y->thinkingOnly)
                    {
                        y = parentOf(y);
                        continue;
                    }
                    break;
                }
                if (!y)
                {
                    plan = windowIsWholeFile ? ContinuationVerdict(ResumeContinuation::ContinuesTurn, L"attachment", {}) :
                                               ContinuationVerdict(ResumeContinuation::NeedMoreHistory, {}, L"the tail's walk left the bytes read");
                }
                else if (y->type == L"assistant")
                {
                    plan = ContinuationVerdict(ResumeContinuation::None, L"reply", L"the turn ended with a reply");
                }
                else
                {
                    plan = ClassifyUserTail(rows, byUuid, *y, true, skippedQueued);
                }
            }
            if (plan.verdict == ResumeContinuation::ContinuesTurn || plan.verdict == ResumeContinuation::ResendsPrompt)
            {
                plan.droppedTool = droppedTool;
            }
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

        // Read + plan with the widening windows, for the interrupt planner and the classifier (ReadAndPlan's loop,
        // with the planner and its "needs more history" test passed in).
        //   path     — the transcript.
        //   planFn   — (std::string_view bytes, bool wholeFile) -> Plan.
        //   needMore — (const Plan&) -> bool: the plan asked for a larger window.
        //   fileSize — receives the size of the LAST read (the append's size check).
        //   readOk   — receives false when the file could not be opened or read.
        // Returns the last plan (a default-constructed one when the first read failed).
        // Throws std::bad_alloc only.
        template<typename Plan, typename PlanFn, typename NeedMoreFn>
        Plan ReadAndPlanWith(const std::wstring& path, PlanFn&& planFn, NeedMoreFn&& needMore, uint64_t& fileSize, bool& readOk)
        {
            Plan plan{};
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
                plan = planFn(std::string_view{ bytes }, whole);
                if (!needMore(plan) || whole)
                {
                    break;
                }
            }
            return plan;
        }

        // The ONE append both rewinds make — RewindApiErrorTail's mechanics, moved here so the interrupt rewind
        // cannot drift from them: refuse while a live claude's heartbeat names the conversation (its exit
        // re-stamps its own leaf and would silently undo the line — ApiErrorRewind.h, TIMING), re-check that the
        // file still has the size the plan was made on, and append the line plus "\n" with ONE FILE_APPEND_DATA
        // write, the way claude itself appends.
        //   path           — the transcript.
        //   sessionId      — the conversation (the holder check).
        //   sizeAtRead     — the size the plan was made on.
        //   markerLine     — the line, without its newline.
        //   whatExitUndoes — what a live holder's exit would put back ("the error", "the interrupt"), for the detail.
        //   outcome        — receives Appended, Skipped or Failed.
        //   appendedAtByte — receives the line's offset (Appended only).
        //   detail         — receives why (Skipped / Failed only).
        // Throws std::bad_alloc only (the callers wrap it).
        void AppendRewindLine(const std::wstring& path, std::wstring_view sessionId, uint64_t sizeAtRead, const std::string& markerLine, std::wstring_view whatExitUndoes, ApiErrorRewindOutcome& outcome, uint64_t& appendedAtByte, std::wstring& detail)
        {
            // Nobody may hold the conversation: a live claude that ran the failed turn re-stamps the error
            // branch as its leaf at exit and would silently undo the line (ApiErrorRewind.h, TIMING). The
            // seams that end a claude wait for its exit first; this is the belt for every caller.
            if (const auto holders = LiveClaudeProcessesForSession(sessionId); !holders.empty())
            {
                outcome = ApiErrorRewindOutcome::Skipped;
                detail = L"claude pid " + JoinPids(holders) + L" still runs this conversation (its exit would put " + std::wstring{ whatExitUndoes } + L" back as the active leaf)";
                return;
            }

            // FILE_APPEND_DATA without FILE_WRITE_DATA: every write lands at the end of the file whatever
            // the file pointer — the same append-only write claude itself makes. FILE_READ_ATTRIBUTES is for
            // the size check below.
            const HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                outcome = ApiErrorRewindOutcome::Failed;
                detail = L"the transcript could not be opened for append (error " + std::to_wstring(::GetLastError()) + L")";
                return;
            }
            LARGE_INTEGER sz{};
            const bool sized = ::GetFileSizeEx(h, &sz) != FALSE;
            if (!sized || static_cast<uint64_t>(sz.QuadPart) != sizeAtRead)
            {
                // The file changed between the read and now (a writer the holder check could not see, e.g.
                // a claude that started this very moment): the plan no longer describes the tail.
                ::CloseHandle(h);
                outcome = ApiErrorRewindOutcome::Skipped;
                detail = sized ? (L"the transcript changed while it was planned (" + std::to_wstring(sizeAtRead) + L" -> " + std::to_wstring(sz.QuadPart) + L" bytes)") :
                                 L"the transcript's size could not be read before the append";
                return;
            }
            const std::string line = markerLine + "\n";
            DWORD written = 0;
            const BOOL ok = ::WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            const DWORD writeError = ok ? 0 : ::GetLastError();
            ::CloseHandle(h);
            if (!ok || written != line.size())
            {
                // A short write leaves a torn last line: Claude Code's loader skips a line it cannot parse,
                // and the next plan here reports TornTail instead of appending onto it.
                outcome = ApiErrorRewindOutcome::Failed;
                detail = L"the append failed (error " + std::to_wstring(writeError) + L", " + std::to_wstring(written) + L" of " + std::to_wstring(line.size()) + L" bytes written)";
                return;
            }
            outcome = ApiErrorRewindOutcome::Appended;
            appendedAtByte = sizeAtRead;
        }

        // A file read for ResolveClaudeAutoContinue: the whole file when it holds at most `cap` bytes, "" when it
        // is missing, bigger, or unreadable (a settings file is a few KB; a huge one is not worth parsing here).
        //   path — the file.
        //   cap  — the largest size read.
        // Returns the UTF-8 bytes. Throws std::bad_alloc only.
        std::string ReadSmallFile(const std::wstring& path, uint64_t cap)
        {
            std::string bytes;
            uint64_t size = 0;
            bool whole = false;
            if (!ReadTail(path, cap + 1, bytes, size, whole) || !whole || size > cap)
            {
                return {};
            }
            return bytes;
        }

        // The value of `env[key]` in one Claude Code settings file, when the file sets it.
        //   path  — the settings file.
        //   key   — the variable.
        //   value — receives the value (a string; a number or a boolean is written as JSON would show it).
        // Returns true when the file exists, parses, and its "env" object has the key.
        // Throws std::bad_alloc only.
        bool SettingsFileEnvValue(const std::wstring& path, std::wstring_view key, std::wstring& value)
        {
            const std::string bytes = ReadSmallFile(path, 4ull * 1024 * 1024);
            if (bytes.empty())
            {
                return false;
            }
            // Strip a UTF-8 BOM (an editor's), which the JSON reader would not accept.
            std::string_view body{ bytes };
            if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF && static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF)
            {
                body.remove_prefix(3);
            }
            const auto parsed = json::Parse(Utf8ToWide(body));
            if (!parsed || parsed->type != json::Value::Type::Obj)
            {
                return false;
            }
            const auto* env = parsed->Find(L"env");
            if (!env || env->type != json::Value::Type::Obj)
            {
                return false;
            }
            const auto* v = env->Find(std::wstring{ key });
            if (!v)
            {
                return false;
            }
            switch (v->type)
            {
            case json::Value::Type::Str:
                value = v->str;
                break;
            case json::Value::Type::Bool:
                value = v->boolean ? L"true" : L"false";
                break;
            case json::Value::Type::Num:
                value = std::to_wstring(static_cast<long long>(v->num));
                break;
            default:
                value.clear();
                break;
            }
            return true;
        }

        // One environment variable of THIS process ("" when unset).
        //   name — the variable.
        // Returns its value. Throws std::bad_alloc only.
        std::wstring ProcessEnv(const wchar_t* name)
        {
            const DWORD need = ::GetEnvironmentVariableW(name, nullptr, 0);
            if (need == 0)
            {
                return {};
            }
            std::wstring v(static_cast<size_t>(need), L'\0');
            const DWORD got = ::GetEnvironmentVariableW(name, v.data(), need);
            v.resize(got < need ? got : 0);
            return v;
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
            // The holder refusal, the size re-check and the single append (AppendRewindLine — the interrupt rewind
            // makes the very same append).
            AppendRewindLine(transcriptPath, sessionId, sizeAtRead, r.plan.markerLine, L"the error", r.outcome, r.appendedAtByte, r.detail);
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

    // ===== the USER-INTERRUPT rewind (HOOKS.md "Interrupted turns") ============================================

    InterruptRewindPlan PlanInterruptRewind(std::string_view tailUtf8, bool windowIsWholeFile, std::wstring_view sessionId) noexcept
    {
        try
        {
            return PlanInterruptImpl(tailUtf8, windowIsWholeFile, sessionId);
        }
        catch (...)
        {
            // A planner failure must never cost the restart (or ask a question about nothing): "nothing to do",
            // with the trace kept (Rule #18).
            LogSwallowedException(L"PlanInterruptRewind");
            InterruptRewindPlan plan;
            plan.reason = L"planner failed";
            return plan;
        }
    }

    InterruptRewindResult RewindInterruptTail(const std::wstring& transcriptPath, std::wstring_view sessionId) noexcept
    {
        InterruptRewindResult r;
        try
        {
            uint64_t sizeAtRead = 0;
            bool readOk = true;
            r.plan = ReadAndPlanWith<InterruptRewindPlan>(
                transcriptPath,
                [sessionId](std::string_view bytes, bool whole) { return PlanInterruptRewind(bytes, whole, sessionId); },
                [](const InterruptRewindPlan& p) { return p.verdict == InterruptRewindVerdict::NeedMoreHistory; },
                sizeAtRead,
                readOk);
            if (!readOk)
            {
                r.outcome = ApiErrorRewindOutcome::Failed;
                r.detail = L"the transcript could not be read (error " + std::to_wstring(::GetLastError()) + L")";
                return r;
            }
            if (r.plan.verdict == InterruptRewindVerdict::NotApplicable)
            {
                return r; // NotNeeded: the user (or a hook, or the old claude's last lines) moved on
            }
            if (r.plan.verdict != InterruptRewindVerdict::Rewind)
            {
                r.outcome = ApiErrorRewindOutcome::Skipped;
                r.detail = r.plan.reason;
                return r;
            }
            AppendRewindLine(transcriptPath, sessionId, sizeAtRead, r.plan.markerLine, L"the interrupt", r.outcome, r.appendedAtByte, r.detail);
        }
        catch (...)
        {
            LogSwallowedException(L"RewindInterruptTail");
            r.outcome = ApiErrorRewindOutcome::Failed;
            r.detail = L"an exception was thrown (see the [exc] line above)";
        }
        return r;
    }

    InterruptRewindResult RewindInterruptTailForSession(std::wstring_view sessionId) noexcept
    {
        try
        {
            const std::wstring path = ResolveClaudeTranscriptPath(sessionId);
            if (path.empty())
            {
                return {}; // NotNeeded: never prompted, nothing to resume
            }
            return RewindInterruptTail(path, sessionId);
        }
        catch (...)
        {
            LogSwallowedException(L"RewindInterruptTailForSession");
            InterruptRewindResult r;
            r.outcome = ApiErrorRewindOutcome::Failed;
            r.detail = L"an exception was thrown (see the [exc] line above)";
            return r;
        }
    }

    InterruptRewindPlan PlanInterruptRewindForSession(std::wstring_view sessionId) noexcept
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
            auto plan = ReadAndPlanWith<InterruptRewindPlan>(
                path,
                [sessionId](std::string_view bytes, bool whole) { return PlanInterruptRewind(bytes, whole, sessionId); },
                [](const InterruptRewindPlan& p) { return p.verdict == InterruptRewindVerdict::NeedMoreHistory; },
                size,
                readOk);
            return readOk ? plan : InterruptRewindPlan{};
        }
        catch (...)
        {
            LogSwallowedException(L"PlanInterruptRewindForSession");
            return {};
        }
    }

    std::wstring DescribeInterruptRewind(const InterruptRewindResult& r, std::wstring_view sessionId, std::wstring_view seam)
    {
        std::wstring s = L"[interrupt-rewind] " + ShortId(std::wstring{ sessionId }) + L" " + std::wstring{ seam } + L": ";
        const auto& p = r.plan;
        std::wstring interrupt;
        if (!p.interruptUuid.empty())
        {
            interrupt = std::wstring{ p.duringToolUse ? L"the interrupt during a tool call" : L"the interrupt during a reply" } + L" (" + ShortId(p.interruptUuid) + L", " + p.interruptTimestamp + L")";
        }
        switch (r.outcome)
        {
        case ApiErrorRewindOutcome::Appended:
            s += L"took back " + interrupt + L" \x2014 " + std::to_wstring(p.partialRows) + L" interrupted-reply row(s), " +
                 std::to_wstring(p.discardedResults) + L" cut tool result(s), " + std::to_wstring(p.resumePromptRows) + L" earlier continue row(s), " +
                 std::to_wstring(p.errorRows) + L" error row(s), " + std::to_wstring(p.rowsAfterInterrupt) + L" row(s) after it \x2014 to " +
                 ShortId(p.targetUuid) + L" (" + p.targetKind + L"); line at byte " + std::to_wstring(r.appendedAtByte) +
                 (p.targetKind == L"prompt" ? L"; the restarted claude sends the prompt again" : L"; the restarted claude continues the turn");
            break;
        case ApiErrorRewindOutcome::NotNeeded:
            s += L"no user interrupt at the end of the conversation any more \x2014 nothing to take back";
            break;
        case ApiErrorRewindOutcome::Skipped:
            s += L"NOT taken back \x2014 " + r.detail + (interrupt.empty() ? std::wstring{} : (L" " + interrupt));
            break;
        case ApiErrorRewindOutcome::Failed:
            s += L"FAILED \x2014 " + r.detail + (interrupt.empty() ? std::wstring{} : (L" " + interrupt));
            break;
        }
        return s;
    }

    // ===== would a resume / a fork continue the turn by itself? =================================================

    ResumeContinuationPlan ClassifyResumeContinuation(std::string_view tailUtf8, bool windowIsWholeFile) noexcept
    {
        try
        {
            return ClassifyImpl(tailUtf8, windowIsWholeFile);
        }
        catch (...)
        {
            // A classifier failure means "no prediction": the fork seams then ask only when the source's live
            // state says it runs (Rule #18: the trace is kept).
            LogSwallowedException(L"ClassifyResumeContinuation");
            ResumeContinuationPlan plan;
            plan.reason = L"classifier failed";
            return plan;
        }
    }

    ResumeContinuationPlan ClassifyResumeContinuationForSession(std::wstring_view sessionId) noexcept
    {
        try
        {
            const std::wstring path = ResolveClaudeTranscriptPath(sessionId);
            if (path.empty())
            {
                ResumeContinuationPlan plan;
                plan.tailKind = L"empty";
                plan.reason = L"no transcript";
                return plan;
            }
            uint64_t size = 0;
            bool readOk = true;
            auto plan = ReadAndPlanWith<ResumeContinuationPlan>(
                path,
                [](std::string_view bytes, bool whole) { return ClassifyResumeContinuation(bytes, whole); },
                [](const ResumeContinuationPlan& p) { return p.verdict == ResumeContinuation::NeedMoreHistory; },
                size,
                readOk);
            if (!readOk)
            {
                ResumeContinuationPlan unreadable;
                unreadable.reason = L"the transcript could not be read";
                return unreadable;
            }
            return plan;
        }
        catch (...)
        {
            LogSwallowedException(L"ClassifyResumeContinuationForSession");
            ResumeContinuationPlan plan;
            plan.reason = L"classifier failed";
            return plan;
        }
    }

    AutoContinueSetting ResolveClaudeAutoContinue(std::wstring_view cwd, const std::vector<std::pair<std::wstring, std::wstring>>& spawnEnv) noexcept
    {
        AutoContinueSetting result;
        try
        {
            const auto decide = [&](const std::wstring& value, std::wstring source, bool fromFile) {
                result.value = value;
                result.on = IsClaudeEnvTruthy(value);
                result.source = std::move(source);
                result.fromSettingsFile = fromFile;
            };
            // Claude Code's settings precedence, highest first: the managed policy, then the project's local and
            // shared files, then the user's. The first file whose "env" sets the key decides, because Claude Code
            // merges the env blocks key by key with the higher scope winning.
            std::vector<std::wstring> files;
            for (const wchar_t* root : { L"ProgramFiles", L"ProgramData" })
            {
                const std::wstring base = ProcessEnv(root);
                if (!base.empty())
                {
                    files.push_back(base + L"\\ClaudeCode\\managed-settings.json");
                }
            }
            if (!cwd.empty())
            {
                std::wstring dir{ cwd };
                while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
                {
                    dir.pop_back();
                }
                if (!dir.empty())
                {
                    files.push_back(dir + L"\\.claude\\settings.local.json");
                    files.push_back(dir + L"\\.claude\\settings.json");
                }
            }
            {
                std::wstring config = ProcessEnv(L"CLAUDE_CONFIG_DIR");
                if (config.empty())
                {
                    const std::wstring home = ProcessEnv(L"USERPROFILE");
                    if (!home.empty())
                    {
                        config = home + L"\\.claude";
                    }
                }
                if (!config.empty())
                {
                    files.push_back(config + L"\\settings.json");
                }
            }
            const std::wstring key{ kClaudeResumeInterruptedTurnEnv };
            for (const auto& file : files)
            {
                std::wstring value;
                if (SettingsFileEnvValue(file, key, value))
                {
                    decide(value, file, true);
                    return result;
                }
            }
            // No settings file sets it: the claude's own environment decides — the launch's additions first (they
            // are applied over Agentmaster's environment when the claude is created), then Agentmaster's own.
            for (const auto& [name, value] : spawnEnv)
            {
                if (SessionIdEq(name, key)) // an ASCII case-insensitive compare: Windows variable names ignore case
                {
                    decide(value, L"spawn env", false);
                    return result;
                }
            }
            const std::wstring processValue = ProcessEnv(kClaudeResumeInterruptedTurnEnv.data());
            if (!processValue.empty())
            {
                decide(processValue, L"process env", false);
                return result;
            }
            result.source = L"nowhere";
        }
        catch (...)
        {
            LogSwallowedException(L"ResolveClaudeAutoContinue");
            result = AutoContinueSetting{};
            result.source = L"error";
        }
        return result;
    }

    ForkContinuationOutlook AssessForkContinuation(std::wstring_view sourceId, bool sourceRunning, std::wstring_view cwd, const std::vector<std::pair<std::wstring, std::wstring>>& spawnEnv) noexcept
    {
        ForkContinuationOutlook outlook;
        try
        {
            outlook.sourceRunning = sourceRunning;
            outlook.tail = ClassifyResumeContinuationForSession(sourceId);
            if (outlook.tail.reason == L"no transcript")
            {
                outlook.why = L"the source has no transcript (the fork starts fresh)";
                return outlook;
            }
            // The switch first: with it off, Claude Code only PREPARES the continuation, and a fork waits whatever its
            // source was doing — there is nothing to stop and nothing to ask.
            outlook.autoContinue = ResolveClaudeAutoContinue(cwd, spawnEnv);
            if (!outlook.autoContinue.on)
            {
                outlook.why = std::wstring{ kClaudeResumeInterruptedTurnEnv } + L" is off (" + outlook.autoContinue.source + L") \x2014 a fork does not continue a turn by itself";
                return outlook;
            }
            const bool tailContinues = outlook.tail.verdict == ResumeContinuation::ContinuesTurn || outlook.tail.verdict == ResumeContinuation::ResendsPrompt;
            if (!tailContinues && !sourceRunning)
            {
                outlook.why = L"the source's turn is finished (" + outlook.tail.tailKind + (outlook.tail.reason.empty() ? std::wstring{} : (L": " + outlook.tail.reason)) + L")";
                return outlook;
            }
            outlook.ask = true;
        }
        catch (...)
        {
            LogSwallowedException(L"AssessForkContinuation");
            outlook = ForkContinuationOutlook{};
            outlook.why = L"the assessment failed (see the [exc] line above)";
        }
        return outlook;
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
