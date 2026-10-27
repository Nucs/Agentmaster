// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — VoiceSpellWords: the app's answer to VoiceVocabulary.h's dictionary lookup (WordTest) — Windows' own en-US
// spell checker, the Spell Checking API of spellcheck.h (ISpellCheckerFactory → ISpellChecker::Check; Windows 8 and later,
// in the box: nothing to ship). The POC asks the very same API through comtypes (tools\voice-poc\vocab.py), so the POC,
// its goldens and the app agree on every word. Plain C++ + COM, header-only (no WinRT, no PCH); links ole32.
//
// ⚠ COM: the factory is created on the CALLING thread, which must have COM initialized (the service's voice thread
// initializes the multithreaded apartment for its whole life, Messaging.Voice.cpp). One checker per thread (thread_local):
// the API documents no thread affinity guarantee, and a checker is cheap to make (~ms).
// ⚠ The checker skips ALL-CAPS words (an acronym is never flagged) — PlainWords asks lowercase or title-case spellings only.

#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <spellcheck.h>

#include <string>
#include <string_view>

#include "VoiceVocabulary.h"

#pragma comment(lib, "ole32.lib")

namespace Agentmaster::Voice
{
    namespace detail
    {
        /// <summary>
        /// The calling thread's en-US checker slot: made on first use, or released when `release` is set (then the next use
        /// makes a new one). Null when the API or the language is missing (Windows N editions without the language pack): the
        /// WordTest then knows every spelling — the safe answer, the dictionary pass rewrites nothing it would have needed the
        /// test for. ⚠ Owners release it before CoUninitialize (ReleaseThreadSpellChecker); the thread_local's destructor is
        /// only the last resort for a thread that never initialized COM itself.
        /// </summary>
        /// <param name="release">Release the checker instead of returning it.</param>
        /// <returns>The checker (owned by the thread), or null (always null when releasing).</returns>
        inline ISpellChecker* ThreadSpellCheckerSlot(bool release) noexcept
        {
            struct Holder
            {
                ISpellChecker* checker{ nullptr };
                bool tried{ false };
                ~Holder()
                {
                    if (checker)
                    {
                        checker->Release();
                    }
                }
            };
            thread_local Holder holder;
            if (release)
            {
                if (holder.checker)
                {
                    holder.checker->Release();
                    holder.checker = nullptr;
                }
                holder.tried = false;
                return nullptr;
            }
            if (!holder.tried)
            {
                holder.tried = true;
                ISpellCheckerFactory* factory = nullptr;
                if (SUCCEEDED(::CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) && factory)
                {
                    BOOL supported = FALSE;
                    if (SUCCEEDED(factory->IsSupported(L"en-US", &supported)) && supported)
                    {
                        factory->CreateSpellChecker(L"en-US", &holder.checker);
                    }
                    factory->Release();
                }
            }
            return holder.checker;
        }
    }

    /// <summary>
    /// Releases the CALLING thread's checker NOW. ⚠ A thread that initialized COM must call it BEFORE CoUninitialize: the
    /// thread_local's own destructor runs at thread exit, after CoUninitialize has unloaded the checker's in-process server,
    /// and its Release then jumps into unmapped code (measured: an access violation at the probe's exit, 2026-10-27). A later
    /// lookup on the same thread makes a new checker (or, with COM gone, none: every spelling then reads as known).
    /// </summary>
    inline void ReleaseThreadSpellChecker() noexcept
    {
        detail::ThreadSpellCheckerSlot(true);
    }

    /// <summary>
    /// The WordTest over the spell checker: true when en-US knows this exact spelling. Asked only about short alphabetic
    /// words (PlainWords caches each answer), so each call is one Check of one word.
    /// </summary>
    /// <returns>The test (it reads the CALLING thread's checker each time, so it may be built on one thread and used on the
    /// thread that initialized COM).</returns>
    inline WordTest SpellCheckerWordTest()
    {
        return [](std::wstring_view spelling) -> bool {
            ISpellChecker* checker = detail::ThreadSpellCheckerSlot(false);
            if (!checker)
            {
                return true; // no dictionary: "known" (never rewrite)
            }
            const std::wstring word{ spelling };
            IEnumSpellingError* errors = nullptr;
            if (FAILED(checker->Check(word.c_str(), &errors)) || !errors)
            {
                return true;
            }
            ISpellingError* first = nullptr;
            // Next gives S_FALSE and no error when the word is spelled right; any error object means it is not a known spelling.
            const HRESULT hr = errors->Next(&first);
            const bool known = hr != S_OK || first == nullptr;
            if (first)
            {
                first->Release();
            }
            errors->Release();
            return known;
        };
    }
}
