// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — a tapped Telegram button stays BUSY while the bot works on what it asked for (MESSAGING.md §21.28). The user's
// request (2026-10-19): "In telegram, buttons we click on should look 'active' as long as they are waiting for serverside to
// react."
//
// Telegram shows a tapped inline button's loading indicator until the bot ANSWERS the tap (answerCallbackQuery; Bot API,
// CallbackQuery: "Telegram clients will display a progress bar until you call answerCallbackQuery"). Until §21.28 the bot answered
// every tap the moment it arrived, so the indicator went off at once while the page flip, the picture, the menu or the launch the
// tap asked for was still on its way through the paced outbox (one call per ~1.05 s, behind whatever was queued before it). Now
// the answer WAITS for what the tap set off:
//   - every outbox item queued while the tap is handled — an edit of the card, a reply, a photo, a deleted menu — holds the answer
//     until Telegram has answered THAT call (the poll thread's TAP SCOPE, read by MessagingService::_enqueue), and so does what
//     such an item's callback queues in turn (a refused rich message sent again as HTML, a refused photo sent as a document);
//   - work that goes on somewhere else carries the tap along explicitly: the hosting window's reply to a screen read, the
//     picture's worker thread, a window's answer to a launch, the window census of a new thread, and the sticky card's NEXT edit
//     or post (a page flip that waits for a card post in flight, a directory dot's re-post, the card that shows a restart);
//   - and at most kTapAnswerHoldMaxMs after the tap came in: Telegram stops waiting for an answer at some point (below), so the
//     hold ends before that, and the answer goes with the handler's toast — or with kTapStillWorkingToast when the work goes on.
//
// The pieces, all in this header (std only: no Windows, no JSON, no service — the m5 suite drives them with a fake sink and an
// injected clock, tests_messaging_taps.cpp):
//   TapAnswer       one tap: Telegram's query id, the toast its answer will carry, its deadline, its live HOLDS, and whether it was
//                   answered — exactly once, by the release of its last hold or by its deadline, whichever comes first.
//   TapHold         one hold (RAII): the answer waits while any hold of the tap lives, and the last hold to go tells the sink.
//   TapScope        the calling thread's current taps: what the service queues inside one is held for them. Scopes nest (an inner
//                   one keeps the outer one's taps) and are made only FROM holds, so a tap in scope always has a live hold and its
//                   count can never reach zero halfway through its own work.
//   TapReleaseSink  who is told that a tap's last hold went (MessagingService: it queues the answer for its send thread).
//
// Telegram's limit is not documented. Measured by others: an answer about 15 s after the tap was refused with QUERY_ID_INVALID
// (d-Rickyy-b's gist "Code for checking out the timeout of answerCallbackQuery", a python-telegram-bot maintainer; a Stack Overflow
// answer gives "about 15 seconds"); some frameworks say 10 s (GramIO's documentation, nutgram's maintainer). The client waits for
// the server, and the server for the bot, so the loading indicator itself ends at that same limit (MTProto
// messages.getBotCallbackAnswer then fails with BOT_RESPONSE_TIMEOUT).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Agentmaster::Msg
{
    // ===== the limits ==================================================================================================

    /// <summary>
    /// The longest a tapped button's answer waits for its work, counted from the moment the tap reached the bot (the poll thread).
    /// Telegram refuses an answer that comes about 15 s after the tap (measured by others — see the file's header; some frameworks
    /// say 10 s), and the button's loading indicator ends there in any case. 9 s leaves room for the update's way to the bot (well
    /// under a second while the long poll waits), for the send loop's look at the due answers (at least every 0.5 s while the
    /// outbox is empty, every 1.2 s while it paces) and for the outbox's gap before the answer itself goes (kSendGapMs, 1.05 s), so
    /// the answer — and its toast — still lands in time. The work is never cut short: it goes on, and its result shows in the chat
    /// when it is there.
    /// </summary>
    inline constexpr int64_t kTapAnswerHoldMaxMs = 9 * 1000;

    /// <summary>
    /// The longest toast an answer carries, in UTF-16 code units. Telegram takes 0-200 characters (answerCallbackQuery's `text`);
    /// the service has always cut at 190 to stay clear of the limit with characters that count twice.
    /// </summary>
    inline constexpr size_t kTapToastMaxChars = 190;

    /// <summary>
    /// The toast of an answer that the hold's LIMIT sent while the tap's work still runs, for a button whose handler had nothing
    /// to say: the button stops spinning (Telegram would drop a later answer), and the user learns that nothing failed — the
    /// result (the picture, the "Opened …" line, the changed card) shows in the chat when it is there.
    /// </summary>
    inline constexpr const wchar_t* kTapStillWorkingToast = L"\x23F3 Still working on it \x2014 the result shows up here when it is done.";

    // ===== the texts ===================================================================================================

    /// <summary>
    /// A toast cut to kTapToastMaxChars UTF-16 code units, never between the two halves of a surrogate pair (an emoji past the
    /// cut would otherwise leave a lone high surrogate, which no JSON encoder writes as valid UTF-8).
    /// </summary>
    /// <param name="text">The toast as the handler wrote it.</param>
    /// <returns>The text, whole when it fits, else its first kTapToastMaxChars code units (one fewer when the cut would split a
    /// pair).</returns>
    /// <exception cref="std::bad_alloc">If the copy cannot be allocated.</exception>
    inline std::wstring TapToastCut(std::wstring_view text)
    {
        if (text.size() <= kTapToastMaxChars)
        {
            return std::wstring{ text };
        }
        size_t cut = kTapToastMaxChars;
        // The last unit kept is a HIGH surrogate (D800-DBFF): its low half is the first unit dropped, so the pair goes whole.
        if (text[cut - 1] >= 0xD800 && text[cut - 1] <= 0xDBFF)
        {
            --cut;
        }
        return std::wstring{ text.substr(0, cut) };
    }

    /// <summary>
    /// The text a tap's answer carries (answerCallbackQuery's `text`; "" = no toast, the indicator just ends). The handler's own
    /// toast when it set one — it names the action ("Restarting it …", "↑ … goes first in the thread list"), which reads right
    /// whether the work is done or still running. With none, and the hold's limit answering while the work goes on,
    /// kTapStillWorkingToast; with none and the work done, nothing — the chat shows what happened.
    /// </summary>
    /// <param name="toast">The handler's toast (TapAnswer::Toast; "" = none).</param>
    /// <param name="workGoesOn">True when the answer goes at the hold's limit while some of the tap's work still holds it.</param>
    /// <returns>The text, cut by TapToastCut.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring TapAnswerText(std::wstring_view toast, bool workGoesOn)
    {
        if (!toast.empty())
        {
            return TapToastCut(toast);
        }
        return workGoesOn ? std::wstring{ kTapStillWorkingToast } : std::wstring{};
    }

    /// <summary>
    /// What the log calls a tapped button: its callback data without the parts that say nothing about the button — a card verb
    /// "c:&lt;verb&gt;:&lt;session id&gt;" becomes "c:&lt;verb&gt;" and a Search pick "p:&lt;n&gt;:&lt;session id&gt;" becomes
    /// "p:&lt;n&gt;" (the log line names no session; the [nav] line of the action does); a single-use token "t:&lt;random&gt;"
    /// becomes "t:…" (its kind lives in the service's token map); anything else is kept, cut at 24 units with a "…".
    /// </summary>
    /// <param name="data">The tap's callback data.</param>
    /// <returns>The label; "(no data)" for an empty one.</returns>
    /// <exception cref="std::bad_alloc">If the label cannot be allocated.</exception>
    inline std::wstring TapAnswerLogLabel(std::wstring_view data)
    {
        constexpr size_t kMaxLabel = 24;
        if (data.empty())
        {
            return L"(no data)";
        }
        if (data.substr(0, 2) == L"c:" || data.substr(0, 2) == L"p:")
        {
            if (const size_t second = data.find(L':', 2); second != std::wstring_view::npos)
            {
                return std::wstring{ data.substr(0, second) };
            }
        }
        if (data.substr(0, 2) == L"t:")
        {
            return L"t:\x2026";
        }
        if (data.size() > kMaxLabel)
        {
            return std::wstring{ data.substr(0, kMaxLabel) } + L"\x2026";
        }
        return std::wstring{ data };
    }

    // ===== one tap =====================================================================================================

    /// <summary>
    /// One tapped button whose answer (answerCallbackQuery) is HELD while what it set off runs (MESSAGING.md §21.28). Shared — by
    /// the service's list of open taps, by every TapHold and by every TapScope — and never copied. Safe from any thread: the
    /// constant fields are set before the tap is shared, the toast is guarded by its own mutex, the counters are atomic.
    /// </summary>
    struct TapAnswer
    {
        /// <summary>Makes an unanswered tap with no hold that may wait until kTapAnswerHoldMaxMs after `receivedTickMs`.</summary>
        /// <param name="callbackQueryId">Telegram's id of the tap: the answer names it.</param>
        /// <param name="label">What the log calls the button (TapAnswerLogLabel of its callback data).</param>
        /// <param name="receivedTickMs">When the tap reached the bot, on a MONOTONIC clock in ms (GetTickCount64 in the service; the
        /// tests pass their own).</param>
        /// <exception cref="std::bad_alloc">If the strings cannot be copied.</exception>
        TapAnswer(std::wstring callbackQueryId, std::wstring label, int64_t receivedTickMs) :
            queryId{ std::move(callbackQueryId) }, logLabel{ std::move(label) }, receivedMs{ receivedTickMs }, deadlineMs{ receivedTickMs + kTapAnswerHoldMaxMs }
        {
        }
        TapAnswer(const TapAnswer&) = delete;
        TapAnswer& operator=(const TapAnswer&) = delete;

        const std::wstring queryId; // Telegram's id of the tap (answerCallbackQuery's callback_query_id)
        const std::wstring logLabel; // the button, as the "[msg] tap …" log line names it
        const int64_t receivedMs; // when the tap reached the bot (monotonic ms)
        const int64_t deadlineMs; // receivedMs + kTapAnswerHoldMaxMs: the answer goes then at the latest

        /// <summary>
        /// The live holds (TapHold objects): the answer waits while this is above 0. Only TapHold changes it — its constructor adds
        /// one, its destructor takes one away and tells the sink when it took the last.
        /// </summary>
        std::atomic<int> holds{ 0 };

        /// <summary>
        /// Claims the ONE answer of this tap. The release of the last hold and the deadline race for it, and a tap is answered once
        /// (Telegram refuses a second answer to a callback query): the first caller gets true and sends the answer, every later
        /// caller gets false and sends nothing.
        /// </summary>
        /// <returns>True for the caller that must send the answer.</returns>
        bool TryClaimAnswer() noexcept
        {
            return !_answered.exchange(true);
        }

        /// <summary>Whether the answer was claimed (sent or about to be). New holds of an answered tap are pointless (HoldTapsOfScope skips it).</summary>
        /// <returns>True once TryClaimAnswer returned true for someone.</returns>
        bool Answered() const noexcept
        {
            return _answered.load();
        }

        /// <summary>
        /// Sets the toast the answer will carry (TapAnswerText; "" = none). The handler sets it while it runs — once its outcome is
        /// known — and before its own hold goes, so the answer that the last hold triggers always carries it; only the hold's limit
        /// can answer earlier, with what was set by then. A later call replaces an earlier one.
        /// </summary>
        /// <param name="text">The toast, cut to kTapToastMaxChars (TapToastCut).</param>
        /// <exception cref="std::bad_alloc">If the text cannot be copied (the previous toast stays).</exception>
        void SetToast(std::wstring_view text)
        {
            std::wstring cut = TapToastCut(text);
            std::lock_guard lk{ _toastMtx };
            _toast.swap(cut);
        }

        /// <summary>The toast set so far ("" = none).</summary>
        /// <returns>A copy of it.</returns>
        /// <exception cref="std::bad_alloc">If the copy cannot be allocated.</exception>
        std::wstring Toast() const
        {
            std::lock_guard lk{ _toastMtx };
            return _toast;
        }

    private:
        std::atomic<bool> _answered{ false };
        mutable std::mutex _toastMtx; // guards _toast: the poll thread writes it, the send thread reads it
        std::wstring _toast;
    };

    /// <summary>
    /// Who is told that a tap's LAST hold went: MessagingService, which queues the tap's answer for its send thread. An interface so
    /// that a hold refers to its sink weakly and the m5 suite can count the calls with a fake one.
    /// </summary>
    class TapReleaseSink
    {
    public:
        /// <summary>
        /// The last hold of `tap` went: the tap's work is done (or dropped), and its answer may go. Called on whatever thread released
        /// that hold — the send thread (a call Telegram answered), the poll thread (the end of a handler), a window's UI thread (a
        /// continuation it dropped or finished), the tick thread, a worker — and possibly while that thread holds the service's
        /// mutex (an outbox item dropped under it). So an implementation takes no lock but a LEAF one (never another lock while it is
        /// held), makes no network call and never throws.
        /// </summary>
        /// <param name="tap">The tap; its holds count is 0 now (a later hold is impossible: holds come only from live holds).</param>
        virtual void OnTapHoldsGone(const std::shared_ptr<TapAnswer>& tap) noexcept = 0;

    protected:
        /// <summary>Not destroyed through this interface: the sink's owner (a shared_ptr of the service) destroys it.</summary>
        ~TapReleaseSink() = default;
    };

    /// <summary>
    /// ONE hold on a tap's answer (RAII): while any hold of the tap lives, its answer waits. Each outbox item queued for the tap
    /// carries one (MessagingService::OutItem::tapHolds), so does each continuation that does the tap's work elsewhere, and so
    /// does the handler itself while it runs. Shared through TapHoldRef: the copies of one outbox item or of one lambda (a window
    /// sink copies its reply function once per window) share one hold, which goes when the last copy goes. Never copied itself.
    /// </summary>
    class TapHold
    {
    public:
        /// <summary>Adds one hold to `tap`.</summary>
        /// <param name="tap">The tap (null = an inert hold that holds nothing).</param>
        /// <param name="sink">Told when this was the tap's last hold; weak, so a hold that outlives the service does nothing.</param>
        TapHold(std::shared_ptr<TapAnswer> tap, std::weak_ptr<TapReleaseSink> sink) noexcept :
            _tap{ std::move(tap) }, _sink{ std::move(sink) }
        {
            if (_tap)
            {
                _tap->holds.fetch_add(1);
            }
        }

        /// <summary>
        /// Takes the hold away; when it was the tap's last, tells the sink (TapReleaseSink::OnTapHoldsGone — on THIS thread, which
        /// may hold the service's mutex: the sink only queues). A sink that is gone (the service was destroyed) is not told.
        /// </summary>
        ~TapHold()
        {
            if (_tap && _tap->holds.fetch_sub(1) == 1)
            {
                if (const auto sink = _sink.lock())
                {
                    sink->OnTapHoldsGone(_tap);
                }
            }
        }

        TapHold(const TapHold&) = delete;
        TapHold& operator=(const TapHold&) = delete;

        /// <summary>The tap this holds (null for an inert hold).</summary>
        /// <returns>The tap.</returns>
        const std::shared_ptr<TapAnswer>& Tap() const noexcept
        {
            return _tap;
        }

    private:
        std::shared_ptr<TapAnswer> _tap;
        std::weak_ptr<TapReleaseSink> _sink;
    };

    /// <summary>A shared hold: what an outbox item, a lambda or a stash keeps (see TapHold).</summary>
    using TapHoldRef = std::shared_ptr<TapHold>;

    /// <summary>
    /// Moves every hold of `from` to the end of `to` and leaves `from` empty: the holds change hands, no tap's count changes. Used
    /// where one piece of work replaces another (a newer outbox item with the same key, a stash taken by the card edit that
    /// carries it) — a hold that went instead would answer the tap before the work it waits for.
    /// </summary>
    /// <param name="from">The holds to move.</param>
    /// <param name="to">Where they go.</param>
    /// <exception cref="std::bad_alloc">If `to` cannot grow (then nothing moved: `from` keeps its holds).</exception>
    inline void MoveTapHolds(std::vector<TapHoldRef>& from, std::vector<TapHoldRef>& to)
    {
        if (from.empty())
        {
            return;
        }
        to.reserve(to.size() + from.size()); // the only step that can throw, before anything moved
        to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
        from.clear();
    }

    /// <summary>
    /// The calling thread's TAP SCOPE: while one is open, what the service queues is held for its taps (MessagingService::_enqueue
    /// asks HoldTapsOfScope). Made only from HOLDS — a list the caller owns, which already holds every tap of it — so a tap in
    /// scope always has a live hold and cannot be answered halfway through its own work. Scopes nest: an inner one adds its taps
    /// to the outer one's, and closing it brings the outer one back. Strictly nested on one thread (a local object); another
    /// thread never sees it.
    /// </summary>
    /// <remarks>
    /// The scope does not copy the list: it points at it, so opening one never allocates and never throws — it is opened around
    /// work that must run in any case (an outbox item's callback, a window's reply, a worker's send). ⚠ The list must outlive the
    /// scope and must not change while the scope is open: declare it before the scope, and move holds out of it only after the
    /// scope closed.
    /// </remarks>
    class TapScope
    {
    public:
        /// <summary>Opens a scope over the taps of `holds`, on top of the thread's current scope (whose taps stay in force).</summary>
        /// <param name="holds">The holds whose taps join the scope (empty = only the enclosing scope's taps, if any; a null entry
        /// and an inert hold are skipped).</param>
        explicit TapScope(const std::vector<TapHoldRef>& holds) noexcept :
            _holds{ &holds }, _outer{ _Current() }
        {
            _Current() = this;
        }

        /// <summary>Closes the scope: the enclosing one (or none) is the thread's scope again.</summary>
        ~TapScope()
        {
            _Current() = _outer;
        }

        TapScope(const TapScope&) = delete;
        TapScope& operator=(const TapScope&) = delete;

        /// <summary>Whether the calling thread is inside a scope that holds at least one tap.</summary>
        /// <returns>True inside such a scope (an empty scope, or one of inert holds only, counts as none).</returns>
        static bool Active() noexcept
        {
            for (const TapScope* s = _Current(); s; s = s->_outer)
            {
                for (const auto& h : *s->_holds)
                {
                    if (h && h->Tap())
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        /// <summary>The taps of the calling thread's scope and of every scope around it, each once, answered ones included.</summary>
        /// <returns>The taps (empty outside a scope).</returns>
        /// <exception cref="std::bad_alloc">If the list cannot be allocated.</exception>
        static std::vector<std::shared_ptr<TapAnswer>> CurrentTaps()
        {
            std::vector<std::shared_ptr<TapAnswer>> taps;
            for (const TapScope* s = _Current(); s; s = s->_outer)
            {
                for (const auto& h : *s->_holds)
                {
                    if (!h || !h->Tap())
                    {
                        continue;
                    }
                    bool known = false;
                    for (const auto& t : taps)
                    {
                        known = known || t == h->Tap();
                    }
                    if (!known)
                    {
                        taps.push_back(h->Tap());
                    }
                }
            }
            return taps;
        }

    private:
        /// <summary>The thread's innermost scope (null = none). One slot per thread for the whole module (an inline function's static).</summary>
        /// <returns>A reference to the calling thread's slot.</returns>
        static const TapScope*& _Current() noexcept
        {
            thread_local const TapScope* current = nullptr;
            return current;
        }

        const std::vector<TapHoldRef>* _holds; // the caller's holds (never null; see the remarks)
        const TapScope* _outer; // the enclosing scope, restored when this one closes (null = none)
    };

    /// <summary>
    /// One NEW hold for every UNANSWERED tap of the calling thread's scope — what an outbox item queued inside the scope keeps, and
    /// what a continuation captures to finish the tap's work on another thread (its own TapScope over these holds then makes what it
    /// queues hold the taps too). Outside a scope — nearly every call: the fleet's own traffic — it costs one thread-local read.
    /// </summary>
    /// <param name="sink">The sink the holds tell (the service, weakly).</param>
    /// <returns>The holds; empty outside a scope, or when every tap in scope was answered already (the hold's limit passed).</returns>
    /// <exception cref="std::bad_alloc">If a hold cannot be allocated (the holds made so far go, which may answer a tap early —
    /// never late).</exception>
    inline std::vector<TapHoldRef> HoldTapsOfScope(const std::weak_ptr<TapReleaseSink>& sink)
    {
        std::vector<TapHoldRef> holds;
        if (!TapScope::Active())
        {
            return holds;
        }
        for (const auto& tap : TapScope::CurrentTaps())
        {
            if (tap && !tap->Answered())
            {
                holds.push_back(std::make_shared<TapHold>(tap, sink));
            }
        }
        return holds;
    }
}
