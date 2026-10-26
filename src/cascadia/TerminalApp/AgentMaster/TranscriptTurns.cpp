// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

// Plain C++ engine TU — no WinRT, no precompiled header (the vcxproj marks it NotUsing and the
// standalone test harness compiles it directly). See TranscriptTurns.h for what it renders and how
// it relates to its reference, the am-agentmaster-cli skill's transcript_turns.py (v9).
//
// Layout — every section mirrors the reference's section of the same name, function for function,
// and each ported function names its Python original, so a change to the reference is easy to find
// and carry over here:
//   1. Python-compatible primitives — dict lookups, str()/repr(), whitespace, splitlines, code points
//   2. Formatting helpers           — fmt_duration / fmt_tokens / quote / one_line / flat / code / blocks
//   3. Timestamps                   — parse_ts / aware_ts and the local/UTC clock behind when()
//   4. Parsing                      — text_of / peer_messages / the live-branch walks / parse()
//   5. Chains                       — find_fork_parent / find_plan_parent / resolve_chain / load_history
//   6. Agents                       — agent_files / agent_files_by_prompt / attach_agents / merged_turn
//   7. Rendering                    — the Renderer class and render_history
//   8. Conversation upstream        — NOT in the reference (Agentmaster): the summary panel's
//                                     whole-conversation view, built on section 5's chain walk
// Section 5 carries three Agentmaster-only additions the reference has no counterpart for, each
// marked as such: ChainOptions (how the summary may deviate — a default-constructed one IS the
// reference walk, which is all Copy ▸ Transcript ever passes), LooksForkCopied and
// AttachRecordedParent.
// The PUBLIC entry points (TranscriptTurns.h) close the file; they are the only never-throw seam.
#define NOMINMAX
#include "TranscriptTurns.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <condition_variable> // section 8: the upstream cache's in-flight wait (one walk per transcript)
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <memory>
#include <mutex> // section 8: the upstream cache lock
#include <system_error>
#include <time.h> // _localtime64_s / _mkgmtime64 — the CRT clock the reference's astimezone() uses on Windows
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "ClaudeSpawn.h" // LogSwallowedException — Rule #18: a contained render failure still reaches hooks.log
#include "ProcessInspect.h" // ScrubLargeBase64Payloads — pasted-image payloads elided before parsing
#include "SummaryConversation.h" // BuildSummaryConversation / CodexSummaryConversation — defined at the end of this file (the consumers of section 8)

namespace Agentmaster
{
    namespace
    {
        // Everything internal lives in this named namespace INSIDE the anonymous one: helper names
        // like Quote / Join / Short are generic, and the extra scope keeps an unqualified call from
        // ever resolving to (or colliding with) a same-named function some engine header declares
        // in namespace Agentmaster.
        namespace tt
        {
            namespace fs = std::filesystem;
            using json::Value;
            using VType = json::Value::Type;

            // A parsed transcript line, by pointer. The lines themselves are owned by the vectors
            // ParseTranscriptJsonl returns, kept alive by a LineFile shared_ptr in whatever holds
            // them — the way the reference passes its dicts by reference, so a fork's history and
            // its parent's share their copied lines instead of duplicating them.
            using Lines = std::vector<const Value*>;
            using LineFile = std::shared_ptr<const std::vector<Value>>;

            // Thrown where the reference raises OSError — a transcript that can't be read or
            // stat'ed — and caught inside this file (the chain walk / agent search skip what they
            // can't read; an unreadable HEAD transcript makes RenderTranscriptTurnsFile return
            // nullopt). Never escapes a public entry point.
            struct TurnsReadError
            {
            };

            constexpr int64_t kMicrosPerSecond = 1'000'000;

            // =====================================================================================
            // 1. Python-compatible primitives
            // =====================================================================================

            constexpr bool IsHighSurrogate(wchar_t c) noexcept
            {
                return c >= 0xD800 && c <= 0xDBFF;
            }

            constexpr bool IsLowSurrogate(wchar_t c) noexcept
            {
                return c >= 0xDC00 && c <= 0xDFFF;
            }

            // Python's str.isspace() — what str.split() / str.strip() and the regex \s treat as
            // whitespace: the bidi classes WS / B / S plus the Zs category. NOT the zero-width space
            // (U+200B) or the BOM, which a naive iswspace-style check would get wrong both ways.
            constexpr bool IsPySpace(wchar_t c) noexcept
            {
                switch (c)
                {
                case 0x09:
                case 0x0A:
                case 0x0B:
                case 0x0C:
                case 0x0D:
                case 0x1C:
                case 0x1D:
                case 0x1E:
                case 0x1F:
                case 0x20:
                case 0x85:
                case 0xA0:
                case 0x1680:
                case 0x2028:
                case 0x2029:
                case 0x202F:
                case 0x205F:
                case 0x3000:
                    return true;
                default:
                    return c >= 0x2000 && c <= 0x200A;
                }
            }

            // Python's str.splitlines() boundaries: \n, \r (and \r\n as one), VT, FF, the
            // file/group/record separators, NEL, and the Unicode line/paragraph separators.
            constexpr bool IsPyLineBreak(wchar_t c) noexcept
            {
                return c == 0x0A || c == 0x0D || c == 0x0B || c == 0x0C || c == 0x1C || c == 0x1D || c == 0x1E || c == 0x85 || c == 0x2028 || c == 0x2029;
            }

            constexpr bool IsAsciiDigit(wchar_t c) noexcept
            {
                return c >= L'0' && c <= L'9';
            }

            constexpr bool IsAsciiLetter(wchar_t c) noexcept
            {
                return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
            }

            constexpr bool IsHexDigit(wchar_t c) noexcept
            {
                return IsAsciiDigit(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
            }

            constexpr wchar_t AsciiLower(wchar_t c) noexcept
            {
                return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
            }

            // The regex \w of a Python str pattern: Unicode alphanumerics and '_' (iswalnum stands in
            // for str.isalnum — identical on every character a tag attribute or name really holds).
            bool IsPyWordChar(wchar_t c) noexcept
            {
                return c == L'_' || std::iswalnum(static_cast<wint_t>(c)) != 0;
            }

            bool StartsWith(std::wstring_view s, std::wstring_view prefix) noexcept
            {
                return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
            }

            bool StartsWithCi(std::wstring_view s, std::wstring_view prefix) noexcept
            {
                if (s.size() < prefix.size())
                {
                    return false;
                }
                for (size_t i = 0; i < prefix.size(); ++i)
                {
                    if (AsciiLower(s[i]) != AsciiLower(prefix[i]))
                    {
                        return false;
                    }
                }
                return true;
            }

            bool EndsWithCi(std::wstring_view s, std::wstring_view suffix) noexcept
            {
                return s.size() >= suffix.size() && StartsWithCi(s.substr(s.size() - suffix.size()), suffix);
            }

            // The first ASCII-case-insensitive occurrence of `needle` at or after `from` (re.I on
            // ASCII text — every phrase this file searches that way is ASCII).
            size_t FindCi(std::wstring_view s, std::wstring_view needle, size_t from) noexcept
            {
                if (needle.empty())
                {
                    return from <= s.size() ? from : std::wstring_view::npos;
                }
                for (size_t i = from; i + needle.size() <= s.size(); ++i)
                {
                    if (StartsWithCi(s.substr(i), needle))
                    {
                        return i;
                    }
                }
                return std::wstring_view::npos;
            }

            std::wstring LowerAscii(std::wstring_view s)
            {
                std::wstring out{ s };
                for (auto& c : out)
                {
                    c = AsciiLower(c);
                }
                return out;
            }

            // A file-system-style lowercase (Windows compares paths case-insensitively, Unicode-wide):
            // the sort key of a listed file name, and the identity key of a path.
            std::wstring LowerForPath(std::wstring s)
            {
                if (!s.empty())
                {
                    ::CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
                }
                return s;
            }

            // str.replace(old, new): every non-overlapping occurrence, left to right.
            std::wstring ReplaceAll(std::wstring s, std::wstring_view from, std::wstring_view to)
            {
                if (from.empty())
                {
                    return s;
                }
                size_t pos = 0;
                while ((pos = s.find(from, pos)) != std::wstring::npos)
                {
                    s.replace(pos, from.size(), to);
                    pos += to.size();
                }
                return s;
            }

            // str.strip() / lstrip() / rstrip() with no argument (Python whitespace, see IsPySpace).
            std::wstring PyStrip(std::wstring_view s)
            {
                size_t begin = 0;
                size_t end = s.size();
                while (begin < end && IsPySpace(s[begin]))
                {
                    ++begin;
                }
                while (end > begin && IsPySpace(s[end - 1]))
                {
                    --end;
                }
                return std::wstring{ s.substr(begin, end - begin) };
            }

            std::wstring PyLStrip(std::wstring_view s)
            {
                size_t begin = 0;
                while (begin < s.size() && IsPySpace(s[begin]))
                {
                    ++begin;
                }
                return std::wstring{ s.substr(begin) };
            }

            std::wstring PyRStrip(std::wstring_view s)
            {
                size_t end = s.size();
                while (end > 0 && IsPySpace(s[end - 1]))
                {
                    --end;
                }
                return std::wstring{ s.substr(0, end) };
            }

            // " ".join(s.split()): every whitespace run becomes one space, the ends are trimmed.
            std::wstring PyJoinWs(std::wstring_view s)
            {
                std::wstring out;
                out.reserve(s.size());
                bool pendingSpace = false;
                for (const wchar_t c : s)
                {
                    if (IsPySpace(c))
                    {
                        pendingSpace = !out.empty(); // a space only BETWEEN words, never leading
                        continue;
                    }
                    if (pendingSpace)
                    {
                        out += L' ';
                        pendingSpace = false;
                    }
                    out += c;
                }
                return out;
            }

            // str.splitlines(): split at every line boundary (IsPyLineBreak; \r\n counts once), no
            // trailing empty piece for a final break, and nothing at all for "".
            std::vector<std::wstring> PySplitLines(std::wstring_view s)
            {
                std::vector<std::wstring> out;
                size_t start = 0;
                size_t i = 0;
                while (i < s.size())
                {
                    if (!IsPyLineBreak(s[i]))
                    {
                        ++i;
                        continue;
                    }
                    out.emplace_back(s.substr(start, i - start));
                    if (s[i] == L'\r' && i + 1 < s.size() && s[i + 1] == L'\n')
                    {
                        ++i;
                    }
                    ++i;
                    start = i;
                }
                if (start < s.size())
                {
                    out.emplace_back(s.substr(start));
                }
                return out;
            }

            // len() of a Python str: code points, so a surrogate pair (an emoji) counts once.
            size_t CpLen(std::wstring_view s) noexcept
            {
                size_t count = 0;
                for (size_t i = 0; i < s.size(); ++i)
                {
                    if (IsHighSurrogate(s[i]) && i + 1 < s.size() && IsLowSurrogate(s[i + 1]))
                    {
                        ++i;
                    }
                    ++count;
                }
                return count;
            }

            // s[:n] of a Python str: the first n code points (never splitting a surrogate pair).
            std::wstring CpPrefix(std::wstring_view s, size_t n)
            {
                size_t i = 0;
                size_t count = 0;
                while (i < s.size() && count < n)
                {
                    i += (IsHighSurrogate(s[i]) && i + 1 < s.size() && IsLowSurrogate(s[i + 1])) ? 2 : 1;
                    ++count;
                }
                return std::wstring{ s.substr(0, i) };
            }

            // The reference's cap: `text if len(text) <= limit else text[: limit - 1] + "…"`.
            std::wstring CapWithEllipsis(std::wstring_view text, size_t limit)
            {
                if (CpLen(text) <= limit)
                {
                    return std::wstring{ text };
                }
                return CpPrefix(text, limit > 0 ? limit - 1 : 0) + L"…";
            }

            std::wstring Join(const std::vector<std::wstring>& parts, std::wstring_view separator)
            {
                std::wstring out;
                for (size_t i = 0; i < parts.size(); ++i)
                {
                    if (i != 0)
                    {
                        out += separator;
                    }
                    out += parts[i];
                }
                return out;
            }

            // Python's integer `//` (floor division; C++'s `/` truncates toward zero instead).
            constexpr int64_t FloorDiv(int64_t a, int64_t b) noexcept
            {
                const int64_t q = a / b;
                return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
            }

            // Python's float `//` for the divisors used here, CPython's algorithm verbatim (fmod, then
            // snap the quotient to the nearest integral value) so a duration never lands one minute
            // off a boundary the reference prints.
            double PyFloorDivFloat(double v, double w) noexcept
            {
                const double mod = std::fmod(v, w);
                double div = (v - mod) / w;
                if (mod != 0 && ((w < 0) != (mod < 0)))
                {
                    div -= 1.0;
                }
                if (div == 0)
                {
                    return 0.0;
                }
                double floored = std::floor(div);
                if (div - floored > 0.5)
                {
                    floored += 1.0;
                }
                return floored;
            }

            // ---- JSON values with Python-dict semantics ----

            // The empty object every "x.get(...) or {}" in the reference falls back to.
            const Value& EmptyObject() noexcept
            {
                static const Value kEmpty = Value::MkObj();
                return kEmpty;
            }

            // dict.get(key): the member's value, or null when `obj` is not an object or lacks the key.
            // A key repeated in the JSON returns its LAST value, as json.loads builds the dict (Json.h's
            // own Find returns the first).
            const Value* JGet(const Value* obj, std::wstring_view key) noexcept
            {
                if (obj == nullptr || obj->type != VType::Obj)
                {
                    return nullptr;
                }
                const Value* found = nullptr;
                for (const auto& member : obj->members)
                {
                    if (member.first == key)
                    {
                        found = &member.second;
                    }
                }
                return found;
            }

            bool IsStr(const Value* v) noexcept
            {
                return v != nullptr && v->type == VType::Str;
            }

            bool IsObj(const Value* v) noexcept
            {
                return v != nullptr && v->type == VType::Obj;
            }

            bool IsArr(const Value* v) noexcept
            {
                return v != nullptr && v->type == VType::Arr;
            }

            // `v is True` — only the JSON literal true (not 1, not "true").
            bool IsTrueBool(const Value* v) noexcept
            {
                return v != nullptr && v->type == VType::Bool && v->boolean;
            }

            // Python truthiness of a JSON value (`if x:`): null/missing, false, 0, "", [] and {} are false.
            bool Truthy(const Value* v) noexcept
            {
                if (v == nullptr)
                {
                    return false;
                }
                switch (v->type)
                {
                case VType::Bool:
                    return v->boolean;
                case VType::Num:
                    return v->num != 0; // NaN is truthy in Python too (nan != 0)
                case VType::Str:
                    return !v->str.empty();
                case VType::Arr:
                    return !v->arr.empty();
                case VType::Obj:
                    return !v->members.empty();
                case VType::Null:
                default:
                    return false;
                }
            }

            bool StrEq(const Value* v, std::wstring_view s) noexcept
            {
                return IsStr(v) && v->str == s;
            }

            // The value when it is a string, else `fallback`.
            std::wstring StrOr(const Value* v, std::wstring_view fallback = {})
            {
                return IsStr(v) ? v->str : std::wstring{ fallback };
            }

            // `x if isinstance(x, dict) else {}` — and every "(x or {}).get(...)" read.
            const Value* ObjOrEmpty(const Value* v) noexcept
            {
                return IsObj(v) ? v : &EmptyObject();
            }

            // dict.items(): each key ONCE, at its first position, with its LAST value (json.loads'
            // dict semantics for a repeated key). The views point into `obj`, which must outlive them.
            std::vector<std::pair<std::wstring_view, const Value*>> PyItems(const Value* obj)
            {
                std::vector<std::pair<std::wstring_view, const Value*>> items;
                if (!IsObj(obj))
                {
                    return items;
                }
                for (const auto& member : obj->members)
                {
                    const auto it = std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.first == member.first; });
                    if (it != items.end())
                    {
                        it->second = &member.second;
                    }
                    else
                    {
                        items.emplace_back(member.first, &member.second);
                    }
                }
                return items;
            }

            std::wstring WidenAscii(std::string_view ascii)
            {
                return std::wstring(ascii.begin(), ascii.end());
            }

            // repr() of a Python float: the shortest round-trip digits, positional when the decimal
            // exponent is in [-4, 16) (always with a ".0" or fraction), scientific otherwise
            // ("1e-05", "1.5e+16") — CPython's float_repr_style 'short' rules.
            std::wstring PyFloatRepr(double x)
            {
                if (std::isnan(x))
                {
                    return L"nan";
                }
                if (std::isinf(x))
                {
                    return x < 0 ? L"-inf" : L"inf";
                }
                if (x == 0)
                {
                    return std::signbit(x) ? L"-0.0" : L"0.0";
                }
                char buf[64];
                const auto res = std::to_chars(buf, buf + sizeof(buf), x, std::chars_format::scientific);
                if (res.ec != std::errc{})
                {
                    return L"0.0"; // unreachable: 64 bytes hold any double's shortest form
                }
                const std::string sci(buf, res.ptr); // e.g. "-1.2345e+16"
                const bool negative = sci[0] == '-';
                const size_t ePos = sci.find('e');
                std::string digits;
                for (size_t k = negative ? 1 : 0; k < ePos; ++k)
                {
                    if (sci[k] != '.')
                    {
                        digits += sci[k];
                    }
                }
                const int exp10 = std::atoi(sci.c_str() + ePos + 1);
                std::string out = negative ? "-" : "";
                if (exp10 >= -4 && exp10 < 16)
                {
                    if (exp10 >= 0)
                    {
                        const size_t intDigits = static_cast<size_t>(exp10) + 1;
                        if (digits.size() <= intDigits)
                        {
                            out += digits;
                            out.append(intDigits - digits.size(), '0');
                            out += ".0";
                        }
                        else
                        {
                            out += digits.substr(0, intDigits);
                            out += '.';
                            out += digits.substr(intDigits);
                        }
                    }
                    else
                    {
                        out += "0.";
                        out.append(static_cast<size_t>(-exp10 - 1), '0');
                        out += digits;
                    }
                }
                else
                {
                    out += digits[0];
                    if (digits.size() > 1)
                    {
                        out += '.';
                        out += digits.substr(1);
                    }
                    out += 'e';
                    out += exp10 < 0 ? '-' : '+';
                    const int magnitude = exp10 < 0 ? -exp10 : exp10;
                    if (magnitude < 10)
                    {
                        out += '0';
                    }
                    out += std::to_string(magnitude);
                }
                return WidenAscii(out);
            }

            // str() of a JSON number. Json.h keeps every number as a double, so an integral value
            // prints as the int json.loads would have produced for it ("5"); see the header's
            // divergence note for "5.0".
            std::wstring PyNumStr(double x)
            {
                if (std::isfinite(x) && x == std::trunc(x) && std::fabs(x) < 9.0e18)
                {
                    return std::to_wstring(static_cast<long long>(x));
                }
                return PyFloatRepr(x);
            }

            // repr() of a Python str: single quotes unless the text holds a ' and no ", with CPython's
            // backslash escapes for the quote, the backslash and the ASCII control characters. ⚠ CPython
            // also escapes every NON-ASCII character Unicode calls non-printable (U+00A0, U+200D, a lone
            // surrogate, ...); this keeps those as they are — a documented divergence (TranscriptTurns.h)
            // that only shows inside a malformed, container-valued argument.
            std::wstring PyStrRepr(std::wstring_view s)
            {
                const bool hasSingle = s.find(L'\'') != std::wstring_view::npos;
                const bool hasDouble = s.find(L'"') != std::wstring_view::npos;
                const wchar_t quote = (hasSingle && !hasDouble) ? L'"' : L'\'';
                std::wstring out(1, quote);
                for (const wchar_t c : s)
                {
                    if (c == quote || c == L'\\')
                    {
                        out += L'\\';
                        out += c;
                    }
                    else if (c == L'\n')
                    {
                        out += L"\\n";
                    }
                    else if (c == L'\r')
                    {
                        out += L"\\r";
                    }
                    else if (c == L'\t')
                    {
                        out += L"\\t";
                    }
                    else if (c < 0x20 || c == 0x7F)
                    {
                        wchar_t buf[8];
                        std::swprintf(buf, 8, L"\\x%02x", static_cast<unsigned>(c));
                        out += buf;
                    }
                    else
                    {
                        out += c;
                    }
                }
                out += quote;
                return out;
            }

            // repr() of any JSON value as json.loads would have built it (None, True, 5, 'x', [..], {..}).
            std::wstring PyRepr(const Value& v)
            {
                switch (v.type)
                {
                case VType::Null:
                    return L"None";
                case VType::Bool:
                    return v.boolean ? L"True" : L"False";
                case VType::Num:
                    return PyNumStr(v.num);
                case VType::Str:
                    return PyStrRepr(v.str);
                case VType::Arr:
                {
                    std::wstring out = L"[";
                    for (size_t i = 0; i < v.arr.size(); ++i)
                    {
                        if (i != 0)
                        {
                            out += L", ";
                        }
                        out += PyRepr(v.arr[i]);
                    }
                    return out + L"]";
                }
                case VType::Obj:
                default:
                {
                    std::wstring out = L"{";
                    bool first = true;
                    for (const auto& [key, value] : PyItems(&v))
                    {
                        if (!first)
                        {
                            out += L", ";
                        }
                        first = false;
                        out += PyStrRepr(key) + L": " + PyRepr(*value);
                    }
                    return out + L"}";
                }
                }
            }

            // str() of a JSON value — what an f-string interpolates: a string is itself, null is
            // "None", a bool "True"/"False", a container its repr. A missing value (`.get()` → None)
            // is "None" too.
            std::wstring PyStr(const Value* v)
            {
                if (v == nullptr)
                {
                    return L"None";
                }
                return IsStr(v) ? v->str : PyRepr(*v);
            }

            // type(v).__name__ for the values the generic tool summary reports by type.
            std::wstring PyTypeName(const Value* v)
            {
                if (v == nullptr || v->type == VType::Null)
                {
                    return L"NoneType";
                }
                switch (v->type)
                {
                case VType::Arr:
                    return L"list";
                case VType::Obj:
                    return L"dict";
                case VType::Bool:
                    return L"bool";
                case VType::Str:
                    return L"str";
                case VType::Num:
                default:
                    return (std::isfinite(v->num) && v->num == std::trunc(v->num)) ? L"int" : L"float";
                }
            }

            // json.dumps(v, ensure_ascii=False) with Python's default separators (", " and ": ").
            void PyJsonDumpsTo(const Value& v, std::wstring& out)
            {
                switch (v.type)
                {
                case VType::Null:
                    out += L"null";
                    return;
                case VType::Bool:
                    out += v.boolean ? L"true" : L"false";
                    return;
                case VType::Num:
                    out += std::isnan(v.num) ? std::wstring{ L"NaN" } : std::isinf(v.num) ? std::wstring{ v.num < 0 ? L"-Infinity" : L"Infinity" } : PyNumStr(v.num);
                    return;
                case VType::Str:
                    out += L'"';
                    for (const wchar_t c : v.str)
                    {
                        switch (c)
                        {
                        case L'"':
                            out += L"\\\"";
                            break;
                        case L'\\':
                            out += L"\\\\";
                            break;
                        case L'\n':
                            out += L"\\n";
                            break;
                        case L'\r':
                            out += L"\\r";
                            break;
                        case L'\t':
                            out += L"\\t";
                            break;
                        case L'\b':
                            out += L"\\b";
                            break;
                        case L'\f':
                            out += L"\\f";
                            break;
                        default:
                            if (c < 0x20)
                            {
                                wchar_t buf[8];
                                std::swprintf(buf, 8, L"\\u%04x", static_cast<unsigned>(c));
                                out += buf;
                            }
                            else
                            {
                                out += c;
                            }
                            break;
                        }
                    }
                    out += L'"';
                    return;
                case VType::Arr:
                    out += L'[';
                    for (size_t i = 0; i < v.arr.size(); ++i)
                    {
                        if (i != 0)
                        {
                            out += L", ";
                        }
                        PyJsonDumpsTo(v.arr[i], out);
                    }
                    out += L']';
                    return;
                case VType::Obj:
                default:
                {
                    out += L'{';
                    bool first = true;
                    for (const auto& [key, value] : PyItems(&v))
                    {
                        if (!first)
                        {
                            out += L", ";
                        }
                        first = false;
                        Value keyValue = Value::MkStr(std::wstring{ key });
                        PyJsonDumpsTo(keyValue, out);
                        out += L": ";
                        PyJsonDumpsTo(*value, out);
                    }
                    out += L'}';
                    return;
                }
                }
            }

            std::wstring PyJsonDumps(const Value& v)
            {
                std::wstring out;
                PyJsonDumpsTo(v, out);
                return out;
            }

            // Python `a == b` between two JSON values (numbers compare by value, True == 1).
            bool PyEquals(const Value* a, const Value* b)
            {
                const bool aNone = a == nullptr || a->type == VType::Null;
                const bool bNone = b == nullptr || b->type == VType::Null;
                if (aNone || bNone)
                {
                    return aNone && bNone;
                }
                const auto asNumber = [](const Value* v, double& out) {
                    if (v->type == VType::Num)
                    {
                        out = v->num;
                        return true;
                    }
                    if (v->type == VType::Bool)
                    {
                        out = v->boolean ? 1.0 : 0.0;
                        return true;
                    }
                    return false;
                };
                double x = 0;
                double y = 0;
                if (asNumber(a, x) && asNumber(b, y))
                {
                    return x == y;
                }
                if (a->type != b->type)
                {
                    return false;
                }
                return json::Dump(*a) == json::Dump(*b);
            }

            // len(x or []) — the item count of a list, the code points of a str, the keys of a dict.
            size_t PyLen(const Value* v)
            {
                if (!Truthy(v))
                {
                    return 0;
                }
                switch (v->type)
                {
                case VType::Arr:
                    return v->arr.size();
                case VType::Str:
                    return CpLen(v->str);
                case VType::Obj:
                    return PyItems(v).size();
                default:
                    return 0; // len() of a number raises in Python; nothing to count here
                }
            }

            // The first `-?\d+` run in a string (safe_int's regex), saturated to int64.
            std::optional<int64_t> FirstIntIn(std::wstring_view s)
            {
                for (size_t i = 0; i < s.size(); ++i)
                {
                    size_t j = i;
                    const bool negative = s[j] == L'-';
                    if (negative)
                    {
                        ++j;
                    }
                    if (j >= s.size() || !IsAsciiDigit(s[j]))
                    {
                        continue; // a '-' not followed by a digit starts no match (the regex backtracks past it)
                    }
                    int64_t value = 0;
                    for (; j < s.size() && IsAsciiDigit(s[j]); ++j)
                    {
                        const int digit = s[j] - L'0';
                        value = value > (INT64_MAX - digit) / 10 ? INT64_MAX : value * 10 + digit;
                    }
                    return negative ? -value : value;
                }
                return std::nullopt;
            }

            // safe_int(): a number read from a model-written argument or metadata field without
            // trusting its type — an int / float as is (truncated), the first digit run of a string
            // ("1025," -> 1025), the first digit run of a container's repr; none for null and bools.
            std::optional<int64_t> SafeInt(const Value* v)
            {
                if (v == nullptr)
                {
                    return std::nullopt;
                }
                switch (v->type)
                {
                case VType::Null:
                case VType::Bool:
                    return std::nullopt; // bool is an int subclass in Python, but True is not an offset
                case VType::Num:
                    if (!std::isfinite(v->num))
                    {
                        return std::nullopt;
                    }
                    if (v->num >= 9.2e18 || v->num <= -9.2e18)
                    {
                        return v->num > 0 ? INT64_MAX : INT64_MIN;
                    }
                    return static_cast<int64_t>(v->num);
                case VType::Str:
                    return FirstIntIn(v->str);
                default:
                    return FirstIntIn(PyRepr(*v));
                }
            }

