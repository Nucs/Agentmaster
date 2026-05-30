// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — the wire contract between the PowerShell hook forwarder and the native
// HooksBridge. Pure C++ (no Win32/WinRT) so it is shared verbatim by the bridge and the
// unit tests, and so the exact format lives in one place.
//
// One hook invocation -> one UTF-8, newline-terminated, TAB-separated record:
//
//     event \t sessionId \t cwd \t isQuestion \t permission \t tool \t tabToken \t prompt \n
//
// Fields after `sessionId` are optional (older/edge forwarders may omit them). `cwd`,
// `tool` and `tabToken` are assumed free of TAB/newline (true for Windows paths, Claude
// tool names, and the plain WT_SESSION GUID). The trailing `prompt` (set only on
// UserPromptSubmit, so the Flight Plan can show EVERY message a session received, not just
// ones we queued) is the one field that CAN contain TAB/newline, so it is escaped
// (\ \t \r \n) by both the forwarder and BuildWireLine and un-escaped on parse — keeping
// the record single-line and the field split unambiguous.
// We deliberately avoid JSON on the wire so the bridge needs no JSON dependency; the
// forwarder (which DOES have PowerShell's ConvertFrom-Json) does the parsing and emits
// these few flat fields.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "HookEvents.h"

namespace Agentmaster
{
    inline constexpr wchar_t kWireFieldSep = L'\t';

    // The canonical local pipe name for a given process id (HOOKS.md transport).
    inline std::wstring HookPipeName(unsigned long pid)
    {
        return L"\\\\.\\pipe\\agentmaster." + std::to_wstring(pid);
    }

    // Escape a free-text field (the prompt body) so it stays TAB/newline-free on the wire:
    //   \ -> \\   tab -> \t   CR -> \r   LF -> \n
    // Single-pass (each input char maps independently), so it is order-independent. The
    // PowerShell forwarder mirrors this exactly. PURE.
    inline std::wstring WireEscape(std::wstring_view s)
    {
        std::wstring o;
        o.reserve(s.size());
        for (const wchar_t c : s)
        {
            switch (c)
            {
            case L'\\':
                o += L"\\\\";
                break;
            case L'\t':
                o += L"\\t";
                break;
            case L'\r':
                o += L"\\r";
                break;
            case L'\n':
                o += L"\\n";
                break;
            default:
                o += c;
                break;
            }
        }
        return o;
    }

    // Inverse of WireEscape. A lone/unknown backslash escape is kept verbatim (total: never
    // throws, never reads past the end). PURE.
    inline std::wstring WireUnescape(std::wstring_view s)
    {
        std::wstring o;
        o.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == L'\\' && i + 1 < s.size())
            {
                switch (s[i + 1])
                {
                case L'\\':
                    o += L'\\';
                    ++i;
                    continue;
                case L't':
                    o += L'\t';
                    ++i;
                    continue;
                case L'r':
                    o += L'\r';
                    ++i;
                    continue;
                case L'n':
                    o += L'\n';
                    ++i;
                    continue;
                default:
                    break; // unknown escape: keep the backslash as-is
                }
            }
            o += s[i];
        }
        return o;
    }

    // Build a single wire line (used by tests and as the reference the forwarder mirrors).
    inline std::wstring BuildWireLine(const HookMessage& m)
    {
        std::wstring s;
        s.append(HookEventName(m.event));
        s.push_back(kWireFieldSep);
        s.append(m.sessionId);
        s.push_back(kWireFieldSep);
        s.append(m.cwd);
        s.push_back(kWireFieldSep);
        s.push_back(m.lastMessageIsQuestion ? L'1' : L'0');
        s.push_back(kWireFieldSep);
        s.push_back(m.permissionRequest ? L'1' : L'0');
        s.push_back(kWireFieldSep);
        s.append(m.tool);
        s.push_back(kWireFieldSep);
        s.append(m.tabToken);
        s.push_back(kWireFieldSep);
        s.append(WireEscape(m.promptText));
        return s;
    }

    // Parse one wire line into a HookMessage. Returns nullopt if there is no event or no
    // sessionId (the two required fields). Tolerates a trailing CR/LF and missing tail
    // fields. PURE + total: never throws.
    inline std::optional<HookMessage> ParseWireLine(std::wstring_view line) noexcept
    {
        // Trim a single trailing CRLF / LF / CR.
        while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r'))
        {
            line.remove_suffix(1);
        }
        if (line.empty())
        {
            return std::nullopt;
        }

        std::vector<std::wstring_view> fields;
        size_t start = 0;
        for (size_t i = 0; i <= line.size(); ++i)
        {
            if (i == line.size() || line[i] == kWireFieldSep)
            {
                fields.push_back(line.substr(start, i - start));
                start = i + 1;
            }
        }

        if (fields.size() < 2)
        {
            return std::nullopt;
        }

        HookMessage m;
        m.event = ParseHookEvent(fields[0]);
        m.sessionId = std::wstring{ fields[1] };
        if (m.sessionId.empty())
        {
            return std::nullopt;
        }
        if (fields.size() > 2)
        {
            m.cwd = std::wstring{ fields[2] };
        }
        if (fields.size() > 3)
        {
            m.lastMessageIsQuestion = (fields[3] == L"1");
        }
        if (fields.size() > 4)
        {
            m.permissionRequest = (fields[4] == L"1");
        }
        if (fields.size() > 5)
        {
            m.tool = std::wstring{ fields[5] };
        }
        if (fields.size() > 6)
        {
            m.tabToken = std::wstring{ fields[6] };
        }
        if (fields.size() > 7)
        {
            // The prompt is the only field that may carry escaped TAB/newline (see WireEscape).
            m.promptText = WireUnescape(fields[7]);
        }
        return m;
    }
}
