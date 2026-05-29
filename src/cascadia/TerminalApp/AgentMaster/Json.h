// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Agentmaster — a tiny, dependency-free JSON value + recursive-descent parser/printer for
// persistence (DESIGN §13). The engine deliberately keeps no JSON dependency on the hot
// path (HooksBridge uses a flat wire); persistence is cold and benefits from a readable,
// inspectable format. Pure C++ + STL, so it is fully unit-tested standalone.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Agentmaster::json
{
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

            Value Number()
            {
                const size_t start = i;
                while (i < s.size() && (::iswdigit(s[i]) || s[i] == L'-' || s[i] == L'+' || s[i] == L'.' || s[i] == L'e' || s[i] == L'E'))
                {
                    ++i;
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
                if (c == L't' || c == L'f' || c == L'n')
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

    inline std::optional<Value> Parse(std::wstring_view text)
    {
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
