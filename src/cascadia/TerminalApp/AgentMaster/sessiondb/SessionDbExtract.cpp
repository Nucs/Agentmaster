// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's line extractor (see SessionDbExtract.h for the contract).

#include "SessionDbExtract.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>

#include "../Json.h"
#include "../ProcessInspect.h" // SummaryPromptOfLine, SummaryCompactionLabelOfLine, SeSummaryFileLabel, NormalizeRecapText, ScrubLargeBase64Payloads
#include "../TranscriptStore.h" // ParseTranscriptTimestamp, IsNoiseUserPrompt, ExtractPathsFromText

namespace Agentmaster::SessionDb
{
    namespace
    {
        // UTF-8 -> UTF-16 (invalid bytes become U+FFFD: a line that is not valid UTF-8 is still
        // classified — its raw bytes are stored apart, untouched).
        std::wstring Widen(std::string_view s)
        {
            if (s.empty())
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

        // TranscriptStore's FirstLineTrim (its title shape), restated: the first non-blank line,
        // trimmed. Kept identical so first_prompt equals the sidecar's firstUserPrompt.
        std::wstring FirstLineTrim(const std::wstring& s)
        {
            size_t i = 0;
            while (i < s.size() && (s[i] == L'\n' || s[i] == L'\r' || s[i] == L' ' || s[i] == L'\t'))
            {
                ++i;
            }
            size_t j = i;
            while (j < s.size() && s[j] != L'\n' && s[j] != L'\r')
            {
                ++j;
            }
            while (j > i && (s[j - 1] == L' ' || s[j - 1] == L'\t'))
            {
                --j;
            }
            return s.substr(i, j - i);
        }

        // The analyzer's SeIsPlansPath, restated (it is file-static there): a "/plans/" segment, the
        // separators and ASCII case folded.
        bool IsPlansPath(const std::wstring& fp)
        {
            std::wstring n = fp;
            for (auto& c : n)
            {
                if (c == L'\\')
                {
                    c = L'/';
                }
                else if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return n.find(L"/plans/") != std::wstring::npos;
        }

        // The analyzer's basename (SeBasename, file-static there).
        std::wstring Basename(const std::wstring& fp)
        {
            const auto pos = fp.find_last_of(L"/\\");
            return pos == std::wstring::npos ? fp : fp.substr(pos + 1);
        }

        // ASCII lower-case copy (the analyzer's own folding for the plan-parent marker).
        std::wstring AsciiLower(std::wstring s)
        {
            for (auto& ch : s)
            {
                if (ch >= L'A' && ch <= L'Z')
                {
                    ch = static_cast<wchar_t>(ch - L'A' + L'a');
                }
            }
            return s;
        }

        // AnalyzeSessionTranscriptImpl's plan-parent link, restated line for line: "read the full
        // transcript at:" (ASCII-case-insensitive) in the STRING content, then spaces/tabs, then a
        // token up to whitespace; a token of 6+ characters ending in ".jsonl" names the parent by its
        // basename without the extension. "" when absent.
        std::wstring PlanParentLink(const std::wstring& contentStr)
        {
            if (contentStr.empty())
            {
                return {};
            }
            const std::wstring lc = AsciiLower(contentStr);
            const auto mk = lc.find(L"read the full transcript at:");
            if (mk == std::wstring::npos)
            {
                return {};
            }
            size_t p = mk + 28; // len("read the full transcript at:")
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
            if (tok.size() < 6)
            {
                return {};
            }
            const std::wstring tl = AsciiLower(tok);
            if (tl.rfind(L".jsonl") != tl.size() - 6)
            {
                return {};
            }
            std::wstring stem = Basename(tok);
            stem.resize(stem.size() - 6);
            return stem;
        }

        // The text of a tool_result's content: a string, or its text blocks joined with '\n'.
        std::wstring ToolResultText(const json::Value* rc)
        {
            if (!rc)
            {
                return {};
            }
            if (rc->type == json::Value::Type::Str)
            {
                return rc->str;
            }
            std::wstring out;
            if (rc->type == json::Value::Type::Arr)
            {
                for (const auto& rb : rc->arr)
                {
                    if (rb.type == json::Value::Type::Obj && rb.StrAt(L"type") == L"text")
                    {
                        if (!out.empty())
                        {
                            out.push_back(L'\n');
                        }
                        out += rb.StrAt(L"text");
                    }
                }
            }
            return out;
        }

        // The created-file marker the analyzer looks for, over the CONCATENATED text blocks (it joins
        // them with no separator — so a marker split across two blocks still counts there; restated).
        bool ResultSaysCreated(const json::Value* rc)
        {
            if (!rc)
            {
                return false;
            }
            std::wstring res;
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
            return res.find(L"File created successfully at:") != std::wstring::npos;
        }

        // The operation a tool's path stands for (path_access.op).
        uint8_t OpForTool(const std::wstring& name)
        {
            if (name == L"Read" || name == L"NotebookRead")
            {
                return kOpRead;
            }
            if (name == L"Edit" || name == L"MultiEdit" || name == L"NotebookEdit")
            {
                return kOpEdit;
            }
            if (name == L"Write")
            {
                return kOpWrite;
            }
            if (name == L"Grep" || name == L"Glob" || name == L"LS")
            {
                return kOpSearch;
            }
            return kOpOther;
        }

        // A JSON number member as int64, tolerating a numeric string (some older lines quote them).
        int64_t I64Of(const json::Value& obj, std::wstring_view key)
        {
            const auto* v = obj.Find(key);
            if (!v)
            {
                return 0;
            }
            if (v->type == json::Value::Type::Num)
            {
                return static_cast<int64_t>(v->num);
            }
            if (v->type == json::Value::Type::Str)
            {
                return _wtoi64(v->str.c_str());
            }
            return 0;
        }
    }

    const char* RoleName(uint8_t role) noexcept
    {
        switch (role)
        {
        case kRolePrompt:
            return "prompt";
        case kRoleReply:
            return "reply";
        case kRoleThinking:
            return "thinking";
        case kRoleToolCall:
            return "tool_call";
        case kRoleToolResult:
            return "tool_result";
        case kRoleSystem:
            return "system";
        case kRoleQueued:
            return "queued";
        default:
            return "other";
        }
    }

    const char* PathOpName(uint8_t op) noexcept
    {
        switch (op)
        {
        case kOpRead:
            return "read";
        case kOpEdit:
            return "edit";
        case kOpWrite:
            return "write";
        case kOpSearch:
            return "search";
        case kOpShell:
            return "shell";
        default:
            return "other";
        }
    }

    LineFacts ExtractLine(std::string_view rawUtf8, bool subagent)
    {
        LineFacts f;
        while (!rawUtf8.empty() && rawUtf8.back() == '\r')
        {
            rawUtf8.remove_suffix(1);
        }
        if (rawUtf8.empty() || rawUtf8.front() != '{')
        {
            return f; // not a JSON object line: stored verbatim, nothing to classify
        }
        // Drop pasted-image base64 runs before widening and parsing: no extracted field reads them, and
        // an image-heavy line otherwise costs several times its size in transient allocations (the
        // analyzer's own footprint rule, ScrubLargeBase64Payloads). The raw line is stored untouched by
        // the caller; only this parse sees the scrubbed copy.
        const std::wstring wide = Widen(ScrubLargeBase64Payloads(std::string{ rawUtf8 }));
        const auto parsed = json::Parse(wide);
        if (!parsed || parsed->type != json::Value::Type::Obj)
        {
            return f;
        }
        const json::Value& obj = *parsed;
        f.json = true;

        // ----- envelope -----
        f.type = obj.StrAt(L"type");
        f.subtype = obj.StrAt(L"subtype");
        f.uuid = obj.StrAt(L"uuid");
        f.parentUuid = obj.StrAt(L"parentUuid");
        f.leafUuid = obj.StrAt(L"leafUuid");
        f.userType = obj.StrAt(L"userType");
        f.cwd = obj.StrAt(L"cwd");
        f.branch = obj.StrAt(L"gitBranch");
        f.version = obj.StrAt(L"version");
        f.entrypoint = obj.StrAt(L"entrypoint");
        if (const auto* fk = obj.Find(L"forkedFrom"); fk && fk->type == json::Value::Type::Obj)
        {
            f.forkedFrom = fk->StrAt(L"sessionId");
        }
        if (const auto* ts = obj.Find(L"timestamp"))
        {
            if (ts->type == json::Value::Type::Str)
            {
                f.tsMs = ParseTranscriptTimestamp(ts->str);
            }
            else if (ts->type == json::Value::Type::Num)
            {
                // The oldest strata wrote epoch numbers (>= 1e11 = ms, else seconds) — TranscriptStore's rule.
                const double n = ts->num;
                f.tsMs = n >= 1e11 ? static_cast<int64_t>(n) : (n >= 1e9 ? static_cast<int64_t>(n * 1000.0) : 0);
            }
        }
        const bool sidechain = obj.BoolAt(L"isSidechain");
        const bool isMeta = obj.BoolAt(L"isMeta");
        const bool compactSummary = obj.BoolAt(L"isCompactSummary");
        const bool apiError = obj.BoolAt(L"isApiErrorMessage");
        if (sidechain)
        {
            f.flags |= kLfSidechain;
        }
        if (isMeta || compactSummary)
        {
            f.flags |= kLfMeta;
        }
        if (apiError)
        {
            f.flags |= kLfApiError;
        }
        if (!f.forkedFrom.empty())
        {
            f.flags |= kLfForkCopy;
        }
        const bool isUser = f.type == L"user";
        const bool isAssistant = f.type == L"assistant";
        const bool isSystem = f.type == L"system";
        const bool isAttachment = f.type == L"attachment";
        if (isUser && f.userType == L"external")
        {
            f.flags |= kLfUserExternal;
        }
        if (isSystem && f.subtype == L"compact_boundary")
        {
            f.flags |= kLfCompactBoundary;
        }
        // A COMPACTION ROOT (TranscriptStore::ActiveBranchUuids' rule): a null parent on a boundary, or on
        // any line that declares a logical parent. Its crossing edges (the logical parent, every preserved
        // message, the preserved segment's head and tail) are kept for the walk; the pre-boundary leaf
        // edge is positional and the walk computes it.
        if (!f.uuid.empty() && f.parentUuid.empty())
        {
            const std::wstring lp = obj.StrAt(L"logicalParentUuid");
            if (!lp.empty() || (f.flags & kLfCompactBoundary))
            {
                f.flags |= kLfCompactRoot;
                if (!lp.empty())
                {
                    f.extraEdges.push_back(lp);
                }
                if (const auto* md = obj.Find(L"compactMetadata"); md && md->type == json::Value::Type::Obj)
                {
                    if (const auto* pm = md->Find(L"preservedMessages"); pm && pm->type == json::Value::Type::Obj)
                    {
                        for (const wchar_t* key : { L"uuids", L"allUuids" })
                        {
                            if (const auto* list = pm->Find(key); list && list->type == json::Value::Type::Arr)
                            {
                                for (const auto& item : list->arr)
                                {
                                    if (item.type == json::Value::Type::Str && !item.str.empty())
                                    {
                                        f.extraEdges.push_back(item.str);
                                    }
                                }
                            }
                        }
                    }
                    if (const auto* ps = md->Find(L"preservedSegment"); ps && ps->type == json::Value::Type::Obj)
                    {
                        for (const wchar_t* key : { L"headUuid", L"tailUuid" })
                        {
                            if (std::wstring v = ps->StrAt(key); !v.empty())
                            {
                                f.extraEdges.push_back(std::move(v));
                            }
                        }
                    }
                }
            }
        }

        const uint8_t blockFlagsBase = static_cast<uint8_t>((sidechain ? kDfSidechain : 0) | (subagent ? kDfSubagent : 0) |
                                                            (apiError ? kDfError : 0) | (f.forkedFrom.empty() ? 0 : kDfForkCopy));
        uint16_t blockIndex = 0;
        const auto addBlock = [&](uint8_t role, uint8_t flags, std::wstring text, std::wstring tool = {}, std::wstring toolUseId = {}) {
            if (text.empty())
            {
                return;
            }
            BlockRec b;
            b.block = blockIndex++;
            b.role = role;
            b.flags = static_cast<uint8_t>(blockFlagsBase | flags);
            b.tool = std::move(tool);
            b.toolUseId = std::move(toolUseId);
            b.text = std::move(text);
            f.blocks.push_back(std::move(b));
        };

        // ----- the summary's prompt rule (the app's own function: typed and queued prompts) -----
        if (!subagent)
        {
            if (std::wstring p = SummaryPromptOfLine(obj); !p.empty())
            {
                f.summaryPromptKind = isAttachment ? 2 : 1;
                f.summaryPrompt = std::move(p);
            }
            f.compactLabel = SummaryCompactionLabelOfLine(obj);
        }

        // ----- per type -----
        if (isUser)
        {
            const auto* msg = obj.Find(L"message");
            const auto* content = (msg && msg->type == json::Value::Type::Obj) ? msg->Find(L"content") : nullptr;
            bool toolResultLine = false;
            std::wstring prompt;
            std::wstring firstText; // the string content, or the first text block (the interrupt test)
            if (content && content->type == json::Value::Type::Str)
            {
                prompt = content->str;
                firstText = content->str;
            }
            else if (content && content->type == json::Value::Type::Arr)
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
                        toolResultLine = true;
                        const auto* rc = blk.Find(L"content");
                        ToolResultRec rr;
                        rr.id = blk.StrAt(L"tool_use_id");
                        rr.created = ResultSaysCreated(rc);
                        rr.error = blk.BoolAt(L"is_error");
                        // The output is searchable (the 🤖 scope); its tool name is the caller's fold's job.
                        addBlock(kRoleToolResult, rr.error ? kDfError : 0, ToolResultText(rc), {}, rr.id);
                        f.toolResults.push_back(std::move(rr));
                    }
                    else if (bt == L"text")
                    {
                        const std::wstring t = blk.StrAt(L"text");
                        if (firstText.empty())
                        {
                            firstText = t;
                        }
                        prompt += t; // ClassifyTranscriptLine joins the text blocks with nothing between
                    }
                    else if (bt == L"image")
                    {
                        ++f.imagesPasted;
                    }
                }
            }
            f.interruptMarker = firstText.rfind(L"[Request interrupted", 0) == 0; // Esc's marker (both variants)
            if (!toolResultLine && !subagent)
            {
                // The analyzer's first-external-user facts (planContent, the plan-parent link) — read
                // here for every external user line; the summary SQL takes the FIRST on the live branch.
                if (f.flags & kLfUserExternal)
                {
                    if (const auto* pc = obj.Find(L"planContent");
                        pc && pc->type != json::Value::Type::Null && !(pc->type == json::Value::Type::Bool && !pc->boolean) &&
                        !(pc->type == json::Value::Type::Str && pc->str.empty()))
                    {
                        f.planContent = true;
                    }
                    if (content && content->type == json::Value::Type::Str)
                    {
                        f.parentLink = PlanParentLink(content->str);
                    }
                }
            }
            else if (!subagent && (f.flags & kLfUserExternal))
            {
                // A tool-result line can still be the first external user line (the analyzer reads its
                // planContent too); its content is not a string, so no parent link.
                if (const auto* pc = obj.Find(L"planContent");
                    pc && pc->type != json::Value::Type::Null && !(pc->type == json::Value::Type::Bool && !pc->boolean) &&
                    !(pc->type == json::Value::Type::Str && pc->str.empty()))
                {
                    f.planContent = true;
                }
            }
            if (!toolResultLine)
            {
                // ClassifyTranscriptLine's prompt rules: meta (isMeta / isCompactSummary) and noise markers
                // are not typed prompts. Kept searchable as SYSTEM text flagged so (excluded from the
                // prompt scope by default), never dropped: the lossless store keeps them anyway.
                const bool noise = !(isMeta || compactSummary) && IsNoiseUserPrompt(prompt);
                if (isMeta || compactSummary)
                {
                    addBlock(kRoleSystem, static_cast<uint8_t>(kDfMeta | (compactSummary ? kDfCompactSummary : 0)), prompt);
                }
                else if (noise)
                {
                    addBlock(kRoleSystem, kDfNoise, prompt);
                }
                else
                {
                    addBlock(kRolePrompt, 0, prompt);
                    if (!sidechain && !prompt.empty())
                    {
                        f.typedPrompt = true;
                        f.promptText = prompt;
                    }
                }
            }
        }
        else if (isAssistant)
        {
            const auto* msg = obj.Find(L"message");
            const json::Value* content = nullptr;
            if (msg && msg->type == json::Value::Type::Obj)
            {
                content = msg->Find(L"content");
                const std::wstring model = msg->StrAt(L"model");
                if (model != L"<synthetic>")
                {
                    f.model = model;
                }
                f.stopReason = msg->StrAt(L"stop_reason");
                if (const auto* usage = msg->Find(L"usage"); usage && usage->type == json::Value::Type::Obj)
                {
                    const int64_t in = usage->I64At(L"input_tokens");
                    const int64_t out = usage->I64At(L"output_tokens");
                    const int64_t cr = usage->I64At(L"cache_read_input_tokens");
                    const int64_t cw = usage->I64At(L"cache_creation_input_tokens");
                    // ClassifyTranscriptLine's context measure (the Ctx column): all four.
                    f.contextTokens = in + cw + cr + out;
                    f.inputContextTokens = in + cr + cw;
                    const std::wstring msgId = msg->StrAt(L"id");
                    if (!msgId.empty() && !f.model.empty())
                    {
                        UsageRec u;
                        u.messageId = msgId;
                        u.model = f.model;
                        u.inputTokens = in;
                        u.outputTokens = out;
                        u.cacheReadTokens = cr;
                        u.cacheWriteTokens = cw;
                        if (const auto* st = usage->Find(L"server_tool_use"); st && st->type == json::Value::Type::Obj)
                        {
                            u.webSearches = static_cast<int32_t>(st->I64At(L"web_search_requests"));
                            u.webFetches = static_cast<int32_t>(st->I64At(L"web_fetch_requests"));
                        }
                        f.usage = std::move(u);
                    }
                }
            }
            f.effort = obj.StrAt(L"effort");
            f.attributionSkill = obj.StrAt(L"attributionSkill");
            f.attributionMcpServer = obj.StrAt(L"attributionMcpServer");
            f.abortedMidStream = obj.BoolAt(L"isAbortedMidStream");
            if (apiError)
            {
                f.apiErrorStatus = obj.StrAt(L"apiErrorStatus");
                if (f.apiErrorStatus.empty())
                {
                    if (const auto* st = obj.Find(L"apiErrorStatus"); st && st->type == json::Value::Type::Num)
                    {
                        f.apiErrorStatus = std::to_wstring(static_cast<int64_t>(st->num));
                    }
                }
            }
            if (content && content->type == json::Value::Type::Arr)
            {
                for (const auto& blk : content->arr)
                {
                    if (blk.type != json::Value::Type::Obj)
                    {
                        continue;
                    }
                    const std::wstring bt = blk.StrAt(L"type");
                    if (bt == L"text")
                    {
                        std::wstring t = blk.StrAt(L"text");
                        if (!f.replyText.empty() && !t.empty())
                        {
                            f.replyText.push_back(L'\n');
                        }
                        f.replyText += t;
                        addBlock(kRoleReply, 0, std::move(t));
                    }
                    else if (bt == L"thinking")
                    {
                        addBlock(kRoleThinking, 0, blk.StrAt(L"thinking"));
                    }
                    else if (bt == L"tool_use")
                    {
                        ToolUseRec tu;
                        tu.id = blk.StrAt(L"id");
                        tu.name = blk.StrAt(L"name");
                        f.toolNames.push_back(tu.name);
                        if (tu.name == L"Task" || tu.name == L"Agent")
                        {
                            ++f.subagentsSpawned;
                        }
                        const auto* in = blk.Find(L"input");
                        std::wstring text = tu.name;
                        if (in && in->type == json::Value::Type::Obj)
                        {
                            text += L"\n";
                            text += json::Dump(*in);
                            tu.path = in->StrAt(L"file_path");
                            tu.skill = in->StrAt(L"skill");
                            if (tu.name == L"TodoWrite")
                            {
                                if (const auto* todos = in->Find(L"todos"); todos && todos->type == json::Value::Type::Arr)
                                {
                                    int16_t done = 0, open = 0;
                                    for (const auto& t : todos->arr)
                                    {
                                        if (t.type != json::Value::Type::Obj)
                                        {
                                            continue;
                                        }
                                        const std::wstring st = t.StrAt(L"status");
                                        if (st == L"completed")
                                        {
                                            ++done;
                                        }
                                        else if (st == L"pending" || st == L"in_progress")
                                        {
                                            ++open;
                                        }
                                    }
                                    tu.todosDone = done;
                                    tu.todosOpen = open;
                                }
                            }
                            // Tool-touched paths, ClassifyTranscriptLine's rule (the same keys, the same 8
                            // per line, each <= 512 characters), each tagged with the tool's operation; then
                            // the absolute paths mined from a shell command (op shell) up to the same cap.
                            const uint8_t op = OpForTool(tu.name);
                            for (const auto key : { L"file_path", L"notebook_path", L"path" })
                            {
                                if (f.paths.size() >= 8)
                                {
                                    break;
                                }
                                const std::wstring p = in->StrAt(key);
                                if (!p.empty() && p.size() <= 512)
                                {
                                    f.paths.push_back(PathRec{ p, op });
                                }
                            }
                            if (f.paths.size() < 8)
                            {
                                const std::wstring cmd = in->StrAt(L"command");
                                if (!cmd.empty())
                                {
                                    for (auto& mined : ExtractPathsFromText(cmd, 8 - f.paths.size()))
                                    {
                                        f.paths.push_back(PathRec{ std::move(mined), kOpShell });
                                    }
                                }
                            }
                        }
                        if (!tu.path.empty())
                        {
                            tu.label = SeSummaryFileLabel(tu.path);
                            tu.plans = IsPlansPath(tu.path);
                        }
                        addBlock(kRoleToolCall, 0, std::move(text), tu.name, tu.id);
                        f.toolUses.push_back(std::move(tu));
                    }
                }
            }
            else if (content && content->type == json::Value::Type::Str)
            {
                f.replyText = content->str;
                addBlock(kRoleReply, 0, content->str); // the oldest strata: plain string content
            }
        }
        else if (isSystem)
        {
            std::wstring text = obj.StrAt(L"content");
            if (f.subtype == L"away_summary" && !subagent)
            {
                f.recap = NormalizeRecapText(text);
            }
            else if (f.subtype == L"turn_duration")
            {
                f.turnDurationMs = I64Of(obj, L"durationMs");
            }
            else if (f.subtype == L"stop_hook_summary")
            {
                int64_t total = 0, mx = 0;
                if (const auto* hooks = obj.Find(L"hookInfos"); hooks && hooks->type == json::Value::Type::Arr)
                {
                    for (const auto& h : hooks->arr)
                    {
                        if (h.type == json::Value::Type::Obj)
                        {
                            const int64_t d = I64Of(h, L"durationMs");
                            total += d;
                            mx = std::max(mx, d);
                        }
                    }
                }
                f.stopHookMsTotal = total;
                f.stopHookMsMax = mx;
            }
            else if (f.subtype == L"compact_boundary")
            {
                if (const auto* md = obj.Find(L"compactMetadata"); md && md->type == json::Value::Type::Obj)
                {
                    f.compactTrigger = md->StrAt(L"trigger");
                    f.compactPreTokens = I64Of(*md, L"preTokens");
                    const int64_t post = I64Of(*md, L"postTokens");
                    if (post > 0)
                    {
                        f.contextTokens = post; // the context the compaction left (the Ctx column's rule)
                    }
                }
            }
            addBlock(kRoleSystem, 0, std::move(text));
        }
        else if (isAttachment)
        {
            if (f.summaryPromptKind == 2)
            {
                f.queuedPrompt = true;
                addBlock(kRoleQueued, 0, f.summaryPrompt);
            }
            if (const auto* att = obj.Find(L"attachment"); att && att->type == json::Value::Type::Obj && att->StrAt(L"type") == L"environment")
            {
                if (const auto* snap = att->Find(L"snapshot"); snap && snap->type == json::Value::Type::Obj)
                {
                    f.environment = true;
                    f.envIsGitRepo = snap->BoolAt(L"isGitRepo");
                    f.envIsWorktree = snap->BoolAt(L"isWorktree");
                    f.envShell = snap->StrAt(L"shell");
                    if (const auto* extra = snap->Find(L"additionalWorkingDirectories"); extra && extra->type == json::Value::Type::Arr)
                    {
                        f.envExtraDirs = static_cast<int32_t>(extra->arr.size());
                    }
                }
            }
        }
        else if (f.type == L"custom-title")
        {
            f.customTitle = FirstLineTrim(obj.StrAt(L"customTitle"));
        }
        else if (f.type == L"ai-title")
        {
            f.aiTitle = FirstLineTrim(obj.StrAt(L"aiTitle"));
        }
        else if (f.type == L"summary")
        {
            f.legacySummary = FirstLineTrim(obj.StrAt(L"summary"));
        }
        else if (f.type == L"agent-name")
        {
            f.agentName = obj.StrAt(L"agentName");
        }
        else if (f.type == L"permission-mode")
        {
            f.permissionMode = obj.StrAt(L"permissionMode");
        }
        else if (f.type == L"mode")
        {
            f.mode = obj.StrAt(L"mode");
        }
        else if (f.type == L"relocated")
        {
            f.relocatedCwd = obj.StrAt(L"relocatedCwd");
        }
        else if (f.type == L"cost-state")
        {
            f.costState = true;
            f.costUsd = obj.NumAt(L"totalCostUSD");
            f.apiMs = I64Of(obj, L"totalAPIDuration");
            f.apiMsNoRetries = I64Of(obj, L"totalAPIDurationWithoutRetries");
            f.toolMs = I64Of(obj, L"totalToolDuration");
            f.wallMs = I64Of(obj, L"totalDuration");
            f.linesAdded = I64Of(obj, L"totalLinesAdded");
            f.linesRemoved = I64Of(obj, L"totalLinesRemoved");
            f.costUnknownModel = obj.BoolAt(L"hasUnknownModelCost");
        }
        else if (f.type == L"worktree-state")
        {
            if (const auto* ws = obj.Find(L"worktreeSession"); ws && ws->type == json::Value::Type::Obj)
            {
                f.worktree = true;
                f.worktreePath = ws->StrAt(L"worktreePath");
                f.worktreeName = ws->StrAt(L"worktreeName");
                f.worktreeBranch = ws->StrAt(L"worktreeBranch");
                f.worktreeOriginalCwd = ws->StrAt(L"originalCwd");
            }
        }
        else if (f.type == L"frame-link")
        {
            f.frameLink = true;
            f.frameTitle = obj.StrAt(L"title");
            f.frameUrl = obj.StrAt(L"frameUrl");
        }
        return f;
    }

    std::optional<BlockRec> ExtractSpillBlock(std::string_view bytes, std::wstring_view stem)
    {
        // A binary spill (an image, an archive) is stored for the lossless store but never indexed: a NUL
        // in its first 4 KB, or bytes that are not UTF-8, mean it is not text.
        const std::string_view head = bytes.substr(0, std::min<size_t>(bytes.size(), 4096));
        if (head.find('\0') != std::string_view::npos)
        {
            return std::nullopt;
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (n <= 0 && !bytes.empty())
        {
            return std::nullopt;
        }
        BlockRec b;
        b.block = 0;
        b.role = kRoleToolResult;
        b.flags = kDfSpill;
        if (stem.substr(0, 6) == L"toolu_")
        {
            b.toolUseId = std::wstring{ stem };
        }
        b.text = Widen(bytes);
        if (b.text.empty())
        {
            return std::nullopt;
        }
        return b;
    }
}
