// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — the wire contract between the PowerShell hook forwarder and the native
// HooksBridge. Pure C++ (no Win32/WinRT) so it is shared verbatim by the bridge and the
// unit tests, and so the exact format lives in one place.
//
// One hook invocation -> one UTF-8, newline-terminated, TAB-separated record:
//
//     event \t sessionId \t cwd \t isQuestion \t permission \t tool \t tabToken \n
//
// Fields after `sessionId` are optional (older/edge forwarders may omit them). `cwd`,
// `tool` and `tabToken` are assumed free of TAB/newline (true for Windows paths, Claude
// tool names, and the plain WT_SESSION GUID).
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
        return m;
    }
}
