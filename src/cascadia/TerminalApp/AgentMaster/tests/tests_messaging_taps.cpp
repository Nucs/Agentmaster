// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.28: a tapped Telegram button stays BUSY while the bot works on what it asked for (the user's
// rule of 2026-10-19: "buttons we click on should look 'active' as long as they are waiting for serverside to react"). Telegram
// keeps a tapped button's loading indicator until the bot answers the tap, so the service now HOLDS that answer while the tap's
// work runs. This file drives the mechanism of MessagingTaps.h — std only, no service — with a fake sink:
//   1. the texts: the toast's cut (never inside a surrogate pair), the answer's text (the handler's toast, the "still working"
//      note at the hold's limit, nothing), the log's label of a button;
//   2. one tap: its deadline, its single answer, its toast;
//   3. the holds: the sink is told exactly once, when the LAST hold goes, and never through a sink that is gone;
//   4. the scopes: nesting, one tap counted once, another thread never sees them, an answered tap gets no new hold;
//   5. the whole path as MessagingService runs it — a handler's scope, two items, a callback that queues a fallback — told once,
//      after the last of them.
// The service's half (the send loop, the card's next change, the window replies, the launches) needs Telegram and a window: the
// live check is the dev bot (MESSAGING.md §21.28 "Check after the build").

#include "m5_tests.h"

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../MessagingTaps.h"

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    /// <summary>
    /// A sink that counts the calls it gets and remembers the last tap: what MessagingService::OnTapHoldsGone would queue for its
    /// send loop. Single-threaded use in these tests (the counter is atomic only for the thread test).
    /// </summary>
    class CountingSink final : public TapReleaseSink
    {
    public:
        std::atomic<int> calls{ 0 };
        std::shared_ptr<TapAnswer> lastTap;

        /// <summary>Counts the call and keeps the tap (the tests read both).</summary>
        /// <param name="tap">The tap whose last hold went.</param>
        void OnTapHoldsGone(const std::shared_ptr<TapAnswer>& tap) noexcept override
        {
            ++calls;
            lastTap = tap;
        }
    };

    /// <summary>A fake outbox item: what MessagingService::OutItem keeps of a tap — its holds.</summary>
    struct FakeItem
    {
        std::vector<TapHoldRef> tapHolds;
    };

    /// <summary>
    /// What MessagingService::_enqueue does with a new item inside a scope: one hold per unanswered tap of the calling thread's scope.
    /// </summary>
    /// <param name="sink">The sink the holds tell.</param>
    /// <returns>The item, holding the scope's taps.</returns>
    FakeItem Enqueue(const std::shared_ptr<CountingSink>& sink)
    {
        FakeItem item;
        item.tapHolds = HoldTapsOfScope(std::weak_ptr<TapReleaseSink>{ sink });
        return item;
    }
}