            // int(x or 0) over a usage field: 0 for anything falsy, the number (truncated), a
            // strictly numeric string's value, 1 for true. (The reference raises on a non-numeric
            // string; a stats line counting it as 0 is the lenient reading.)
            int64_t PyIntOrZero(const Value* v)
            {
                if (!Truthy(v))
                {
                    return 0;
                }
                if (v->type == VType::Bool)
                {
                    return 1;
                }
                if (v->type == VType::Num)
                {
                    return std::isfinite(v->num) ? static_cast<int64_t>(v->num) : 0;
                }
                if (v->type == VType::Str)
                {
                    const std::wstring t = PyStrip(v->str);
                    size_t i = (!t.empty() && (t[0] == L'-' || t[0] == L'+')) ? 1 : 0;
                    if (i >= t.size())
                    {
                        return 0;
                    }
                    int64_t value = 0;
                    for (; i < t.size(); ++i)
                    {
                        if (!IsAsciiDigit(t[i]))
                        {
                            return 0;
                        }
                        const int digit = t[i] - L'0';
                        value = value > (INT64_MAX - digit) / 10 ? INT64_MAX : value * 10 + digit; // saturate: no signed overflow
                    }
                    return t[0] == L'-' ? -value : value;
                }
                return 0;
            }

            // =====================================================================================
            // 2. Formatting helpers
            // =====================================================================================

            // fmt_duration(): 0.4s, 12s, 4m 40s, 1h 02m. Negative (and NaN) is treated as 0.
            std::wstring FmtDuration(double seconds)
            {
                const double s = seconds > 0.0 ? seconds : 0.0;
                wchar_t buf[64];
                if (s < 10)
                {
                    std::swprintf(buf, 64, L"%.1fs", s);
                    return buf;
                }
                if (s < 60)
                {
                    return std::to_wstring(static_cast<long long>(s)) + L"s";
                }
                if (s < 3600)
                {
                    std::swprintf(buf, 64, L"%lldm %02llds", static_cast<long long>(PyFloorDivFloat(s, 60.0)), static_cast<long long>(std::fmod(s, 60.0)));
                    return buf;
                }
                std::swprintf(buf, 64, L"%lldh %02lldm", static_cast<long long>(PyFloorDivFloat(s, 3600.0)), static_cast<long long>(PyFloorDivFloat(std::fmod(s, 3600.0), 60.0)));
                return buf;
            }

            // fmt_tokens(): 812, 24.1k, 1.23M.
            std::wstring FmtTokens(int64_t n)
            {
                if (n < 1000)
                {
                    return std::to_wstring(n);
                }
                wchar_t buf[64];
                if (n < 1'000'000)
                {
                    std::swprintf(buf, 64, L"%.1fk", static_cast<double>(n) / 1000.0);
                }
                else
                {
                    std::swprintf(buf, 64, L"%.2fM", static_cast<double>(n) / 1'000'000.0);
                }
                return buf;
            }

            // quote(): blockquotes text line by line ("> line", a bare ">" for a blank one) so its own
            // Markdown stays inside the quote. "" for whitespace-only text.
            std::wstring Quote(std::wstring_view text)
            {
                std::wstring out;
                bool first = true;
                for (const auto& line : PySplitLines(PyStrip(text)))
                {
                    if (!first)
                    {
                        out += L'\n';
                    }
                    first = false;
                    if (!PyStrip(line).empty())
                    {
                        out += L"> ";
                        out += line;
                    }
                    else
                    {
                        out += L">";
                    }
                }
                return out;
            }

            // plus_lines(): "+1 line" / "+N lines".
            std::wstring PlusLines(size_t n)
            {
                return n == 1 ? std::wstring{ L"+1 line" } : L"+" + std::to_wstring(n) + L" lines";
            }

            // one_line(): the first line of a (possibly multi-line) value, capped at `limit` code
            // points, with a " … (+N lines)" tail when more lines followed.
            std::wstring OneLine(std::wstring_view value, size_t limit = 110)
            {
                std::vector<std::wstring> lines = PySplitLines(PyStrip(value));
                if (lines.empty())
                {
                    lines.emplace_back();
                }
                std::wstring first = PyStrip(lines[0]);
                if (CpLen(first) > limit)
                {
                    first = CpPrefix(first, limit - 1) + L"…";
                }
                if (lines.size() > 1)
                {
                    first += L" … (" + PlusLines(lines.size() - 1) + L")";
                }
                return first;
            }

            // flat(): every whitespace run (newlines included) collapsed, capped at `limit` code points.
            std::wstring Flat(std::wstring_view value, size_t limit)
            {
                return CapWithEllipsis(PyJoinWs(value), limit);
            }

            // code(): an inline-code span that survives backticks inside the text.
            std::wstring Code(std::wstring_view text)
            {
                if (text.find(L'`') != std::wstring_view::npos)
                {
                    return L"`` " + std::wstring{ text } + L" ``";
                }
                return L"`" + std::wstring{ text } + L"`";
            }

            // indented_block(): a fenced code block inside a dash-list item, content verbatim. The
            // fence grows to four backticks when the text holds three. `bullet` opens it as a "  - "
            // sub-item (nested-blocks), its lines then indented 4; else a bare block indented 2.
            std::wstring IndentedBlock(std::wstring_view text, std::wstring_view lang, bool bullet)
            {
                const std::wstring fence = text.find(L"```") != std::wstring_view::npos ? L"````" : L"```";
                const std::wstring lead = bullet ? L"  - " : L"  ";
                const std::wstring indent = bullet ? L"    " : L"  ";
                std::wstring out = lead + fence + std::wstring{ lang };
                for (const auto& line : PySplitLines(PyRStrip(text)))
                {
                    out += L'\n';
                    if (!PyStrip(line).empty())
                    {
                        out += indent;
                        out += line;
                    }
                }
                out += L'\n';
                out += indent;
                out += fence;
                return out;
            }

            // question_texts(): the question texts an AskUserQuestion call's arguments carry, whatever
            // their shape — an object's "question" (str() of a non-string, "" for null/absent), a bare
            // string entry, or a bare-string `questions` as the one question.
            std::vector<std::wstring> QuestionTexts(const Value* inp)
            {
                const Value* qs = JGet(inp, L"questions");
                if (IsStr(qs))
                {
                    return { qs->str };
                }
                if (!IsArr(qs))
                {
                    return {};
                }
                std::vector<std::wstring> texts;
                for (const Value& entry : qs->arr)
                {
                    if (IsObj(&entry))
                    {
                        const Value* question = JGet(&entry, L"question");
                        texts.push_back((question == nullptr || question->type == VType::Null) ? std::wstring{} : PyStr(question));
                    }
                    else if (IsStr(&entry))
                    {
                        texts.push_back(entry.str);
                    }
                    else
                    {
                        texts.emplace_back();
                    }
                }
                return texts;
            }

            // Path(path).suffix.lower() on Windows: a file name's suffix (".py"; "" for "x", ".bashrc", "x."),
            // lowercased (ASCII: every _LANG key is). The drive and root are split off first exactly as
            // ntpath.splitroot does it ("X:", "\\server\share", "\\?\UNC\server\share" — any '/' read as '\'),
            // then the NAME is the last part that is neither empty nor "." (pathlib drops both, so "x.py\" and
            // "x.py\." still name x.py, and "C:x.py" names x.py), and its suffix starts at the name's last '.'
            // unless that dot leads or ends the name.
            //   path — the path as written in a tool argument (anything; never touched on disk).
            // Returns the lowercased suffix, or "" when there is none. Throws std::bad_alloc only.
            std::wstring LowerSuffixOf(std::wstring_view path)
            {
                std::wstring norm{ path };
                std::replace(norm.begin(), norm.end(), L'/', L'\\');
                size_t relStart = 0;
                if (!norm.empty() && norm[0] == L'\\')
                {
                    if (norm.size() >= 2 && norm[1] == L'\\')
                    {
                        // A UNC or device path: the drive runs through "\\server\share" (counted after a
                        // leading "\\?\UNC\" when there is one). Without both parts it is ALL drive: no name.
                        const size_t start = (norm.size() >= 8 && StartsWithCi(norm, L"\\\\?\\UNC\\")) ? 8 : 2;
                        const size_t index = norm.find(L'\\', start);
                        if (index == std::wstring::npos)
                        {
                            return {};
                        }
                        const size_t index2 = norm.find(L'\\', index + 1);
                        if (index2 == std::wstring::npos)
                        {
                            return {};
                        }
                        relStart = index2 + 1;
                    }
                    else
                    {
                        relStart = 1; // rooted without a drive ("\Windows")
                    }
                }
                else if (norm.size() >= 2 && norm[1] == L':')
                {
                    relStart = (norm.size() >= 3 && norm[2] == L'\\') ? 3 : 2; // "X:\..." or the drive-relative "X:..."
                }
                const std::wstring_view rel = std::wstring_view{ norm }.substr(relStart);
                std::wstring_view name;
                size_t end = rel.size();
                while (end > 0)
                {
                    const size_t sep = rel.rfind(L'\\', end - 1);
                    const size_t begin = sep == std::wstring_view::npos ? 0 : sep + 1;
                    const std::wstring_view part = rel.substr(begin, end - begin);
                    if (!part.empty() && part != L".")
                    {
                        name = part;
                        break;
                    }
                    if (sep == std::wstring_view::npos)
                    {
                        break;
                    }
                    end = sep;
                }
                const size_t dot = name.rfind(L'.');
                if (dot == std::wstring_view::npos || dot == 0 || dot + 1 == name.size())
                {
                    return {};
                }
                return LowerAscii(name.substr(dot));
            }

            // _LANG: a file extension -> its code-fence language, for full-write blocks.
            std::wstring FenceLanguageFor(std::wstring_view path)
            {
                static const std::pair<const wchar_t*, const wchar_t*> kLanguages[] = {
                    { L".py", L"python" },
                    { L".js", L"javascript" },
                    { L".ts", L"typescript" },
                    { L".json", L"json" },
                    { L".md", L"markdown" },
                    { L".sh", L"bash" },
                    { L".ps1", L"powershell" },
                    { L".cs", L"csharp" },
                    { L".cpp", L"cpp" },
                    { L".h", L"cpp" },
                    { L".c", L"c" },
                    { L".html", L"html" },
                    { L".css", L"css" },
                    { L".yml", L"yaml" },
                    { L".yaml", L"yaml" },
                    { L".toml", L"toml" },
                    { L".xml", L"xml" },
                    { L".sql", L"sql" },
                    { L".bat", L"bat" },
                    { L".cmd", L"bat" },
                };
                const std::wstring suffix = LowerSuffixOf(path);
                for (const auto& [ext, lang] : kLanguages)
                {
                    if (suffix == ext)
                    {
                        return lang;
                    }
                }
                return {};
            }

            // =====================================================================================
            // 3. Timestamps
            // =====================================================================================

            // A parsed transcript timestamp: microseconds since the epoch — the UTC instant for a
            // stamp with a zone ("Z", "+02:00"); for one without, its wall-clock fields read as UTC.
            struct IsoStamp
            {
                int64_t micros = 0;
                bool aware = false;
            };

            // Days from 1970-01-01 to y-m-d in the proleptic Gregorian calendar (Hinnant's days_from_civil).
            constexpr int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) noexcept
            {
                y -= m <= 2 ? 1 : 0;
                const int64_t era = (y >= 0 ? y : y - 399) / 400;
                const unsigned yoe = static_cast<unsigned>(y - era * 400);
                const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
                const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
                return era * 146097 + static_cast<int64_t>(doe) - 719468;
            }

