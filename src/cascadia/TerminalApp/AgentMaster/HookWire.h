// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the wire contract between the PowerShell hook forwarder and the native
// HooksBridge. Pure C++ (no Win32/WinRT) so it is shared verbatim by the bridge and the
// unit tests, and so the exact format lives in one place.
//
// One hook invocation -> one UTF-8, newline-terminated, TAB-separated record:
//
//     event \t sessionId \t cwd \t isQuestion \t permission \t tool \t tabToken \t prompt \t ts \t source \n
//
// Fields after `sessionId` are optional (older/edge forwarders may omit them). `cwd`,
// `tool` and `tabToken` are assumed free of TAB/newline (true for Windows paths, Claude
// tool names, and the plain WT_SESSION GUID). The `prompt` (set only on
// UserPromptSubmit, so the Auto Testing can show EVERY message a session received, not just
// ones we queued) is the one field that CAN contain TAB/newline, so it is escaped
// (\ \t \r \n) by both the forwarder and BuildWireLine and un-escaped on parse — keeping
// the record single-line and the field split unambiguous. The trailing `ts` is the hook's
// FIRE time (unix ms, UTC), stamped by the forwarder FIRST — before any slow work (the Stop
// path reads the transcript) — so the registry can ORDER events that arrive out of order
// (a slow Stop landing after the next turn's UserPromptSubmit; see NextSessionStateOrdered).
// It is appended LAST so an old 8-field line still parses (ts -> 0 == arrival-order
// fallback) and an old parser simply ignores the extra field. The trailing `source` (the 10th
// field, same append-last compatibility) is the hook payload's own discriminator — SessionStart
// `source`, SessionEnd `reason`, Pre/PostCompact `trigger` — which lets the state machine tell a
// compaction's SessionStart from a real (re)start (HOOKS.md "Compaction"). A 9-field line (an old
// forwarder, e.g. a session launched before this build that still runs its pre-update hooks)
// parses with source "" and takes the historical, source-blind path.
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

    // The longest `source` token the wire accepts. The real vocabulary is a handful of short words
    // (startup / resume / clear / compact / fork / manual / auto / logout / prompt_input_exit /
    // other); anything longer is not a value the state machine could act on anyway.
    inline constexpr size_t kWireSourceMaxChars = 32;

    // Sanitize the `source` field to a plain identifier token: keep only ASCII letters, digits, '_'
    // and '-', and return EMPTY for anything else (or when longer than kWireSourceMaxChars) rather than
    // a trimmed guess — the state machine compares it LITERALLY against "compact" / "manual", so a
    // mangled value must degrade to the historical source-blind path, never to a partial match. The
    // PowerShell forwarder applies the SAME rule before sending (a case-sensitive
    // '^[A-Za-z0-9_-]{0,32}\z' test, else empty — belt + suspenders). PURE + total.
    inline std::wstring SanitizeWireSource(std::wstring_view s)
    {
        if (s.size() > kWireSourceMaxChars)
        {
            return {};
        }
        for (const wchar_t c : s)
        {
            const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'-';
            if (!ok)
            {
                return {};
            }
        }
        return std::wstring{ s };
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
        s.push_back(kWireFieldSep);
        s.append(std::to_wstring(m.ts)); // hook fire time (unix ms) — the ordering key
        s.push_back(kWireFieldSep);
        s.append(SanitizeWireSource(m.source)); // the payload discriminator (source / reason / trigger) — "" when none
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
        if (fields.size() > 8)
        {
            // ts: the hook's FIRE time (unix ms), stamped by the forwarder before any slow work,
            // so the registry can order out-of-order arrivals. Digits-only + bounded (a 13-digit
            // ms epoch fits well inside 18); anything else (an old forwarder, garbage) leaves 0
            // and the ordered machine falls back to arrival order.
            const auto f = fields[8];
            if (!f.empty() && f.size() <= 18)
            {
                int64_t v = 0;
                bool ok = true;
                for (const wchar_t c : f)
                {
                    if (c < L'0' || c > L'9')
                    {
                        ok = false;
                        break;
                    }
                    v = v * 10 + (c - L'0');
                }
                if (ok)
                {
                    m.ts = v;
                }
            }
        }
        if (fields.size() > 9)
        {
            // source: the payload discriminator (SessionStart source / SessionEnd reason / Pre-
            // PostCompact trigger). Sanitized to a short identifier token — anything else reads "",
            // i.e. the historical source-blind path (see SanitizeWireSource).
            m.source = SanitizeWireSource(fields[9]);
        }
        return m;
    }
}