void TestMessagingTaps()
{
    // ASCII only, like the other groups' headers: the console's code page would garble a section sign.
    std::wprintf(L"== TestMessagingTaps (MESSAGING.md 21.28: a tapped button stays busy while its work runs) ==\n");

    // ===== 1. the texts =====
    {
        // The limit: under Telegram's ~15 s (measured by others), with room for the update's way in, the send loop's pass and the gap.
        CHECK(kTapAnswerHoldMaxMs > 0 && kTapAnswerHoldMaxMs <= 10 * 1000, "the hold's limit stays well under Telegram's ~15 s");
        CHECK(std::wstring{ kTapStillWorkingToast }.size() <= kTapToastMaxChars, "the still-working toast fits a toast");

        CHECK(TapToastCut(L"short") == L"short", "a short toast is kept whole");
        const std::wstring longText(250, L'a');
        CHECK(TapToastCut(longText).size() == kTapToastMaxChars, "a long toast is cut to the limit");
        // An emoji (a surrogate pair) right across the cut: the pair goes whole, the cut moves one unit back.
        std::wstring pair(kTapToastMaxChars - 1, L'b');
        pair += L"\U0001F5BC"; // two UTF-16 units: the high one at index 189, the low one at 190
        pair += L"tail";
        const std::wstring cut = TapToastCut(pair);
        CHECK(cut.size() == kTapToastMaxChars - 1, "a cut never splits a surrogate pair");
        CHECK(!cut.empty() && !(cut.back() >= 0xD800 && cut.back() <= 0xDBFF), "the cut toast ends without a lone high surrogate");
        // A pair that fits wholly before the cut is kept.
        std::wstring fits(kTapToastMaxChars - 2, L'c');
        fits += L"\U0001F5BC";
        fits += L"more";
        CHECK(TapToastCut(fits).size() == kTapToastMaxChars, "a pair that ends at the limit is kept whole");

        CHECK(TapAnswerText(L"Restarting it", false) == L"Restarting it", "the handler's toast is the answer's text");
        CHECK(TapAnswerText(L"Restarting it", true) == L"Restarting it", "the handler's toast wins at the hold's limit too");
        CHECK(TapAnswerText(L"", true) == kTapStillWorkingToast, "no toast at the hold's limit with work going on: the still-working note");
        CHECK(TapAnswerText(L"", false).empty(), "no toast once the work is in: no text (the chat shows what happened)");
        CHECK(TapAnswerText(longText, false).size() == kTapToastMaxChars, "the answer's text is cut like a toast");

        CHECK(TapAnswerLogLabel(L"c:session:0123abcd-4567-89ef-0123-456789abcdef") == L"c:session", "a card verb's label drops the session id");
        CHECK(TapAnswerLogLabel(L"p:3:0123abcd-4567-89ef-0123-456789abcdef") == L"p:3", "a Search pick's label keeps its number");
        CHECK(TapAnswerLogLabel(L"t:9f8e7d6c5b4a") == L"t:\x2026", "a single-use token's label hides its random part");
        CHECK(TapAnswerLogLabel(L"d:front:22371") == L"d:front:22371", "a directory dot's label is kept (short)");
        CHECK(TapAnswerLogLabel(L"nt:f:3") == L"nt:f:3", "a wizard button's label is kept (short)");
        CHECK(TapAnswerLogLabel(std::wstring(30, L'x')) == std::wstring(24, L'x') + L"\x2026", "a long unknown label is cut at 24 with an ellipsis");
        CHECK(TapAnswerLogLabel(L"") == L"(no data)", "an empty callback data has a label");
        CHECK(TapAnswerLogLabel(L"c:") == L"c:", "a card prefix without a verb is kept as it is");
    }

    // ===== 2. one tap =====
    {
        TapAnswer tap{ L"q-1", L"c:session", 1000 };
        CHECK(tap.queryId == L"q-1" && tap.logLabel == L"c:session", "a tap keeps its query id and label");
        CHECK(tap.receivedMs == 1000 && tap.deadlineMs == 1000 + kTapAnswerHoldMaxMs, "a tap's deadline is the hold's limit after its arrival");
        CHECK(tap.holds.load() == 0 && !tap.Answered(), "a new tap has no hold and no answer");
        CHECK(tap.Toast().empty(), "a new tap has no toast");
        tap.SetToast(L"first");
        tap.SetToast(L"second");
        CHECK(tap.Toast() == L"second", "the last toast set wins");
        tap.SetToast(std::wstring(260, L'z'));
        CHECK(tap.Toast().size() == kTapToastMaxChars, "a toast is cut when it is set");
        CHECK(tap.TryClaimAnswer(), "the first claim of the answer wins");
        CHECK(!tap.TryClaimAnswer(), "a second claim gets nothing: a tap is answered once");
        CHECK(tap.Answered(), "a claimed tap reads answered");
    }

    // ===== 3. the holds =====
    {
        const auto sink = std::make_shared<CountingSink>();
        const auto tap = std::make_shared<TapAnswer>(L"q-2", L"c:screenimage", 0);
        {
            auto a = std::make_shared<TapHold>(tap, std::weak_ptr<TapReleaseSink>{ sink });
            auto b = std::make_shared<TapHold>(tap, std::weak_ptr<TapReleaseSink>{ sink });
            CHECK(tap->holds.load() == 2, "two holds count two");
            auto aCopy = a; // a copy of an item shares its hold
            a.reset();
            CHECK(tap->holds.load() == 2 && sink->calls.load() == 0, "a shared hold stays while one copy lives");
            aCopy.reset();
            CHECK(tap->holds.load() == 1 && sink->calls.load() == 0, "the sink is not told while a hold is left");
            b.reset();
            CHECK(tap->holds.load() == 0 && sink->calls.load() == 1, "the LAST hold tells the sink, once");
            CHECK(sink->lastTap == tap, "the sink is told which tap");
        }
        // An inert hold (no tap) holds nothing and tells nobody.
        {
            auto inert = std::make_shared<TapHold>(nullptr, std::weak_ptr<TapReleaseSink>{ sink });
            CHECK(!inert->Tap(), "an inert hold has no tap");
        }
        CHECK(sink->calls.load() == 1, "an inert hold tells the sink nothing");
        // A hold that outlives its sink (a window's reply after the service went) tells nobody and does not crash.
        {
            auto gone = std::make_shared<CountingSink>();
            const auto tap2 = std::make_shared<TapAnswer>(L"q-3", L"d:front:1", 0);
            auto h = std::make_shared<TapHold>(tap2, std::weak_ptr<TapReleaseSink>{ gone });
            gone.reset();
            h.reset();
            CHECK(tap2->holds.load() == 0, "a hold whose sink is gone still lets the tap go");
        }
        // MoveTapHolds: the holds change hands, no count changes, the source is left empty.
        {
            const auto tap3 = std::make_shared<TapAnswer>(L"q-4", L"c:session", 0);
            std::vector<TapHoldRef> from{ std::make_shared<TapHold>(tap3, std::weak_ptr<TapReleaseSink>{ sink }) };
            std::vector<TapHoldRef> to;
            MoveTapHolds(from, to);
            CHECK(from.empty() && to.size() == 1 && tap3->holds.load() == 1, "MoveTapHolds moves a hold without letting it go");
            std::vector<TapHoldRef> none;
            MoveTapHolds(none, to);
            CHECK(to.size() == 1, "moving no hold changes nothing");
            const int before = sink->calls.load();
            to.clear();
            CHECK(sink->calls.load() == before + 1, "the moved hold tells the sink when it goes");
        }
    }

    // ===== 4. the scopes =====
    {
        const auto sink = std::make_shared<CountingSink>();
        const std::weak_ptr<TapReleaseSink> weakSink{ sink };
        CHECK(!TapScope::Active() && TapScope::CurrentTaps().empty(), "no scope outside a handler");
        CHECK(HoldTapsOfScope(weakSink).empty(), "outside a scope nothing is held (the fleet's own traffic)");

        const auto tapA = std::make_shared<TapAnswer>(L"q-a", L"c:session", 0);
        const auto tapB = std::make_shared<TapAnswer>(L"q-b", L"d:front:7", 0);
        const std::vector<TapHoldRef> holdA{ std::make_shared<TapHold>(tapA, weakSink) };
        {
            const TapScope outer{ holdA };
            CHECK(TapScope::Active(), "a scope of a hold is active");
            CHECK(TapScope::CurrentTaps().size() == 1 && TapScope::CurrentTaps()[0] == tapA, "the scope holds its tap");
            {
                auto held = HoldTapsOfScope(weakSink);
                CHECK(held.size() == 1 && tapA->holds.load() == 2, "an item queued in the scope holds its tap");
            }
            CHECK(tapA->holds.load() == 1 && sink->calls.load() == 0, "the item's hold went; the scope's own keeps the tap");

            const std::vector<TapHoldRef> holdB{ std::make_shared<TapHold>(tapB, weakSink) };
            {
                const TapScope inner{ holdB };
                CHECK(TapScope::CurrentTaps().size() == 2, "an inner scope keeps the outer one's tap and adds its own");
                auto held = HoldTapsOfScope(weakSink);
                CHECK(held.size() == 2 && tapA->holds.load() == 2 && tapB->holds.load() == 2, "an item in the inner scope holds both taps");
            }
            CHECK(TapScope::CurrentTaps().size() == 1 && TapScope::CurrentTaps()[0] == tapA, "closing the inner scope brings the outer one back");
            // A scope over a hold of a tap already in scope: the tap counts once.
            const std::vector<TapHoldRef> holdA2{ std::make_shared<TapHold>(tapA, weakSink) };
            {
                const TapScope again{ holdA2 };
                CHECK(TapScope::CurrentTaps().size() == 1, "a tap in two nested scopes counts once");
                CHECK(HoldTapsOfScope(weakSink).size() == 1, "an item gets one hold per tap, not one per scope");
            }
            // Another thread never sees this thread's scope.
            bool otherActive = true;
            size_t otherTaps = 99;
            std::thread([&otherActive, &otherTaps]() {
                otherActive = TapScope::Active();
                otherTaps = TapScope::CurrentTaps().size();
            }).join();
            CHECK(!otherActive && otherTaps == 0, "a scope belongs to its own thread");
        }
        CHECK(!TapScope::Active(), "every scope closed: none is active");

        // A scope over nothing (an item without holds: the send loop opens one around every callback) is no scope.
        {
            const std::vector<TapHoldRef> empty;
            const TapScope none{ empty };
            CHECK(!TapScope::Active() && HoldTapsOfScope(weakSink).empty(), "a scope without holds holds nothing");
            const std::vector<TapHoldRef> inert{ std::make_shared<TapHold>(nullptr, weakSink) };
            const TapScope alsoNone{ inert };
            CHECK(!TapScope::Active(), "a scope of inert holds is no scope");
        }

        // An answered tap (its hold's limit passed) gets no new hold: holding it would only delay nothing.
        {
            const auto late = std::make_shared<TapAnswer>(L"q-late", L"c:fork", 0);
            const std::vector<TapHoldRef> hold{ std::make_shared<TapHold>(late, weakSink) };
            const TapScope scope{ hold };
            late->TryClaimAnswer();
            CHECK(HoldTapsOfScope(weakSink).empty(), "an answered tap gets no new hold");
            CHECK(TapScope::CurrentTaps().size() == 1, "...though it is still in scope");
        }
    }

    // ===== 5. the whole path, as the service runs it =====
    {
        const auto sink = std::make_shared<CountingSink>();
        const std::weak_ptr<TapReleaseSink> weakSink{ sink };
        const auto tap = std::make_shared<TapAnswer>(L"q-path", L"c:session", 5000);
        FakeItem edit;
        FakeItem reply;
        {
            // _handleCallback: the handler's own hold, and its scope.
            const std::vector<TapHoldRef> handlerHold{ std::make_shared<TapHold>(tap, weakSink) };
            const TapScope scope{ handlerHold };
            edit = Enqueue(sink); // the card's edit (⚙ Session)
            reply = Enqueue(sink); // a reply in the topic
            tap->SetToast(L"done");
            CHECK(tap->holds.load() == 3, "the handler and its two items hold the tap");
        }
        CHECK(sink->calls.load() == 0 && tap->holds.load() == 2, "the handler is done, its items still hold the answer");
        // The edit is answered and dropped: the reply still holds.
        edit = FakeItem{};
        CHECK(sink->calls.load() == 0 && tap->holds.load() == 1, "one item answered: the other still holds");
        // The reply's callback runs inside a scope of its holds and queues a fallback (a refused rich message sent as HTML).
        FakeItem fallback;
        {
            const TapScope callbackScope{ reply.tapHolds };
            fallback = Enqueue(sink);
        }
        reply = FakeItem{};
        CHECK(sink->calls.load() == 0 && tap->holds.load() == 1, "the fallback holds the tap after its item went");
        fallback = FakeItem{};
        CHECK(sink->calls.load() == 1 && sink->lastTap == tap, "the last hold of the path tells the sink, once");
        CHECK(tap->TryClaimAnswer() && tap->Toast() == L"done", "the answer is claimed once, with the handler's toast");
        // A replaced item hands its holds to its replacement (the same message's newer edit).
        const auto tap2 = std::make_shared<TapAnswer>(L"q-replace", L"c:main", 0);
        FakeItem queued;
        {
            const std::vector<TapHoldRef> handlerHold{ std::make_shared<TapHold>(tap2, weakSink) };
            const TapScope scope{ handlerHold };
            queued = Enqueue(sink);
        }
        FakeItem newer; // queued outside any scope (the tick's own refresh of the same card)
        MoveTapHolds(queued.tapHolds, newer.tapHolds);
        queued = FakeItem{};
        CHECK(sink->calls.load() == 1 && tap2->holds.load() == 1, "the replaced edit's hold lives on in its replacement");
        newer = FakeItem{};
        CHECK(sink->calls.load() == 2 && sink->lastTap == tap2, "the replacement's answer lets the tap go");
    }
}
