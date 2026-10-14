// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (vcxproj marks it NotUsing).
// The SDK-free half of the agent harness; see AgentHarnessCore.h and SDK_HARNESS.md.
#include "AgentHarnessCore.h"

#include "ClaudeSpawn.h" // AgentmasterStateDir, AppendStateLog, LogSwallowedException
#include "Json.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace
{
    using namespace Agentmaster;

    // Cut `text` to at most `maxChars` UTF-16 units. A cut text ends in "..." (inside the limit),
    // and a surrogate pair is never split.
    std::wstring CutText(std::wstring text, size_t maxChars)
    {
        if (text.size() <= maxChars)
        {
            return text;
        }
        size_t keep = maxChars > 3 ? maxChars - 3 : 0;
        // Do not end on the first half of a surrogate pair: the lone half is not valid text.
        if (keep > 0 && text[keep - 1] >= 0xD800 && text[keep - 1] <= 0xDBFF)
        {
            --keep;
        }
        text.resize(keep);
        text += L"...";
        return text;
    }

    // One line of digest text: line breaks and tabs become single spaces, runs of spaces collapse,
    // and the result is cut to `maxChars`. A digest is a list of lines; a multi-line message must
    // not look like several entries.
    std::wstring OneLine(std::wstring_view text, size_t maxChars)
    {
        std::wstring out;
        out.reserve((std::min)(text.size(), maxChars + 8));
        bool lastSpace = true; // drops leading whitespace
        for (const wchar_t c : text)
        {
            const bool space = c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
            if (space)
            {
                if (!lastSpace)
                {
                    out.push_back(L' ');
                }
                lastSpace = true;
                continue;
            }
            out.push_back(c);
            lastSpace = false;
        }
        while (!out.empty() && out.back() == L' ')
        {
            out.pop_back();
        }
        return CutText(std::move(out), maxChars);
    }

    const wchar_t* KindToken(HarnessItemKind k) noexcept
    {
        switch (k)
        {
        case HarnessItemKind::User:
            return L"user";
        case HarnessItemKind::Assistant:
            return L"assistant";
        case HarnessItemKind::ToolCall:
            return L"tool";
        case HarnessItemKind::ToolResult:
            return L"result";
        case HarnessItemKind::Notice:
        default:
            return L"notice";
        }
    }

    std::optional<HarnessItemKind> KindFromToken(std::wstring_view t) noexcept
    {
        if (t == L"user")
        {
            return HarnessItemKind::User;
        }
        if (t == L"assistant")
        {
            return HarnessItemKind::Assistant;
        }
        if (t == L"tool")
        {
            return HarnessItemKind::ToolCall;
        }
        if (t == L"result")
        {
            return HarnessItemKind::ToolResult;
        }
        if (t == L"notice")
        {
            return HarnessItemKind::Notice;
        }
        return std::nullopt;
    }

    // The atomic-write recipe of the engine's other persistence writers (Persistence.cpp
    // WriteAllUtf8): full bytes to a sibling temp file, flush, then an atomic replace — a crash
    // leaves the old file or the new one, never a torn one.
    //   returns — true when `path` now holds `content`.
    //   throws  — std::bad_alloc (the caller is noexcept and reports it).
    bool WriteFileAtomicUtf8(const std::wstring& path, std::wstring_view content)
    {
        const std::string bytes = HarnessToUtf8(content);
        const std::wstring tmp = path + L".tmp." + std::to_wstring(::GetCurrentThreadId());
        const HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD wrote = 0;
        const BOOL ok = bytes.empty() ? TRUE : ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
        if (ok)
        {
            ::FlushFileBuffers(h); // the data must be on disk before the rename can commit it
        }
        ::CloseHandle(h);
        if (!ok || wrote != bytes.size())
        {
            ::DeleteFileW(tmp.c_str());
            return false;
        }
        if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            ::DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }

    // `c` in lower case, for ASCII letters only (the words compared here are ASCII).
    constexpr wchar_t AsciiLower(wchar_t c) noexcept
    {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
    }

    // True when `a` and `b` hold the same text without regard to ASCII case.
    bool EqualsAsciiNoCase(std::wstring_view a, std::wstring_view b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (AsciiLower(a[i]) != AsciiLower(b[i]))
            {
                return false;
            }
        }
        return true;
    }

    // The levels in rotation order: index == the enum's value. One table, so that the word, the
    // parse and the rotation can never disagree.
    constexpr Agentmaster::HarnessThinkingLevel kThinkingLevels[] = {
        Agentmaster::HarnessThinkingLevel::Off,
        Agentmaster::HarnessThinkingLevel::Low,
        Agentmaster::HarnessThinkingLevel::Medium,
        Agentmaster::HarnessThinkingLevel::High,
        Agentmaster::HarnessThinkingLevel::XHigh,
        Agentmaster::HarnessThinkingLevel::Max,
    };
}

namespace Agentmaster
{
    // ===== text conversion ==================================================================

