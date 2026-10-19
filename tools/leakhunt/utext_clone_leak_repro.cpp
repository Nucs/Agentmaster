// Agentmaster leak hunt: utext_clone_leak_repro - reproduce (and prove the fix of) the 144-byte UText leak in
// src/buffer/out/UTextAdapter.cpp, outside the app, against the SAME system ICU (icu.dll) the terminal uses.
//
// The bug (found in production's dump, 2026-10-19: 131,622 leaked UTexts, +27,864 in 53 minutes): the adapter's
// clone callback does `dest = utext_setup(dest, 0, status); memcpy(dest, src, sizeof(UText));`. utext_setup marks
// a heap clone UTEXT_HEAP_ALLOCATED | UTEXT_OPEN; the memcpy then overwrites `flags` with the SOURCE's flags (a
// stack UText: UTEXT_OPEN only). When ICU later closes the clone (uregex_close -> ~RegexMatcher -> utext_close),
// utext_close runs the provider's close, NULLs pFuncs, and frees the struct only if UTEXT_HEAP_ALLOCATED is set -
// which the memcpy erased. ICU's own shallowTextClone saves and restores `flags` and `pExtra` around its memcpy
// for exactly this reason. Every URL scan (Terminal::_getPatterns) and every TextBuffer::SearchText hands a
// UText to a fresh regex matcher and so leaks one 144-byte struct.
//
// What this program does: build a trivial text provider over a static string, then N times do what
// Terminal::_getPatterns does - set up a stack UText, uregex_clone an interned pattern, uregex_setUText,
// uregex_find, uregex_close, utext_close - once with the adapter's current clone ("buggy") and once with the
// fixed clone that keeps dest's flags / pExtra / extraSize ("fixed"), and print the process's private-byte
// growth for each. Expected: buggy grows ~N x 144 bytes (plus heap overhead), fixed stays flat.
//
// Build (VS 2022 x64 developer prompt): cl /nologo /EHsc /O2 /W4 /std:c++20 utext_clone_leak_repro.cpp /link icu.lib psapi.lib
// Usage:   utext_clone_leak_repro [iterations=200000]
// Exit code 0 when the measurement matches the expectation (buggy leaks, fixed does not), 1 otherwise.

#include <windows.h>
#include <psapi.h>
#include <icu.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator> // std::size

namespace
{
    /// The text every UText in this program wraps - long enough to hold a URL match, like a terminal row.
    constexpr wchar_t kText[] = L"see https://github.com/microsoft/terminal/issues/1 for details";
    constexpr int64_t kLength = static_cast<int64_t>(std::size(kText) - 1);

    /// The provider's access callback: the whole text is one chunk, so every index is answered at once.
    /// @param ut the UText. @param index the native index ICU wants. @param forward the direction (unused).
    /// @returns TRUE when the index lies inside the text (always the case for this single chunk).
    /// @remarks Never throws; ICU calls it on its own threads' stacks.
    UBool U_CALLCONV Access(UText* ut, int64_t index, UBool /*forward*/) noexcept
    {
        ut->chunkContents = reinterpret_cast<const UChar*>(kText);
        ut->chunkNativeStart = 0;
        ut->chunkNativeLimit = kLength;
        ut->chunkLength = static_cast<int32_t>(kLength);
        ut->nativeIndexingLimit = static_cast<int32_t>(kLength);
        ut->chunkOffset = static_cast<int32_t>(index < 0 ? 0 : (index > kLength ? kLength : index));
        return index >= 0 && index <= kLength;
    }

    /// The provider's length callback. @param ut unused. @returns the text's length in UTF-16 units. Never throws.
    int64_t U_CALLCONV NativeLength(UText* /*ut*/) noexcept
    {
        return kLength;
    }

    /// The provider's close callback - nothing to release here (the adapter releases its RefcountBuffer).
    /// @param ut unused. Never throws.
    void U_CALLCONV Close(UText* /*ut*/) noexcept
    {
    }

    /// The adapter's CURRENT clone (UTextAdapter.cpp utextClone): setup, then a full memcpy over dest.
    /// @param dest the UText to fill, or nullptr for ICU to heap-allocate one. @param src the UText cloned.
    /// @param deep unsupported (as in the adapter). @param status ICU's error out-param.
    /// @returns dest (possibly newly heap-allocated by utext_setup). Never throws.
    UText* U_CALLCONV CloneBuggy(UText* dest, const UText* src, UBool deep, UErrorCode* status) noexcept
    {
        if (deep)
        {
            *status = U_UNSUPPORTED_ERROR;
            return dest;
        }
        dest = utext_setup(dest, 0, status);
        if (*status > U_ZERO_ERROR)
        {
            return dest;
        }
        memcpy(dest, src, sizeof(UText));
        return dest;
    }

