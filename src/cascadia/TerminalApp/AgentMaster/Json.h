// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — a tiny, dependency-free JSON value + recursive-descent parser/printer for
// persistence (DESIGN §13). The engine deliberately keeps no JSON dependency on the hot
// path (HooksBridge uses a flat wire); persistence is cold and benefits from a readable,
// inspectable format. Pure C++ + STL, so it is fully unit-tested standalone.

#pragma once

#include <cmath> // std::isfinite (Dump: JSON has no NaN / infinity)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Agentmaster::json
{
    struct Value;

    namespace detail
    {
        /// <summary>
        /// Agentmaster (PERSISTENCE.md §17, "what a build understands"): an observer of member lookups. While one is
        /// installed on a thread (StateCompat.h ObserveLookups), every Value::Find on that thread — and so every StrAt /
        /// NumAt / BoolAt / I64At / U32At — reports the object it was called on and the key it was asked for. The state-file
        /// residue installs one around a loader's run over a file: a member the loader LOOKED AT is one this build
        /// understands, even when it then leaves it out of what it writes (a consumed migration key, whose presence would
        /// re-run its migration at every load), so only the members it never asked for are kept for a newer build.
        /// </summary>
        /// <remarks>Lookups made by iterating `members` directly are not reported: such a loader reads the whole map, and
        /// what it then does not write is treated as unknown (kept).</remarks>
        struct FindObserver
        {
            /// <summary>Called by Value::Find on the observing thread, before the lookup.</summary>
            /// <param name="object">The value Find was called on (normally an object of the document being read).</param>
            /// <param name="key">The key it was asked for (valid only during the call: copy it to keep it).</param>
            virtual void OnFind(const Value& object, std::wstring_view key) noexcept = 0;

        protected:
            ~FindObserver() = default;
        };

        /// <summary>The thread's installed lookup observer; null (the normal state) outside an observed loader run, so
        /// the cost of a Find is one thread-local load.</summary>
        inline thread_local FindObserver* t_findObserver = nullptr;
    }

    struct Value
    {
        enum class Type
        {
            Null,
            Bool,
            Num,
            Str,
            Arr,
            Obj
        };

        Type type{ Type::Null };
        bool boolean{ false };
        double num{ 0 };
        std::wstring str;
        std::vector<Value> arr;
        std::vector<std::pair<std::wstring, Value>> members; // insertion-ordered object

        static Value MkStr(std::wstring s)
        {
            Value v;
            v.type = Type::Str;
            v.str = std::move(s);
            return v;
        }
        static Value MkNum(double n)
        {
            Value v;
            v.type = Type::Num;
            v.num = n;
            return v;
        }
        static Value MkBool(bool x)
        {
            Value v;
            v.type = Type::Bool;
            v.boolean = x;
            return v;
        }
        static Value MkObj()
        {
            Value v;
            v.type = Type::Obj;
            return v;
        }
        static Value MkArr()
        {
            Value v;
            v.type = Type::Arr;
            return v;
        }

        void Set(const std::wstring& key, Value v) { members.emplace_back(key, std::move(v)); }
        void Push(Value v) { arr.push_back(std::move(v)); }

        const Value* Find(std::wstring_view key) const
        {
            // Agentmaster (PERSISTENCE.md §17): an observed loader run records what it asked for (detail::FindObserver).
            if (auto* const observer = detail::t_findObserver)
            {
                observer->OnFind(*this, key);
            }
            for (const auto& [k, v] : members)
            {
                if (k == key)
                {
                    return &v;
                }
            }
            return nullptr;
        }

        std::wstring AsStr(std::wstring_view def = L"") const { return type == Type::Str ? str : std::wstring{ def }; }
        double AsNum(double def = 0) const { return type == Type::Num ? num : def; }
        int64_t AsI64(int64_t def = 0) const { return type == Type::Num ? static_cast<int64_t>(num) : def; }
        uint32_t AsU32(uint32_t def = 0) const { return type == Type::Num ? static_cast<uint32_t>(num) : def; }
        bool AsBool(bool def = false) const { return type == Type::Bool ? boolean : def; }

        // Convenience: a member's string / number / bool with a default.
        std::wstring StrAt(std::wstring_view key, std::wstring_view def = L"") const
        {
            const auto* m = Find(key);
            return m ? m->AsStr(def) : std::wstring{ def };
        }
        int64_t I64At(std::wstring_view key, int64_t def = 0) const
        {
            const auto* m = Find(key);
            return m ? m->AsI64(def) : def;
        }
        uint32_t U32At(std::wstring_view key, uint32_t def = 0) const
        {
            const auto* m = Find(key);
            return m ? m->AsU32(def) : def;
        }
        bool BoolAt(std::wstring_view key, bool def = false) const
        {
            const auto* m = Find(key);
            return m ? m->AsBool(def) : def;
        }
        double NumAt(std::wstring_view key, double def = 0) const
        {
            const auto* m = Find(key);
            return m ? m->AsNum(def) : def;
        }
    };

    namespace detail
    {
        struct Parser
        {
            std::wstring_view s;
            size_t i{ 0 };
            bool ok{ true };

            void Ws()
            {
                while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\n' || s[i] == L'\r'))
                {
                    ++i;
                }
            }

            std::wstring Str()
            {
                std::wstring out;
                ++i; // opening quote
                while (i < s.size())
                {
                    const wchar_t c = s[i++];
                    if (c == L'"')
                    {
                        return out;
                    }
                    if (c == L'\\' && i < s.size())
                    {
                        const wchar_t e = s[i++];
                        switch (e)
                        {
                        case L'"':
                            out += L'"';
                            break;
                        case L'\\':
                            out += L'\\';
                            break;
                        case L'/':
                            out += L'/';
                            break;
                        case L'n':
                            out += L'\n';
                            break;
                        case L't':
                            out += L'\t';
                            break;
                        case L'r':
                            out += L'\r';
                            break;
                        case L'b':
                            out += L'\b';
                            break;
                        case L'f':
                            out += L'\f';
                            break;
                        case L'u':
                            if (i + 4 <= s.size())
                            {
                                const std::wstring hex{ s.substr(i, 4) };
                                out += static_cast<wchar_t>(::wcstoul(hex.c_str(), nullptr, 16));
                                i += 4;
                            }
                            break;
                        default:
                            out += e;
                            break;
                        }
                    }
                    else
                    {
                        out += c;
                    }
                }
                ok = false; // unterminated
                return out;
            }

            // Agentmaster (PERSISTENCE.md "Unreadable state files"): consume a NON-FINITE number token at `i` — the
            // spellings MSVC's printf gives a non-finite double ("nan", "-nan(ind)", "inf", "-inf") and the ones
            // Python's json module writes ("NaN", "Infinity", "-Infinity"). JSON has no such token, yet Dump wrote
            // "nan" / "inf" for a non-finite double until 2026-10-26, and ONE such field made the whole document
            // unparseable: every loader fails open, so a settings.json or window record holding one read as "nothing
            // saved" and the next save wrote that over the file. The caller reads the token as null, so that field
            // takes its default and every other field survives.
            //   returns - true and `i` past the token (a sign and MSVC's "(ind)" / "(snan)" payload tag included);
            //             false and `i` unchanged when the text at `i` is not one of those spellings.
            // Never throws (wstring_view::compare cannot go out of range here: the offset never passes size()).
            bool NonFinite()
            {
                size_t k = i;
                if (k < s.size() && (s[k] == L'-' || s[k] == L'+'))
                {
                    ++k;
                }
                size_t len = 0;
                if (s.compare(k, 8, L"Infinity") == 0)
                {
                    len = 8;
                }
                else if (s.compare(k, 3, L"inf") == 0 || s.compare(k, 3, L"NaN") == 0 || s.compare(k, 3, L"nan") == 0)
                {
                    len = 3;
                }
                else
                {
                    return false;
                }
                k += len;
                // MSVC tags a NaN with its kind: "-nan(ind)", "nan(snan)". A short run of letters in parentheses is
                // taken with it; anything else after the word is left for the caller to judge.
                if (k < s.size() && s[k] == L'(')
                {
                    size_t close = k + 1;
                    while (close < s.size() && close - k <= 8 && ::iswalpha(s[close]))
                    {
                        ++close;
                    }
                    if (close < s.size() && s[close] == L')')
                    {
                        k = close + 1;
                    }
                }
                i = k;
                return true;
            }

            Value Number()
            {
                // A non-finite spelling ("-inf", "-nan(ind)", "Infinity") reads as null, never as half a number.
                if (NonFinite())
                {
                    return Value{};
                }
                const size_t start = i;
                while (i < s.size() && (::iswdigit(s[i]) || s[i] == L'-' || s[i] == L'+' || s[i] == L'.' || s[i] == L'e' || s[i] == L'E'))
                {
                    ++i;
                }
                // Agentmaster (PERSISTENCE.md "Unreadable state files"): Val() sends every character it does not
                // recognize here, so a value that starts with none of a number's characters is NOT a number. It used
                // to read as 0 with the parse still "ok": at the root, a file of NUL bytes (what a power loss can leave)
                // or one that starts with garbage parsed as the number 0, and a loader looking for an object then
                // silently read "nothing saved". Inside an object or array the next separator check failed anyway, so
                // only the root changes: such text is now a failed parse, which every caller already handles.
                if (i == start)
                {
                    ok = false;
                    return Value{};
                }
                const std::wstring tok{ s.substr(start, i - start) };
                return Value::MkNum(::wcstod(tok.c_str(), nullptr));
            }

            Value Literal()
            {
                if (s.compare(i, 4, L"true") == 0)
                {
                    i += 4;
                    return Value::MkBool(true);
                }
                if (s.compare(i, 5, L"false") == 0)
                {
                    i += 5;
                    return Value::MkBool(false);
                }
                if (s.compare(i, 4, L"null") == 0)
                {
                    i += 4;
                    return Value{};
                }
                // "nan", "NaN", "inf", "Infinity": a non-finite number an older Dump (or Python) wrote — null.
                if (NonFinite())
                {
                    return Value{};
                }
                ok = false;
                return Value{};
            }

            Value Arr()
            {
                Value v = Value::MkArr();
                ++i; // [
                Ws();
                if (i < s.size() && s[i] == L']')
                {
                    ++i;
                    return v;
                }
                for (;;)
                {
                    v.arr.push_back(Val());
                    Ws();
                    if (i < s.size() && s[i] == L',')
                    {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == L']')
                    {
                        ++i;
                        break;
                    }
                    ok = false;
                    break;
                }
                return v;
            }

            Value Obj()
            {
                Value v = Value::MkObj();
                ++i; // {
                Ws();
                if (i < s.size() && s[i] == L'}')
                {
                    ++i;
                    return v;
                }
                for (;;)
                {
                    Ws();
                    if (i >= s.size() || s[i] != L'"')
                    {
                        ok = false;
                        break;
                    }
                    std::wstring key = Str();
                    Ws();
                    if (i < s.size() && s[i] == L':')
                    {
                        ++i;
                    }
                    else
                    {
                        ok = false;
                        break;
                    }
                    v.members.emplace_back(std::move(key), Val());
                    Ws();
                    if (i < s.size() && s[i] == L',')
                    {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == L'}')
                    {
                        ++i;
                        break;
                    }
                    ok = false;
                    break;
                }
                return v;
            }

            Value Val()
            {
                Ws();
                if (i >= s.size())
                {
                    ok = false;
                    return Value{};
                }
                const wchar_t c = s[i];
                if (c == L'{')
                {
                    return Obj();
                }
                if (c == L'[')
                {
                    return Arr();
                }
                if (c == L'"')
                {
                    return Value::MkStr(Str());
                }
                // 'i' / 'I' / 'N' start no JSON value, but they start "inf" / "Infinity" / "NaN" (Literal reads those
                // as null; NonFinite says why).
                if (c == L't' || c == L'f' || c == L'n' || c == L'i' || c == L'I' || c == L'N')
                {
                    return Literal();
                }
                return Number();
            }
        };

        inline void Escape(const std::wstring& s, std::wstring& out)
        {
            out += L'"';
            for (const wchar_t c : s)
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
                        ::swprintf(buf, 8, L"\\u%04x", static_cast<unsigned>(c));
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
        }

        inline void Dump(const Value& v, std::wstring& out)
        {
            switch (v.type)
            {
            case Value::Type::Null:
                out += L"null";
                break;
            case Value::Type::Bool:
                out += v.boolean ? L"true" : L"false";
                break;
            case Value::Type::Num:
            {
                // Agentmaster (PERSISTENCE.md "Unreadable state files"): JSON has no NaN or infinity, and "%g" spells
                // them "nan" / "-nan(ind)" / "inf" — tokens that made the WHOLE document unreadable on the next start
                // (Parser::NonFinite). Write null instead: the reader then takes that field's default and nothing else.
                if (!std::isfinite(v.num))
                {
                    out += L"null";
                    break;
                }
                wchar_t buf[32];
                if (v.num == static_cast<double>(static_cast<int64_t>(v.num)))
                {
                    ::swprintf(buf, 32, L"%lld", static_cast<long long>(static_cast<int64_t>(v.num)));
                }
                else
                {
                    ::swprintf(buf, 32, L"%g", v.num);
                }
                out += buf;
                break;
            }
            case Value::Type::Str:
                Escape(v.str, out);
                break;
            case Value::Type::Arr:
                out += L'[';
                for (size_t k = 0; k < v.arr.size(); ++k)
                {
                    if (k)
                    {
                        out += L',';
                    }
                    Dump(v.arr[k], out);
                }
                out += L']';
                break;
            case Value::Type::Obj:
                out += L'{';
                for (size_t k = 0; k < v.members.size(); ++k)
                {
                    if (k)
                    {
                        out += L',';
                    }
                    Escape(v.members[k].first, out);
                    out += L':';
                    Dump(v.members[k].second, out);
                }
                out += L'}';
                break;
            }
        }
    }

    // Parse one JSON document. Lenient by design where it costs nothing (an unknown escape keeps its character, text
    // after the document is ignored), strict where leniency lost data (PERSISTENCE.md "Unreadable state files").
    //   text    - the document. A leading U+FEFF (a UTF-8 BOM an editor or Windows PowerShell 5.1's
    //             "Set-Content -Encoding UTF8" put there, decoded by MultiByteToWideChar) is skipped: it used to make
    //             the whole file parse as the number 0, so a sessions.json saved with one loaded as an empty fleet.
    //   returns - the root value; nullopt for text that is not a JSON value (empty, torn, garbage, NUL-filled).
    // Throws std::bad_alloc only.
    inline std::optional<Value> Parse(std::wstring_view text)
    {
        if (!text.empty() && text.front() == static_cast<wchar_t>(0xFEFF))
        {
            text.remove_prefix(1);
        }
        detail::Parser p{ text };
        Value v = p.Val();
        if (!p.ok)
        {
            return std::nullopt;
        }
        return v;
    }

    inline std::wstring Dump(const Value& v)
    {
        std::wstring out;
        detail::Dump(v, out);
        return out;
    }
}