    std::string HarnessToUtf8(std::wstring_view text)
    {
        if (text.empty())
        {
            return {};
        }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0)
        {
            return {};
        }
        std::string out(static_cast<size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    std::wstring HarnessFromUtf8(std::string_view text)
    {
        if (text.empty())
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (n <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
        return out;
    }

    // ===== the record =======================================================================

    void AppendHarnessItem(HarnessRecord& record, HarnessItem item)
    {
        item.text = CutText(std::move(item.text), item.kind == HarnessItemKind::ToolResult ? kHarnessMaxToolResultChars : kHarnessMaxItemTextChars);
        item.argumentsJson = CutText(std::move(item.argumentsJson), kHarnessMaxItemTextChars);
        if (item.atUnixMs > record.updatedUnixMs)
        {
            record.updatedUnixMs = item.atUnixMs;
        }
        record.items.push_back(std::move(item));
        if (record.items.size() > kHarnessMaxItems)
        {
            // Drop the OLDEST items. A turn may lose its first lines this way; the digest and the
            // UI both read whatever is left, so nothing depends on a turn being whole.
            record.items.erase(record.items.begin(), record.items.begin() + static_cast<std::ptrdiff_t>(record.items.size() - kHarnessMaxItems));
        }
    }

    std::wstring SerializeHarnessRecord(const HarnessRecord& record)
    {
        using json::Value;
        Value root = Value::MkObj();
        root.Set(L"version", Value::MkNum(1));
        root.Set(L"name", Value::MkStr(record.name));
        root.Set(L"model", Value::MkStr(record.model));
        root.Set(L"created", Value::MkNum(static_cast<double>(record.createdUnixMs)));
        root.Set(L"updated", Value::MkNum(static_cast<double>(record.updatedUnixMs)));
        root.Set(L"turns", Value::MkNum(static_cast<double>(record.turns)));
        root.Set(L"compactions", Value::MkNum(record.compactions));

        Value usage = Value::MkObj();
        usage.Set(L"contextTokens", Value::MkNum(static_cast<double>(record.usage.contextTokens)));
        usage.Set(L"contextWindow", Value::MkNum(static_cast<double>(record.usage.contextWindow)));
        usage.Set(L"input", Value::MkNum(static_cast<double>(record.usage.inputTokens)));
        usage.Set(L"output", Value::MkNum(static_cast<double>(record.usage.outputTokens)));
        usage.Set(L"cacheRead", Value::MkNum(static_cast<double>(record.usage.cacheReadTokens)));
        usage.Set(L"cacheCreation", Value::MkNum(static_cast<double>(record.usage.cacheCreationTokens)));
        usage.Set(L"costUsd", Value::MkNum(record.usage.costUsd));
        root.Set(L"usage", std::move(usage));

        Value items = Value::MkArr();
        for (const auto& it : record.items)
        {
            Value o = Value::MkObj();
            o.Set(L"k", Value::MkStr(KindToken(it.kind)));
            o.Set(L"t", Value::MkNum(static_cast<double>(it.turn)));
            o.Set(L"at", Value::MkNum(static_cast<double>(it.atUnixMs)));
            // Only the fields a kind uses are written: the file stays small and readable.
            if (!it.text.empty())
            {
                o.Set(L"text", Value::MkStr(it.text));
            }
            if (!it.toolName.empty())
            {
                o.Set(L"name", Value::MkStr(it.toolName));
            }
            if (!it.argumentsJson.empty())
            {
                o.Set(L"args", Value::MkStr(it.argumentsJson));
            }
            if (it.isError)
            {
                o.Set(L"err", Value::MkBool(true));
            }
            items.Push(std::move(o));
        }
        root.Set(L"items", std::move(items));
        return json::Dump(root);
    }

    HarnessRecord DeserializeHarnessRecord(std::wstring_view text)
    {
        HarnessRecord record;
        const auto parsed = json::Parse(text);
        if (!parsed || parsed->type != json::Value::Type::Obj || parsed->I64At(L"version") != 1)
        {
            return record; // not a version-1 record: "no conversation"
        }
        HarnessRecord out;
        out.name = parsed->StrAt(L"name");
        out.model = parsed->StrAt(L"model");
        out.createdUnixMs = parsed->I64At(L"created");
        out.updatedUnixMs = parsed->I64At(L"updated");
        out.turns = static_cast<uint64_t>((std::max)(int64_t{ 0 }, parsed->I64At(L"turns")));
        out.compactions = static_cast<int>(parsed->I64At(L"compactions"));
        if (const auto* u = parsed->Find(L"usage"); u && u->type == json::Value::Type::Obj)
        {
            out.usage.contextTokens = u->I64At(L"contextTokens");
            out.usage.contextWindow = u->I64At(L"contextWindow");
            out.usage.inputTokens = u->I64At(L"input");
            out.usage.outputTokens = u->I64At(L"output");
            out.usage.cacheReadTokens = u->I64At(L"cacheRead");
            out.usage.cacheCreationTokens = u->I64At(L"cacheCreation");
            out.usage.costUsd = u->NumAt(L"costUsd");
        }
        if (const auto* arr = parsed->Find(L"items"); arr && arr->type == json::Value::Type::Arr)
        {
            for (const auto& o : arr->arr)
            {
                if (o.type != json::Value::Type::Obj)
                {
                    continue; // an entry that is not an object is skipped, the rest is kept
                }
                const auto kind = KindFromToken(o.StrAt(L"k"));
                if (!kind)
                {
                    continue; // a kind a newer version wrote: skip it rather than misread it
                }
                HarnessItem it;
                it.kind = *kind;
                it.turn = static_cast<uint64_t>((std::max)(int64_t{ 0 }, o.I64At(L"t")));
                it.atUnixMs = o.I64At(L"at");
                it.text = o.StrAt(L"text");
                it.toolName = o.StrAt(L"name");
                it.argumentsJson = o.StrAt(L"args");
                it.isError = o.BoolAt(L"err");
                // Through AppendHarnessItem so a hand-edited or oversized file is capped like a
                // live record. It must not move `updated`: restore the stored value afterwards.
                AppendHarnessItem(out, std::move(it));
            }
            out.updatedUnixMs = parsed->I64At(L"updated");
        }
        return out;
    }

    std::wstring BuildHarnessDigest(const HarnessRecord& record, size_t maxTurns, size_t maxChars)
    {
        if (maxTurns == 0 || maxChars == 0)
        {
            return {};
        }
        // One text block per turn that still has its user message, oldest first.
        std::vector<std::pair<uint64_t, std::wstring>> blocks;
        for (const auto& it : record.items)
        {
            if (it.turn == 0)
            {
                continue; // a notice outside any turn says nothing about the conversation
            }
            std::wstring line;
            switch (it.kind)
            {
            case HarnessItemKind::User:
                line = L"user: " + OneLine(it.text, 600);
                break;
            case HarnessItemKind::Assistant:
                line = L"assistant: " + OneLine(it.text, 600);
                break;
            case HarnessItemKind::ToolCall:
                line = L"tool " + it.toolName + L" " + OneLine(it.argumentsJson, 400);
                break;
            case HarnessItemKind::ToolResult: // large, and the model can call the tool again
            case HarnessItemKind::Notice:
            default:
                continue;
            }
            if (blocks.empty() || blocks.back().first != it.turn)
            {
                if (it.kind != HarnessItemKind::User)
                {
                    continue; // the turn's user message was dropped by the item cap: skip the rest of it
                }
                blocks.emplace_back(it.turn, std::wstring{});
            }
            auto& block = blocks.back().second;
            if (!block.empty())
            {
                block += L"\n";
            }
            block += line;
        }
        if (blocks.empty())
        {
            return {};
        }
        if (blocks.size() > maxTurns)
        {
            blocks.erase(blocks.begin(), blocks.end() - static_cast<std::ptrdiff_t>(maxTurns));
        }
        const auto total = [&blocks]() {
            size_t n = 0;
            for (const auto& b : blocks)
            {
                n += b.second.size() + 1; // + the line break between blocks
            }
            return n;
        };
        // Too long: forget the OLDEST turns first — the newest are what a follow-up refers to.
        while (blocks.size() > 1 && total() > maxChars)
        {
            blocks.erase(blocks.begin());
        }
        std::wstring digest;
        for (const auto& b : blocks)
        {
            if (!digest.empty())
            {
                digest += L"\n";
            }
            digest += b.second;
        }
        return CutText(std::move(digest), maxChars); // a single turn can still be too long
    }

    std::wstring ComposeColdStartMessage(std::wstring_view digest, std::wstring_view userText)
    {
        if (digest.empty())
        {
            return std::wstring{ userText };
        }
        std::wstring out;
        out.reserve(digest.size() + userText.size() + 260);
        out += L"[Restored context. This process is new and has not seen the conversation so far. The last turns, oldest first:]\n";
        out += digest;
        out += L"\n[End of restored context. The user's new message follows.]\n\n";
        out += userText;
        return out;
    }

    bool IsValidHarnessName(std::wstring_view name) noexcept
    {
        if (name.empty() || name.size() > 48)
        {
            return false;
        }
        for (const wchar_t c : name)
        {
            const bool ok = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-';
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    std::wstring HarnessStateDir()
    {
        return AgentmasterStateDir() + L"\\harness";
    }

    std::wstring HarnessRecordPathIn(const std::wstring& dir, std::wstring_view name)
    {
        return dir + L"\\" + std::wstring{ name } + L".json";
    }

    bool SaveHarnessRecordTo(const std::wstring& path, const HarnessRecord& record) noexcept
    {
        try
        {
            if (record.Empty())
            {
                return DeleteHarnessRecordAt(path);
            }
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path{ path }.parent_path(), ec); // an existing folder is not an error
            if (WriteFileAtomicUtf8(path, SerializeHarnessRecord(record)))
            {
                return true;
            }
            AppendStateLog(L"hooks.log", L"[persist-fail] " + path + L" (the harness record was NOT saved)\n");
            return false;
        }
        catch (...)
        {
            LogSwallowedException(L"SaveHarnessRecordTo");
            return false;
        }
    }

    HarnessRecord LoadHarnessRecordFrom(const std::wstring& path) noexcept
    {
        try
        {
            std::ifstream f(std::filesystem::path{ path }, std::ios::binary);
            if (!f)
            {
                return {}; // no file: no conversation yet
            }
            const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return DeserializeHarnessRecord(HarnessFromUtf8(bytes));
        }
        catch (...)
        {
            LogSwallowedException(L"LoadHarnessRecordFrom");
            return {};
        }
    }

    bool DeleteHarnessRecordAt(const std::wstring& path) noexcept
    {
        if (::DeleteFileW(path.c_str()))
        {
            return true;
        }
        const DWORD error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }

    // ===== launch policy ====================================================================

    int64_t ClampHarnessAutoCompactTokens(int64_t tokens) noexcept
    {
        if (tokens <= 0)
        {
            return 0;
        }
        return std::clamp<int64_t>(tokens, 100000, 1000000);
    }

    const wchar_t* HarnessPermissionModeWord(HarnessPermissionMode mode) noexcept
    {
        switch (mode)
        {
        case HarnessPermissionMode::Bypass:
            return L"bypassPermissions";
        case HarnessPermissionMode::Auto:
            return L"auto";
        case HarnessPermissionMode::DontAsk:
        default:
            return L"dontAsk";
        }
    }

    std::optional<HarnessPermissionMode> NextHarnessPermissionMode(HarnessPermissionMode mode) noexcept
    {
        switch (mode)
        {
        case HarnessPermissionMode::Bypass:
            return HarnessPermissionMode::Auto;
        case HarnessPermissionMode::Auto:
            return HarnessPermissionMode::DontAsk;
        case HarnessPermissionMode::DontAsk:
        default:
            return std::nullopt;
        }
    }

    bool HarnessStartFailureNamesPermissionMode(std::wstring_view failureText) noexcept
    {
        // Each sign as Claude Code 2.1.291 writes it (lower case here; the text is folded below):
        //   "error: option '--permission-mode <mode>' argument 'x' is invalid"   -> permission-mode
        //   "Error: bypassPermissions not supported in restricted mode"           -> bypasspermissions
        //   "--dangerously-skip-permissions cannot be used with ..."              -> skip-permissions
        // and the wordings a later version may choose for the same refusals.
        static constexpr std::wstring_view kSigns[] = {
            L"permission-mode",
            L"permission mode",
            L"bypasspermissions",
            L"bypass permissions",
            L"skip-permissions",
            L"auto mode",
        };
        const auto lower = [](wchar_t c) noexcept { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c; };
        for (const std::wstring_view sign : kSigns)
        {
            // An ASCII-case-insensitive search without a copy: this function is noexcept, and a
            // failure text can be long (the SDK's message plus eight stderr lines).
            const auto hit = std::search(failureText.begin(), failureText.end(), sign.begin(), sign.end(), [&lower](wchar_t a, wchar_t b) noexcept { return lower(a) == b; });
            if (hit != failureText.end())
            {
                return true;
            }
        }
        return false;
    }

    bool HarnessPermissionModeNeverAsks(std::wstring_view reportedMode) noexcept
    {
        return reportedMode == L"bypassPermissions" || reportedMode == L"auto" || reportedMode == L"dontAsk";
    }

    HarnessPermissionStep DecideHarnessPermissionStep(HarnessPermissionMode asked, std::wstring_view reportedMode) noexcept
    {
        if (reportedMode.empty() || HarnessPermissionModeNeverAsks(reportedMode))
        {
            // Nothing to correct — or nothing known yet: a blind switch could LOWER a process that
            // runs in bypass and only did not say so.
            return HarnessPermissionStep::Keep;
        }
        // A mode that can ask. After a launch that asked for bypass, auto was not tried by us yet
        // (Claude Code's own fallback may have skipped it); after any other launch it was.
        return asked == HarnessPermissionMode::Bypass ? HarnessPermissionStep::SwitchToAuto : HarnessPermissionStep::SwitchToDontAsk;
    }

    // ===== the thinking policy ==============================================================

    const wchar_t* HarnessThinkingLevelWord(HarnessThinkingLevel level) noexcept
    {
        switch (level)
        {
        case HarnessThinkingLevel::Low:
            return L"low";
        case HarnessThinkingLevel::Medium:
            return L"medium";
        case HarnessThinkingLevel::High:
            return L"high";
        case HarnessThinkingLevel::XHigh:
            return L"xhigh";
        case HarnessThinkingLevel::Max:
            return L"max";
        case HarnessThinkingLevel::Off:
        default:
            return L"off";
        }
    }

    std::optional<HarnessThinkingLevel> ParseHarnessThinkingLevel(std::wstring_view word) noexcept
    {
        // A hand-edited settings.json may carry spaces; the word itself is compared caselessly.
        while (!word.empty() && (word.front() == L' ' || word.front() == L'\t'))
        {
            word.remove_prefix(1);
        }
        while (!word.empty() && (word.back() == L' ' || word.back() == L'\t'))
        {
            word.remove_suffix(1);
        }
        for (const HarnessThinkingLevel level : kThinkingLevels)
        {
            if (EqualsAsciiNoCase(word, HarnessThinkingLevelWord(level)))
            {
                return level;
            }
        }
        return std::nullopt;
    }

    bool HarnessThinkingLevelThinks(HarnessThinkingLevel level) noexcept
    {
        return level != HarnessThinkingLevel::Off;
    }

    const wchar_t* HarnessThinkingLevelEffort(HarnessThinkingLevel level) noexcept
    {
        // Off shares Low's effort word: thinking off is "the fastest", and low effort is the
        // fastest effort (the search agent's one setting before the levels existed).
        return level == HarnessThinkingLevel::Off ? L"low" : HarnessThinkingLevelWord(level);
    }

    HarnessThinkingLevel NextHarnessThinkingLevel(HarnessThinkingLevel level, const std::vector<std::wstring>& supportedEfforts) noexcept
    {
        const auto supported = [&supportedEfforts](HarnessThinkingLevel candidate) noexcept {
            if (candidate == HarnessThinkingLevel::Off || supportedEfforts.empty())
            {
                return true; // Off needs no effort level; an unknown list hides nothing
            }
            const std::wstring_view effort = HarnessThinkingLevelEffort(candidate);
            return std::any_of(supportedEfforts.begin(), supportedEfforts.end(), [effort](const std::wstring& s) noexcept { return EqualsAsciiNoCase(s, effort); });
        };
        constexpr size_t count = std::size(kThinkingLevels);
        const size_t from = static_cast<size_t>(level) < count ? static_cast<size_t>(level) : 0;
        // At most one full turn: Off is always supported, so the loop always finds a level.
        for (size_t step = 1; step <= count; ++step)
        {
            const HarnessThinkingLevel candidate = kThinkingLevels[(from + step) % count];
            if (supported(candidate))
            {
                return candidate;
            }
        }
        return HarnessThinkingLevel::Off;
    }

    bool IsHarnessEffortWord(std::wstring_view effort) noexcept
    {
        return effort == L"low" || effort == L"medium" || effort == L"high" || effort == L"xhigh" || effort == L"max";
    }

    bool HarnessModelRefusesDisabledThinking(std::wstring_view model) noexcept
    {
        // Reduce the name to Claude Code's canonical form without allocating (noexcept): views
        // only, every comparison without regard to ASCII case.
        while (!model.empty() && (model.front() == L' ' || model.front() == L'\t'))
        {
            model.remove_prefix(1);
        }
        while (!model.empty() && (model.back() == L' ' || model.back() == L'\t'))
        {
            model.remove_suffix(1);
        }
        constexpr std::wstring_view kOneMillion = L"[1m]"; // Claude Code's 1M-context marker
        if (model.size() >= kOneMillion.size() && EqualsAsciiNoCase(model.substr(model.size() - kOneMillion.size()), kOneMillion))
        {
            model.remove_suffix(kOneMillion.size());
        }
        // Bedrock: "us.anthropic.claude-sonnet-4-5-20250929-v1:0" — the id starts after "anthropic.".
        constexpr std::wstring_view kProvider = L"anthropic.";
        for (size_t i = 0; i + kProvider.size() <= model.size(); ++i)
        {
            if (EqualsAsciiNoCase(model.substr(i, kProvider.size()), kProvider))
            {
                model.remove_prefix(i + kProvider.size());
                break;
            }
        }
        // Vertex: "claude-sonnet-4-5@20250929".
        if (const size_t at = model.find(L'@'); at != std::wstring_view::npos)
        {
            model = model.substr(0, at);
        }
        const auto isDigit = [](wchar_t c) noexcept { return c >= L'0' && c <= L'9'; };
        // Bedrock's "-v1" / "-v1:0" version suffix.
        if (const size_t dash = model.rfind(L"-v"); dash != std::wstring_view::npos && dash + 2 < model.size())
        {
            bool versionOnly = true;
            for (size_t i = dash + 2; i < model.size(); ++i)
            {
                versionOnly = versionOnly && (isDigit(model[i]) || model[i] == L':');
            }
            if (versionOnly)
            {
                model = model.substr(0, dash);
            }
        }
        // A dated id: "claude-sonnet-4-5-20250929" -> "claude-sonnet-4-5".
        if (model.size() > 9 && model[model.size() - 9] == L'-')
        {
            bool date = true;
            for (size_t i = model.size() - 8; i < model.size(); ++i)
            {
                date = date && isDigit(model[i]);
            }
            if (date)
            {
                model.remove_suffix(9);
            }
        }
        // Claude 3 in every variant takes "disabled" (Claude Code tests `includes("claude-3-")`).
        constexpr std::wstring_view kClaude3 = L"claude-3-";
        for (size_t i = 0; i + kClaude3.size() <= model.size(); ++i)
        {
            if (EqualsAsciiNoCase(model.substr(i, kClaude3.size()), kClaude3))
            {
                return false;
            }
        }
        // Claude Code 2.1.291's list of the models that take "disabled". "claude-opus-4" and
        // "claude-sonnet-4" are the 4.0 models' undated names, which the date drop above yields
        // for their ids (claude-opus-4-20250514).
        static constexpr std::wstring_view kTakesDisabled[] = {
            L"claude-opus-4",
            L"claude-opus-4-0",
            L"claude-opus-4-1",
            L"claude-opus-4-5",
            L"claude-opus-4-6",
            L"claude-opus-4-7",
            L"claude-opus-4-8",
            L"claude-opus-5",
            L"claude-sonnet-4",
            L"claude-sonnet-4-0",
            L"claude-sonnet-4-5",
            L"claude-sonnet-4-6",
            L"claude-sonnet-5",
            L"claude-haiku-4-5",
        };
        for (const std::wstring_view listed : kTakesDisabled)
        {
            if (EqualsAsciiNoCase(model, listed))
            {
                return false;
            }
        }
        return true;
    }

    HarnessThinkingChange PlanHarnessThinkingChange(const HarnessReasoningState& applied, const HarnessReasoningState& wanted)
    {
        HarnessThinkingChange change;
        change.thinkingOff = applied.thinks && !wanted.thinks;
        change.thinkingOn = !applied.thinks && wanted.thinks;
        // between_tools only ever goes with Claude Code's thinking off: the API refuses it next to
        // Claude Code's adaptive request (the clear_thinking strategy), so a wanted pair of
        // "thinks" and "betweenTools" is read as "thinks" alone.
        const bool wantBetweenTools = wanted.betweenTools && !wanted.thinks;
        change.betweenToolsOn = !applied.betweenTools && wantBetweenTools;
        change.betweenToolsOff = applied.betweenTools && !wantBetweenTools;
        // An unknown wanted word is never sent (IsHarnessEffortWord): the applied effort stays.
        if (IsHarnessEffortWord(wanted.effort) && wanted.effort != applied.effort)
        {
            change.effort = wanted.effort;
        }
        return change;
    }

    HarnessModelReasoning ReadHarnessModelReasoning(std::wstring_view modelsJson, std::wstring_view model)
    {
        HarnessModelReasoning out;
        const auto parsed = json::Parse(modelsJson);
        if (!parsed || parsed->type != json::Value::Type::Arr || model.empty())
        {
            return out;
        }
        for (const auto& entry : parsed->arr)
        {
            if (entry.type != json::Value::Type::Obj)
            {
                continue;
            }
            // Claude Code lists an alias row ("sonnet") whose resolvedModel is the wire id; the
            // harness passes the wire id, so either field can name it.
            if (!EqualsAsciiNoCase(entry.StrAt(L"value"), model) && !EqualsAsciiNoCase(entry.StrAt(L"resolvedModel"), model))
            {
                continue;
            }
            out.known = true;
            // The wire id the row resolves to: what decides whether the model takes "disabled"
            // (HarnessModelRefusesDisabledThinking). An older Claude Code without the field lists
            // the id itself as the value.
            out.resolvedModel = entry.StrAt(L"resolvedModel");
            if (out.resolvedModel.empty())
            {
                out.resolvedModel = entry.StrAt(L"value");
            }
            out.adaptiveThinking = entry.BoolAt(L"supportsAdaptiveThinking");
            if (const auto* levels = entry.Find(L"supportedEffortLevels"); levels && levels->type == json::Value::Type::Arr)
            {
                for (const auto& level : levels->arr)
                {
                    if (level.type != json::Value::Type::Str)
                    {
                        continue;
                    }
                    std::wstring word = level.str;
                    for (auto& c : word)
                    {
                        c = AsciiLower(c);
                    }
                    if (IsHarnessEffortWord(word))
                    {
                        out.supportedEfforts.push_back(std::move(word));
                    }
                }
            }
            break; // the first matching row decides: Claude Code lists a model once
        }
        return out;
    }

    // The env object of the --settings text: ONE builder for BuildHarnessSettingsJson and
    // BuildHarnessEnvPatchJson, so that a live patch can never drop a variable the start set.
    //   throws — std::bad_alloc.
    static json::Value BuildHarnessSettingsEnvObject(const HarnessSettingsSpec& spec)
    {
        using json::Value;
        Value env = Value::MkObj();
        env.Set(L"CLAUDE_CODE_MAX_RETRIES", Value::MkStr(std::to_wstring(std::clamp(spec.apiMaxRetries, 0, 15))));
        env.Set(L"CLAUDE_CODE_RETRY_WATCHDOG", Value::MkStr(L"0"));
        // No CLAUDE_CODE_EFFORT_LEVEL here (the header's thinking policy): it would override every
        // live effort change while Claude Code answers each one with "success".
        env.Set(L"DISABLE_AUTOUPDATER", Value::MkStr(L"1"));
        env.Set(L"CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC", Value::MkStr(L"1"));
        env.Set(L"CLAUDE_CODE_DISABLE_TERMINAL_TITLE", Value::MkStr(L"1"));
        env.Set(L"CLAUDE_CODE_DISABLE_CLAUDE_MDS", Value::MkStr(L"1"));
        env.Set(L"CLAUDE_CODE_DISABLE_AUTO_MEMORY", Value::MkStr(L"1"));
        env.Set(L"CLAUDE_CODE_DISABLE_BACKGROUND_TASKS", Value::MkStr(L"1"));
        // Always written, empty included: an empty value is "no extra body" to Claude Code (it
        // tests the variable for truth), and a live patch that turns between_tools off must
        // overwrite the earlier value, not leave it behind.
        env.Set(L"CLAUDE_CODE_EXTRA_BODY", Value::MkStr(spec.extraRequestBody));
        return env;
    }

    std::wstring BuildHarnessEnvPatchJson(const HarnessSettingsSpec& spec)
    {
        using json::Value;
        Value root = Value::MkObj();
        root.Set(L"env", BuildHarnessSettingsEnvObject(spec));
        return json::Dump(root);
    }

    std::wstring BuildHarnessSettingsJson(const HarnessSettingsSpec& spec)
    {
        using json::Value;
        Value root = Value::MkObj();
        root.Set(L"verbose", Value::MkBool(false));
        if (spec.disableBypassPermissions || spec.disableAutoMode)
        {
            // The test seam: the switches of an administrator's policy, for this process only.
            Value permissions = Value::MkObj();
            if (spec.disableBypassPermissions)
            {
                permissions.Set(L"disableBypassPermissionsMode", Value::MkStr(L"disable"));
            }
            if (spec.disableAutoMode)
            {
                permissions.Set(L"disableAutoMode", Value::MkStr(L"disable"));
            }
            root.Set(L"permissions", std::move(permissions));
        }
        if (const int64_t window = ClampHarnessAutoCompactTokens(spec.autoCompactTokens); window != 0)
        {
            // Both keys: see the header for why the flag alone is not enough.
            root.Set(L"autoCompactEnabled", Value::MkBool(true));
            root.Set(L"autoCompactWindow", Value::MkNum(static_cast<double>(window)));
        }
        root.Set(L"env", BuildHarnessSettingsEnvObject(spec));
        return json::Dump(root);
    }

    std::map<std::string, std::string> BuildHarnessEnvironment(std::wstring_view name)
    {
        // Each of these, when inherited, makes the child misreport or misbehave:
        static constexpr const char* kRemove[] = {
            // a parent Claude Code session (Agentmaster started from one of its tool calls)
            "CLAUDECODE",
            "CLAUDE_CODE_CHILD_SESSION",
            "CLAUDE_CODE_SESSION_ID",
            "CLAUDE_CODE_SESSION_NAME",
            "CLAUDE_CODE_SESSION_KIND",
            "CLAUDE_CODE_SESSION_ATTENDED",
            "CLAUDE_CODE_MESSAGING_SOCKET",
            "CLAUDE_CODE_MESSAGING_TOKEN",
            "CLAUDE_CODE_EXECPATH",
            "CLAUDE_CODE_PLUGIN_DIRS",
            "CLAUDE_PID",
            "AI_AGENT",
            // a terminal tab and Agentmaster's own session wiring (the Fleet Observer keys on these)
            "WT_SESSION",
            "WT_PROFILE_ID",
            "AM_SESSION",
            "CCMGR_HOOK_PIPE",
            "CCMGR_SESSION_ID",
            // settings a variable would override: effort, thinking, the compaction window, retries
            "CLAUDE_CODE_EFFORT_LEVEL",
            "CLAUDE_EFFORT",
            "MAX_THINKING_TOKENS",
            // The thinking level is the harness's (the thinking policy, AgentHarnessCore.h): with
            // the first, Claude Code never asks for adaptive thinking; the second switches some
            // models to budgeted thinking; the third drops thinking between tool calls.
            "CLAUDE_CODE_DISABLE_THINKING",
            "CLAUDE_CODE_DISABLE_ADAPTIVE_THINKING",
            "DISABLE_INTERLEAVED_THINKING",
            // Set by the harness itself through its settings (between_tools for an "off" level):
            // an inherited value could carry a `thinking` field of its own.
            "CLAUDE_CODE_EXTRA_BODY",
            "CLAUDE_CODE_AUTO_COMPACT_WINDOW",
            "CLAUDE_AUTOCOMPACT_PCT_OVERRIDE",
            "DISABLE_AUTO_COMPACT",
            "DISABLE_COMPACT",
            "CLAUDE_CODE_MAX_RETRIES",
            "CLAUDE_CODE_RETRY_WATCHDOG",
            // debug output: a debug log per run under ~/.claude/debug, even without persistence
            "DEBUG",
        };
        std::map<std::string, std::string> env;
        for (const char* key : kRemove)
        {
            env.emplace(key, std::string{}); // empty == remove (ClaudeSdkProcessWin32.cpp)
        }
        env["AM_HARNESS"] = HarnessToUtf8(name);
        return env;
    }

    std::optional<std::string> NarrowPathForSdk(const std::wstring& path)
    {
        if (path.empty())
        {
            return std::nullopt;
        }
        // The ANSI form, accepted only when NO character had to be replaced. NO_BEST_FIT keeps
        // the system from silently choosing a look-alike letter, which would name another file.
        const auto exactAnsi = [](const std::wstring& p) -> std::optional<std::string> {
            BOOL usedDefault = FALSE;
            const int n = ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, p.data(), static_cast<int>(p.size()), nullptr, 0, nullptr, &usedDefault);
            if (n <= 0 || usedDefault)
            {
                return std::nullopt;
            }
            std::string out(static_cast<size_t>(n), '\0');
            usedDefault = FALSE;
            ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, p.data(), static_cast<int>(p.size()), out.data(), n, nullptr, &usedDefault);
            if (usedDefault)
            {
                return std::nullopt;
            }
            return out;
        };
        if (auto ansi = exactAnsi(path))
        {
            return ansi;
        }
        // The 8.3 short form exists only on volumes that keep short names; it is ASCII then.
        std::wstring shortPath(32768, L'\0');
        const DWORD n = ::GetShortPathNameW(path.c_str(), shortPath.data(), static_cast<DWORD>(shortPath.size()));
        if (n == 0 || n >= shortPath.size())
        {
            return std::nullopt;
        }
        shortPath.resize(n);
        return exactAnsi(shortPath);
    }

    std::wstring HarnessToolFullName(std::wstring_view server, std::wstring_view tool)
    {
        std::wstring out;
        out.reserve(server.size() + tool.size() + 7);
        out += L"mcp__";
        out += server;
        out += L"__";
        out += tool;
        return out;
    }

    // ===== readers ==========================================================================

    HarnessSystemFacts ClassifyHarnessSystemMessage(std::wstring_view rawJson)
    {
        HarnessSystemFacts facts;
        const auto parsed = json::Parse(rawJson);
        if (!parsed || parsed->type != json::Value::Type::Obj || parsed->StrAt(L"type") != L"system")
        {
            return facts;
        }
        const std::wstring subtype = parsed->StrAt(L"subtype");
        if (subtype == L"init")
        {
            facts.kind = HarnessSystemKind::Init;
            facts.model = parsed->StrAt(L"model");
            facts.sessionId = parsed->StrAt(L"session_id");
            facts.cliVersion = parsed->StrAt(L"claude_code_version");
            facts.permissionMode = parsed->StrAt(L"permissionMode");
            facts.apiKeySource = parsed->StrAt(L"apiKeySource");
            facts.outputStyle = parsed->StrAt(L"output_style");
            // A list of names. Claude Code 2.1.291 writes strings for tools, slash_commands,
            // skills and agents, and objects with a "name" for plugins; both forms are read for
            // every list, so a version that changes one does not empty it.
            const auto names = [&parsed](const wchar_t* key) {
                std::vector<std::wstring> out;
                if (const auto* list = parsed->Find(key); list && list->type == json::Value::Type::Arr)
                {
                    for (const auto& item : list->arr)
                    {
                        if (item.type == json::Value::Type::Str)
                        {
                            out.push_back(item.str);
                        }
                        else if (item.type == json::Value::Type::Obj)
                        {
                            if (std::wstring name = item.StrAt(L"name"); !name.empty())
                            {
                                out.push_back(std::move(name));
                            }
                        }
                    }
                }
                return out;
            };
            facts.tools = names(L"tools");
            facts.slashCommands = names(L"slash_commands");
            facts.skills = names(L"skills");
            facts.agents = names(L"agents");
            facts.plugins = names(L"plugins");
            if (const auto* servers = parsed->Find(L"mcp_servers"); servers && servers->type == json::Value::Type::Arr)
            {
                for (const auto& s : servers->arr)
                {
                    if (s.type == json::Value::Type::Obj)
                    {
                        facts.mcpServers.emplace_back(s.StrAt(L"name"), s.StrAt(L"status"));
                    }
                }
            }
            return facts;
        }
        if (subtype == L"permission_denied")
        {
            // {"tool_name":"mcp__x__y","message":"Permission to use ... has been denied because
            //  Claude Code is running in don't ask mode ..."} — the model gets the same text as the
            // tool's (error) result; this line is how the HOST learns of it.
            facts.kind = HarnessSystemKind::PermissionDenied;
            facts.deniedTool = parsed->StrAt(L"tool_name");
            facts.deniedReason = parsed->StrAt(L"message");
            return facts;
        }
        if (subtype == L"status")
        {
            // {"status":"compacting"} opens a compaction; {"status":null,"compact_result":"success"}
            // closes it. Any other status line is not ours.
            if (parsed->StrAt(L"status") == L"compacting")
            {
                facts.kind = HarnessSystemKind::Compacting;
            }
            else if (const auto* r = parsed->Find(L"compact_result"))
            {
                facts.kind = HarnessSystemKind::CompactFinished;
                facts.compactOk = r->AsStr() == L"success";
            }
            return facts;
        }
        if (subtype == L"compact_boundary")
        {
            facts.kind = HarnessSystemKind::CompactBoundary;
            if (const auto* m = parsed->Find(L"compact_metadata"); m && m->type == json::Value::Type::Obj)
            {
                facts.compactTrigger = m->StrAt(L"trigger");
                facts.preTokens = m->I64At(L"pre_tokens");
                facts.postTokens = m->I64At(L"post_tokens");
                facts.compactDurationMs = m->I64At(L"duration_ms");
            }
            return facts;
        }
        if (subtype == L"thinking_tokens")
        {
            // {"estimated_tokens": 207, "estimated_tokens_delta": 157, ...} while the model thinks
            // (Claude Code 2.1.291). A negative number is no estimate.
            facts.kind = HarnessSystemKind::ThinkingTokens;
            facts.thinkingTokens = (std::max)(int64_t{ 0 }, parsed->I64At(L"estimated_tokens"));
            facts.thinkingTokensDelta = (std::max)(int64_t{ 0 }, parsed->I64At(L"estimated_tokens_delta"));
            return facts;
        }
        return facts;
    }

    HarnessResultFacts ParseHarnessResultMessage(std::wstring_view rawJson)
    {
        HarnessResultFacts facts;
        const auto parsed = json::Parse(rawJson);
        if (!parsed || parsed->type != json::Value::Type::Obj || parsed->StrAt(L"type") != L"result")
        {
            return facts;
        }
        facts.valid = true;
        facts.isError = parsed->BoolAt(L"is_error");
        facts.subtype = parsed->StrAt(L"subtype");
        facts.text = parsed->StrAt(L"result");
        facts.stopReason = parsed->StrAt(L"stop_reason");
        facts.apiErrorStatus = static_cast<int>(parsed->I64At(L"api_error_status"));
        facts.totalCostUsd = parsed->NumAt(L"total_cost_usd");
        facts.durationMs = parsed->I64At(L"duration_ms");
        facts.durationApiMs = parsed->I64At(L"duration_api_ms");
        facts.numTurns = static_cast<int>(parsed->I64At(L"num_turns"));
        if (const auto* errors = parsed->Find(L"errors"); errors && errors->type == json::Value::Type::Arr)
        {
            for (const auto& e : errors->arr)
            {
                if (e.type == json::Value::Type::Str)
                {
                    facts.errors.push_back(e.str);
                }
            }
        }
        if (const auto* mu = parsed->Find(L"modelUsage"); mu && mu->type == json::Value::Type::Obj)
        {
            // Keyed by model id. One model answers a harness conversation; with several entries
            // (a fallback model answered once) the largest window is the one that matters.
            for (const auto& member : mu->members)
            {
                if (member.second.type == json::Value::Type::Obj)
                {
                    facts.contextWindow = (std::max)(facts.contextWindow, member.second.I64At(L"contextWindow"));
                }
            }
        }
        // A result that is not a plain success but carries no is_error flag (an older CLI) is
        // still a failure: "error_during_execution", "error_max_turns", ...
        if (!facts.isError && !facts.subtype.empty() && facts.subtype != L"success")
        {
            facts.isError = true;
        }
        return facts;
    }

    HarnessRequestUsage ParseHarnessRequestUsage(std::wstring_view usageJson)
    {
        HarnessRequestUsage usage;
        const auto parsed = json::Parse(usageJson);
        if (!parsed || parsed->type != json::Value::Type::Obj)
        {
            return usage;
        }
        usage.inputTokens = parsed->I64At(L"input_tokens");
        usage.outputTokens = parsed->I64At(L"output_tokens");
        usage.cacheReadTokens = parsed->I64At(L"cache_read_input_tokens");
        usage.cacheCreationTokens = parsed->I64At(L"cache_creation_input_tokens");
        return usage;
    }
}