    /// The FIXED clone: the same memcpy, but dest keeps the flags / pExtra / extraSize utext_setup gave it
    /// (UTEXT_HEAP_ALLOCATED among them), exactly as ICU's shallowTextClone does.
    /// @param dest the UText to fill, or nullptr. @param src the UText cloned. @param deep unsupported.
    /// @param status ICU's error out-param. @returns dest. Never throws.
    UText* U_CALLCONV CloneFixed(UText* dest, const UText* src, UBool deep, UErrorCode* status) noexcept
    {
        if (deep)
        {
            *status = U_UNSUPPORTED_ERROR;
            return dest;
        }
        dest = utext_setup(dest, 0, status);
        if (*status > U_ZERO_ERROR)
        {
            return dest;
        }
        const auto flags = dest->flags;
        const auto extra = dest->pExtra;
        const auto extraSize = dest->extraSize;
        memcpy(dest, src, sizeof(UText));
        dest->flags = flags;
        dest->pExtra = extra;
        dest->extraSize = extraSize;
        return dest;
    }

    /// The provider tables, one per clone variant (ICU calls through pFuncs only). Designated initializers, as in
    /// UTextAdapter.cpp: UTextFuncs has extract / replace / copy / map slots between access and close.
    constexpr UTextFuncs kBuggyFuncs{ .tableSize = sizeof(UTextFuncs), .clone = CloneBuggy, .nativeLength = NativeLength, .access = Access, .close = Close };
    constexpr UTextFuncs kFixedFuncs{ .tableSize = sizeof(UTextFuncs), .clone = CloneFixed, .nativeLength = NativeLength, .access = Access, .close = Close };

    /// The process's commit charge (private bytes) right now. @returns bytes, 0 when the query fails. Never throws.
    unsigned long long PrivateBytes() noexcept
    {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
        return pmc.PrivateUsage;
    }

    /// Run the _getPatterns sequence n times with one provider table and report the private-byte growth.
    /// @param funcs the provider table (buggy or fixed clone). @param proto the interned pattern to clone.
    /// @param n the iteration count. @param matches receives how many iterations found the URL (a sanity check).
    /// @returns the growth in bytes (signed). Never throws.
    long long Measure(const UTextFuncs* funcs, const URegularExpression* proto, int n, int& matches) noexcept
    {
        matches = 0;
        const auto before = PrivateBytes();
        for (int i = 0; i < n; ++i)
        {
            UErrorCode status = U_ZERO_ERROR;
            UText ut = UTEXT_INITIALIZER;
            utext_setup(&ut, 0, &status);
            ut.providerProperties = (1 << UTEXT_PROVIDER_LENGTH_IS_EXPENSIVE) | (1 << UTEXT_PROVIDER_STABLE_CHUNKS);
            ut.pFuncs = funcs;
            Access(&ut, 0, true);
            URegularExpression* re = uregex_clone(proto, &status);
            uregex_setUText(re, &ut, &status);
            if (uregex_find(re, -1, &status))
            {
                ++matches;
            }
            uregex_close(re);
            utext_close(&ut);
        }
        return static_cast<long long>(PrivateBytes()) - static_cast<long long>(before);
    }
}

/// Entry point: compile the URL pattern the terminal uses, measure both clone variants, judge the result.
/// @param argc the argument count. @param argv [iterations].
/// @returns 0 when the buggy clone leaks (>= 100 bytes per iteration) and the fixed one does not (< 10), else 1.
/// @remarks Never throws.
int wmain(int argc, wchar_t** argv)
{
    const int n = argc > 1 ? _wtoi(argv[1]) : 200000;
    // The exact pattern Terminal::_getPatterns scans for (src/cascadia/TerminalCore/Terminal.cpp).
    const wchar_t* pattern = LR"(\b(?:https?|ftp|file)://[-A-Za-z0-9+&@#/%?=~_|$!:,.;]*[A-Za-z0-9+&@#/%=~_|$])";
    UErrorCode status = U_ZERO_ERROR;
    URegularExpression* proto = uregex_open(reinterpret_cast<const UChar*>(pattern), -1, 0, nullptr, &status);
    if (U_FAILURE(status) || !proto)
    {
        wprintf(L"uregex_open failed: %d\n", status);
        return 1;
    }
    int warmMatches = 0;
    Measure(&kFixedFuncs, proto, 2000, warmMatches); // warm ICU's caches so they don't count against either run
    int buggyMatches = 0, fixedMatches = 0;
    const long long buggy = Measure(&kBuggyFuncs, proto, n, buggyMatches);
    const long long fixed = Measure(&kFixedFuncs, proto, n, fixedMatches);
    uregex_close(proto);
    wprintf(L"iterations %d: buggy clone  +%lld bytes (%.1f bytes/iteration, %d matches)\n", n, buggy, double(buggy) / n, buggyMatches);
    wprintf(L"iterations %d: fixed clone  %+lld bytes (%.1f bytes/iteration, %d matches)\n", n, fixed, double(fixed) / n, fixedMatches);
    const bool ok = buggy >= 100LL * n && fixed < 10LL * n && buggyMatches == n && fixedMatches == n;
    wprintf(L"%ls\n", ok ? L"REPRODUCED: the current clone leaks ~144 B per scan; the fixed clone does not" : L"NOT reproduced as expected");
    return ok ? 0 : 1;
}