            // The inverse: a day count since 1970-01-01 -> year / month / day (Hinnant's civil_from_days).
            void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) noexcept
            {
                z += 719468;
                const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
                const unsigned doe = static_cast<unsigned>(z - era * 146097);
                const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
                y = static_cast<int64_t>(yoe) + era * 400;
                const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
                const unsigned mp = (5 * doy + 2) / 153;
                d = doy - (153 * mp + 2) / 5 + 1;
                m = mp < 10 ? mp + 3 : mp - 9;
                y += m <= 2 ? 1 : 0;
            }

            bool IsLeapYear(int64_t y) noexcept
            {
                return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
            }

            unsigned DaysInMonth(int64_t y, unsigned m) noexcept
            {
                static const unsigned kDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
                return (m == 2 && IsLeapYear(y)) ? 29u : kDays[m - 1];
            }

            // `count` ASCII digits at s[pos..] as an int; false when any is missing or not a digit.
            bool ReadDigits(std::wstring_view s, size_t pos, size_t count, int& value) noexcept
            {
                if (pos + count > s.size())
                {
                    return false;
                }
                int v = 0;
                for (size_t i = 0; i < count; ++i)
                {
                    const wchar_t c = s[pos + i];
                    if (!IsAsciiDigit(c))
                    {
                        return false;
                    }
                    v = v * 10 + (c - L'0');
                }
                value = v;
                return true;
            }

            // The time part of an ISO stamp, as datetime.fromisoformat (3.11+) reads it: "HH",
            // "HH:MM", "HH:MM:SS" or the compact "HHMM" / "HHMMSS", then an optional fraction after
            // '.' or ',' (its first 6 digits are microseconds, more digits are dropped).
            bool ParseIsoTime(std::wstring_view t, int& hour, int& minute, int& second, int& micro) noexcept
            {
                hour = minute = second = micro = 0;
                int* fields[3] = { &hour, &minute, &second };
                size_t p = 0;
                bool separated = false;
                bool fraction = false;
                for (int i = 0; i < 3; ++i)
                {
                    if (!ReadDigits(t, p, 2, *fields[i]))
                    {
                        return false;
                    }
                    p += 2;
                    if (p >= t.size())
                    {
                        break; // the time ends cleanly after this component
                    }
                    const wchar_t c = t[p];
                    if (i == 0)
                    {
                        separated = c == L':';
                    }
                    if (separated && c == L':' && i < 2)
                    {
                        ++p;
                        continue;
                    }
                    if ((c == L'.' || c == L',') && i == 2)
                    {
                        ++p;
                        fraction = true;
                        break;
                    }
                    if (!separated && IsAsciiDigit(c) && i < 2)
                    {
                        continue; // compact form: the next component follows directly
                    }
                    return false;
                }
                if (fraction)
                {
                    size_t digits = 0;
                    int value = 0;
                    while (p < t.size() && IsAsciiDigit(t[p]))
                    {
                        if (digits < 6)
                        {
                            value = value * 10 + (t[p] - L'0');
                            ++digits;
                        }
                        ++p;
                    }
                    if (digits == 0)
                    {
                        return false;
                    }
                    for (size_t k = digits; k < 6; ++k)
                    {
                        value *= 10;
                    }
                    micro = value;
                }
                if (p != t.size())
                {
                    return false;
                }
                return hour < 24 && minute < 60 && second < 60;
            }

            // A UTC offset "+HH:MM[:SS[.ffffff]]", "+HHMM" or "+HH" (sign included) in seconds.
            bool ParseIsoOffset(std::wstring_view z, int64_t& offsetSeconds) noexcept
            {
                if (z.empty() || (z[0] != L'+' && z[0] != L'-'))
                {
                    return false;
                }
                const int sign = z[0] == L'-' ? -1 : 1;
                int hour = 0;
                int minute = 0;
                int second = 0;
                int micro = 0;
                if (!ParseIsoTime(z.substr(1), hour, minute, second, micro))
                {
                    return false;
                }
                offsetSeconds = sign * (static_cast<int64_t>(hour) * 3600 + minute * 60 + second);
                return true;
            }

            // parse_ts(): datetime.fromisoformat(ts.replace("Z", "+00:00")) — nullopt when empty or malformed.
            std::optional<IsoStamp> ParseIsoTimestamp(std::wstring_view raw)
            {
                if (raw.empty())
                {
                    return std::nullopt;
                }
                const std::wstring ts = ReplaceAll(std::wstring{ raw }, L"Z", L"+00:00");
                const std::wstring_view s{ ts };
                int year = 0;
                int month = 0;
                int day = 0;
                if (s.size() < 10 || !ReadDigits(s, 0, 4, year) || s[4] != L'-' || !ReadDigits(s, 5, 2, month) || s[7] != L'-' || !ReadDigits(s, 8, 2, day))
                {
                    return std::nullopt;
                }
                if (year < 1 || month < 1 || month > 12 || day < 1 || static_cast<unsigned>(day) > DaysInMonth(year, static_cast<unsigned>(month)))
                {
                    return std::nullopt;
                }
                int hour = 0;
                int minute = 0;
                int second = 0;
                int micro = 0;
                int64_t offsetSeconds = 0;
                bool aware = false;
                if (s.size() > 10)
                {
                    if (s.size() == 11)
                    {
                        return std::nullopt; // a separator with no time after it
                    }
                    const std::wstring_view rest = s.substr(11); // s[10] is the separator: any one character
                    const size_t zone = rest.find_first_of(L"+-");
                    const std::wstring_view timePart = zone == std::wstring_view::npos ? rest : rest.substr(0, zone);
                    if (!ParseIsoTime(timePart, hour, minute, second, micro))
                    {
                        return std::nullopt;
                    }
                    if (zone != std::wstring_view::npos)
                    {
                        if (!ParseIsoOffset(rest.substr(zone), offsetSeconds))
                        {
                            return std::nullopt;
                        }
                        aware = true;
                    }
                }
                const int64_t days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
                const int64_t wall = ((days * 24 + hour) * 60 + minute) * 60 + second;
                return IsoStamp{ (wall - offsetSeconds) * kMicrosPerSecond + micro, aware };
            }

            // aware_ts(): parse_ts for comparisons — microseconds since the epoch; a stamp without a
            // zone counts as UTC. nullopt when empty or malformed.
            std::optional<int64_t> AwareTs(std::wstring_view ts)
            {
                const auto parsed = ParseIsoTimestamp(ts);
                if (!parsed)
                {
                    return std::nullopt;
                }
                return parsed->micros;
            }

            // aware_ts() over a JSON field (anything that isn't a string reads as missing).
            std::optional<int64_t> AwareTs(const Value* v)
            {
                return IsStr(v) ? AwareTs(v->str) : std::nullopt;
            }

            // (end - start).total_seconds() between two parsed stamps.
            double SecondsBetween(const IsoStamp& start, const IsoStamp& end) noexcept
            {
                return static_cast<double>(end.micros - start.micros) / 1e6;
            }

            // A UTC instant -> this machine's local wall clock, exactly as the reference's
            // astimezone() gets it on Windows: the CRT's localtime (honoring TZ and the system zone)
            // at the whole second, its offset applied to the instant. Out of the CRT's range (before
            // 1970): the instant stays UTC.
            int64_t UtcToLocalMicros(int64_t utcMicros) noexcept
            {
                const __time64_t seconds = static_cast<__time64_t>(FloorDiv(utcMicros, kMicrosPerSecond));
                tm local{};
                if (_localtime64_s(&local, &seconds) != 0)
                {
                    return utcMicros;
                }
                const __time64_t localAsUtc = _mkgmtime64(&local);
                if (localAsUtc == -1)
                {
                    return utcMicros;
                }
                return utcMicros + static_cast<int64_t>(localAsUtc - seconds) * kMicrosPerSecond;
            }

            // strftime("%b %d %H:%M") with the C locale's English month names: "Sep 26 19:34".
            std::wstring FormatMonthDayTime(int64_t micros)
            {
                static const wchar_t* const kMonths[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
                const int64_t seconds = FloorDiv(micros, kMicrosPerSecond);
                const int64_t days = FloorDiv(seconds, 86400);
                const int64_t ofDay = seconds - days * 86400;
                int64_t year = 0;
                unsigned month = 1;
                unsigned day = 1;
                CivilFromDays(days, year, month, day);
                wchar_t buf[32];
                std::swprintf(buf, 32, L"%ls %02u %02d:%02d", kMonths[(month - 1) % 12], day, static_cast<int>(ofDay / 3600), static_cast<int>((ofDay % 3600) / 60));
                return buf;
            }

            // Epoch seconds -> microseconds the way datetime.fromtimestamp rounds the fraction (half to even).
            int64_t EpochSecondsToMicros(double seconds) noexcept
            {
                double whole = 0;
                const double fraction = std::modf(seconds, &whole);
                int64_t micros = static_cast<int64_t>(std::nearbyint(fraction * 1e6));
                int64_t secs = static_cast<int64_t>(whole);
                if (micros >= kMicrosPerSecond)
                {
                    secs += 1;
                    micros -= kMicrosPerSecond;
                }
                else if (micros < 0)
                {
                    secs -= 1;
                    micros += kMicrosPerSecond;
                }
                return secs * kMicrosPerSecond + micros;
            }

            // =====================================================================================
            // 4. Parsing
            // =====================================================================================

            // json.loads on one line's text: the value must be the WHOLE text. Python rejects
            // "Extra data" after the value, which Json.h's own Parse does not check.
            std::optional<Value> ParseJsonExact(std::wstring_view text)
            {
                json::detail::Parser parser{ text };
                Value value = parser.Val();
                if (!parser.ok)
                {
                    return std::nullopt;
                }
                parser.Ws();
                if (parser.i != text.size())
                {
                    return std::nullopt;
                }
                return value;
            }

            // UTF-8 -> UTF-16, refusing invalid UTF-8 the way Python's decoder does (MB_ERR_INVALID_CHARS).
            std::optional<std::wstring> DecodeUtf8Strict(std::string_view bytes)
            {
                if (bytes.empty())
                {
                    return std::wstring{};
                }
                if (bytes.size() > static_cast<size_t>(INT_MAX))
                {
                    return std::nullopt;
                }
                const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
                if (n <= 0)
                {
                    return std::nullopt;
                }
                std::wstring out(static_cast<size_t>(n), L'\0');
                ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
                return out;
            }

            // One JSONL line -> the object it holds: json.loads(raw_bytes), objects only. A UTF-8 BOM
            // at the line's start is accepted (json.loads detects 'utf-8-sig' on bytes).
            std::optional<Value> ParseJsonlLine(std::string_view raw)
            {
                const auto text = DecodeUtf8Strict(raw);
                if (!text)
                {
                    return std::nullopt;
                }
                std::wstring_view view{ *text };
                if (!view.empty() && view.front() == 0xFEFF)
                {
                    view.remove_prefix(1);
                }
                auto value = ParseJsonExact(view);
                if (!value || value->type != VType::Obj)
                {
                    return std::nullopt;
                }
                return value;
            }

            Lines PointersTo(const std::vector<Value>& values)
            {
                Lines out;
                out.reserve(values.size());
                for (const auto& v : values)
                {
                    out.push_back(&v);
                }
                return out;
            }

            // text_of(): the plain text of a message content — a string as is, or the "text" blocks
            // of a list joined with a blank line ("" for anything else).
            std::wstring JoinTextBlocks(const Value* content, std::wstring_view separator)
            {
                if (IsStr(content))
                {
                    return content->str;
                }
                if (!IsArr(content))
                {
                    return {};
                }
                std::wstring out;
                bool first = true;
                for (const Value& block : content->arr)
                {
                    if (!IsObj(&block) || !StrEq(JGet(&block, L"type"), L"text"))
                    {
                        continue;
                    }
                    if (!first)
                    {
                        out += separator;
                    }
                    first = false;
                    out += StrOr(JGet(&block, L"text"));
                }
                return out;
            }

            std::wstring TextOf(const Value* content)
            {
                return JoinTextBlocks(content, L"\n\n");
            }

            // result_text_of(): a tool_result's text blocks joined by a single newline (images skipped).
            std::wstring ResultTextOf(const Value* content)
            {
                return JoinTextBlocks(content, L"\n");
            }

            // A user line's content is a list holding a tool_result block (a tool's answer, not typed input).
            bool HasToolResult(const Value* content)
            {
                if (!IsArr(content))
                {
                    return false;
                }
                for (const Value& block : content->arr)
                {
                    if (IsObj(&block) && StrEq(JGet(&block, L"type"), L"tool_result"))
                    {
                        return true;
                    }
                }
                return false;
            }

            // first_prompt_text(): the first message that would start a turn — a user text that is not
            // meta, a compaction summary, a tool result, a slash-command echo or output, or an
            // interrupt marker. "" when there is none.
            std::wstring FirstPromptText(const Lines& lines)
            {
                for (const Value* line : lines)
                {
                    if (!StrEq(JGet(line, L"type"), L"user") || Truthy(JGet(line, L"isMeta")) || Truthy(JGet(line, L"isCompactSummary")))
                    {
                        continue;
                    }
                    const Value* content = JGet(ObjOrEmpty(JGet(line, L"message")), L"content");
                    if (HasToolResult(content))
                    {
                        continue;
                    }
                    std::wstring text = PyStrip(TextOf(content));
                    if (!text.empty() && !StartsWith(text, L"<command-name>") && !StartsWith(text, L"<local-command-stdout>") && !StartsWith(text, L"[Request interrupted"))
                    {
                        return text;
                    }
                }
                return {};
            }

            // ---- peer messages (a teammate or another session talking to this one) ----

            struct PeerMessage
            {
                std::wstring sender; // the teammate / session it came from
                std::wstring said; // what they said, in words (describe_peer_payload)
                std::wstring note; // their own one-line summary, or ""
            };

            struct PeerBlock
            {
                std::wstring attrs;
                std::wstring inner;
            };

            // _PEER_BLOCK.findall: <(teammate-message|agent-message)\b([^>]*)>(.*?)</\1> (re.S) — each
            // block's attribute text and inner text, scanning left to right without overlap.
            std::vector<PeerBlock> FindPeerBlocks(std::wstring_view s)
            {
                std::vector<PeerBlock> found;
                size_t pos = 0;
                while (pos < s.size())
                {
                    const size_t lt = s.find(L'<', pos);
                    if (lt == std::wstring_view::npos)
                    {
                        break;
                    }
                    std::wstring_view tag;
                    if (StartsWith(s.substr(lt + 1), L"teammate-message"))
                    {
                        tag = L"teammate-message";
                    }
                    else if (StartsWith(s.substr(lt + 1), L"agent-message"))
                    {
                        tag = L"agent-message";
                    }
                    const size_t afterName = lt + 1 + tag.size();
                    // \b after the name: its last character is a word character, so the next one
                    // must not be (or the text ends) — "<agent-messages>" is not this tag.
                    if (tag.empty() || (afterName < s.size() && IsPyWordChar(s[afterName])))
                    {
                        pos = lt + 1;
                        continue;
                    }
                    const size_t gt = s.find(L'>', afterName); // [^>]* runs to the first '>'
                    if (gt == std::wstring_view::npos)
                    {
                        pos = lt + 1;
                        continue;
                    }
                    const std::wstring close = L"</" + std::wstring{ tag } + L">";
                    const size_t end = s.find(close, gt + 1); // (.*?) stops at the first closing tag
                    if (end == std::wstring_view::npos)
                    {
                        pos = lt + 1;
                        continue;
                    }
                    found.push_back(PeerBlock{ std::wstring{ s.substr(afterName, gt - afterName) }, std::wstring{ s.substr(gt + 1, end - gt - 1) } });
                    pos = end + close.size();
                }
                return found;
            }

            // dict(_PEER_ATTR.findall(attrs)): every `name="value"` pair ([\w-]+ names), a repeated
            // name keeping its first position and its last value.
            std::vector<std::pair<std::wstring, std::wstring>> PeerAttrs(std::wstring_view s)
            {
                std::vector<std::pair<std::wstring, std::wstring>> attrs;
                const auto isNameChar = [](wchar_t c) { return IsPyWordChar(c) || c == L'-'; };
                size_t i = 0;
                while (i < s.size())
                {
                    if (!isNameChar(s[i]))
                    {
                        ++i;
                        continue;
                    }
                    size_t j = i;
                    while (j < s.size() && isNameChar(s[j]))
                    {
                        ++j;
                    }
                    // Only a whole run can match: every suffix of it ends at the same j, so if the run
                    // isn't followed by ="..." none of its suffixes is either.
                    if (j + 1 < s.size() && s[j] == L'=' && s[j + 1] == L'"')
                    {
                        const size_t close = s.find(L'"', j + 2);
                        if (close != std::wstring_view::npos)
                        {
                            std::wstring name{ s.substr(i, j - i) };
                            std::wstring value{ s.substr(j + 2, close - j - 2) };
                            const auto it = std::find_if(attrs.begin(), attrs.end(), [&](const auto& a) { return a.first == name; });
                            if (it != attrs.end())
                            {
                                it->second = std::move(value);
                            }
                            else
                            {
                                attrs.emplace_back(std::move(name), std::move(value));
                            }
                            i = close + 1;
                            continue;
                        }
                    }
                    i = j;
                }
                return attrs;
            }

            std::wstring AttrOf(const std::vector<std::pair<std::wstring, std::wstring>>& attrs, std::wstring_view name)
            {
                for (const auto& [key, value] : attrs)
                {
                    if (key == name)
                    {
                        return value;
                    }
                }
                return {};
            }

            // describe_peer_payload(): a peer message in words — Claude Code's JSON notices become a
            // phrase ("is idle (available)", "has shut down", "assigned task 5: Run the tests"); a
            // JSON list or plain text is the sender's own content, kept whole.
            std::wstring DescribePeerPayload(const std::wstring& body)
            {
                if (!StartsWith(body, L"{"))
                {
                    return body;
                }
                const auto data = ParseJsonExact(body);
                if (!data || data->type != VType::Obj)
                {
                    return body; // not JSON after all (or not an object): the sender's own text
                }
                const Value* object = &*data;
                const Value* kind = JGet(object, L"type");
                if (StrEq(kind, L"idle_notification"))
                {
                    const Value* reason = JGet(object, L"idleReason");
                    return L"is idle" + ((IsStr(reason) && !reason->str.empty()) ? L" (" + reason->str + L")" : std::wstring{});
                }
                if (StrEq(kind, L"teammate_terminated"))
                {
                    const Value* message = JGet(object, L"message");
                    if (IsStr(message))
                    {
                        std::wstring text = PyStrip(message->str);
                        if (!text.empty())
                        {
                            return text;
                        }
                    }
                    return L"has shut down";
                }
                if (StrEq(kind, L"shutdown_approved"))
                {
                    return L"approved shutting down";
                }
                if (StrEq(kind, L"task_assignment"))
                {
                    const Value* taskId = JGet(object, L"taskId");
                    const Value* subject = JGet(object, L"subject");
                    std::wstring out = L"assigned task " + (taskId != nullptr ? PyStr(taskId) : std::wstring{ L"?" });
                    if (IsStr(subject) && !subject->str.empty())
                    {
                        out += L": " + subject->str;
                    }
                    return out;
                }
                for (const wchar_t* key : { L"message", L"summary", L"content" })
                {
                    const Value* value = JGet(object, key);
                    if (IsStr(value))
                    {
                        std::wstring text = PyStrip(value->str);
                        if (!text.empty())
                        {
                            return ((IsStr(kind) && !kind->str.empty()) ? kind->str + L": " : std::wstring{}) + text;
                        }
                    }
                }
                return body;
            }

            // peer_messages(): the messages a teammate or another session sent, when `text` is such a
            // delivery — recognized only at the START of the text (Claude Code's preamble, or the tag
            // itself), so a prompt that merely mentions the tag stays the user's. nullopt otherwise.
            std::optional<std::vector<PeerMessage>> PeerMessages(std::wstring_view text)
            {
                static constexpr std::wstring_view kPreamble = L"Another Claude session sent a message:";
                const std::wstring body = PyLStrip(text);
                const bool preamble = StartsWith(body, kPreamble);
                if (!(preamble || StartsWith(body, L"<teammate-message") || StartsWith(body, L"<agent-message")))
                {
                    return std::nullopt;
                }
                std::vector<PeerMessage> found;
                for (const auto& block : FindPeerBlocks(body))
                {
                    const auto attrs = PeerAttrs(block.attrs);
                    std::wstring sender = AttrOf(attrs, L"teammate_id");
                    if (sender.empty())
                    {
                        sender = AttrOf(attrs, L"from");
                    }
                    if (sender.empty())
                    {
                        sender = L"another session";
                    }
                    found.push_back(PeerMessage{ std::move(sender), DescribePeerPayload(PyStrip(block.inner)), PyStrip(AttrOf(attrs, L"summary")) });
                }
                if (!found.empty())
                {
                    return found;
                }
                if (preamble)
                {
                    // The preamble around a tag this doesn't know: still not the user's words.
                    return std::vector<PeerMessage>{ PeerMessage{ L"another session", PyStrip(std::wstring_view{ body }.substr(kPreamble.size())), L"" } };
                }
                return std::nullopt;
            }

            // ---- the live branch ----

            using UuidSet = std::unordered_set<std::wstring>;
            using EdgeMap = std::unordered_map<std::wstring, std::wstring>;

            // live_uuids(): the file's LAST uuid line and its ancestors via parentUuid, crossing a
            // compaction through its logicalParentUuid (and, for a chain, a plan-restart graft). The
            // parentUuid-only walk compacted-history replaces; kept for the feature's off position.
            UuidSet LiveUuids(const Lines& lines, const EdgeMap* extraEdges)
            {
                std::unordered_map<std::wstring, const Value*> byId; // a later copy of a uuid wins, like the reference's dict
                for (const Value* line : lines)
                {
                    const Value* uuid = JGet(line, L"uuid");
                    if (IsStr(uuid) && !uuid->str.empty())
                    {
                        byId[uuid->str] = line;
                    }
                }
                std::optional<std::wstring> leaf;
                for (auto it = lines.rbegin(); it != lines.rend(); ++it)
                {
                    const Value* uuid = JGet(*it, L"uuid");
                    if (IsStr(uuid) && !uuid->str.empty())
                    {
                        leaf = uuid->str;
                        break;
                    }
                }
                UuidSet live;
                while (leaf && !leaf->empty())
                {
                    const auto node = byId.find(*leaf);
                    if (node == byId.end() || live.count(*leaf) != 0)
                    {
                        break; // the `not in live` guard also stops a cycle
                    }
                    live.insert(*leaf);
                    const Value* parent = JGet(node->second, L"parentUuid");
                    if (!Truthy(parent) && StrEq(JGet(node->second, L"type"), L"system") && StrEq(JGet(node->second, L"subtype"), L"compact_boundary"))
                    {
                        parent = JGet(node->second, L"logicalParentUuid");
                    }
                    std::optional<std::wstring> next;
                    if (Truthy(parent))
                    {
                        if (IsStr(parent))
                        {
                            next = parent->str;
                        }
                    }
                    else if (extraEdges != nullptr && !extraEdges->empty())
                    {
                        const auto graft = extraEdges->find(*leaf);
                        if (graft != extraEdges->end())
                        {
                            next = graft->second;
                        }
                    }
                    leaf = std::move(next);
                }
                return live;
            }

            // deep_live_uuids(): the live branch crossing EVERY compaction into the history it
            // summarized (compacted-history) — an upward search over every edge: each occurrence's
            // parentUuid, and at a compaction root its logical parent, every message it preserved and
            // the last line written before it. It can only ever reach MORE than LiveUuids.
            UuidSet DeepLiveUuids(const Lines& lines, const EdgeMap* extraEdges)
            {
                std::unordered_map<std::wstring, std::vector<std::wstring>> up;
                const auto addEdge = [&up](const std::wstring& node, const std::wstring& target) {
                    if (target.empty() || target == node)
                    {
                        return; // empty and self references are malformed
                    }
                    auto& edges = up[node];
                    if (std::find(edges.begin(), edges.end(), target) == edges.end())
                    {
                        edges.push_back(target);
                    }
                };
                const auto addEdgeValue = [&addEdge](const std::wstring& node, const Value* target) {
                    if (IsStr(target))
                    {
                        addEdge(node, target->str); // non-string references are malformed: ignored
                    }
                };
                struct PreBoundary
                {
                    std::wstring root;
                    std::wstring marker; // the leafUuid marker in force at the compaction
                    std::wstring before; // the last non-sidechain node before it
                    bool afterMarker = false; // a node followed that marker
                };
                std::vector<PreBoundary> preBoundary;
                std::wstring marker;
                std::wstring tail;
                std::wstring mainTail;
                bool nodeAfterMarker = false;
                for (const Value* line : lines)
                {
                    const Value* leafUuid = JGet(line, L"leafUuid");
                    if (IsStr(leafUuid) && !leafUuid->str.empty())
                    {
                        marker = leafUuid->str; // a last-prompt line names the current leaf
                        nodeAfterMarker = false;
                    }
                    const Value* uuidValue = JGet(line, L"uuid");
                    if (!IsStr(uuidValue) || uuidValue->str.empty())
                    {
                        continue;
                    }
                    const std::wstring& uuid = uuidValue->str;
                    const bool firstSighting = up.find(uuid) == up.end();
                    up.try_emplace(uuid);
                    const Value* parent = JGet(line, L"parentUuid");
                    addEdgeValue(uuid, parent);
                    const Value* logical = JGet(line, L"logicalParentUuid");
                    if (!Truthy(parent) && (Truthy(logical) || StrEq(JGet(line, L"subtype"), L"compact_boundary")))
                    {
                        addEdgeValue(uuid, logical);
                        const Value* meta = ObjOrEmpty(JGet(line, L"compactMetadata"));
                        const Value* kept = ObjOrEmpty(JGet(meta, L"preservedMessages"));
                        for (const wchar_t* key : { L"uuids", L"allUuids" })
                        {
                            const Value* list = JGet(kept, key);
                            if (IsArr(list))
                            {
                                for (const Value& item : list->arr)
                                {
                                    addEdgeValue(uuid, &item);
                                }
                            }
                        }
                        const Value* segment = ObjOrEmpty(JGet(meta, L"preservedSegment"));
                        addEdgeValue(uuid, JGet(segment, L"headUuid"));
                        addEdgeValue(uuid, JGet(segment, L"tailUuid"));
                        if (firstSighting)
                        {
                            preBoundary.push_back(PreBoundary{ uuid, marker, mainTail, nodeAfterMarker });
                        }
                    }
                    tail = uuid;
                    if (!Truthy(JGet(line, L"isSidechain")))
                    {
                        mainTail = uuid; // an inlined subagent chain is not the conversation
                    }
                    nodeAfterMarker = true;
                }
                // The last line before each compaction, resolved now that every node is known: the tail
                // written after the last marker continues that marker's branch, else the marker names it.
                for (const auto& pb : preBoundary)
                {
                    if (pb.afterMarker && up.count(pb.before) != 0)
                    {
                        addEdge(pb.root, pb.before);
                    }
                    else if (up.count(pb.marker) != 0)
                    {
                        addEdge(pb.root, pb.marker);
                    }
                    else if (up.count(pb.before) != 0)
                    {
                        addEdge(pb.root, pb.before);
                    }
                }
                if (extraEdges != nullptr)
                {
                    for (const auto& [child, parent] : *extraEdges)
                    {
                        addEdge(child, parent);
                    }
                }
                UuidSet live;
                std::vector<std::wstring> pending;
                if (!tail.empty())
                {
                    pending.push_back(tail);
                }
                while (!pending.empty())
                {
                    std::wstring cur = std::move(pending.back());
                    pending.pop_back();
                    if (!live.insert(cur).second)
                    {
                        continue; // the visited set is also the cycle guard
                    }
                    const auto edges = up.find(cur);
                    if (edges != up.end())
                    {
                        for (const auto& next : edges->second)
                        {
                            pending.push_back(next);
                        }
                    }
                }
                return live;
            }

            // ---- the data model ----

            struct AgentRun;

            // Input: one message a turn received.
            struct TurnInput
            {
                std::wstring kind; // "start" (began the turn) | "queued" (arrived while it worked) | "continued"
                std::wstring ts;
                std::wstring text;
                std::wstring sender; // (peer-messages) the teammate / session it came from; "" = the user
                std::wstring note; // (peer-messages) the sender's own one-line summary, when it gave one
            };

            // A background-task completion notice for a call: (ts, task id, status, exit code).
            struct BgEvent
            {
                std::wstring ts;
                std::wstring taskId;
                std::wstring status;
                std::wstring exitCode;
            };

            // Op: one tool call and what came back from it. Heap-allocated and shared: the events of
            // its turn, the tool_use-id map of the parse and (agents) an agent's merged turn all
            // point at the same call, as the reference's lists all hold the same Op object.
            struct TurnOp
            {
                std::wstring toolId; // the tool_use id; its tool_result and background notices carry it
                std::wstring name;
                Value ownedInp; // the object synthesized when the model sent a non-object input ({"input": raw})
                const Value* inp = nullptr; // the call's arguments: always an object (a line's, ownedInp, or the empty one)
                std::wstring ts; // when the call was made
                std::wstring resultTs; // when its result arrived ("" = never)
                bool isError = false;
                std::wstring resultText;
                const Value* tur = nullptr; // the result line's toolUseResult object (or the empty one)
                std::vector<BgEvent> bg;
                std::optional<int> origin; // (chain) index of the session it ran in: its side files live there
                std::shared_ptr<AgentRun> agent; // (agents) the subagent / teammate this call spawned, when found
            };

            // One entry of a turn's timeline: ("op", Op) | ("say", mid, ts, text) | ("compact", ts,
            // text) | ("input", Input) — the last only in an agent's merged turn.
            struct TurnEvent
            {
                enum class Kind
                {
                    Op,
                    Say,
                    Compact,
                    Input,
                };
                Kind kind = Kind::Say;
                std::shared_ptr<TurnOp> op;
                std::wstring mid;
                std::wstring ts;
                std::wstring text;
                TurnInput input;

                static TurnEvent OfOp(std::shared_ptr<TurnOp> call)
                {
                    TurnEvent e;
                    e.kind = Kind::Op;
                    e.op = std::move(call);
                    return e;
                }
                static TurnEvent OfSay(std::wstring messageId, std::wstring at, std::wstring said)
                {
                    TurnEvent e;
                    e.kind = Kind::Say;
                    e.mid = std::move(messageId);
                    e.ts = std::move(at);
                    e.text = std::move(said);
                    return e;
                }
                static TurnEvent OfCompact(std::wstring at, std::wstring what)
                {
                    TurnEvent e;
                    e.kind = Kind::Compact;
                    e.ts = std::move(at);
                    e.text = std::move(what);
                    return e;
                }
                static TurnEvent OfInput(TurnInput in)
                {
                    TurnEvent e;
                    e.kind = Kind::Input;
                    e.input = std::move(in);
                    return e;
                }
            };

            // A note outside a turn: (ts, text, kind "cmd" | "compact").
            struct TurnNote
            {
                std::wstring ts;
                std::wstring text;
                std::wstring kind;
            };

            // Turn: what started it, what the assistant did, and how it ended.
            struct Turn
            {
                std::vector<TurnInput> inputs; // what started it ("start" / "continued"), then what arrived while it worked
                std::vector<TurnEvent> events;
                std::vector<TurnNote> notesBefore; // just before it
                std::wstring last; // the last text the assistant wrote in this turn
                std::optional<std::wstring> lastMid; // the message id that text belongs to (excluded from narration)
                std::wstring lastTs;
                std::wstring endTs; // the latest timestamp seen in the turn (string max, like the reference)
                bool ended = false; // the latest assistant stop_reason was end_turn
                bool interrupted = false;
                std::vector<std::pair<std::wstring, const Value*>> usage; // message id -> usage, first occurrence, in order
                std::vector<std::wstring> models;
                bool continued = false; // work before the file's first prompt: its prompt isn't in this file
                std::optional<int> origin; // (chain) index of the session the turn began in
            };

            // ChainLink: one session of a chain and the part of it in the head's history.
            struct ChainLink
            {
                fs::path path;
                std::wstring sessionId;
                double born = 0; // when its transcript file was created (epoch seconds): a fork's moment of birth
                LineFile owner; // keeps `lines` alive
                Lines lines; // its parsed lines; for an ancestor only those before its successor began
                std::wstring via; // how THIS session came from the previous link: "" (the oldest), "fork", "plan"
                std::optional<std::pair<std::wstring, std::wstring>> graft; // (plan) its root uuid, the parent's last uuid before it began
                std::wstring missingParent; // (the oldest link) a plan parent named by its first prompt whose transcript is gone
                std::wstring title; // the session's own custom (or AI) title, when its transcript records one
            };

            // History: what a render reads — one transcript, or a whole chain merged into one.
            // `lines` keeps each uuid's FIRST copy (for rendering); `edgeLines` every copy (for the walk).
            struct History
            {
                Lines lines;
                Lines edgeLines;
                std::vector<ChainLink> links; // oldest first; empty for a plain transcript
                std::unordered_map<std::wstring, int> origins; // uuid -> index into links of the file it first appears in
                EdgeMap extraEdges; // plan-restart grafts: child root uuid -> parent's last uuid
                std::optional<std::wstring> headId; // the current session's id (none: read it from the lines)
                std::optional<fs::path> path; // the current session's transcript (none: lines only — no agents)
                std::vector<LineFile> owners; // keeps a plain transcript's lines alive
            };

            // Session: a parsed transcript — turns plus the notes that don't belong inside one. Turns
            // live in a deque so the parse can keep a pointer to the current one while appending.
            struct TurnsSession
            {
                std::deque<Turn> turns;
                std::vector<TurnNote> notesFirst; // before the first turn
                std::vector<TurnNote> notesLast; // after the last turn
                std::wstring sessionId;
                const std::vector<ChainLink>* links = nullptr; // (chain) the history's links, oldest first; null = none
            };

            // AgentRun: a subagent or teammate a call spawned, and what it did (the agents feature).
            struct AgentRun
            {
                std::wstring label; // its name as shown: "finder-reuse", "Explore"; nested: "finder-A › Explore"
                std::wstring kind; // "teammate" | "subagent" | "background subagent" | "fork"
                std::wstring description; // what the spawning call said it was for
                TurnsSession session; // its transcript(s), parsed
                std::vector<fs::path> paths;
                bool completed = false; // the spawning call's result says it finished (status "completed")
                // The parsed lines of `paths`, kept alive for as long as this run exists. ⚠ `session` holds
                // RAW POINTERS into them (each call's arguments and result, each message's usage), and the
                // render reads those long after the agent search that parsed them has returned and dropped
                // its own cache — without this, every agent entry would read freed memory.
                std::vector<LineFile> owners;
            };

            // ---- the parse ----

            // The slash command named by an echo: text.split("<command-name>")[1].split("</command-name>")[0].
            std::wstring CommandNameOf(const std::wstring& text)
            {
                static constexpr std::wstring_view kOpen = L"<command-name>";
                static constexpr std::wstring_view kClose = L"</command-name>";
                const size_t start = text.find(kOpen);
                if (start == std::wstring::npos)
                {
                    return {};
                }
                const size_t from = start + kOpen.size();
                const size_t nextOpen = text.find(kOpen, from);
                std::wstring segment = text.substr(from, nextOpen == std::wstring::npos ? std::wstring::npos : nextOpen - from);
                const size_t close = segment.find(kClose);
                if (close != std::wstring::npos)
                {
                    segment.resize(close);
                }
                return segment;
            }

            // Its arguments: text.split("<command-args>")[1].split("</command-args>")[0] (the caller strips).
            std::wstring CommandArgsOf(const std::wstring& text)
            {
                static constexpr std::wstring_view kOpen = L"<command-args>";
                static constexpr std::wstring_view kClose = L"</command-args>";
                const size_t start = text.find(kOpen);
                if (start == std::wstring::npos)
                {
                    return {};
                }
                const size_t from = start + kOpen.size();
                const size_t nextOpen = text.find(kOpen, from);
                std::wstring segment = text.substr(from, nextOpen == std::wstring::npos ? std::wstring::npos : nextOpen - from);
                const size_t close = segment.find(kClose);
                if (close != std::wstring::npos)
                {
                    segment.resize(close);
                }
                return segment;
            }

            // _NOTICE_FIELD: the stripped text between the first <field> and the next </field>, or "".
            std::wstring NoticeField(const std::wstring& text, std::wstring_view field)
            {
                const std::wstring open = L"<" + std::wstring{ field } + L">";
                const std::wstring close = L"</" + std::wstring{ field } + L">";
                const size_t start = text.find(open);
                if (start == std::wstring::npos)
                {
                    return {};
                }
                const size_t end = text.find(close, start + open.size());
                if (end == std::wstring::npos)
                {
                    return {};
                }
                return PyStrip(std::wstring_view{ text }.substr(start + open.size(), end - start - open.size()));
            }

            // re.search(r"exit code (-?\d+)", summary): the first such exit code, or "".
            std::wstring ExitCodeIn(const std::wstring& summary)
            {
                static constexpr std::wstring_view kPhrase = L"exit code ";
                for (size_t p = summary.find(kPhrase); p != std::wstring::npos; p = summary.find(kPhrase, p + 1))
                {
                    const size_t start = p + kPhrase.size();
                    size_t j = start;
                    if (j < summary.size() && summary[j] == L'-')
                    {
                        ++j;
                    }
                    size_t k = j;
                    while (k < summary.size() && IsAsciiDigit(summary[k]))
                    {
                        ++k;
                    }
                    if (k > j)
                    {
                        return summary.substr(start, k - start);
                    }
                }
                return {};
            }

            // parse(): transcript lines -> turns. A turn starts at a typed user prompt (or a slash
            // command the assistant then works on) and runs until the next one; tool results are
            // matched to their calls by tool_use id across the whole session, background notices too.
            //   lines     — every line, in file order (a chain: the merged history, each uuid once).
            //   opt       — the features and rules (only the rules and peer-messages matter here).
            //   live      — the live branch to keep (null: keep every line — the rule is off).
            //   origins   — (chain) uuid -> index of the session the line first appeared in; null otherwise.
            //   sessionId — the session's id; null reads it from the first line that carries one.
            // Returns the session. Throws std::bad_alloc only.
            TurnsSession ParseTurns(const Lines& lines,
                                    const TurnsOptions& opt,
                                    const UuidSet* live,
                                    const std::unordered_map<std::wstring, int>* origins,
                                    const std::wstring* sessionId)
            {
                TurnsSession sess;
                if (sessionId != nullptr)
                {
                    sess.sessionId = *sessionId;
                }
                else
                {
                    for (const Value* line : lines)
                    {
                        const Value* sid = JGet(line, L"sessionId");
                        if (Truthy(sid))
                        {
                            sess.sessionId = StrOr(sid);
                            break;
                        }
                    }
                }
                std::unordered_map<std::wstring, std::shared_ptr<TurnOp>> ops;
                std::vector<TurnNote> pendingNotes; // waiting for the next turn
                std::optional<std::pair<std::wstring, std::wstring>> pendingCmd; // a slash-command echo not yet explained: (ts, command)
                std::optional<int> pendingCmdOrigin; // (chain) the session that echo came from
                Turn* cur = nullptr; // into sess.turns (a deque: appending never moves it)

                const auto originOf = [origins](const Value* line) -> std::optional<int> {
                    if (origins == nullptr)
                    {
                        return std::nullopt;
                    }
                    const Value* uuid = JGet(line, L"uuid");
                    if (!IsStr(uuid))
                    {
                        return std::nullopt;
                    }
                    const auto it = origins->find(uuid->str);
                    if (it == origins->end())
                    {
                        return std::nullopt;
                    }
                    return it->second;
                };
                // start_turn(): opens a new turn and hands it the notes collected since the previous
                // one (the first turn's go to notesFirst instead).
                const auto startTurn = [&](const std::wstring& ts, const std::wstring& text, bool continued, std::optional<int> origin, const std::wstring& sender, const std::wstring& note) -> Turn* {
                    Turn turn;
                    turn.inputs.push_back(TurnInput{ continued ? L"continued" : L"start", ts, text, sender, note });
                    if (!sess.turns.empty())
                    {
                        turn.notesBefore = std::move(pendingNotes);
                    }
                    else
                    {
                        sess.notesFirst.insert(sess.notesFirst.end(), pendingNotes.begin(), pendingNotes.end());
                    }
                    pendingNotes.clear();
                    turn.continued = continued;
                    turn.origin = origin;
                    sess.turns.push_back(std::move(turn));
                    return &sess.turns.back();
                };
                // flush_cmd(): a slash command that neither printed output nor started assistant work: a bare note.
                const auto flushCmd = [&]() {
                    if (pendingCmd)
                    {
                        pendingNotes.push_back(TurnNote{ pendingCmd->first, L"the user ran `" + pendingCmd->second + L"`", L"cmd" });
                        pendingCmd.reset();
                    }
                };
                // deliver_peers(): hands messages from teammates / other sessions to their turn. A
                // delivery that can land while a turn runs (`joinsRunning`) joins it; a real user line
                // always starts the next turn, exactly as a typed prompt does.
                const auto deliverPeers = [&](const std::wstring& ts, const std::vector<PeerMessage>& peers, const Value* line, bool joinsRunning) {
                    size_t firstRest = 0;
                    std::wstring kind = L"queued";
                    if (!joinsRunning || cur == nullptr || cur->ended)
                    {
                        flushCmd();
                        cur = startTurn(ts, peers[0].said, false, originOf(line), peers[0].sender, peers[0].note);
                        firstRest = 1;
                        kind = L"start";
                    }
                    for (size_t i = firstRest; i < peers.size(); ++i)
                    {
                        cur->inputs.push_back(TurnInput{ kind, ts, peers[i].said, peers[i].sender, peers[i].note });
                    }
                };

                for (const Value* line : lines)
                {
                    const Value* uuid = JGet(line, L"uuid");
                    if (!Truthy(uuid))
                    {
                        continue; // bookkeeping lines (mode, last-prompt, queue-operation ...) carry no uuid
                    }
                    if (live != nullptr && !(IsStr(uuid) && live->count(uuid->str) != 0))
                    {
                        continue; // an abandoned (rewound) branch
                    }
                    if (opt.skipSidechains && Truthy(JGet(line, L"isSidechain")))
                    {
                        continue;
                    }
                    const Value* type = JGet(line, L"type");
                    const std::wstring ts = StrOr(JGet(line, L"timestamp"));
                    const Value* msg = ObjOrEmpty(JGet(line, L"message"));

                    if (StrEq(type, L"user"))
                    {
                        const Value* content = JGet(msg, L"content");
                        if (Truthy(JGet(line, L"isCompactSummary")))
                        {
                            continue;
                        }
                        if (Truthy(JGet(line, L"isMeta")))
                        {
                            // Claude Code's own lines (caveats, reminders...) — except, with peer-messages,
                            // a teammate's report, which newer versions file as meta (<agent-message>).
                            if (opt.peerMessages)
                            {
                                const auto peers = PeerMessages(TextOf(content));
                                if (peers && !peers->empty())
                                {
                                    deliverPeers(ts, *peers, line, true);
                                }
                            }
                            continue;
                        }
                        if (HasToolResult(content))
                        {
                            for (const Value& block : content->arr)
                            {
                                if (!IsObj(&block) || !StrEq(JGet(&block, L"type"), L"tool_result"))
                                {
                                    continue;
                                }
                                const Value* toolUseId = JGet(&block, L"tool_use_id");
                                if (!IsStr(toolUseId))
                                {
                                    continue;
                                }
                                const auto found = ops.find(toolUseId->str);
                                if (found == ops.end())
                                {
                                    continue;
                                }
                                TurnOp& op = *found->second;
                                op.resultTs = ts;
                                op.isError = Truthy(JGet(&block, L"is_error"));
                                op.resultText = ResultTextOf(JGet(&block, L"content"));
                                op.tur = ObjOrEmpty(JGet(line, L"toolUseResult"));
                            }
                            if (cur != nullptr)
                            {
                                cur->ended = false; // a tool finished; the turn goes on
                                cur->endTs = std::max(cur->endTs, ts);
                            }
                            continue;
                        }
                        const std::wstring text = PyStrip(TextOf(content));
                        if (StartsWith(text, L"<command-name>"))
                        {
                            flushCmd();
                            const std::wstring name = CommandNameOf(text);
                            const std::wstring args = text.find(L"<command-args>") != std::wstring::npos ? PyStrip(CommandArgsOf(text)) : std::wstring{};
                            pendingCmd = std::make_pair(ts, args.empty() ? name : name + L" " + args);
                            pendingCmdOrigin = originOf(line);
                            continue;
                        }
                        if (StartsWith(text, L"<local-command-stdout>"))
                        {
                            const std::wstring out = PyStrip(ReplaceAll(ReplaceAll(text, L"<local-command-stdout>", L""), L"</local-command-stdout>", L""));
                            if (pendingCmd)
                            {
                                pendingNotes.push_back(TurnNote{ pendingCmd->first, L"the user ran `" + pendingCmd->second + L"`; it replied: " + out, L"cmd" });
                                pendingCmd.reset();
                            }
                            continue;
                        }
                        if (StartsWith(text, L"[Request interrupted"))
                        {
                            if (cur != nullptr)
                            {
                                cur->interrupted = true;
                            }
                            continue;
                        }
                        if (opt.peerMessages)
                        {
                            const auto peers = PeerMessages(text);
                            if (peers && !peers->empty())
                            {
                                deliverPeers(ts, *peers, line, false); // not the user's words: the sender's
                                continue;
                            }
                        }
                        flushCmd();
                        cur = startTurn(ts, text, false, originOf(line), L"", L"");
                    }
                    else if (StrEq(type, L"attachment") && StrEq(JGet(ObjOrEmpty(JGet(line, L"attachment")), L"type"), L"queued_command"))
                    {
                        const Value* att = JGet(line, L"attachment");
                        const Value* prompt = JGet(att, L"prompt");
                        const std::wstring text = PyStrip(IsStr(prompt) ? prompt->str : TextOf(prompt));
                        const bool isNotice = StrEq(JGet(att, L"commandMode"), L"task-notification") || StartsWith(text, L"<task-notification>") || StartsWith(text, L"<bash-notification>");
                        if (isNotice)
                        {
                            const auto found = ops.find(NoticeField(text, L"tool-use-id"));
                            if (found != ops.end())
                            {
                                found->second->bg.push_back(BgEvent{ ts, NoticeField(text, L"task-id"), NoticeField(text, L"status"), ExitCodeIn(NoticeField(text, L"summary")) });
                            }
                            if (opt.skipTaskNotices)
                            {
                                continue;
                            }
                        }
                        std::optional<std::vector<PeerMessage>> peers;
                        if (opt.peerMessages && !isNotice)
                        {
                            peers = PeerMessages(text);
                        }
                        if (peers && !peers->empty())
                        {
                            deliverPeers(ts, *peers, line, true);
                        }
                        else if (cur == nullptr || cur->ended)
                        {
                            flushCmd();
                            cur = startTurn(ts, text, false, originOf(line), L"", L"");
                        }
                        else
                        {
                            cur->inputs.push_back(TurnInput{ L"queued", ts, text, L"", L"" });
                        }
                    }
                    else if (StrEq(type, L"system") && StrEq(JGet(line, L"subtype"), L"compact_boundary"))
                    {
                        const Value* meta = ObjOrEmpty(JGet(line, L"compactMetadata"));
                        const int64_t pre = SafeInt(JGet(meta, L"preTokens")).value_or(0);
                        const int64_t post = SafeInt(JGet(meta, L"postTokens")).value_or(0);
                        const Value* trigger = JGet(meta, L"trigger");
                        std::wstring text = L"context compacted (" + (Truthy(trigger) ? PyStr(trigger) : std::wstring{ L"unknown trigger" }) + L")";
                        if (pre != 0 || post != 0)
                        {
                            text += L" · " + FmtTokens(pre) + L" → " + FmtTokens(post) + L" tokens";
                        }
                        const auto took = SafeInt(JGet(meta, L"durationMs"));
                        if (took && *took != 0)
                        {
                            text += L" · took " + FmtDuration(static_cast<double>(*took) / 1000.0);
                        }
                        if (cur != nullptr && !cur->ended)
                        {
                            cur->events.push_back(TurnEvent::OfCompact(ts, text)); // mid-turn (auto-compaction while it worked)
                        }
                        else
                        {
                            pendingNotes.push_back(TurnNote{ ts, text, L"compact" }); // between turns: shown before the next one
                        }
                    }
                    else if (StrEq(type, L"assistant"))
                    {
                        if (opt.skipSynthetic && StrEq(JGet(msg, L"model"), L"<synthetic>") && !Truthy(JGet(line, L"isApiErrorMessage")))
                        {
                            // Claude Code's own filler (e.g. "No response requested." after a resume):
                            // not something the assistant said, and it would become the turn's last text.
                            continue;
                        }
                        if (pendingCmd && cur != nullptr && !cur->ended)
                        {
                            // the assistant is still busy with the previous turn; the command waits
                        }
                        else if (pendingCmd)
                        {
                            // A slash command whose expansion the assistant is now working on: it started this turn.
                            const std::wstring cmdTs = pendingCmd->first;
                            const std::wstring cmd = pendingCmd->second;
                            pendingCmd.reset();
                            cur = startTurn(cmdTs, StartsWith(cmd, L"/") ? cmd : L"/" + cmd, false, pendingCmdOrigin, L"", L"");
                        }
                        if (cur == nullptr)
                        {
                            if (!opt.keepPrePromptWork)
                            {
                                continue; // the old behavior: work before any prompt was silently dropped
                            }
                            // The file opens mid-turn (after a compaction or a resume): collect the work in a
                            // pseudo-turn rather than lose it.
                            cur = startTurn(ts, L"", true, originOf(line), L"", L"");
                        }
                        const Value* messageId = JGet(msg, L"id");
                        const std::wstring mid = Truthy(messageId) ? PyStr(messageId) : PyStr(uuid);
                        if (std::none_of(cur->usage.begin(), cur->usage.end(), [&](const auto& u) { return u.first == mid; }))
                        {
                            const Value* usage = JGet(msg, L"usage");
                            cur->usage.emplace_back(mid, Truthy(usage) ? usage : &EmptyObject());
                        }
                        const Value* model = JGet(msg, L"model");
                        if (IsStr(model) && !model->str.empty() && model->str != L"<synthetic>" && std::find(cur->models.begin(), cur->models.end(), model->str) == cur->models.end())
                        {
                            cur->models.push_back(model->str);
                        }
                        const Value* blocks = JGet(msg, L"content");
                        if (IsArr(blocks))
                        {
                            for (const Value& block : blocks->arr)
                            {
                                if (!IsObj(&block))
                                {
                                    continue;
                                }
                                const Value* blockType = JGet(&block, L"type");
                                if (StrEq(blockType, L"text") && !PyStrip(StrOr(JGet(&block, L"text"))).empty())
                                {
                                    const std::wstring said = JGet(&block, L"text")->str;
                                    cur->events.push_back(TurnEvent::OfSay(mid, ts, said));
                                    if (cur->lastMid && *cur->lastMid == mid)
                                    {
                                        cur->last += L"\n\n" + said;
                                    }
                                    else
                                    {
                                        cur->last = said;
                                        cur->lastMid = mid;
                                    }
                                    cur->lastTs = ts;
                                }
                                else if (StrEq(blockType, L"tool_use"))
                                {
                                    auto op = std::make_shared<TurnOp>();
                                    // Arguments come from the model; a non-object (a bare string) is kept, not trusted.
                                    const Value* rawInput = JGet(&block, L"input");
                                    if (IsObj(rawInput))
                                    {
                                        op->inp = rawInput;
                                    }
                                    else if (rawInput == nullptr || rawInput->type == VType::Null)
                                    {
                                        op->inp = &EmptyObject();
                                    }
                                    else
                                    {
                                        op->ownedInp = Value::MkObj();
                                        op->ownedInp.Set(L"input", *rawInput);
                                        op->inp = &op->ownedInp; // stable: the op is heap-allocated and never moves
                                    }
                                    op->toolId = StrOr(JGet(&block, L"id"));
                                    const Value* toolName = JGet(&block, L"name");
                                    op->name = toolName == nullptr ? std::wstring{ L"?" } : PyStr(toolName);
                                    op->ts = ts;
                                    op->tur = &EmptyObject();
                                    op->origin = originOf(line);
                                    ops[op->toolId] = op;
                                    cur->events.push_back(TurnEvent::OfOp(op));
                                }
                            }
                        }
                        cur->endTs = std::max(cur->endTs, ts);
                        const Value* stop = JGet(msg, L"stop_reason");
                        if (Truthy(stop))
                        {
                            cur->ended = StrEq(stop, L"end_turn");
                        }
                    }
                }

                flushCmd();
                if (!sess.turns.empty())
                {
                    sess.notesLast = std::move(pendingNotes);
                }
                else
                {
                    sess.notesFirst.insert(sess.notesFirst.end(), pendingNotes.begin(), pendingNotes.end());
                }
                return sess;
            }

            // =====================================================================================
            // 5. Chains
            // =====================================================================================

            // Longest chain followed (the longest seen in 30 days of local transcripts was 7 sessions).
            constexpr size_t kMaxChain = 64;

            // A fork's parent is still being written when the copy is made, so its last write is
            // after the first copied line; a candidate last written well before that can't be the
            // parent. The slack absorbs clock and time-zone oddities.
            constexpr double kForkMtimeSlackSeconds = 86400.0;

            // Agentmaster (NOT in the reference): how ResolveChain may deviate from resolve_chain() for
            // the summary panel's conversation view (section 8). A DEFAULT-CONSTRUCTED value is exactly
            // the reference walk — Copy ▸ Transcript passes none, so its output (and the goldens) cannot
            // change through this struct.
            struct ChainOptions
            {
                // Run the fork search for a link only when that link's transcript OPENS with a copied line
                // (LooksForkCopied). The search reads the older transcripts of the whole project folder;
                // the summary walks a chain on the first summary load of every session, where a search
                // per ordinary (never-forked) session would be pure waste.
                bool forkSearchOnlyWhenCopied = false;
                // After the fork and plan-restart tests both fail, follow the predecessor Agentmaster
                // recorded for the link (AttachRecordedParent). Null = none (the reference).
                const RecordedPredecessorFn* recorded = nullptr;
            };

            // Agentmaster (NOT in the reference): how much older than its file a transcript's first line
            // must be to count as COPIED. An ordinary session writes its first line as it creates the
            // file — only the UserPromptSubmit hooks run between the Enter that stamps the line and that
            // write (measured up to ~15 s on a slow hook host before the exec-form hooks) — while a fork
            // copies lines minutes to days older than the fork's own first write. 30 s keeps every
            // ordinary session out of the folder scan with room to spare; a fork made within 30 s of its
            // parent's FIRST line (an un-compacted parent copies from its very start) is the one case it
            // gives up, and Copy ▸ Transcript (which never uses this gate) still finds it.
            constexpr double kForkCopySlackSeconds = 30.0;

            // Agentmaster (NOT in the reference): does this transcript OPEN with a copied line? A fork
            // copies its parent's context (from the parent's last /compact on) with the original lines'
            // timestamps, so its first uuid line was written well before its own file was created; an
            // ordinary session's first line is written as the file appears. Decides whether
            // ChainOptions::forkSearchOnlyWhenCopied lets the fork search run for a link.
            //   lines — the transcript's lines, in file order.
            //   born  — its file's creation time, epoch seconds.
            // Returns true when the first uuid line predates `born` by more than kForkCopySlackSeconds,
            // AND when that line's time can't be read (searching is the answer that can't miss a fork);
            // false when the transcript has no uuid line at all (nothing could have been copied).
            // Throws std::bad_alloc only.
            bool LooksForkCopied(const Lines& lines, double born)
            {
                for (const Value* line : lines)
                {
                    const Value* uuid = JGet(line, L"uuid");
                    if (!IsStr(uuid) || uuid->str.empty())
                    {
                        continue; // a uuid-less bookkeeping line is never a copied conversation line
                    }
                    const auto stamp = AwareTs(JGet(line, L"timestamp"));
                    if (!stamp)
                    {
                        return true; // no readable time: search rather than risk missing a fork
                    }
                    return static_cast<double>(*stamp) / static_cast<double>(kMicrosPerSecond) < born - kForkCopySlackSeconds;
                }
                return false;
            }

            // Owns a Win32 file handle for the duration of a read (closed on every path, a throw included).
            struct FileHandle
            {
                HANDLE h = INVALID_HANDLE_VALUE;
                explicit FileHandle(HANDLE handle) noexcept :
                    h(handle)
                {
                }
                FileHandle(const FileHandle&) = delete;
                FileHandle& operator=(const FileHandle&) = delete;
                ~FileHandle()
                {
                    if (h != INVALID_HANDLE_VALUE)
                    {
                        ::CloseHandle(h);
                    }
                }
            };

            // Opens a file for reading while Claude Code may still be appending to it (full sharing).
            HANDLE OpenForRead(const fs::path& path) noexcept
            {
                return ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            }

            // Path.read_bytes(): the whole file, or nullopt when it can't be opened or read. A file
            // that grows meanwhile is read up to its size at open (the torn tail line is then skipped
            // by the parse). Throws std::bad_alloc only.
            std::optional<std::string> ReadAllBytes(const fs::path& path)
            {
                FileHandle file{ OpenForRead(path) };
                if (file.h == INVALID_HANDLE_VALUE)
                {
                    return std::nullopt;
                }
                LARGE_INTEGER size{};
                if (!::GetFileSizeEx(file.h, &size) || size.QuadPart < 0)
                {
                    return std::nullopt;
                }
                std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
                size_t got = 0;
                while (got < bytes.size())
                {
                    const DWORD want = static_cast<DWORD>(std::min<size_t>(bytes.size() - got, size_t{ 1 } << 24));
                    DWORD read = 0;
                    if (!::ReadFile(file.h, bytes.data() + got, want, &read, nullptr))
                    {
                        return std::nullopt;
                    }
                    if (read == 0)
                    {
                        break; // the file shrank since it was measured: keep what is there
                    }
                    got += read;
                }
                bytes.resize(got);
                return bytes;
            }

            // The first `maxLines` lines' worth of a file (what a binary readline() loop would read):
            // 64 KiB chunks until that many '\n' arrived or the file ended. nullopt when unreadable.
            std::optional<std::string> ReadHeadLines(const fs::path& path, size_t maxLines)
            {
                FileHandle file{ OpenForRead(path) };
                if (file.h == INVALID_HANDLE_VALUE)
                {
                    return std::nullopt;
                }
                std::string bytes;
                std::string chunk(size_t{ 64 } * 1024, '\0');
                size_t newlines = 0;
                for (;;)
                {
                    DWORD read = 0;
                    if (!::ReadFile(file.h, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr))
                    {
                        return std::nullopt;
                    }
                    if (read == 0)
                    {
                        break;
                    }
                    bytes.append(chunk.data(), read);
                    newlines += static_cast<size_t>(std::count(chunk.data(), chunk.data() + read, '\n'));
                    if (newlines >= maxLines)
                    {
                        break;
                    }
                }
                return bytes;
            }

            double FileTimeToEpochSeconds(const FILETIME& ft) noexcept
            {
                ULARGE_INTEGER t{};
                t.LowPart = ft.dwLowDateTime;
                t.HighPart = ft.dwHighDateTime;
                return (static_cast<double>(t.QuadPart) - 116444736000000000.0) / 1e7;
            }

            // Path.stat().st_mtime in epoch seconds, or nullopt when the file can't be stat'ed.
            std::optional<double> FileMTime(const fs::path& path) noexcept
            {
                WIN32_FILE_ATTRIBUTE_DATA data{};
                if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
                {
                    return std::nullopt;
                }
                return FileTimeToEpochSeconds(data.ftLastWriteTime);
            }

            // Path.is_file() (following a symlink; false on any error).
            bool IsFile(const fs::path& path) noexcept
            {
                std::error_code ec;
                return fs::is_regular_file(path, ec);
            }

            // A path's identity — Path.resolve() + Windows' case-insensitive Path equality: canonical
            // where it exists (symlinks and ".." resolved), else absolute + normalized, lowercased.
            std::wstring PathKey(const fs::path& path)
            {
                std::error_code ec;
                fs::path canonical = fs::weakly_canonical(path, ec);
                if (ec)
                {
                    std::error_code absoluteEc;
                    canonical = fs::absolute(path, absoluteEc).lexically_normal();
                    if (absoluteEc)
                    {
                        canonical = path.lexically_normal();
                    }
                }
                return LowerForPath(canonical.wstring());
            }

            // path.with_suffix(""): the transcript's own folder ("<session>.jsonl" -> "<session>").
            fs::path WithoutSuffix(const fs::path& path)
            {
                fs::path out = path;
                out.replace_extension();
                return out;
            }

            // The regular files directly in `dir` whose NAME passes `keep`, sorted the way Python sorts
            // a Windows glob (case-insensitively by name). Empty when `dir` can't be listed.
            std::vector<fs::path> ListFilesSorted(const fs::path& dir, const std::function<bool(const std::wstring&)>& keep)
            {
                std::vector<std::pair<std::wstring, fs::path>> found;
                std::error_code ec;
                fs::directory_iterator it{ dir, ec };
                const fs::directory_iterator end;
                for (; !ec && it != end; it.increment(ec))
                {
                    std::error_code entryEc;
                    if (!it->is_regular_file(entryEc))
                    {
                        continue;
                    }
                    const std::wstring name = it->path().filename().wstring();
                    if (keep(name))
                    {
                        found.emplace_back(LowerForPath(name), it->path());
                    }
                }
                std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                std::vector<fs::path> out;
                out.reserve(found.size());
                for (auto& f : found)
                {
                    out.push_back(std::move(f.second));
                }
                return out;
            }

            // The subfolders of `dir`, sorted case-insensitively by name (the `*` of a "*/<id>.jsonl" glob).
            std::vector<fs::path> ListSubdirsSorted(const fs::path& dir)
            {
                std::vector<std::pair<std::wstring, fs::path>> found;
                std::error_code ec;
                fs::directory_iterator it{ dir, ec };
                const fs::directory_iterator end;
                for (; !ec && it != end; it.increment(ec))
                {
                    std::error_code entryEc;
                    if (it->is_directory(entryEc))
                    {
                        found.emplace_back(LowerForPath(it->path().filename().wstring()), it->path());
                    }
                }
                std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                std::vector<fs::path> out;
                out.reserve(found.size());
                for (auto& f : found)
                {
                    out.push_back(std::move(f.second));
                }
                return out;
            }

            std::string NarrowUtf8(std::wstring_view s)
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

            constexpr bool IsAsciiSpaceByte(char c) noexcept
            {
                return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
            }

            // The fork parent's cheap pre-filter: the raw bytes hold `"uuid"\s*:\s*"<uuid>"`.
            bool ContainsUuidField(const std::string& data, const std::string& uuidUtf8)
            {
                static constexpr std::string_view kKey = "\"uuid\"";
                for (size_t p = data.find(kKey); p != std::string::npos; p = data.find(kKey, p + 1))
                {
                    size_t i = p + kKey.size();
                    while (i < data.size() && IsAsciiSpaceByte(data[i]))
                    {
                        ++i;
                    }
                    if (i >= data.size() || data[i] != ':')
                    {
                        continue;
                    }
                    ++i;
                    while (i < data.size() && IsAsciiSpaceByte(data[i]))
                    {
                        ++i;
                    }
                    if (i >= data.size() || data[i] != '"')
                    {
                        continue;
                    }
                    ++i;
                    if (data.compare(i, uuidUtf8.size(), uuidUtf8) != 0)
                    {
                        continue;
                    }
                    i += uuidUtf8.size();
                    if (i < data.size() && data[i] == '"')
                    {
                        return true;
                    }
                }
                return false;
            }

            // transcript_title(): the last custom title, else the last AI title (stripped); "" for none.
            std::wstring TranscriptTitle(const Lines& lines)
            {
                std::wstring custom;
                std::wstring ai;
                for (const Value* line : lines)
                {
                    const Value* type = JGet(line, L"type");
                    if (StrEq(type, L"custom-title") && IsStr(JGet(line, L"customTitle")))
                    {
                        custom = PyStrip(JGet(line, L"customTitle")->str);
                    }
                    else if (StrEq(type, L"ai-title") && IsStr(JGet(line, L"aiTitle")))
                    {
                        ai = PyStrip(JGet(line, L"aiTitle")->str);
                    }
                }
                return !custom.empty() ? custom : ai;
            }

            // The fork walk's parsed candidates, by path identity, reused across the chain's steps.
            using ChainCache = std::unordered_map<std::wstring, LineFile>;

            struct ForkParent
            {
                fs::path path;
                Lines kept; // its lines up to the last one the child also has
                LineFile every; // all its lines (owner of `kept`)
            };

            // find_fork_parent(): the transcript this one was forked from, if any. A fork copies the
            // parent's context with the SAME uuids, so the file's first uuid line also lives in older
            // files of the same project folder; the parent is the one holding the copied lines
            // furthest along (ties: the earliest-born, then the path).
            // Throws TurnsReadError when this file's own creation time can't be read.
            std::optional<ForkParent> FindForkParent(const fs::path& path, const Lines& lines, const TurnsBirthFn& born, ChainCache& cache)
            {
                Lines uuidLines;
                for (const Value* line : lines)
                {
                    const Value* uuid = JGet(line, L"uuid");
                    if (IsStr(uuid) && !uuid->str.empty())
                    {
                        uuidLines.push_back(line);
                    }
                }
                if (uuidLines.empty())
                {
                    return std::nullopt;
                }
                const Value* first = uuidLines[0];
                const std::string needle = NarrowUtf8(JGet(first, L"uuid")->str);
                const auto firstTs = AwareTs(JGet(first, L"timestamp"));
                const std::optional<double> mtimeFloor = firstTs ? std::optional<double>{ static_cast<double>(*firstTs) / 1e6 - kForkMtimeSlackSeconds } : std::nullopt;
                const std::wstring here = PathKey(path);
                const auto nodeBorn = born(path);
                if (!nodeBorn)
                {
                    throw TurnsReadError{};
                }
                std::optional<std::tuple<size_t, double, std::wstring>> bestKey;
                ForkParent best;
                const auto candidates = ListFilesSorted(path.parent_path(), [](const std::wstring& name) { return EndsWithCi(name, L".jsonl"); });
                for (const fs::path& cand : candidates)
                {
                    if (StartsWith(cand.filename().wstring(), L"agent-"))
                    {
                        continue; // a 2.0.x agent's transcript sits here too; it is never a session's parent
                    }
                    const std::wstring candKey = PathKey(cand);
                    if (candKey == here)
                    {
                        continue;
                    }
                    const auto candBorn = born(cand);
                    if (!candBorn || *candBorn >= *nodeBorn)
                    {
                        continue; // unreadable, or not OLDER (a later file is a fork of this one)
                    }
                    if (mtimeFloor)
                    {
                        const auto mtime = FileMTime(cand);
                        if (!mtime || *mtime < *mtimeFloor)
                        {
                            continue; // last written well before the copied line existed: not the parent
                        }
                    }
                    auto cached = cache.find(candKey);
                    if (cached == cache.end())
                    {
                        auto bytes = ReadAllBytes(cand);
                        if (!bytes || !ContainsUuidField(*bytes, needle))
                        {
                            continue; // vanished / locked, or the cheap filter says no relative
                        }
                        cached = cache.emplace(candKey, std::make_shared<const std::vector<Value>>(ParseTranscriptJsonl(std::move(*bytes)))).first;
                    }
                    const std::vector<Value>& candLines = *cached->second;
                    std::unordered_map<std::wstring, size_t> pos;
                    for (size_t i = 0; i < candLines.size(); ++i)
                    {
                        const Value* uuid = JGet(&candLines[i], L"uuid");
                        if (IsStr(uuid))
                        {
                            pos.try_emplace(uuid->str, i);
                        }
                    }
                    std::vector<size_t> shared;
                    for (size_t k = 0; k < uuidLines.size(); ++k)
                    {
                        if (pos.count(JGet(uuidLines[k], L"uuid")->str) != 0)
                        {
                            shared.push_back(k);
                        }
                    }
                    if (shared.empty())
                    {
                        continue;
                    }
                    const size_t junction = shared.back(); // this file's last copied line
                    size_t cut = 0; // the same point in the candidate
                    for (const size_t k : shared)
                    {
                        cut = std::max(cut, pos[JGet(uuidLines[k], L"uuid")->str]);
                    }
                    auto key = std::make_tuple(junction, -*candBorn, cand.wstring()); // furthest along, then earliest-born, then stable
                    if (!bestKey || key > *bestKey)
                    {
                        bestKey = std::move(key);
                        best.path = cand;
                        best.every = cached->second;
                        best.kept.clear();
                        for (size_t i = 0; i <= cut && i < candLines.size(); ++i)
                        {
                            best.kept.push_back(&candLines[i]);
                        }
                    }
                }
                if (!bestKey)
                {
                    return std::nullopt;
                }
                return best;
            }

            bool IsUuidAt(std::wstring_view s, size_t at) noexcept
            {
                if (at + 36 > s.size())
                {
                    return false;
                }
                for (size_t i = 0; i < 36; ++i)
                {
                    const wchar_t c = s[at + i];
                    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
                    if (dash ? c != L'-' : !IsHexDigit(c))
                    {
                        return false;
                    }
                }
                return true;
            }

            struct PlanMatch
            {
                std::wstring literal; // group 1: the path as written
                std::wstring parentId; // group 2: its session id
            };

            // _PLAN_PARENT.search: "read the full transcript at: (\S*?(<uuid>)\.jsonl)" (re.I) — the
            // id must be a real session id, so a prompt that merely quotes "<parent>.jsonl" isn't a link.
            std::optional<PlanMatch> MatchPlanParent(std::wstring_view s)
            {
                static constexpr std::wstring_view kPhrase = L"read the full transcript at: ";
                for (size_t p = FindCi(s, kPhrase, 0); p != std::wstring_view::npos; p = FindCi(s, kPhrase, p + 1))
                {
                    const size_t start = p + kPhrase.size();
                    for (size_t k = start; k < s.size(); ++k)
                    {
                        if (k > start && IsPySpace(s[k - 1]))
                        {
                            break; // \S*? can't run across whitespace
                        }
                        if (IsUuidAt(s, k) && StartsWithCi(s.substr(k + 36), L".jsonl"))
                        {
                            return PlanMatch{ std::wstring{ s.substr(start, k + 36 + 6 - start) }, std::wstring{ s.substr(k, 36) } };
                        }
                    }
                }
                return std::nullopt;
            }

            struct PlanParent
            {
                std::wstring parentId;
                std::optional<fs::path> path; // none: its transcript is gone
            };

            // find_plan_parent(): the transcript a plan-mode restart continues, if this is one. Only the
            // FIRST prompt counts; the parent is looked for at the literal path, beside this file, then
            // in every project folder.
            std::optional<PlanParent> FindPlanParent(const fs::path& path, const Lines& lines)
            {
                const auto match = MatchPlanParent(FirstPromptText(lines));
                if (!match || LowerAscii(match->parentId) == LowerAscii(path.stem().wstring()))
                {
                    return std::nullopt;
                }
                const std::wstring parentId = LowerAscii(match->parentId);
                std::vector<fs::path> candidates;
                const fs::path literal{ match->literal };
                if (literal.is_absolute())
                {
                    candidates.push_back(literal);
                }
                candidates.push_back(path.parent_path() / (parentId + L".jsonl"));
                for (const auto& folder : ListSubdirsSorted(path.parent_path().parent_path()))
                {
                    candidates.push_back(folder / (parentId + L".jsonl"));
                }
                for (const auto& cand : candidates)
                {
                    if (IsFile(cand))
                    {
                        return PlanParent{ parentId, cand };
                    }
                }
                return PlanParent{ parentId, std::nullopt };
            }

            // lines_before(): a plan parent's lines up to the moment its child began.
            Lines LinesBefore(const Lines& lines, const std::wstring& ts)
            {
                const auto limit = AwareTs(ts);
                if (!limit)
                {
                    return lines;
                }
                Lines out;
                for (const Value* line : lines)
                {
                    const auto stamp = AwareTs(JGet(line, L"timestamp"));
                    if (Truthy(JGet(line, L"uuid")) && stamp && *stamp > *limit)
                    {
                        break;
                    }
                    out.push_back(line);
                }
                return out;
            }

            // root_uuid(): the first uuid line with no parent of its own (a compaction doesn't count —
            // it continues through its logical parent). "" when every uuid line has a parent.
            std::wstring RootUuid(const Lines& lines)
            {
                for (const Value* line : lines)
                {
                    const Value* uuid = JGet(line, L"uuid");
                    if (IsStr(uuid) && !uuid->str.empty() && !Truthy(JGet(line, L"parentUuid")))
                    {
                        if (!Truthy(JGet(line, L"logicalParentUuid")) && !StrEq(JGet(line, L"subtype"), L"compact_boundary"))
                        {
                            return uuid->str;
                        }
                    }
                }
                return {};
            }

            // Agentmaster (NOT in the reference): attach the predecessor Agentmaster RECORDED for
            // links[child] — a /clear in the same tab, a /handover origin (RecordedPredecessorFn) — the way
            // ResolveChain attaches a plan-restart parent, since neither leaves a link in any transcript:
            // the predecessor's lines written before the child's first line, grafted under the child's
            // root so the live-branch walk crosses into it. Its transcript is looked for beside the
            // child's first (a /clear stays in its project folder), then in every project folder (a
            // handover successor may run in another directory) — FindPlanParent's search order.
            // Deliberately a separate copy of the plan attach rather than a shared helper: the plan path
            // is pinned byte for byte to the reference, and this one must never move it.
            //   links    — the chain so far (child = the link asked about); the parent is appended on success.
            //   child    — the index of the link whose predecessor is asked for.
            //   seen     — the path identities already in the chain (the cycle guard); extended on success.
            //   cache    — the walk's parsed files (the fork search may already hold the predecessor).
            //   born     — file creation times.
            //   recorded — the lookup (non-empty).
            // Returns true when a parent was appended (the walk continues from it); false when nothing was
            // recorded, it names the child itself, its transcript is gone / unreadable / already in the
            // chain, or there is nothing to attach to. Throws std::bad_alloc only: a throwing lookup is
            // contained and logged here (Rule #18) and reads as "nothing recorded".
            bool AttachRecordedParent(std::vector<ChainLink>& links, size_t child, UuidSet& seen, ChainCache& cache, const TurnsBirthFn& born, const RecordedPredecessorFn& recorded)
            {
                std::optional<RecordedPredecessor> rec;
                try
                {
                    rec = recorded(links[child].sessionId);
                }
                catch (...)
                {
                    LogSwallowedException(L"TranscriptTurns AttachRecordedParent lookup");
                    return false;
                }
                if (!rec || rec->sessionId.empty() || LowerAscii(rec->sessionId) == LowerAscii(links[child].sessionId))
                {
                    return false; // nothing recorded, or a self-reference (malformed: never follow it)
                }
                const fs::path childPath = links[child].path; // a copy: the push_back below may move the vector
                std::optional<fs::path> parentPath;
                {
                    std::vector<fs::path> candidates;
                    candidates.push_back(childPath.parent_path() / (rec->sessionId + L".jsonl"));
                    for (const auto& folder : ListSubdirsSorted(childPath.parent_path().parent_path()))
                    {
                        candidates.push_back(folder / (rec->sessionId + L".jsonl"));
                    }
                    for (const auto& cand : candidates)
                    {
                        if (IsFile(cand))
                        {
                            parentPath = cand;
                            break;
                        }
                    }
                }
                if (!parentPath || seen.count(PathKey(*parentPath)) != 0)
                {
                    return false; // its transcript is gone (a never-prompted predecessor has none), or a cycle
                }
                LineFile every;
                if (const auto cached = cache.find(PathKey(*parentPath)); cached != cache.end() && !cached->second->empty())
                {
                    every = cached->second;
                }
                else
                {
                    auto bytes = ReadAllBytes(*parentPath);
                    if (!bytes)
                    {
                        return false; // there but unreadable: as good as gone
                    }
                    every = std::make_shared<const std::vector<Value>>(ParseTranscriptJsonl(std::move(*bytes)));
                }
                const auto parentBorn = born(*parentPath);
                if (!parentBorn)
                {
                    return false;
                }
                // The child's first stamped line marks where the predecessor's part ends: a /clear
                // predecessor ended there anyway, and a /handover origin's later work is ITS own future
                // (downstream of the hand-off), never the successor's history.
                std::wstring firstTs;
                for (const Value* line : links[child].lines)
                {
                    if (Truthy(JGet(line, L"uuid")) && Truthy(JGet(line, L"timestamp")))
                    {
                        firstTs = StrOr(JGet(line, L"timestamp"));
                        break;
                    }
                }
                Lines kept = LinesBefore(PointersTo(*every), firstTs);
                std::wstring leaf;
                for (auto it = kept.rbegin(); it != kept.rend(); ++it)
                {
                    const Value* uuid = JGet(*it, L"uuid");
                    if (IsStr(uuid) && !uuid->str.empty())
                    {
                        leaf = uuid->str;
                        break;
                    }
                }
                const std::wstring root = RootUuid(links[child].lines);
                if (leaf.empty() || root.empty())
                {
                    return false; // nothing to attach to (an empty predecessor, or a child with no root)
                }
                links[child].via = rec->via;
                links[child].graft = std::make_pair(root, leaf);
                ChainLink parent;
                parent.path = *parentPath;
                parent.sessionId = parentPath->stem().wstring();
                parent.born = *parentBorn;
                parent.owner = every;
                parent.lines = std::move(kept);
                parent.title = TranscriptTitle(PointersTo(*every));
                seen.insert(PathKey(parent.path));
                links.push_back(std::move(parent));
                return true;
            }

            // resolve_chain(): the sessions a transcript continues, oldest first; the transcript itself
            // is last. One link at a time — a fork first, else a plan restart — until neither applies,
            // a parent is gone, or a file repeats. Throws TurnsReadError when the head can't be read.
            // Agentmaster: `chainOpts` (NOT in the reference) lets the summary's walk skip the fork search
            // for a link that opens with its own line and continue through a recorded predecessor; the
            // default-constructed value Copy ▸ Transcript passes leaves the walk exactly the reference's.
            std::vector<ChainLink> ResolveChain(const fs::path& head, const TurnsBirthFn& born, const ChainOptions& chainOpts = {})
            {
                ChainCache cache;
                auto headBytes = ReadAllBytes(head);
                if (!headBytes)
                {
                    throw TurnsReadError{};
                }
                const auto headBorn = born(head);
                if (!headBorn)
                {
                    throw TurnsReadError{};
                }
                std::vector<ChainLink> links;
                {
                    ChainLink link;
                    link.path = head;
                    link.sessionId = head.stem().wstring();
                    link.born = *headBorn;
                    link.owner = std::make_shared<const std::vector<Value>>(ParseTranscriptJsonl(std::move(*headBytes)));
                    link.lines = PointersTo(*link.owner);
                    link.title = TranscriptTitle(link.lines);
                    links.push_back(std::move(link));
                }
                UuidSet seen{ PathKey(head) };
                while (links.size() < kMaxChain)
                {
                    const size_t child = links.size() - 1; // an index: push_back below may move the vector
                    // Agentmaster (NOT in the reference): ChainOptions::forkSearchOnlyWhenCopied skips the
                    // folder-wide search for a link whose transcript opens with its own (uncopied) line.
                    std::optional<ForkParent> fork;
                    if (!chainOpts.forkSearchOnlyWhenCopied || LooksForkCopied(links[child].lines, links[child].born))
                    {
                        fork = FindForkParent(links[child].path, links[child].lines, born, cache);
                    }
                    if (fork)
                    {
                        if (seen.count(PathKey(fork->path)) != 0)
                        {
                            break; // a cycle can't happen with creation times, but a copied file could fake one
                        }
                        const auto parentBorn = born(fork->path);
                        if (!parentBorn)
                        {
                            throw TurnsReadError{}; // the reference stats it outside any guard
                        }
                        links[child].via = L"fork";
                        ChainLink parent;
                        parent.path = fork->path;
                        parent.sessionId = fork->path.stem().wstring();
                        parent.born = *parentBorn;
                        parent.owner = fork->every;
                        parent.lines = std::move(fork->kept);
                        parent.title = TranscriptTitle(PointersTo(*fork->every));
                        seen.insert(PathKey(parent.path));
                        links.push_back(std::move(parent));
                        continue;
                    }
                    const auto plan = FindPlanParent(links[child].path, links[child].lines);
                    if (!plan)
                    {
                        // Agentmaster (NOT in the reference): no on-disk link — a predecessor Agentmaster
                        // recorded (a /clear in the same tab, a /handover origin) continues the walk.
                        if (chainOpts.recorded != nullptr && *chainOpts.recorded &&
                            AttachRecordedParent(links, child, seen, cache, born, *chainOpts.recorded))
                        {
                            continue;
                        }
                        break;
                    }
                    if (!plan->path)
                    {
                        links[child].missingParent = plan->parentId;
                        break;
                    }
                    if (seen.count(PathKey(*plan->path)) != 0)
                    {
                        break;
                    }
                    LineFile every;
                    const auto cached = cache.find(PathKey(*plan->path));
                    if (cached != cache.end() && !cached->second->empty())
                    {
                        every = cached->second;
                    }
                    else
                    {
                        auto bytes = ReadAllBytes(*plan->path);
                        if (!bytes)
                        {
                            links[child].missingParent = plan->parentId; // there but unreadable: as good as gone
                            break;
                        }
                        every = std::make_shared<const std::vector<Value>>(ParseTranscriptJsonl(std::move(*bytes)));
                    }
                    const auto parentBorn = born(*plan->path);
                    if (!parentBorn)
                    {
                        links[child].missingParent = plan->parentId;
                        break;
                    }
                    std::wstring firstTs;
                    for (const Value* line : links[child].lines)
                    {
                        if (Truthy(JGet(line, L"uuid")) && Truthy(JGet(line, L"timestamp")))
                        {
                            firstTs = StrOr(JGet(line, L"timestamp"));
                            break;
                        }
                    }
                    Lines kept = LinesBefore(PointersTo(*every), firstTs);
                    std::wstring leaf;
                    for (auto it = kept.rbegin(); it != kept.rend(); ++it)
                    {
                        const Value* uuid = JGet(*it, L"uuid");
                        if (IsStr(uuid) && !uuid->str.empty())
                        {
                            leaf = uuid->str;
                            break;
                        }
                    }
                    const std::wstring root = RootUuid(links[child].lines);
                    if (leaf.empty() || root.empty())
                    {
                        break; // nothing to attach to (an empty parent, or a child with no root)
                    }
                    links[child].via = L"plan";
                    links[child].graft = std::make_pair(root, leaf);
                    ChainLink parent;
                    parent.path = *plan->path;
                    parent.sessionId = plan->parentId;
                    parent.born = *parentBorn;
                    parent.owner = every;
                    parent.lines = std::move(kept);
                    parent.title = TranscriptTitle(PointersTo(*every));
                    seen.insert(PathKey(parent.path));
                    links.push_back(std::move(parent));
                }
                std::reverse(links.begin(), links.end());
                return links;
            }

            // load_history(): what a render shows — the transcript, or (chain) its whole chain merged:
            // each ancestor's lines up to where its successor began, oldest first, a uuid rendered at
            // its FIRST occurrence while every copy still feeds the walk. Throws TurnsReadError when
            // the transcript can't be read.
            // Agentmaster: `chainOpts` (NOT in the reference) is handed to ResolveChain unchanged — the
            // summary's section-8 walk passes its options; every render passes the default.
            History LoadHistory(const fs::path& path, const TurnsOptions& opt, const TurnsBirthFn& born, const ChainOptions& chainOpts = {})
            {
                History history;
                history.path = path;
                if (!opt.chain)
                {
                    auto bytes = ReadAllBytes(path);
                    if (!bytes)
                    {
                        throw TurnsReadError{};
                    }
                    history.owners.push_back(std::make_shared<const std::vector<Value>>(ParseTranscriptJsonl(std::move(*bytes))));
                    history.lines = PointersTo(*history.owners.back());
                    history.edgeLines = history.lines;
                    return history;
                }
                history.links = ResolveChain(path, born, chainOpts);
                for (size_t index = 0; index < history.links.size(); ++index)
                {
                    const ChainLink& link = history.links[index];
                    if (link.graft)
                    {
                        history.extraEdges[link.graft->first] = link.graft->second;
                    }
                    for (const Value* line : link.lines)
                    {
                        history.edgeLines.push_back(line);
                        const Value* uuid = JGet(line, L"uuid");
                        if (IsStr(uuid) && !uuid->str.empty())
                        {
                            if (history.origins.count(uuid->str) != 0)
                            {
                                continue; // a copy of a line an older session wrote
                            }
                            history.origins.emplace(uuid->str, static_cast<int>(index));
                        }
                        history.lines.push_back(line);
                    }
                }
                history.headId = path.stem().wstring();
                return history;
            }

            // =====================================================================================
            // 6. Agents
            // =====================================================================================

            // The tools that spawn a subagent or a teammate (Task is Claude Code 2.0.x's name).
            bool IsAgentTool(const std::wstring& name) noexcept
            {
                return name == L"Agent" || name == L"Task";
            }

            // How deep agents spawned by agents are followed (real transcripts reach depth 2).
            constexpr int kMaxAgentDepth = 4;

            // One agent transcript in a subagents folder and its .meta.json (empty object when none).
            struct AgentMeta
            {
                fs::path path;
                std::shared_ptr<const Value> meta;

                const Value* Meta() const noexcept
                {
                    return meta ? meta.get() : &EmptyObject();
                }
            };

            // The per-render caches the agent search shares (the reference's meta_cache, line_cache,
            // head_cache), keyed by path identity. It lives only as long as the search: anything the
            // render reads later must not point into these caches (AgentRun::owners keeps the lines).
            struct AgentContext
            {
                // A context for one agent search (an explicit constructor rather than an aggregate: a
                // reference member in an aggregate draws MSVC's level-4 "can never be instantiated"
                // family of warnings, which the app build treats as errors).
                //   source — the history being rendered; it outlives the search (the search runs inside the render).
                //   opt    — the render's options; agentOpts starts as a copy (AttachAgents adjusts its rules).
                AgentContext(const History& source, const TurnsOptions& opt) :
                    history(source), agentOpts(opt)
                {
                }

                const History& history;
                TurnsOptions agentOpts;
                std::unordered_map<std::wstring, std::vector<AgentMeta>> metaCache;
                std::unordered_map<std::wstring, LineFile> lineCache;
                std::unordered_map<std::wstring, std::pair<std::wstring, std::wstring>> headCache;
            };

            // agent_metas(): every agent transcript in a subagents folder (name order) with its meta.
            const std::vector<AgentMeta>& AgentMetas(AgentContext& cx, const fs::path& folder)
            {
                const std::wstring key = PathKey(folder);
                const auto cached = cx.metaCache.find(key);
                if (cached != cx.metaCache.end())
                {
                    return cached->second;
                }
                std::vector<AgentMeta> found;
                const auto files = ListFilesSorted(folder, [](const std::wstring& name) { return StartsWithCi(name, L"agent-") && EndsWithCi(name, L".jsonl"); });
                for (const auto& file : files)
                {
                    AgentMeta entry;
                    entry.path = file;
                    const std::wstring name = file.filename().wstring();
                    const fs::path side = file.parent_path() / (name.substr(0, name.size() - 6) + L".meta.json");
                    if (IsFile(side))
                    {
                        // An unreadable / non-UTF-8 / non-JSON / non-object meta is just {} — the file can
                        // still be linked by its agentId. read_text() keeps a BOM, which json.loads then
                        // rejects, so a BOM'd meta reads as {} too.
                        const auto bytes = ReadAllBytes(side);
                        const auto text = bytes ? DecodeUtf8Strict(*bytes) : std::nullopt;
                        if (text)
                        {
                            auto parsed = ParseJsonExact(*text);
                            if (parsed && parsed->type == VType::Obj)
                            {
                                entry.meta = std::make_shared<const Value>(std::move(*parsed));
                            }
                        }
                    }
                    found.push_back(std::move(entry));
                }
                return cx.metaCache.emplace(key, std::move(found)).first->second;
            }

            // lines_of(): a transcript's lines, read once per render.
            //   cx   — the agent search's caches.
            //   path — the agent transcript.
            // Returns the SHARED owner of the parsed lines, or null when the file can't be read (a failure
            // is not cached, so a later ask retries — the reference re-raises on every ask). A caller that
            // keeps pointers into the lines past the search (AttachIn, for the run it builds) must keep this
            // owner too: the cache dies with the search. Throws std::bad_alloc only.
            LineFile LinesOf(AgentContext& cx, const fs::path& path)
            {
                const std::wstring key = PathKey(path);
                const auto cached = cx.lineCache.find(key);
                if (cached != cx.lineCache.end())
                {
                    return cached->second;
                }
                auto bytes = ReadAllBytes(path);
                if (!bytes)
                {
                    return nullptr;
                }
                LineFile lines = std::make_shared<const std::vector<Value>>(ParseTranscriptJsonl(std::move(*bytes)));
                cx.lineCache.emplace(key, lines);
                return lines;
            }

            // first_stamp(): a transcript's earliest timestamp (by instant, then text); "" when it is
            // unreadable or unstamped.
            std::wstring FirstStamp(AgentContext& cx, const fs::path& path)
            {
                const LineFile lines = LinesOf(cx, path);
                if (!lines)
                {
                    return {};
                }
                std::optional<std::pair<int64_t, std::wstring>> best;
                for (const Value& line : *lines)
                {
                    const Value* ts = JGet(&line, L"timestamp");
                    if (!IsStr(ts))
                    {
                        continue;
                    }
                    const auto stamp = AwareTs(ts->str);
                    if (!stamp)
                    {
                        continue;
                    }
                    std::pair<int64_t, std::wstring> candidate{ *stamp, ts->str };
                    if (!best || candidate < *best)
                    {
                        best = std::move(candidate);
                    }
                }
                return best ? best->second : std::wstring{};
            }

            // agent_head(): an agent transcript's sessionId and first prompt, from its first 64 lines
            // only (one render can have hundreds of agent transcripts to look at). Throws
            // TurnsReadError when the file can't be read.
            std::pair<std::wstring, std::wstring> AgentHead(const fs::path& path)
            {
                constexpr size_t kMaxLines = 64;
                const auto bytes = ReadHeadLines(path, kMaxLines);
                if (!bytes)
                {
                    throw TurnsReadError{};
                }
                std::vector<Value> head;
                size_t pos = 0;
                for (size_t count = 0; count < kMaxLines && pos < bytes->size(); ++count)
                {
                    const size_t newline = bytes->find('\n', pos);
                    const size_t end = newline == std::string::npos ? bytes->size() : newline + 1; // readline() keeps the '\n'
                    auto value = ParseJsonlLine(std::string_view{ bytes->data() + pos, end - pos });
                    pos = end;
                    if (!value)
                    {
                        continue; // a torn or non-text line
                    }
                    head.push_back(std::move(*value));
                    const Value* object = &head.back();
                    if (StrEq(JGet(object, L"type"), L"user") && !FirstPromptText(Lines{ object }).empty())
                    {
                        break;
                    }
                }
                std::wstring sessionId;
                for (const Value& line : head)
                {
                    const Value* sid = JGet(&line, L"sessionId");
                    if (IsStr(sid))
                    {
                        sessionId = sid->str;
                        break;
                    }
                }
                return { sessionId, FirstPromptText(PointersTo(head)) };
            }

            // head_of(): AgentHead, read once per file. Throws TurnsReadError when unreadable.
            const std::pair<std::wstring, std::wstring>& HeadOf(AgentContext& cx, const fs::path& path)
            {
                const std::wstring key = PathKey(path);
                const auto cached = cx.headCache.find(key);
                if (cached != cx.headCache.end())
                {
                    return cached->second;
                }
                return cx.headCache.emplace(key, AgentHead(path)).first->second;
            }

            // spawned_name(): the name a call gave the agent it spawned (a teammate's is its address), or "".
            std::wstring SpawnedName(const TurnOp& op)
            {
                for (const Value* value : { JGet(op.tur, L"name"), JGet(op.inp, L"name") })
                {
                    if (IsStr(value))
                    {
                        std::wstring name = PyStrip(value->str);
                        if (!name.empty())
                        {
                            return name;
                        }
                    }
                }
                return {};
            }

            using NameCalls = std::unordered_map<std::wstring, std::vector<std::wstring>>;

            // agent_files(): the transcript(s) of the agent a call spawned, by the links in order of
            // trust — the result's agentId, a meta's toolUseId, a teammate's name (the last same-name
            // call made before a file began owns it). Empty when none is found.
            std::vector<fs::path> AgentFiles(AgentContext& cx, const TurnOp& op, const fs::path& sessionPath, const NameCalls& nameCalls)
            {
                const fs::path folder = WithoutSuffix(sessionPath) / L"subagents";
                const Value* agentId = JGet(op.tur, L"agentId");
                if (IsStr(agentId) && !agentId->str.empty())
                {
                    const std::wstring leaf = L"agent-" + agentId->str + L".jsonl";
                    for (const fs::path& cand : { folder / leaf, sessionPath.parent_path() / leaf })
                    {
                        if (IsFile(cand))
                        {
                            return { cand };
                        }
                    }
                }
                const auto& listed = AgentMetas(cx, folder);
                std::vector<fs::path> byCall;
                for (const auto& entry : listed)
                {
                    if (!op.toolId.empty() && StrEq(JGet(entry.Meta(), L"toolUseId"), op.toolId))
                    {
                        byCall.push_back(entry.path);
                    }
                }
                if (!byCall.empty())
                {
                    return byCall;
                }
                const std::wstring name = SpawnedName(op);
                if (name.empty())
                {
                    return {};
                }
                std::vector<fs::path> mine;
                for (const auto& entry : listed)
                {
                    const Value* metaName = JGet(entry.Meta(), L"name");
                    if (StrEq(metaName, name) || (!Truthy(metaName) && StrEq(JGet(entry.Meta(), L"agentType"), name)))
                    {
                        mine.push_back(entry.path);
                    }
                }
                std::vector<std::wstring> calls;
                if (const auto it = nameCalls.find(name); it != nameCalls.end())
                {
                    for (const auto& ts : it->second)
                    {
                        if (AwareTs(ts))
                        {
                            calls.push_back(ts);
                        }
                    }
                }
                std::sort(calls.begin(), calls.end()); // the reference sorts the stamps as STRINGS
                if (calls.size() <= 1)
                {
                    return mine;
                }
                // owner(): the last same-name call made at most 5 s after the file began (clock skew), else the first.
                const auto owner = [&](const fs::path& file) -> std::wstring {
                    const auto begun = AwareTs(FirstStamp(cx, file));
                    if (!begun)
                    {
                        return calls[0];
                    }
                    std::wstring last;
                    for (const auto& ts : calls)
                    {
                        if (*AwareTs(ts) <= *begun + 5 * kMicrosPerSecond)
                        {
                            last = ts;
                        }
                    }
                    return !last.empty() ? last : calls[0];
                };
                std::vector<fs::path> out;
                for (const auto& file : mine)
                {
                    if (owner(file) == op.ts)
                    {
                        out.push_back(file);
                    }
                }
                return out;
            }

            // agent_files_by_prompt(): the fallback for a call with no link (interrupted before its
            // result): the agent transcript that begins with exactly the prompt the call sent. At most
            // one: the only match, or among several the earliest that began no more than 5 s before
            // the call. Throws TurnsReadError when a candidate can't be read.
            std::vector<fs::path> AgentFilesByPrompt(AgentContext& cx, const TurnOp& op, const fs::path& sessionPath)
            {
                const Value* promptValue = JGet(op.inp, L"prompt");
                const std::wstring prompt = IsStr(promptValue) ? PyJoinWs(promptValue->str) : std::wstring{};
                if (prompt.empty())
                {
                    return {};
                }
                std::vector<fs::path> candidates;
                for (const auto& entry : AgentMetas(cx, WithoutSuffix(sessionPath) / L"subagents"))
                {
                    candidates.push_back(entry.path);
                }
                // Claude Code 2.0.x: agent transcripts beside the session's own, told apart by sessionId.
                const std::wstring sessionStem = sessionPath.stem().wstring();
                const auto beside = ListFilesSorted(sessionPath.parent_path(), [](const std::wstring& name) { return StartsWithCi(name, L"agent-") && EndsWithCi(name, L".jsonl"); });
                for (const auto& file : beside)
                {
                    if (HeadOf(cx, file).first == sessionStem)
                    {
                        candidates.push_back(file);
                    }
                }
                std::vector<fs::path> matches;
                for (const auto& file : candidates)
                {
                    const std::wstring first = HeadOf(cx, file).second;
                    const auto peers = PeerMessages(first);
                    const std::wstring opening = (peers && !peers->empty()) ? (*peers)[0].said : first;
                    if (PyJoinWs(opening) == prompt)
                    {
                        matches.push_back(file);
                    }
                }
                if (matches.size() <= 1)
                {
                    return matches;
                }
                const auto called = AwareTs(op.ts);
                std::vector<fs::path> later;
                for (const auto& file : matches)
                {
                    if (!called)
                    {
                        later.push_back(file);
                        continue;
                    }
                    const auto begun = AwareTs(FirstStamp(cx, file));
                    if (begun.value_or(*called) >= *called - 5 * kMicrosPerSecond)
                    {
                        later.push_back(file);
                    }
                }
                std::vector<fs::path>& pool = later.empty() ? matches : later;
                std::stable_sort(pool.begin(), pool.end(), [&](const fs::path& a, const fs::path& b) {
                    return AwareTs(FirstStamp(cx, a)).value_or(INT64_MAX) < AwareTs(FirstStamp(cx, b)).value_or(INT64_MAX);
                });
                return { pool.front() };
            }

            // agent_label(): its own name, else its agent type; "agent" when nothing names it.
            std::wstring AgentLabelOf(const TurnOp& op, const Value* meta)
            {
                for (const Value* value : { JGet(op.inp, L"name"), JGet(meta, L"name"), JGet(op.tur, L"name"), JGet(op.inp, L"subagent_type"), JGet(meta, L"agentType") })
                {
                    if (IsStr(value))
                    {
                        std::wstring label = PyStrip(value->str);
                        if (!label.empty())
                        {
                            return label;
                        }
                    }
                }
                return L"agent";
            }

            // agent_kind(): "teammate" (it lives on and talks to the lead), "fork" (it inherits the
            // conversation's context), "background subagent" (the call returned at once) or "subagent".
            std::wstring AgentKindOf(const TurnOp& op, const Value* meta)
            {
                if (StrEq(JGet(op.tur, L"status"), L"teammate_spawned") || StrEq(JGet(meta, L"taskKind"), L"in_process_teammate") || Truthy(JGet(op.inp, L"team_name")))
                {
                    return L"teammate";
                }
                if (IsTrueBool(JGet(meta, L"isFork")) || StrEq(JGet(op.inp, L"subagent_type"), L"fork") || StrEq(JGet(meta, L"agentType"), L"fork"))
                {
                    return L"fork";
                }
                if (StrEq(JGet(op.tur, L"status"), L"async_launched") || IsTrueBool(JGet(op.inp, L"run_in_background")))
                {
                    return L"background subagent";
                }
                return L"subagent";
            }

            using SessionPathOf = std::function<fs::path(const TurnOp&)>;

            // attach() of attach_agents(): finds, parses and attaches the agent each Agent / Task call
            // inside one parsed session spawned, then theirs, up to kMaxAgentDepth. Pass 1 takes the
            // exact links; pass 2 links a still-unlinked call by the prompt it sent — only after every
            // exact link is taken, so it never claims a file another call names exactly. A call whose
            // agent can't be found or read keeps its line without an entry.
            void AttachIn(AgentContext& cx, TurnsSession& sess, const SessionPathOf& pathOf, int depth, const std::wstring& prefix, const UuidSet& seen)
            {
                std::vector<std::shared_ptr<TurnOp>> calls;
                for (const Turn& turn : sess.turns)
                {
                    for (const TurnEvent& ev : turn.events)
                    {
                        if (ev.kind == TurnEvent::Kind::Op && IsAgentTool(ev.op->name))
                        {
                            calls.push_back(ev.op);
                        }
                    }
                }
                NameCalls nameCalls;
                for (const auto& op : calls)
                {
                    const std::wstring name = SpawnedName(*op);
                    if (!name.empty())
                    {
                        nameCalls[name].push_back(op->ts);
                    }
                }
                std::unordered_map<size_t, std::vector<fs::path>> chosen;
                UuidSet used = seen; // path keys already taken
                for (const bool exact : { true, false })
                {
                    for (size_t i = 0; i < calls.size(); ++i)
                    {
                        if (chosen.count(i) != 0)
                        {
                            continue;
                        }
                        std::vector<fs::path> found;
                        try
                        {
                            found = exact ? AgentFiles(cx, *calls[i], pathOf(*calls[i]), nameCalls) : AgentFilesByPrompt(cx, *calls[i], pathOf(*calls[i]));
                        }
                        catch (const TurnsReadError&)
                        {
                            // Expected control flow (the reference's `except OSError: continue`): a
                            // transcript vanished or is locked, so this call keeps its line alone.
                            continue;
                        }
                        std::vector<fs::path> fresh;
                        for (const auto& file : found)
                        {
                            if (used.count(PathKey(file)) == 0)
                            {
                                fresh.push_back(file);
                            }
                        }
                        if (!fresh.empty())
                        {
                            for (const auto& file : fresh)
                            {
                                used.insert(PathKey(file));
                            }
                            chosen.emplace(i, std::move(fresh));
                        }
                    }
                }
                for (size_t i = 0; i < calls.size(); ++i)
                {
                    const auto picked = chosen.find(i);
                    if (picked == chosen.end())
                    {
                        continue;
                    }
                    TurnOp& op = *calls[i];
                    const fs::path sessionPath = pathOf(op);
                    std::vector<fs::path> files = picked->second;
                    std::stable_sort(files.begin(), files.end(), [&](const fs::path& a, const fs::path& b) {
                        return AwareTs(FirstStamp(cx, a)).value_or(INT64_MIN) < AwareTs(FirstStamp(cx, b)).value_or(INT64_MIN);
                    });
                    std::deque<Turn> turns;
                    std::vector<LineFile> owners; // the lines `turns` points into; handed to the run below
                    bool unreadable = false;
                    for (const auto& file : files)
                    {
                        const LineFile all = LinesOf(cx, file);
                        if (!all)
                        {
                            unreadable = true; // vanished or locked meanwhile: the call keeps its line, no entry
                            break;
                        }
                        owners.push_back(all);
                        // Without the walk, a repeated line would render twice: keep each uuid's first copy.
                        Lines lines;
                        UuidSet seenUuids;
                        for (const Value& line : *all)
                        {
                            const Value* uuid = JGet(&line, L"uuid");
                            if (IsStr(uuid) && !uuid->str.empty() && !seenUuids.insert(uuid->str).second)
                            {
                                continue;
                            }
                            lines.push_back(&line);
                        }
                        TurnsSession part = ParseTurns(lines, cx.agentOpts, nullptr, nullptr, nullptr);
                        for (auto& turn : part.turns)
                        {
                            turns.push_back(std::move(turn));
                        }
                    }
                    if (unreadable || turns.empty())
                    {
                        continue;
                    }
                    const Value* meta = &EmptyObject();
                    const std::wstring firstKey = PathKey(files[0]);
                    for (const auto& entry : AgentMetas(cx, WithoutSuffix(sessionPath) / L"subagents"))
                    {
                        if (PathKey(entry.path) == firstKey)
                        {
                            meta = entry.Meta();
                            break;
                        }
                    }
                    std::wstring label = AgentLabelOf(op, meta);
                    if (!prefix.empty())
                    {
                        label = prefix + L" › " + label;
                    }
                    // `inp.description if a str else meta.get("description", "")`, then str(x or "").
                    const Value* inputDescription = JGet(op.inp, L"description");
                    std::wstring description;
                    if (IsStr(inputDescription))
                    {
                        description = inputDescription->str;
                    }
                    else
                    {
                        const Value* metaDescription = JGet(meta, L"description");
                        description = Truthy(metaDescription) ? PyStr(metaDescription) : std::wstring{};
                    }
                    auto run = std::make_shared<AgentRun>();
                    run->label = label;
                    run->kind = AgentKindOf(op, meta);
                    run->description = std::move(description);
                    run->session.turns = std::move(turns);
                    run->session.sessionId = files[0].stem().wstring();
                    run->paths = files;
                    // The run owns the lines its turns point into: this search's cache (cx) is gone before
                    // the render reads them.
                    run->owners = std::move(owners);
                    run->completed = StrEq(JGet(op.tur, L"status"), L"completed");
                    op.agent = run;
                    if (depth + 1 < kMaxAgentDepth)
                    {
                        // An agent's own agents live in the same session's folder.
                        UuidSet deeper = seen;
                        for (const auto& file : files)
                        {
                            deeper.insert(PathKey(file));
                        }
                        AttachIn(cx, run->session, [sessionPath](const TurnOp&) { return sessionPath; }, depth + 1, label, deeper);
                    }
                }
            }

            // attach_agents(): the agents feature over a whole session. Every line of an agent's
            // transcript is a sidechain, so skip-sidechains is off for it, and so is the live-branch
            // walk (an agent gets no rewinds, and its parent links are often broken).
            void AttachAgents(TurnsSession& session, const History& history, const TurnsOptions& opt)
            {
                if (!history.path)
                {
                    return; // lines alone: there is no folder to look in
                }
                AgentContext cx{ history, opt };
                cx.agentOpts.skipSidechains = false;
                cx.agentOpts.liveBranch = false;
                const SessionPathOf sessionPathOf = [&history](const TurnOp& op) -> fs::path {
                    // The transcript of the session a call ran in: its chain link, else the head.
                    if (!history.links.empty() && op.origin && *op.origin >= 0 && static_cast<size_t>(*op.origin) < history.links.size())
                    {
                        return history.links[static_cast<size_t>(*op.origin)].path;
                    }
                    return *history.path;
                };
                AttachIn(cx, session, sessionPathOf, 0, L"", UuidSet{});
            }

            // merged_turn(): an agent's whole transcript as ONE turn, for its entry — each message that
            // reached it becomes an Input event where it arrived, its intermediate replies stay as
            // remarks, and its last reply is the entry's final text.
            Turn MergedTurn(const TurnsSession& session)
            {
                Turn merged;
                for (const Turn& turn : session.turns)
                {
                    for (const TurnInput& input : turn.inputs)
                    {
                        if (input.kind != L"continued")
                        {
                            merged.events.push_back(TurnEvent::OfInput(input));
                        }
                    }
                    merged.events.insert(merged.events.end(), turn.events.begin(), turn.events.end());
                    for (const auto& entry : turn.usage)
                    {
                        // setdefault: a message id already seen keeps its first usage.
                        const std::wstring& mid = entry.first;
                        if (std::none_of(merged.usage.begin(), merged.usage.end(), [&mid](const auto& u) { return u.first == mid; }))
                        {
                            merged.usage.emplace_back(entry);
                        }
                    }
                    for (const auto& model : turn.models)
                    {
                        if (std::find(merged.models.begin(), merged.models.end(), model) == merged.models.end())
                        {
                            merged.models.push_back(model);
                        }
                    }
                    if (!turn.last.empty())
                    {
                        merged.last = turn.last;
                        merged.lastMid = turn.lastMid;
                        merged.lastTs = turn.lastTs;
                    }
                    merged.endTs = std::max(merged.endTs, turn.endTs);
                }
                if (!session.turns.empty() && !session.turns.front().inputs.empty())
                {
                    merged.inputs = { session.turns.front().inputs.front() };
                }
                else
                {
                    merged.inputs = { TurnInput{ L"start", L"", L"", L"", L"" } };
                }
                return merged;
            }

            // worked_past_last_remark(): the agent kept calling tools (other than SendMessage) after its
            // last remark and never wrote again — its transcript stops mid-work.
            bool WorkedPastLastRemark(const Turn& turn)
            {
                bool found = false;
                size_t lastSay = 0;
                for (size_t i = 0; i < turn.events.size(); ++i)
                {
                    if (turn.events[i].kind == TurnEvent::Kind::Say && turn.lastMid && turn.events[i].mid == *turn.lastMid)
                    {
                        lastSay = i;
                        found = true;
                    }
                }
                if (!found)
                {
                    return false;
                }
                for (size_t i = lastSay + 1; i < turn.events.size(); ++i)
                {
                    if (turn.events[i].kind == TurnEvent::Kind::Op && turn.events[i].op->name != L"SendMessage")
                    {
                        return true;
                    }
                }
                return false;
            }

            // =====================================================================================
            // 7. Rendering
            // =====================================================================================

            // Claude Code's canned rejection when the user picks "clarify" on an AskUserQuestion.
            constexpr std::wstring_view kClarify = L"The user wants to clarify these questions";
            // The result Claude Code writes for a question cut off by the session ending.
            constexpr std::wstring_view kSessionEnded = L"[Tool call interrupted: the session ended";

            // _USER_SAID.search(text).group(1): what follows "the user said:" and its whitespace
            // (re.S | re.I) — unstripped; nullopt when the phrase isn't there.
            std::optional<std::wstring> UserSaidWords(std::wstring_view text)
            {
                static constexpr std::wstring_view kSaid = L"the user said:";
                const size_t p = FindCi(text, kSaid, 0);
                if (p == std::wstring_view::npos)
                {
                    return std::nullopt;
                }
                size_t i = p + kSaid.size();
                while (i < text.size() && IsPySpace(text[i]))
                {
                    ++i;
                }
                return std::wstring{ text.substr(i) };
            }

            // _DISMISSED.match(result): "The user doesn.t want to proceed with this tool use" at the
            // start ('.' = any one character but a newline — a typographic apostrophe too).
            bool MatchesDismissed(std::wstring_view s)
            {
                static constexpr std::wstring_view kHead = L"The user doesn";
                static constexpr std::wstring_view kTail = L"t want to proceed with this tool use";
                if (!StartsWith(s, kHead) || s.size() <= kHead.size() || s[kHead.size()] == L'\n')
                {
                    return false;
                }
                size_t i = kHead.size();
                i += (IsHighSurrogate(s[i]) && i + 1 < s.size() && IsLowSurrogate(s[i + 1])) ? 2 : 1;
                return StartsWith(s.substr(i), kTail);
            }

            // re.match(r"Exit code (-?\d+)", first): the code at the start of the line, or nullopt.
            std::optional<std::wstring> ExitCodePrefix(std::wstring_view first)
            {
                static constexpr std::wstring_view kHead = L"Exit code ";
                if (!StartsWith(first, kHead))
                {
                    return std::nullopt;
                }
                size_t j = kHead.size();
                if (j < first.size() && first[j] == L'-')
                {
                    ++j;
                }
                size_t k = j;
                while (k < first.size() && IsAsciiDigit(first[k]))
                {
                    ++k;
                }
                if (k == j)
                {
                    return std::nullopt;
                }
                return std::wstring{ first.substr(kHead.size(), k - kHead.size()) };
            }

            // re.fullmatch(r"Exit code -?\d+", line): the whole line is an exit-code line.
            bool IsExitCodeLine(std::wstring_view line)
            {
                const auto code = ExitCodePrefix(line);
                return code && std::wstring_view{ L"Exit code " }.size() + code->size() == line.size();
            }

            // The Renderer: renders a parsed session as Markdown under a fixed set of features, holding
            // the per-render state the pieces share — the clock (local or UTC), the session temp-folder
            // prefixes seen (for short paths and the legend), and the options.
            class Renderer
            {
            public:
                // ids — the session's id (its temp folder ends with it), or (chain) every chained
                //       session's id, since each wrote into its own temp folder. Empty ids are dropped.
                Renderer(const TurnsOptions& opt, std::vector<std::wstring> ids, bool utc) :
                    _opt(opt), _utc(utc)
                {
                    for (auto& id : ids)
                    {
                        if (!id.empty())
                        {
                            _ids.push_back(std::move(id));
                        }
                    }
                }

                // document(): the whole session (with a chain: the whole chain, headed per session).
                std::wstring Document(const TurnsSession& session)
                {
                    static const std::vector<ChainLink> kNoLinks;
                    const std::vector<ChainLink>& links = session.links != nullptr ? *session.links : kNoLinks;
                    const bool chained = links.size() > 1 || (!links.empty() && !links[0].missingParent.empty());
                    const bool headed = links.size() > 1; // a lone session (its plan parent gone) needs no per-session headings
                    std::vector<std::wstring> body;
                    int number = 0; // real turns only: the pre-prompt pseudo-turn doesn't take "Turn 1"
                    std::optional<int> shownOrigin;
                    for (const Turn& turn : session.turns)
                    {
                        if (!turn.continued)
                        {
                            ++number; // numbered across the whole chain: it is one conversation
                        }
                        if (headed && turn.origin && turn.origin != shownOrigin && static_cast<size_t>(*turn.origin) < links.size())
                        {
                            body.push_back(SessionHeader(links, static_cast<size_t>(*turn.origin)));
                            shownOrigin = turn.origin;
                        }
                        for (auto& chunk : RenderTurn(number, turn))
                        {
                            body.push_back(std::move(chunk));
                        }
                    }
                    const std::wstring after = NotesText(session.notesLast);
                    if (!after.empty())
                    {
                        body.push_back(L"After the last turn: " + after + L".\n");
                    }
                    std::vector<std::wstring> head;
                    if (chained)
                    {
                        head.push_back(ChainBlock(links));
                    }
                    const std::wstring first = NotesText(session.notesFirst);
                    if (!first.empty())
                    {
                        head.push_back(L"Before the first turn: " + first + L".\n");
                    }
                    const std::wstring legend = Legend(); // known only after the body rendered its paths
                    if (!legend.empty())
                    {
                        head.push_back(legend);
                    }
                    head.push_back(Join(body, L"\n"));
                    return Join(head, L"\n") + L"\n";
                }

            private:
                const TurnsOptions& _opt;
                bool _utc = false;
                std::vector<std::wstring> _ids;
                std::wstring _tmpRoot; // the first shortened prefix, remembered for the legend
                std::vector<std::wstring> _tmpRoots; // every distinct shortened prefix (a chain has one per session)

                // when(): 'Mon DD HH:MM' in local time (or UTC); '?' when unknown.
                std::wstring When(std::wstring_view ts) const
                {
                    const auto parsed = ParseIsoTimestamp(ts);
                    if (!parsed)
                    {
                        return L"?";
                    }
                    int64_t micros = parsed->micros;
                    if (parsed->aware && !_utc)
                    {
                        micros = UtcToLocalMicros(micros); // a zoneless stamp prints its wall clock as written
                    }
                    return FormatMonthDayTime(micros);
                }

                // when_epoch(): epoch seconds (a file's creation time) formatted like When().
                std::wstring WhenEpoch(double seconds) const
                {
                    int64_t micros = EpochSecondsToMicros(seconds);
                    if (!_utc)
                    {
                        micros = UtcToLocalMicros(micros);
                    }
                    return FormatMonthDayTime(micros);
                }

                static constexpr bool IsSep(wchar_t c) noexcept
                {
                    return c == L'\\' || c == L'/';
                }

                // The class [^\s"'`\\/] of a temp-path component.
                static bool IsComponentChar(wchar_t c) noexcept
                {
                    return !(IsPySpace(c) || c == L'"' || c == L'\'' || c == L'`' || IsSep(c));
                }

                // _tmp_rx at `s`: [A-Za-z]:[\\/](?:[^\s"'`\\/]+[\\/])*?(?:<id>|<id>...)[\\/] — the end
                // of the match, or 0 when none starts there. The lazy component loop tries the id at
                // each component boundary first; a component is a maximal run of class characters
                // that must end at a separator.
                size_t MatchTempRootAt(std::wstring_view t, size_t s) const
                {
                    if (s + 3 > t.size() || !IsAsciiLetter(t[s]) || t[s + 1] != L':' || !IsSep(t[s + 2]))
                    {
                        return 0;
                    }
                    size_t pos = s + 3;
                    for (;;)
                    {
                        for (const auto& id : _ids)
                        {
                            if (pos + id.size() < t.size() && t.compare(pos, id.size(), id) == 0 && IsSep(t[pos + id.size()]))
                            {
                                return pos + id.size() + 1;
                            }
                        }
                        size_t j = pos;
                        while (j < t.size() && IsComponentChar(t[j]))
                        {
                            ++j;
                        }
                        if (j == pos || j >= t.size() || !IsSep(t[j]))
                        {
                            return 0;
                        }
                        pos = j + 1;
                    }
                }

                // short(): strips the session-temp prefix from every path in `text` (short-paths),
                // recording each distinct prefix (separators as '\') for the legend.
                std::wstring Short(std::wstring_view text)
                {
                    if (!_opt.shortPaths || _ids.empty())
                    {
                        return std::wstring{ text };
                    }
                    std::wstring out;
                    size_t copied = 0;
                    size_t s = 0;
                    while (s < text.size())
                    {
                        const size_t end = MatchTempRootAt(text, s);
                        if (end == 0)
                        {
                            ++s;
                            continue;
                        }
                        std::wstring root{ text.substr(s, end - s) };
                        std::replace(root.begin(), root.end(), L'/', L'\\');
                        if (_tmpRoot.empty())
                        {
                            _tmpRoot = root;
                        }
                        if (std::find(_tmpRoots.begin(), _tmpRoots.end(), root) == _tmpRoots.end())
                        {
                            _tmpRoots.push_back(root);
                        }
                        out.append(text.substr(copied, s - copied));
                        copied = end;
                        s = end;
                    }
                    out.append(text.substr(copied));
                    return out;
                }

                // A tool argument read as `inp.get(key, '')`: "" when absent, else its str().
                static std::wstring ArgText(const Value* inp, std::wstring_view key)
                {
                    const Value* v = JGet(inp, key);
                    return v == nullptr ? std::wstring{} : PyStr(v);
                }

                // op_summary(): the dash-list line for a tool call — the tool and the arguments that say
                // what it did (without the "- "). Paths are shortened BEFORE a line is capped, so a cap
                // never cuts a long temp path mid-name.
                std::wstring OpSummary(const TurnOp& op)
                {
                    const std::wstring& name = op.name;
                    const Value* inp = op.inp;
                    if (name == L"Bash" && IsStr(JGet(inp, L"command")))
                    {
                        const std::wstring cmd = Code(OneLine(Short(JGet(inp, L"command")->str)));
                        const Value* desc = JGet(inp, L"description");
                        const std::wstring line = Truthy(desc) ? L"**Bash** — " + PyStr(desc) + L": " + cmd : L"**Bash** " + cmd;
                        std::vector<std::wstring> flags;
                        if (Truthy(JGet(inp, L"run_in_background")))
                        {
                            flags.push_back(L"in background");
                        }
                        const auto timeout = SafeInt(JGet(inp, L"timeout"));
                        if (timeout && *timeout != 0)
                        {
                            flags.push_back(L"timeout " + std::to_wstring(FloorDiv(*timeout, 1000)) + L"s");
                        }
                        return line + (flags.empty() ? std::wstring{} : L" (" + Join(flags, L", ") + L")");
                    }
                    if (name == L"Write")
                    {
                        const Value* content = JGet(inp, L"content");
                        const size_t lineCount = Truthy(content) ? PySplitLines(PyStr(content)).size() : 0;
                        return L"**Write** " + Code(Short(ArgText(inp, L"file_path"))) + L" (" + std::to_wstring(lineCount) + L" lines)";
                    }
                    if (name == L"Read")
                    {
                        std::wstring range;
                        if (Truthy(JGet(inp, L"offset")) || Truthy(JGet(inp, L"limit")))
                        {
                            const auto offset = SafeInt(JGet(inp, L"offset"));
                            const int64_t start = (offset && *offset != 0) ? *offset : 1; // safe_int(...) or 1
                            const auto limit = SafeInt(JGet(inp, L"limit"));
                            range = (limit && *limit != 0) ? L" lines " + std::to_wstring(start) + L"–" + std::to_wstring(start + *limit - 1) : L" from line " + std::to_wstring(start);
                        }
                        return L"**Read** " + Code(Short(ArgText(inp, L"file_path"))) + range;
                    }
                    if (name == L"Edit" || name == L"MultiEdit")
                    {
                        const std::wstring extra = Truthy(JGet(inp, L"replace_all")) ? L" (replace all)" : L"";
                        return L"**" + name + L"** " + Code(Short(ArgText(inp, L"file_path"))) + extra;
                    }
                    if (name == L"Grep")
                    {
                        const Value* where = JGet(inp, L"path");
                        if (!Truthy(where))
                        {
                            where = JGet(inp, L"glob");
                        }
                        std::wstring out = L"**Grep** " + Code(OneLine(ArgText(inp, L"pattern"), 80));
                        if (Truthy(where))
                        {
                            out += L" in " + Code(Short(PyStr(where)));
                        }
                        return out;
                    }
                    if (name == L"Glob")
                    {
                        std::wstring out = L"**Glob** " + Code(ArgText(inp, L"pattern"));
                        if (Truthy(JGet(inp, L"path")))
                        {
                            out += L" in " + Code(Short(PyStr(JGet(inp, L"path"))));
                        }
                        return out;
                    }
                    if (name == L"WebFetch")
                    {
                        return L"**WebFetch** " + Code(ArgText(inp, L"url"));
                    }
                    if (name == L"WebSearch")
                    {
                        return L"**WebSearch** " + Code(OneLine(ArgText(inp, L"query"), 100));
                    }
                    if (name == L"Task" || name == L"Agent")
                    {
                        const Value* subagentType = JGet(inp, L"subagent_type");
                        const std::wstring kind = Truthy(subagentType) ? L" (" + PyStr(subagentType) + L")" : L"";
                        return L"**" + name + L"** — " + ArgText(inp, L"description") + kind;
                    }
                    if (name == L"Skill")
                    {
                        std::wstring out = L"**Skill** " + Code(ArgText(inp, L"skill"));
                        if (Truthy(JGet(inp, L"args")))
                        {
                            out += L" " + Code(OneLine(PyStr(JGet(inp, L"args")), 80));
                        }
                        return out;
                    }
                    if (name == L"TodoWrite")
                    {
                        return L"**TodoWrite** (" + std::to_wstring(PyLen(JGet(inp, L"todos"))) + L" items)";
                    }
                    if (name == L"AskUserQuestion")
                    {
                        const auto questions = QuestionTexts(inp);
                        const std::wstring first = questions.empty() ? std::wstring{} : OneLine(questions[0], 100);
                        return L"**AskUserQuestion** — " + first + (questions.size() > 1 ? L" (+" + std::to_wstring(questions.size() - 1) + L" more)" : std::wstring{});
                    }
                    if (name == L"SendMessage" && _opt.peerMessages)
                    {
                        // Who it went to and what it said (its own summary when it gave one).
                        const Value* to = JGet(inp, L"to");
                        if (!Truthy(to))
                        {
                            to = JGet(inp, L"recipient");
                        }
                        const Value* what = JGet(inp, L"summary");
                        if (!Truthy(what))
                        {
                            what = JGet(inp, L"message");
                        }
                        if (!Truthy(what))
                        {
                            what = JGet(inp, L"content");
                        }
                        const std::wstring whatText = Truthy(what) ? Short(IsStr(what) ? what->str : PyJsonDumps(*what)) : std::wstring{};
                        return L"**SendMessage** → " + ((IsStr(to) && !to->str.empty()) ? to->str : std::wstring{ L"?" }) + (whatText.empty() ? std::wstring{} : L": " + OneLine(whatText, 120));
                    }
                    // Any other tool: its scalar arguments, long ones reduced to their size.
                    std::vector<std::wstring> parts;
                    for (const auto& [key, value] : PyItems(inp))
                    {
                        if (value->type == VType::Str || value->type == VType::Num || value->type == VType::Bool)
                        {
                            const std::wstring shown = Short(PyStr(value));
                            const size_t length = CpLen(shown);
                            parts.push_back(std::wstring{ key } + L"=" + (length <= 60 ? Code(OneLine(shown, 60)) : L"(" + std::to_wstring(length) + L" chars)"));
                        }
                        else
                        {
                            parts.push_back(std::wstring{ key } + L"=(" + PyTypeName(value) + L")");
                        }
                    }
                    return L"**" + name + L"** " + Join(parts, L", ");
                }

                // op_outcome(): how a call ended, in a word or two ("ok", "✗ exit 1", "timed out →
                // background `id`", "created", "40 of 120 lines"; "no result" when none arrived).
                static std::wstring OpOutcome(const TurnOp& op)
                {
                    if (op.resultTs.empty())
                    {
                        return L"no result";
                    }
                    const auto lines = PySplitLines(PyStrip(op.resultText));
                    const std::wstring first = lines.empty() ? std::wstring{} : PyStrip(lines[0]);
                    if (op.name == L"Bash")
                    {
                        if (op.isError)
                        {
                            const auto exitCode = ExitCodePrefix(first);
                            return exitCode ? L"✗ exit " + *exitCode : std::wstring{ L"✗ error" };
                        }
                        if (Truthy(JGet(op.tur, L"interrupted")))
                        {
                            return L"interrupted";
                        }
                        const Value* background = JGet(op.tur, L"backgroundTaskId");
                        if (Truthy(background))
                        {
                            const bool moved = op.resultText.find(L"moved to the background") != std::wstring::npos;
                            return (moved ? std::wstring{ L"timed out → background `" } : std::wstring{ L"→ background `" }) + PyStr(background) + L"`";
                        }
                        return L"ok";
                    }
                    if (op.isError)
                    {
                        return L"✗ error";
                    }
                    if (op.name == L"Write")
                    {
                        const Value* kind = JGet(op.tur, L"type");
                        if (StrEq(kind, L"create"))
                        {
                            return L"created";
                        }
                        if (StrEq(kind, L"update"))
                        {
                            return L"updated";
                        }
                        return L"ok";
                    }
                    if (op.name == L"Read")
                    {
                        const Value* info = ObjOrEmpty(JGet(op.tur, L"file"));
                        const Value* n = JGet(info, L"numLines");
                        const Value* total = JGet(info, L"totalLines");
                        if (Truthy(n) && Truthy(total) && !PyEquals(total, n))
                        {
                            return PyStr(n) + L" of " + PyStr(total) + L" lines";
                        }
                        return Truthy(n) ? PyStr(n) + L" lines" : std::wstring{ L"ok" };
                    }
                    return L"ok";
                }

                // op_meta(): the " · duration · outcome" suffix — the duration (op-duration) only when
                // both timestamps parse.
                std::wstring OpMeta(const TurnOp& op) const
                {
                    std::wstring took;
                    if (_opt.opDuration)
                    {
                        const auto start = ParseIsoTimestamp(op.ts);
                        const auto end = ParseIsoTimestamp(op.resultTs);
                        if (start && end)
                        {
                            took = L" · " + FmtDuration(SecondsBetween(*start, *end));
                        }
                    }
                    return took + L" · " + OpOutcome(op);
                }

                // op_output(): the first meaningful line of what a call returned, as a code span plus
                // "(+N lines)"; nullopt where the outcome already says it all (file reads / writes /
                // edits, a Bash call sent to the background) or the answers feature shows it in full.
                static std::optional<std::wstring> OpOutput(const TurnOp& op, bool answersShown)
                {
                    if (op.resultTs.empty() || op.name == L"Write" || op.name == L"Read" || op.name == L"Edit" || op.name == L"MultiEdit" || op.name == L"TodoWrite")
                    {
                        return std::nullopt;
                    }
                    if (answersShown && (op.name == L"AskUserQuestion" || (op.isError && UserSaidWords(op.resultText))))
                    {
                        return std::nullopt;
                    }
                    std::vector<std::wstring> rows;
                    for (auto& row : PySplitLines(op.resultText))
                    {
                        if (!PyStrip(row).empty())
                        {
                            rows.push_back(std::move(row));
                        }
                    }
                    if (op.name == L"Bash")
                    {
                        if (Truthy(JGet(op.tur, L"backgroundTaskId")))
                        {
                            return std::nullopt;
                        }
                        if (op.isError && rows.size() > 1 && IsExitCodeLine(PyStrip(rows[0])))
                        {
                            rows.erase(rows.begin()); // "Exit code N" gives way to the error text after it
                        }
                    }
                    if (rows.empty())
                    {
                        return std::nullopt;
                    }
                    const std::wstring more = rows.size() > 1 ? L" (" + PlusLines(rows.size() - 1) + L")" : std::wstring{};
                    return Code(Flat(rows[0], 100)) + more;
                }

                // ---- Agentmaster, NOT in the reference: TurnsOptions::editDiffs (📜 All Screen's "File diffs", MESSAGING.md
                // section 21.39). No level sets it, so nothing below runs for Copy > Transcript or the goldens. ----

                /// <summary>Whether a call changes a file, and so can carry a diff: Edit, MultiEdit, Write, NotebookEdit.</summary>
                /// <param name="op">The call.</param>
                /// <returns>True for one of those tools (whether it went through is EditDiffText's question).</returns>
                static bool IsFileChangeCall(const TurnOp& op) noexcept
                {
                    return op.name == L"Edit" || op.name == L"MultiEdit" || op.name == L"Write" || op.name == L"NotebookEdit";
                }

                /// <summary>
                /// The change a file-changing call made, as unified-diff text ("@@ -a,b +c,d @@" hunk headers, then lines that start
                /// with ' ', '-' or '+'), read in this order:
                ///   1. the result's toolUseResult.structuredPatch - Claude Code's OWN hunks, the very lines its TUI draws, with their
                ///      line numbers (Edit, MultiEdit, a Write over an existing file);
                ///   2. a new file (Write, structuredPatch empty): its content, every line added, from line 1;
                ///   3. no hunks recorded (an older Claude Code): the call's own old_string / new_string (each edit of a MultiEdit;
                ///      a NotebookEdit's new_source), with no hunk header, so no line numbers.
                /// </summary>
                /// <param name="op">The call.</param>
                /// <returns>The diff text, or nullopt for a call that changed nothing to show: not a file change, no result yet
                /// (interrupted, still running), or refused (an error result means the file was not written).</returns>
                /// <exception cref="std::bad_alloc">If the text cannot be built (the render's own net contains it).</exception>
                static std::optional<std::wstring> EditDiffText(const TurnOp& op)
                {
                    if (!IsFileChangeCall(op) || op.resultTs.empty() || op.isError)
                    {
                        return std::nullopt;
                    }
                    std::wstring out;
                    const Value* patch = JGet(op.tur, L"structuredPatch");
                    if (IsArr(patch))
                    {
                        for (const Value& hunk : patch->arr)
                        {
                            if (!IsObj(&hunk))
                            {
                                continue;
                            }
                            // The numbers through SafeInt: a field of an unexpected type reads as 0 instead of failing the render.
                            const auto number = [&hunk](const wchar_t* key) { return std::to_wstring(SafeInt(JGet(&hunk, key)).value_or(0)); };
                            out += L"@@ -" + number(L"oldStart") + L"," + number(L"oldLines") + L" +" + number(L"newStart") + L"," + number(L"newLines") + L" @@\n";
                            const Value* rows = JGet(&hunk, L"lines");
                            if (!IsArr(rows))
                            {
                                continue;
                            }
                            for (const Value& row : rows->arr)
                            {
                                if (IsStr(&row))
                                {
                                    out += row.str;
                                    out += L'\n';
                                }
                            }
                        }
                    }
                    if (!out.empty())
                    {
                        return out;
                    }
                    if (op.name == L"Write")
                    {
                        // A new file: Claude Code records no hunks, only the content (the result's, else the call's own).
                        const Value* content = JGet(op.tur, L"content");
                        if (!IsStr(content))
                        {
                            content = JGet(op.inp, L"content");
                        }
                        if (IsStr(content) && !content->str.empty())
                        {
                            const auto rows = PySplitLines(content->str);
                            out = L"@@ -0,0 +1," + std::to_wstring(rows.size()) + L" @@\n";
                            for (const auto& row : rows)
                            {
                                out += L'+';
                                out += row;
                                out += L'\n';
                            }
                        }
                        return out.empty() ? std::nullopt : std::optional<std::wstring>{ std::move(out) };
                    }
                    // No hunks: the strings the call itself carried, removed then added (no numbers: no header).
                    const auto replaced = [&out](const Value* from, const Value* to) {
                        if (!IsStr(from) || !IsStr(to))
                        {
                            return;
                        }
                        for (const auto& row : PySplitLines(from->str))
                        {
                            out += L'-';
                            out += row;
                            out += L'\n';
                        }
                        for (const auto& row : PySplitLines(to->str))
                        {
                            out += L'+';
                            out += row;
                            out += L'\n';
                        }
                    };
                    if (op.name == L"Edit")
                    {
                        replaced(JGet(op.inp, L"old_string"), JGet(op.inp, L"new_string"));
                    }
                    else if (op.name == L"MultiEdit")
                    {
                        const Value* edits = JGet(op.inp, L"edits");
                        if (IsArr(edits))
                        {
                            for (const Value& edit : edits->arr)
                            {
                                if (IsObj(&edit))
                                {
                                    replaced(JGet(&edit, L"old_string"), JGet(&edit, L"new_string"));
                                }
                            }
                        }
                    }
                    else if (op.name == L"NotebookEdit")
                    {
                        const Value* source = JGet(op.inp, L"new_source");
                        if (IsStr(source))
                        {
                            for (const auto& row : PySplitLines(source->str))
                            {
                                out += L'+';
                                out += row;
                                out += L'\n';
                            }
                        }
                    }
                    if (out.empty())
                    {
                        return std::nullopt;
                    }
                    return out;
                }

                // asked(): the question an AskUserQuestion call asked, for a reply shown without its call.
                static std::wstring Asked(const TurnOp& op)
                {
                    const auto questions = QuestionTexts(op.inp);
                    std::wstring text = (!questions.empty() && !PyStrip(questions[0]).empty()) ? Flat(questions[0], 90) : std::wstring{ L"a question" };
                    if (questions.size() > 1)
                    {
                        text += L" (+" + std::to_wstring(questions.size() - 1) + L" more)";
                    }
                    return text;
                }

                // user_answers(): what the user said in reply to a call — "question → **answer**" per
                // question of an answered AskUserQuestion; their words when they rejected a call; how an
                // unanswered question ended. `standalone`: printed WITHOUT its call's line (ops off), so
                // each reply names what it answers.
                static std::vector<std::wstring> UserAnswers(const TurnOp& op, bool standalone)
                {
                    if (op.resultTs.empty())
                    {
                        return {};
                    }
                    const Value* answers = JGet(op.tur, L"answers");
                    if (op.name == L"AskUserQuestion" && IsObj(answers) && !answers->members.empty())
                    {
                        std::vector<std::wstring> out;
                        for (const auto& [question, answer] : PyItems(answers))
                        {
                            out.push_back(L"❯ " + Flat(question, 90) + L" → **" + Flat(PyStr(answer), 400) + L"**");
                        }
                        return out;
                    }
                    const std::optional<std::wstring> said = op.isError ? UserSaidWords(op.resultText) : std::nullopt;
                    const std::wstring words = said ? PyStrip(*said) : std::wstring{};
                    const std::wstring result = PyLStrip(op.resultText);
                    std::wstring reply;
                    // Order matters: a rejection WITH words starts with the same generic prefix as a
                    // silent dismissal, so the words are checked first.
                    if (StartsWith(words, kClarify))
                    {
                        reply = L"chose to clarify the question instead of answering it";
                    }
                    else if (!words.empty())
                    {
                        reply = L"rejected — the user said: **" + Flat(words, 400) + L"**";
                    }
                    else if (op.name != L"AskUserQuestion")
                    {
                        return {}; // a call the user said nothing to (a silent rejection is op-output's job)
                    }
                    else if (MatchesDismissed(result))
                    {
                        reply = L"dismissed the question without answering";
                    }
                    else if (StartsWith(result, kSessionEnded))
                    {
                        reply = L"no answer — the session ended before one was recorded";
                    }
                    else
                    {
                        const std::wstring flat = Flat(op.resultText, 120);
                        reply = L"no answer — " + (flat.empty() ? std::wstring{ L"none recorded" } : flat);
                    }
                    if (!standalone)
                    {
                        return { L"❯ " + reply };
                    }
                    if (op.name == L"AskUserQuestion")
                    {
                        return { L"❯ " + Asked(op) + L" → " + reply };
                    }
                    return { L"❯ **" + op.name + L"** call " + reply };
                }

                // stats_line(): "_N tool calls · duration · N API calls · models · context Xk · out Yk
                // tokens (Zk thinking)_" from the de-duplicated per-message usage.
                std::wstring StatsLine(const Turn& turn) const
                {
                    const size_t calls = static_cast<size_t>(std::count_if(turn.events.begin(), turn.events.end(), [](const TurnEvent& ev) { return ev.kind == TurnEvent::Kind::Op; }));
                    std::vector<std::wstring> parts;
                    parts.push_back(std::to_wstring(calls) + L" tool call" + (calls != 1 ? L"s" : L""));
                    const auto start = ParseIsoTimestamp(turn.inputs[0].ts);
                    const auto end = ParseIsoTimestamp(turn.endTs);
                    if (start && end)
                    {
                        parts.push_back(FmtDuration(SecondsBetween(*start, *end)));
                    }
                    parts.push_back(std::to_wstring(turn.usage.size()) + L" API call" + (turn.usage.size() != 1 ? L"s" : L""));
                    if (!turn.models.empty())
                    {
                        parts.push_back(Join(turn.models, L" + "));
                    }
                    if (!turn.usage.empty())
                    {
                        const Value* last = turn.usage.back().second;
                        int64_t context = 0;
                        for (const wchar_t* key : { L"input_tokens", L"cache_creation_input_tokens", L"cache_read_input_tokens" })
                        {
                            context += PyIntOrZero(JGet(last, key));
                        }
                        int64_t out = 0;
                        int64_t thinking = 0;
                        for (const auto& entry : turn.usage)
                        {
                            const Value* usage = entry.second; // (entry.first is the message id: de-duplication already happened in the parse)
                            out += PyIntOrZero(JGet(usage, L"output_tokens"));
                            thinking += PyIntOrZero(JGet(ObjOrEmpty(JGet(usage, L"output_tokens_details")), L"thinking_tokens"));
                        }
                        parts.push_back(L"context " + FmtTokens(context));
                        parts.push_back(L"out " + FmtTokens(out) + L" tokens" + (thinking != 0 ? L" (" + FmtTokens(thinking) + L" thinking)" : std::wstring{}));
                    }
                    return L"_" + Join(parts, L" · ") + L"_";
                }

                // notes_text(): the notes whose feature is on (cmd: notes, compact: compactions), in TIME
                // order, as "<time> — <text>" (times) or "<text>", joined by "; ". "" when none is on.
                std::wstring NotesText(const std::vector<TurnNote>& notes) const
                {
                    std::vector<const TurnNote*> shown;
                    for (const TurnNote& note : notes)
                    {
                        if ((note.kind == L"cmd" && _opt.notes) || (note.kind == L"compact" && _opt.compactions))
                        {
                            shown.push_back(&note);
                        }
                    }
                    // Time order, not record order: a manual /compact records its compaction before the
                    // command's stdout completes the command note, yet the command came first.
                    std::stable_sort(shown.begin(), shown.end(), [](const TurnNote* a, const TurnNote* b) { return a->ts < b->ts; });
                    std::vector<std::wstring> parts;
                    for (const TurnNote* note : shown)
                    {
                        parts.push_back(_opt.times ? When(note->ts) + L" — " + note->text : note->text);
                    }
                    return Join(parts, L"; ");
                }

                // input_label(): the bold label over a message that reached a turn.
                std::wstring InputLabel(const TurnInput& input) const
                {
                    if (input.sender.empty())
                    {
                        if (input.kind == L"start")
                        {
                            return L"**❯ User**";
                        }
                        return _opt.times ? L"**❯ User, typed while it worked (" + When(input.ts) + L")**" : std::wstring{ L"**❯ User, typed while it worked**" };
                    }
                    const std::wstring note = input.note.empty() ? std::wstring{} : L" · " + input.note;
                    if (input.kind == L"start")
                    {
                        return L"**✉ " + input.sender + L"**" + note;
                    }
                    const std::wstring when = _opt.times ? L" (" + When(input.ts) + L")" : std::wstring{};
                    return L"**✉ " + input.sender + L", sent while it worked" + when + L"**" + note;
                }

                // event_lines(): the timeline lines for one event of a turn.
                std::vector<std::wstring> EventLines(const TurnEvent& ev, const Turn& turn)
                {
                    std::vector<std::wstring> lines;
                    if (ev.kind == TurnEvent::Kind::Op && !_opt.ops)
                    {
                        if (_opt.answers)
                        {
                            // Calls hidden: the user's answers still show, each naming what it answers.
                            for (auto& said : UserAnswers(*ev.op, true))
                            {
                                lines.push_back(L"- " + said);
                            }
                        }
                        if (_opt.editDiffs)
                        {
                            // Agentmaster (editDiffs, not in the reference): the level hides the calls, but the file changes show
                            // anyway - each file-changing call's own line, then its diff - since the option means "whatever else the
                            // level hides".
                            if (auto diff = EditDiffText(*ev.op))
                            {
                                const TurnOp& op = *ev.op;
                                lines.push_back(L"- " + OpSummary(op) + (_opt.opMeta ? OpMeta(op) : std::wstring{}));
                                lines.push_back(IndentedBlock(*diff, L"diff", _opt.nestedBlocks));
                            }
                        }
                    }
                    else if (ev.kind == TurnEvent::Kind::Op)
                    {
                        const TurnOp& op = *ev.op;
                        // (The reference guards op_summary against model-written arguments of any shape;
                        // here every argument is read through total accessors, so there is nothing to
                        // guard — see the header's divergence note.)
                        const std::wstring summary = OpSummary(op);
                        const std::wstring pointer = (_opt.agents && op.agent) ? L" · → ● " + op.agent->label : std::wstring{};
                        lines.push_back(L"- " + summary + (_opt.opMeta ? OpMeta(op) : std::wstring{}) + pointer);
                        if (_opt.fullBash && op.name == L"Bash")
                        {
                            const Value* command = JGet(op.inp, L"command");
                            if (IsStr(command) && !command->str.empty())
                            {
                                // Verbatim, not path-shortened: the block is the exact, re-runnable command.
                                lines.push_back(IndentedBlock(command->str, L"bash", _opt.nestedBlocks));
                            }
                        }
                        // Agentmaster (editDiffs, not in the reference): the call's change as a diff, read once here. A Write's diff
                        // takes the place of fullWrite's content block below - the same lines, shown once.
                        const std::optional<std::wstring> diff = _opt.editDiffs ? EditDiffText(op) : std::nullopt;
                        if (_opt.fullWrite && op.name == L"Write" && !diff)
                        {
                            const Value* content = JGet(op.inp, L"content");
                            if (IsStr(content))
                            {
                                lines.push_back(IndentedBlock(content->str, FenceLanguageFor(StrOr(JGet(op.inp, L"file_path"))), _opt.nestedBlocks));
                            }
                        }
                        if (diff)
                        {
                            lines.push_back(IndentedBlock(*diff, L"diff", _opt.nestedBlocks));
                        }
                        // A call whose agent has its own entry: the entry holds what came back. A delivered
                        // SendMessage: its result is routing metadata, and its line says who it went to.
                        const bool saidElsewhere = (_opt.agents && op.agent) || (_opt.peerMessages && op.name == L"SendMessage" && !op.resultTs.empty() && !op.isError);
                        if (_opt.opOutput && !saidElsewhere)
                        {
                            if (const auto shown = OpOutput(op, _opt.answers))
                            {
                                lines.push_back(L"  - ⇢ " + *shown);
                            }
                        }
                        if (_opt.bgEvents)
                        {
                            for (const BgEvent& bg : op.bg)
                            {
                                const std::wstring at = _opt.times ? L" at " + When(bg.ts) : std::wstring{};
                                const std::wstring tail = bg.exitCode.empty() ? std::wstring{} : L" · exit code " + bg.exitCode;
                                lines.push_back(L"  - ↳ background task " + Code(bg.taskId.empty() ? std::wstring{ L"?" } : bg.taskId) + L" " + (bg.status.empty() ? std::wstring{ L"finished" } : bg.status) + at + tail);
                            }
                        }
                        if (_opt.answers)
                        {
                            for (auto& said : UserAnswers(op, false))
                            {
                                lines.push_back(L"  - " + said);
                            }
                        }
                    }
                    else if (ev.kind == TurnEvent::Kind::Say && _opt.narration && !(turn.lastMid && ev.mid == *turn.lastMid))
                    {
                        lines.push_back(L"- 💬 " + Flat(ev.text, 200));
                    }
                    else if (ev.kind == TurnEvent::Kind::Compact && _opt.compactions)
                    {
                        const std::wstring at = _opt.times ? L" at " + When(ev.ts) : std::wstring{};
                        lines.push_back(L"- 🗜 " + ev.text + at);
                    }
                    else if (ev.kind == TurnEvent::Kind::Input)
                    {
                        const TurnInput& input = ev.input;
                        if (!input.sender.empty())
                        {
                            const std::wstring note = input.note.empty() ? std::wstring{} : L" (" + input.note + L")";
                            lines.push_back(L"- ✉ **" + input.sender + L"**" + note + L": " + OneLine(input.text, 200));
                        }
                        else
                        {
                            lines.push_back(L"- ❯ " + OneLine(input.text, 200));
                        }
                    }
                    return lines;
                }

                // speaker_block(): the part of a turn one speaker did — its label, stats, timeline and
                // last text. Claude's part of every turn goes through here, and so does each agent's
                // entry (over its merged turn). `unfinished`: (agent entries) its transcript stops
                // mid-work, so its last text is a remark, not a final report.
                std::vector<std::wstring> SpeakerBlock(const std::wstring& label, const Turn& turn, bool unfinished)
                {
                    std::vector<std::wstring> out{ label };
                    if (_opt.stats)
                    {
                        out.push_back(StatsLine(turn));
                    }
                    // A timeline shows when calls or remarks are on — or, with compactions / answers on,
                    // when there is a compaction to mark or an answer to show; an agent's entry always
                    // shows the messages that reached it.
                    bool hasCompaction = false;
                    bool hasAnswers = false;
                    bool hasInputs = false;
                    bool hasDiffs = false; // Agentmaster (editDiffs): a file change to show, so a timeline even at a level without calls
                    for (const TurnEvent& ev : turn.events)
                    {
                        hasCompaction = hasCompaction || ev.kind == TurnEvent::Kind::Compact;
                        hasInputs = hasInputs || ev.kind == TurnEvent::Kind::Input;
                        if (_opt.answers && !hasAnswers && ev.kind == TurnEvent::Kind::Op && !UserAnswers(*ev.op, false).empty())
                        {
                            hasAnswers = true;
                        }
                        // The exact test (EditDiffText, not just the tool's name): a change with nothing to show must not open an
                        // empty timeline, which would print "(no remarks)" where the level shows nothing.
                        if (_opt.editDiffs && !hasDiffs && ev.kind == TurnEvent::Kind::Op && IsFileChangeCall(*ev.op) && EditDiffText(*ev.op))
                        {
                            hasDiffs = true;
                        }
                    }
                    if (_opt.ops || _opt.narration || (_opt.compactions && hasCompaction) || hasAnswers || hasInputs || hasDiffs)
                    {
                        std::vector<std::wstring> timeline;
                        for (const TurnEvent& ev : turn.events)
                        {
                            for (auto& line : EventLines(ev, turn))
                            {
                                timeline.push_back(std::move(line));
                            }
                        }
                        if (!timeline.empty())
                        {
                            out.push_back(Join(timeline, L"\n"));
                        }
                        else
                        {
                            out.push_back(_opt.ops ? L"- (no tool calls)" : L"- (no remarks)");
                        }
                        out.push_back(L""); // a blank line keeps the quote out of the last list item
                    }
                    else if (_opt.stats)
                    {
                        out.push_back(L"");
                    }
                    if (unfinished && !turn.last.empty())
                    {
                        out.push_back(L"_Its transcript ends mid-work (no final report); its last remark:_");
                    }
                    if (!turn.last.empty())
                    {
                        out.push_back(Quote(turn.last) + L"\n");
                    }
                    else if (!turn.events.empty())
                    {
                        out.push_back(L"> (no text — the turn ended on a tool call)\n");
                    }
                    else
                    {
                        out.push_back(L"> (no reply)\n"); // e.g. a prompt the user rewound before any answer
                    }
                    return out;
                }

                // agent_entry(): an agent's entry under the turn that spawned it — "**● <name>** · <kind>
                // · <what for> · <start → end>", its timeline and last text — then its own agents'.
                std::vector<std::wstring> AgentEntry(const AgentRun& run)
                {
                    const Turn merged = MergedTurn(run.session);
                    std::wstring head = L"**● " + run.label + L"** · " + run.kind;
                    if (!run.description.empty())
                    {
                        head += L" · " + Flat(run.description, 100);
                    }
                    if (_opt.times)
                    {
                        // Until its last activity, not its last remark: an agent can work on long after it.
                        const std::wstring& start = merged.inputs[0].ts;
                        const std::wstring& end = !merged.endTs.empty() ? merged.endTs : merged.lastTs;
                        if (!start.empty() || !end.empty())
                        {
                            head += L" · " + When(start) + L" → " + When(end);
                        }
                    }
                    auto out = SpeakerBlock(head, merged, !run.completed && WorkedPastLastRemark(merged));
                    for (const TurnEvent& ev : merged.events)
                    {
                        if (ev.kind == TurnEvent::Kind::Op && ev.op->agent)
                        {
                            for (auto& chunk : AgentEntry(*ev.op->agent))
                            {
                                out.push_back(std::move(chunk));
                            }
                        }
                    }
                    return out;
                }

                // turn(): one turn — its header, what started it, Claude's part, then (agents) an entry
                // per agent it spawned, in the order it spawned them.
                std::vector<std::wstring> RenderTurn(int number, const Turn& turn)
                {
                    std::vector<std::wstring> out;
                    const std::wstring before = NotesText(turn.notesBefore);
                    if (!before.empty())
                    {
                        out.push_back(L"_Before this turn: " + before + L"._\n");
                    }
                    std::wstring header = turn.continued ? std::wstring{ L"### Work before the first prompt" } : L"### Turn " + std::to_wstring(number);
                    if (_opt.times)
                    {
                        header += L" · " + When(turn.inputs[0].ts);
                        if (!turn.lastTs.empty())
                        {
                            header += L" → last reply " + When(turn.lastTs);
                        }
                    }
                    if (turn.interrupted)
                    {
                        header += L" · the user interrupted it";
                    }
                    out.push_back(header + L"\n");
                    for (const TurnInput& input : turn.inputs)
                    {
                        if (input.kind == L"continued")
                        {
                            out.push_back(L"_This transcript opens mid-turn: the prompt that started this work is not in the file (it was compacted away, or the session was resumed)._\n");
                            continue;
                        }
                        out.push_back(InputLabel(input) + L"\n" + Quote(input.text) + L"\n");
                    }
                    for (auto& chunk : SpeakerBlock(L"**● Claude**", turn, false))
                    {
                        out.push_back(std::move(chunk));
                    }
                    if (_opt.agents)
                    {
                        for (const TurnEvent& ev : turn.events)
                        {
                            if (ev.kind == TurnEvent::Kind::Op && ev.op->agent)
                            {
                                for (auto& chunk : AgentEntry(*ev.op->agent))
                                {
                                    out.push_back(std::move(chunk));
                                }
                            }
                        }
                    }
                    return out;
                }

                // link_origin(): how a chained session began — "started Sep 23 18:42", "forked from
                // session 2 on Sep 26 01:03", "restarted from session 1 to implement its plan on ...".
                std::wstring LinkOrigin(const std::vector<ChainLink>& links, size_t index) const
                {
                    const ChainLink& link = links[index];
                    // The EARLIEST stamp, not the first line's: a session can write an attachment before
                    // the prompt that started it.
                    std::optional<std::pair<int64_t, std::wstring>> earliest;
                    for (const Value* line : link.lines)
                    {
                        const Value* ts = JGet(line, L"timestamp");
                        if (!Truthy(JGet(line, L"uuid")) || !IsStr(ts) || ts->str.empty())
                        {
                            continue;
                        }
                        const auto stamp = AwareTs(ts->str);
                        if (!stamp)
                        {
                            continue;
                        }
                        std::pair<int64_t, std::wstring> candidate{ *stamp, ts->str };
                        if (!earliest || candidate < *earliest)
                        {
                            earliest = std::move(candidate);
                        }
                    }
                    const std::wstring firstTs = earliest ? earliest->second : std::wstring{};
                    if (link.via == L"fork")
                    {
                        return L"forked from session " + std::to_wstring(index) + L" on " + WhenEpoch(link.born); // born = the fork's moment
                    }
                    if (link.via == L"plan")
                    {
                        return L"restarted from session " + std::to_wstring(index) + L" to implement its plan on " + When(firstTs);
                    }
                    return L"started " + When(firstTs);
                }

                // chain_block(): the chain list at the top of a chained document, oldest first.
                std::wstring ChainBlock(const std::vector<ChainLink>& links) const
                {
                    std::vector<std::wstring> rows{ L"**Session chain**, oldest first (the last one is this session):" };
                    if (!links[0].missingParent.empty())
                    {
                        rows.push_back(L"- `" + links[0].missingParent + L"` — its transcript is gone; session 1 was restarted from it to implement its plan");
                    }
                    for (size_t index = 0; index < links.size(); ++index)
                    {
                        const ChainLink& link = links[index];
                        const std::wstring title = link.title.empty() ? std::wstring{} : L" · “" + Flat(link.title, 80) + L"”";
                        const std::wstring self = index + 1 == links.size() ? L" · this session" : L"";
                        rows.push_back(L"- session " + std::to_wstring(index + 1) + L" · `" + link.sessionId + L"`" + title + L" — " + LinkOrigin(links, index) + self);
                    }
                    return Join(rows, L"\n") + L"\n";
                }

                // session_header(): "## Session 2 of 4 · `edd314dd` · forked from session 1 on Sep 23 19:52".
                std::wstring SessionHeader(const std::vector<ChainLink>& links, size_t index) const
                {
                    const std::wstring self = index + 1 == links.size() ? L" · this session" : L"";
                    return L"## Session " + std::to_wstring(index + 1) + L" of " + std::to_wstring(links.size()) + L" · `" + CpPrefix(links[index].sessionId, 8) + L"` · " + LinkOrigin(links, index) + self + L"\n";
                }

                // legend(): the line explaining shortened temp paths ("" when none was shortened). A
                // chain's sessions each wrote into their own temp folder: several prefixes are named by
                // their shared parent plus "<session id>" (or listed).
                std::wstring Legend() const
                {
                    if (_tmpRoots.size() <= 1)
                    {
                        if (_tmpRoot.empty())
                        {
                            return {};
                        }
                        return L"Paths like `scratchpad\\…` and `tasks\\…` are inside the session's temp folder `" + _tmpRoot + L"`.\n";
                    }
                    std::vector<std::wstring> parents; // a set in the reference: only its size and lone member matter
                    for (const auto& root : _tmpRoots)
                    {
                        std::wstring trimmed = root;
                        while (!trimmed.empty() && trimmed.back() == L'\\')
                        {
                            trimmed.pop_back();
                        }
                        const size_t cut = trimmed.rfind(L'\\');
                        std::wstring parent = (cut == std::wstring::npos ? trimmed : trimmed.substr(0, cut)) + L"\\";
                        if (std::find(parents.begin(), parents.end(), parent) == parents.end())
                        {
                            parents.push_back(std::move(parent));
                        }
                    }
                    std::wstring where;
                    if (parents.size() == 1)
                    {
                        where = L"`" + parents[0] + L"<session id>\\`";
                    }
                    else
                    {
                        std::vector<std::wstring> quoted;
                        for (const auto& root : _tmpRoots)
                        {
                            quoted.push_back(L"`" + root + L"`");
                        }
                        where = Join(quoted, L", ");
                    }
                    return L"Paths like `scratchpad\\…` and `tasks\\…` are inside the temp folder of the session that wrote them: " + where + L".\n";
                }
            };

            // render_history(): walks the live branch, parses and renders a history (one transcript,
            // or a merged chain). Throws std::bad_alloc only.
            std::wstring RenderHistory(const History& history, const TurnsOptions& opt, bool utc)
            {
                std::optional<UuidSet> live;
                if (opt.liveBranch)
                {
                    const EdgeMap* extra = history.extraEdges.empty() ? nullptr : &history.extraEdges;
                    live = opt.compactedHistory ? DeepLiveUuids(history.edgeLines, extra) : LiveUuids(history.edgeLines, extra);
                }
                TurnsSession session = ParseTurns(history.lines, opt, live ? &*live : nullptr, history.origins.empty() ? nullptr : &history.origins, history.headId ? &*history.headId : nullptr);
                session.links = &history.links;
                if (opt.agents)
                {
                    AttachAgents(session, history, opt);
                }
                // Every chained session wrote into its own temp folder; a single session keeps its one id.
                std::vector<std::wstring> ids;
                if (!history.links.empty())
                {
                    for (const auto& link : history.links)
                    {
                        ids.push_back(link.sessionId);
                    }
                }
                else
                {
                    ids.push_back(session.sessionId);
                }
                Renderer renderer(opt, std::move(ids), utc);
                return renderer.Document(session);
            }

            // =====================================================================================
            // 8. Conversation upstream — NOT in the reference (Agentmaster: the summary panel's
            //    whole-conversation view, TranscriptTurns.h CollectConversationUpstream)
            // =====================================================================================

            // Turns a resolved chain into the summary's upstream: every EARLIER session's prompts, oldest
            // session first, split at each session's own /compact boundaries. A prompt belongs to the
            // session it FIRST appears in (History::origins — a fork's copies of its parent's lines carry
            // the same uuids, so they count once, for the parent), only lines on the conversation's live
            // branch are listed (the deep walk crosses every compaction and every fork / graft, so a prompt
            // rewound away before the hand-off is dropped), and prompts are recognized + deduplicated per
            // segment by the summary's OWN rule (ProcessInspect.h SummaryPromptOfLine), so an earlier
            // session reads exactly like the current session's Messages list.
            //   history — LoadHistory's merged chain (links oldest first, the head last).
            // Returns the upstream; empty when the chain is the head alone. Throws std::bad_alloc only.
            ConversationUpstream BuildUpstream(const History& history)
            {
                ConversationUpstream up;
                const size_t count = history.links.size();
                if (count < 2)
                {
                    return up; // the transcript continues nothing
                }
                const int head = static_cast<int>(count - 1);
                for (const auto& [uuid, index] : history.origins)
                {
                    if (index < head)
                    {
                        up.uuids.insert(uuid); // owned by an earlier session: the caller drops the head's copies of it
                    }
                }
                const EdgeMap* extra = history.extraEdges.empty() ? nullptr : &history.extraEdges;
                const UuidSet live = DeepLiveUuids(history.edgeLines, extra);
                std::vector<std::vector<ConversationSegment>> perLink(count - 1);
                std::vector<std::unordered_set<std::wstring>> seenText(count - 1); // per link: its CURRENT segment's texts (the analyzer's per-segment dedup)
                for (const Value* line : history.lines)
                {
                    const Value* uuid = JGet(line, L"uuid");
                    if (!IsStr(uuid) || uuid->str.empty())
                    {
                        continue; // a uuid-less bookkeeping line carries no prompt and no boundary
                    }
                    const auto origin = history.origins.find(uuid->str);
                    if (origin == history.origins.end() || origin->second >= head)
                    {
                        continue; // the head's own lines: AnalyzeSessionTranscript lists those (the caller)
                    }
                    if (!live.empty() && live.count(uuid->str) == 0)
                    {
                        continue; // on a branch a rewind abandoned: not part of the conversation
                    }
                    const size_t link = static_cast<size_t>(origin->second);
                    auto& segs = perLink[link];
                    if (segs.empty())
                    {
                        segs.emplace_back();
                    }
                    if (std::wstring label = ::Agentmaster::SummaryCompactionLabelOfLine(*line); !label.empty())
                    {
                        segs.back().label = std::move(label);
                        segs.emplace_back();
                        seenText[link].clear(); // dedup is per segment, like CollectConversationSegments
                        continue;
                    }
                    if (std::wstring prompt = ::Agentmaster::SummaryPromptOfLine(*line); !prompt.empty() && seenText[link].insert(prompt).second)
                    {
                        segs.back().userMsgs.push_back(std::move(prompt));
                        segs.back().uuids.push_back(uuid->str);
                    }
                }
                for (size_t link = 0; link + 1 < count; ++link)
                {
                    auto& segs = perLink[link];
                    // Empty segments between two compactions are noise (the in-file previous segments drop
                    // them too) — but the session's LAST segment always stays, empty or not: it carries
                    // continuedBy, which is what draws the session's separator.
                    std::vector<ConversationSegment> kept;
                    for (size_t i = 0; i < segs.size(); ++i)
                    {
                        if (!segs[i].userMsgs.empty() || i + 1 == segs.size())
                        {
                            kept.push_back(std::move(segs[i]));
                        }
                    }
                    if (kept.empty())
                    {
                        kept.emplace_back(); // nothing of this session is listed, yet it IS a link of the chain
                    }
                    for (auto& seg : kept)
                    {
                        seg.sessionId = history.links[link].sessionId;
                        seg.sessionTitle = history.links[link].title;
                    }
                    kept.back().continuedBy = history.links[link + 1].via; // how the next session continued from this one
                    for (auto& seg : kept)
                    {
                        up.segments.push_back(std::move(seg));
                    }
                }
                return up;
            }

            // How many lines of a transcript's head CollectUpstream's cheap gate reads: the plan-restart
            // link rides the FIRST prompt, at most a few bookkeeping lines in.
            constexpr size_t kUpstreamGateLines = 200;

            // How many results the upstream cache keeps (least recently used out). One entry per session
            // whose summary was shown; a result holds only prompt texts, so the cap bounds memory, not speed.
            constexpr size_t kUpstreamCacheSlots = 32;

            // How long after its creation a head transcript's answer is never cached. Claude writes a new
            // transcript's opening lines — a fork's whole copied context, a session's first prompt — in
            // the moments right after it creates the file, and the summary reloads on every write, so a
            // load can catch the file mid-write: the gate then misses the copy or the plan-restart
            // reference, or the walk sees a partial chain. Cached, that wrong answer would stay for the
            // life of the process (the key cannot tell the partial file from the finished one). Past this
            // age the opening lines are on disk and the answer cannot change any more; until then every
            // reload decides again — a few cheap head reads at most.
            constexpr double kUpstreamSettleSeconds = 10.0;

            // One cached upstream: `key` = transcript identity + its creation time + its recorded link —
            // everything the result depends on that can differ between calls (the earlier sessions' parts
            // end where their successor began, so later growth never changes them).
            struct UpstreamCacheEntry
            {
                std::wstring key;
                ConversationUpstream result;
                uint64_t lastUse = 0;
            };

            // The cache and its in-flight list, guarded by one mutex: a summary panel and a side panel that
            // open the same session at the same moment wait for ONE walk instead of each reading the chain.
            std::mutex g_upstreamMutex;
            std::condition_variable g_upstreamCv;
            std::vector<UpstreamCacheEntry> g_upstreamCache;
            std::vector<std::wstring> g_upstreamInFlight;
            uint64_t g_upstreamUseTick = 0;

            // The body of CollectConversationUpstream (see TranscriptTurns.h): the cache, the cheap gate
            // (walk only when a link can exist: a recorded predecessor, a plan-restart reference in the
            // first prompt, or a copied first line), then the chain walk + BuildUpstream. Only a FINAL
            // answer is cached: not one for a head without its first prompt yet, nor for a head younger
            // than kUpstreamSettleSeconds (it may be mid-write).
            //   transcript — the head transcript; recorded — the lookup (may be empty); bornIn — creation
            //   times (empty = TranscriptFileBirth).
            // Returns the upstream. Throws TurnsReadError when a transcript in the chain can't be read or
            // stat'ed, and std::bad_alloc; nothing is cached on a throw (a later call retries).
            ConversationUpstream CollectUpstream(const fs::path& transcript, const RecordedPredecessorFn& recorded, const TurnsBirthFn& bornIn)
            {
                const TurnsBirthFn born = bornIn ? bornIn : TurnsBirthFn{ [](const fs::path& path) { return TranscriptFileBirth(path); } };
                const auto headBorn = born(transcript);
                if (!headBorn)
                {
                    return {}; // gone or locked: nothing to say, and nothing worth caching
                }
                // The head's recorded link is part of the key: the registry may record it after a first
                // summary load (the /clear pairing lands with the new session's SessionStart).
                std::optional<RecordedPredecessor> rec;
                if (recorded)
                {
                    try
                    {
                        rec = recorded(transcript.stem().wstring());
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"CollectConversationUpstream recorded lookup"); // Rule #18; reads as "none"
                    }
                }
                std::wstring key = PathKey(transcript);
                key += L'|';
                key += std::to_wstring(*headBorn);
                key += L'|';
                if (rec)
                {
                    key += rec->via;
                    key += L':';
                    key += rec->sessionId;
                }
                {
                    std::unique_lock<std::mutex> lock{ g_upstreamMutex };
                    for (;;)
                    {
                        for (auto& entry : g_upstreamCache)
                        {
                            if (entry.key == key)
                            {
                                entry.lastUse = ++g_upstreamUseTick;
                                return entry.result; // a copy: callers own their result
                            }
                        }
                        if (std::find(g_upstreamInFlight.begin(), g_upstreamInFlight.end(), key) == g_upstreamInFlight.end())
                        {
                            g_upstreamInFlight.push_back(key);
                            break; // this call walks
                        }
                        g_upstreamCv.wait(lock); // another call is walking this chain: wait, then re-check the cache
                    }
                }
                // RAII: ALWAYS release the in-flight key and wake the waiters — a throw below must never
                // strand them (they then walk themselves; nothing was cached).
                struct InFlightRelease
                {
                    std::wstring key;
                    ~InFlightRelease()
                    {
                        std::lock_guard<std::mutex> guard{ g_upstreamMutex };
                        g_upstreamInFlight.erase(std::remove(g_upstreamInFlight.begin(), g_upstreamInFlight.end(), key), g_upstreamInFlight.end());
                        g_upstreamCv.notify_all();
                    }
                } release{ key };

                bool walk = rec.has_value();
                // Is the gate's answer FINAL? A recorded link is. A "no link" verdict is final only once
                // the head holds its first prompt: before that the plan-restart reference (which rides
                // the first prompt) may simply not be written yet.
                bool settled = true;
                if (!walk)
                {
                    // The cheap gate: an ordinary session opens with its own first prompt, carries no
                    // plan-restart reference and has no recorded predecessor — it continues nothing, and
                    // must not pay the chain walk (the fork search alone reads the whole project folder).
                    auto headBytes = ReadHeadLines(transcript, kUpstreamGateLines);
                    if (!headBytes)
                    {
                        return {}; // unreadable right now: answer "none", cache nothing (a later call retries)
                    }
                    const std::vector<Value> parsed = ParseTranscriptJsonl(std::move(*headBytes));
                    const Lines lines = PointersTo(parsed);
                    const std::wstring firstPrompt = FirstPromptText(lines);
                    walk = MatchPlanParent(firstPrompt).has_value() || LooksForkCopied(lines, *headBorn);
                    settled = walk || !firstPrompt.empty();
                }
                ConversationUpstream result;
                if (walk)
                {
                    TurnsOptions opt; // LoadHistory reads only `chain` from it
                    opt.chain = true;
                    ChainOptions chainOpts;
                    chainOpts.forkSearchOnlyWhenCopied = true;
                    chainOpts.recorded = recorded ? &recorded : nullptr;
                    const History history = LoadHistory(transcript, opt, born, chainOpts);
                    result = BuildUpstream(history);
                }
                // Cache only a FINAL answer (kUpstreamSettleSeconds): an unsettled gate, or a head still
                // young enough to be mid-write, is answered but not stored — the next reload decides again.
                FILETIME nowFt{};
                ::GetSystemTimePreciseAsFileTime(&nowFt);
                if (!settled || FileTimeToEpochSeconds(nowFt) - *headBorn < kUpstreamSettleSeconds)
                {
                    return result;
                }
                {
                    std::lock_guard<std::mutex> guard{ g_upstreamMutex };
                    UpstreamCacheEntry* slot = nullptr;
                    if (g_upstreamCache.size() < kUpstreamCacheSlots)
                    {
                        slot = &g_upstreamCache.emplace_back();
                    }
                    else
                    {
                        slot = &g_upstreamCache.front(); // evict the least recently used
                        for (auto& entry : g_upstreamCache)
                        {
                            if (entry.lastUse < slot->lastUse)
                            {
                                slot = &entry;
                            }
                        }
                    }
                    slot->key = key;
                    slot->result = result;
                    slot->lastUse = ++g_upstreamUseTick;
                }
                return result;
            }
        }
    }

    TurnsOptions TurnsOptionsForLevel(TurnsLevel level) noexcept
    {
        TurnsOptions opt; // the latest preset (v9) as it stands at its default level
        const int value = static_cast<int>(level);
        const int rank = (value < 0 || value >= kTurnsLevelCount) ? static_cast<int>(TurnsLevel::Default) : value;
        // The reference's LEVELS table: cumulative sets over the eleven LADDER features.
        const bool compact = rank >= static_cast<int>(TurnsLevel::Compact);
        const bool steps = rank >= static_cast<int>(TurnsLevel::Default);
        const bool full = rank >= static_cast<int>(TurnsLevel::Full);
        const bool maximal = rank >= static_cast<int>(TurnsLevel::Max);
        opt.notes = compact;
        opt.compactions = compact;
        opt.answers = compact;
        opt.agents = compact;
        opt.ops = steps;
        opt.narration = steps;
        opt.fullBash = full;
        opt.opOutput = full;
        opt.opDuration = full;
        opt.fullWrite = maximal;
        opt.stats = maximal;
        return opt;
    }

    const wchar_t* TurnsLevelName(TurnsLevel level) noexcept
    {
        switch (level)
        {
        case TurnsLevel::Minimal:
            return L"minimal";
        case TurnsLevel::Compact:
            return L"compact";
        case TurnsLevel::Full:
            return L"full";
        case TurnsLevel::Max:
            return L"max";
        case TurnsLevel::Default:
        default:
            return L"default";
        }
    }

    std::optional<double> TranscriptFileBirth(const std::filesystem::path& path) noexcept
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        {
            return std::nullopt;
        }
        return tt::FileTimeToEpochSeconds(data.ftCreationTime);
    }

    std::optional<int64_t> TranscriptTimestampToUnixMicros(std::wstring_view ts) noexcept
    {
        try
        {
            return tt::AwareTs(ts);
        }
        catch (...)
        {
            // Only an allocation failure can land here (the "Z" replacement copy). Rule #18: logged,
            // and the stamp reads as unknown — what a malformed one reads as anyway.
            LogSwallowedException(L"TranscriptTimestampToUnixMicros");
            return std::nullopt;
        }
    }

    std::vector<json::Value> ParseTranscriptJsonl(std::string bytes)
    {
        // Pasted-image base64 payloads go first: a screenshot-heavy transcript is mostly base64, and
        // widening + parsing it would cost memory and time for text no render ever shows.
        bytes = ScrubLargeBase64Payloads(std::move(bytes));
        std::vector<json::Value> lines;
        const size_t n = bytes.size();
        size_t pos = 0;
        while (pos < n)
        {
            size_t end = pos;
            while (end < n && bytes[end] != '\n' && bytes[end] != '\r')
            {
                ++end;
            }
            if (auto value = tt::ParseJsonlLine(std::string_view{ bytes.data() + pos, end - pos }))
            {
                lines.push_back(std::move(*value));
            }
            if (end + 1 < n && bytes[end] == '\r' && bytes[end + 1] == '\n')
            {
                ++end; // "\r\n" is one line break (bytes.splitlines)
            }
            pos = end + 1;
        }
        return lines;
    }

    std::wstring RenderTranscriptTurns(const std::vector<json::Value>& lines, const TurnsOptions& opts, bool utc) noexcept
    {
        try
        {
            tt::History history;
            history.lines = tt::PointersTo(lines);
            history.edgeLines = history.lines;
            return tt::RenderHistory(history, opts, utc);
        }
        catch (...)
        {
            // Rule #18: never lose a swallowed exception. Recovery = an empty render (the caller copies nothing).
            LogSwallowedException(L"RenderTranscriptTurns");
            return {};
        }
    }

    std::optional<std::wstring> RenderTranscriptTurnsFile(const std::filesystem::path& transcript,
                                                          const TurnsOptions& opts,
                                                          bool utc,
                                                          const TurnsBirthFn& born) noexcept
    {
        try
        {
            const TurnsBirthFn birth = born ? born : TurnsBirthFn{ [](const std::filesystem::path& path) { return TranscriptFileBirth(path); } };
            const tt::History history = tt::LoadHistory(transcript, opts, birth);
            return tt::RenderHistory(history, opts, utc);
        }
        catch (const tt::TurnsReadError&)
        {
            // Expected control flow, not a fault: the transcript (or its creation time) can't be read —
            // gone, locked, or never written. The caller reports "nothing copied" in its own trace.
            return std::nullopt;
        }
        catch (...)
        {
            // Rule #18: never lose a swallowed exception. Recovery = no render (the caller copies nothing).
            LogSwallowedException(L"RenderTranscriptTurnsFile");
            return std::nullopt;
        }
    }

    ConversationUpstream CollectConversationUpstream(const std::filesystem::path& transcript,
                                                     const RecordedPredecessorFn& recorded,
                                                     const TurnsBirthFn& born) noexcept
    {
        try
        {
            return tt::CollectUpstream(transcript, recorded, born);
        }
        catch (const tt::TurnsReadError&)
        {
            // Expected control flow, not a fault: a transcript of the chain (or its creation time) can't be
            // read — gone, locked mid-write, or never written. The summary then shows the session alone,
            // and nothing was cached, so its next reload tries again.
            return {};
        }
        catch (...)
        {
            // Rule #18: never lose a swallowed exception (a std::bad_alloc on a huge chain is the realistic
            // one). Recovery = no upstream: the summary shows the session alone instead of failing.
            LogSwallowedException(L"CollectConversationUpstream");
            return {};
        }
    }

    // =============================================================================================
    // Agentmaster — the summary panel's conversation list (SummaryConversation.h), built from section 8's
    // upstream, and the label its separator rows show. Pure: no I/O; NOT part of the transcript_turns.py port.
    // =============================================================================================

    std::vector<SummaryConversationRow> BuildSummaryConversation(const SessionSummary& a,
                                                                 const ConversationUpstream& up,
                                                                 bool showHistory,
                                                                 const std::function<std::wstring(const std::wstring&)>& titleOf)
    {
        std::vector<SummaryConversationRow> rows;
        // The uuid of entry i of a lockstep list ("" past its end — a defensive bound; the analyzer keeps
        // the lists equally long, but a short list must never index out of range).
        const auto uuidAt = [](const std::vector<std::wstring>& uuids, size_t i) -> std::wstring {
            return i < uuids.size() ? uuids[i] : std::wstring{};
        };
        const auto addPrompt = [&rows](const std::wstring& text, int headIndex) {
            SummaryConversationRow row;
            row.text = text;
            row.headIndex = headIndex;
            rows.push_back(std::move(row));
        };
        if (!showHistory)
        {
            for (size_t i = 0; i < a.userMsgs.size(); ++i)
            {
                addPrompt(a.userMsgs[i], static_cast<int>(i));
            }
            return rows;
        }
        // uuid -> index in a.userMsgs: an earlier session's prompt that this session's current part also
        // holds (a fork's copy) keeps that index for its ▸ jump button.
        std::unordered_map<std::wstring, int> headIndexOf;
        for (size_t i = 0; i < a.userMsgs.size(); ++i)
        {
            if (const std::wstring u = uuidAt(a.userMsgUuids, i); !u.empty())
            {
                headIndexOf.emplace(u, static_cast<int>(i));
            }
        }
        // (1) The earlier sessions, oldest first; a separator closes each one (after its LAST segment).
        for (size_t s = 0; s < up.segments.size(); ++s)
        {
            const auto& seg = up.segments[s];
            for (size_t k = 0; k < seg.userMsgs.size(); ++k)
            {
                const std::wstring u = uuidAt(seg.uuids, k);
                const auto hit = u.empty() ? headIndexOf.end() : headIndexOf.find(u);
                addPrompt(seg.userMsgs[k], hit != headIndexOf.end() ? hit->second : -1);
            }
            const bool lastOfSession = (s + 1 == up.segments.size()) || (up.segments[s + 1].sessionId != seg.sessionId);
            if (lastOfSession && !seg.sessionId.empty())
            {
                SummaryConversationRow sep;
                sep.sessionId = seg.sessionId;
                sep.sessionTitle = titleOf ? titleOf(seg.sessionId) : std::wstring{};
                if (sep.sessionTitle.empty())
                {
                    sep.sessionTitle = seg.sessionTitle; // the transcript's own custom / AI title
                }
                sep.continuedBy = seg.continuedBy;
                rows.push_back(std::move(sep));
            }
        }
        // A prompt an earlier session owns (a fork's copy of its parent's line) is listed above already.
        const auto ownedUpstream = [&up](const std::wstring& u) { return !u.empty() && up.uuids.count(u) != 0; };
        // (2) This session's own parts before its last /compact — no jump index: the analyzer's jump list
        // (a.userMsgs) restarts at each compaction, so these were never in it.
        for (const auto& seg : a.previousSegments)
        {
            for (size_t k = 0; k < seg.userMsgs.size(); ++k)
            {
                if (!ownedUpstream(uuidAt(seg.uuids, k)))
                {
                    addPrompt(seg.userMsgs[k], -1);
                }
            }
        }
        // (3) The current part (the jump list itself).
        for (size_t i = 0; i < a.userMsgs.size(); ++i)
        {
            if (!ownedUpstream(uuidAt(a.userMsgUuids, i)))
            {
                addPrompt(a.userMsgs[i], static_cast<int>(i));
            }
        }
        return rows;
    }

    std::vector<SummaryConversationRow> CodexSummaryConversation(const std::vector<std::wstring>& prompts)
    {
        std::vector<SummaryConversationRow> rows;
        rows.reserve(prompts.size());
        for (const auto& p : prompts)
        {
            SummaryConversationRow row;
            row.text = p;
            rows.push_back(std::move(row));
        }
        return rows;
    }

    std::wstring SummarySeparatorLabel(const SummaryConversationRow& row, size_t idChars)
    {
        if (row.sessionId.empty())
        {
            return {}; // a prompt row: the renderers number it and never draw it as a rule
        }
        std::wstring label;
        // Only a fork names its link on the rule (the user's request). The token is the chain walk's own
        // ChainLink::via ("fork" when ResolveChain's FindForkParent found the parent), compared exactly:
        // an unknown or future token must read as the bare id, never as a guessed word.
        if (row.continuedBy == L"fork")
        {
            label = L"fork ";
        }
        // append(str, pos, n) stops at the end of the id by itself, so an id shorter than idChars shows
        // whole; npos (idChars 0) takes all of it.
        label.append(row.sessionId, 0, idChars == 0 ? std::wstring::npos : idChars);
        return label;
    }
}
