// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — Messaging (see Messaging.h): the service's lifecycle, the outbox, the topic sync and
// the transcript mirror. The inbound half (updates, commands, buttons, the Search topic) is in
// Messaging.Inbound.cpp — one class, two TUs, like TerminalPage's partial files.

#include "Messaging.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

#include "ClaudeSpawn.h" // AppendStateLog, LogNav, ShortId, ResolveClaudeTranscriptPath, LogSwallowedException
#include "Persistence.h" // LoadAppSettings, ResolveSessionColorHex
#include "ScreenImage.h" // §21.42: RenderScreenSnapshotPng - the card's screenshot, drawn on the picture worker
#include "ScreenSnapshot.h" // §21.42: ParseScreenSnapshot - the window's screen read
#include "ProfileBootstrap.h" // Profiles::detail::WideToUtf8 / Utf8ToWide
#include "Scheduler.h"
#include "SessionRegistry.h"
#include "SessionStore.h" // the directory's tags (§21.16): LoadAllSessionTags, LoadKnownTags, LoadAllTagColors, ResolveTagDisplayHex
#include "StateFileGuard.h" // PERSISTENCE.md §16: state.json's read (ReadStateFileUtf8) and the replace guard (PrepareStateFileReplace)
#include "TelegramApi.h"
#include "TranscriptTurns.h" // ParseTranscriptJsonl

#pragma comment(lib, "advapi32.lib") // CredReadW / CredWriteW / CredDeleteW

namespace Agentmaster::Msg
{
    namespace
    {
        // The tick's cadence: the transcript mirror and the topic sync run this often when nothing wakes
        // them sooner (a registry notify does).
        constexpr int64_t kTickMs = 1500;
        // getUpdates' long-poll timeout (seconds) and the receive timeout around it.
        constexpr int64_t kPollTimeoutSec = 40;
        constexpr uint32_t kPollReceiveTimeoutMs = 60000;
        // While Threaded Mode is off, getMe is retried this often (the user turns it on in @BotFather).
        constexpr int64_t kGetMeRetryMs = 60 * 1000;
        // The outbox's pacing: Telegram allows about one message per second in one chat (every topic of
        // the private chat shares it); calls that send no message (reactions, answers to buttons) are
        // spaced less.
        constexpr int64_t kSendGapMs = 1050;
        // ⚠ Edits count against the same per-chat budget as messages: with a 300 ms gap here, the card
        // refreshes of 47 topics (each card's "3m" becoming "4m") ran at ~1 edit/s for ten minutes and
        // Telegram answered an editMessageText with 429 + retry_after 1526 s (2026-10-16). Every call now
        // keeps the ~1/s per-chat pace; what makes it bearable is that nothing periodic edits any more.
        constexpr int64_t kLightGapMs = 1050;
        // §21.28: the outbox sequence number of every answer to a tapped button — the lowest one _enqueue keeps (0 means "number
        // it"). The outbox serves by priority, then by seq, so an answer goes before every other item of its priority (only the
        // run's very first item is numbered 1 too, and it is long gone by the first tap): it is no message of the chat, and the
        // button keeps spinning until it lands. Answers among themselves keep the order they were queued in.
        constexpr uint64_t kTapAnswerOutSeq = 1;
        // How often a work-log message is edited while the turn runs, and how long it may grow before it
        // rolls over into a new message.
        constexpr int64_t kWorkLogEditGapMs = 15 * 1000;
        constexpr size_t kWorkLogRolloverChars = 3200;
        // The card is edited at most this often while the state does not change.
        constexpr int64_t kCardEditGapMs = 20 * 1000;
        // The STICKY CARD (§21.5): a card with newer posts below it is deleted and sent again at the bottom once
        // the posts have been quiet for this long (a reply and its remarks cost ONE re-post, two calls, not one
        // each); a card whose post Telegram refused is tried again after kCardRepostAfterFailureMs, never every tick.
        constexpr int64_t kCardStickyLagMs = 6 * 1000;
        constexpr int64_t kCardRepostAfterFailureMs = 60 * 1000;
        // A new topic reads this much of the transcript's tail (§21.6): the last away_summary and the newest message
        // for the recap rule, and — for a session that is mid-turn — the start of the turn's own prompt line, where
        // the mirror then begins (a turn longer than this starts mirroring from the file's end instead).
        constexpr int64_t kRecapSeedBytes = 256 * 1024;
        // How long a text sent AS THE USER is remembered for the echo guard (_takeSecretaryEcho): Telegram
        // delivers such a message back, if it does, within seconds; two minutes covers a slow poll.
        constexpr int64_t kSecretaryEchoMs = 2 * 60 * 1000;
        // The HISTORY replay (§21.9). Its messages go out at most one per kBackfillGapMs (half the chat's ~1 call/s, so
        // live traffic — which always goes first — keeps the other half; the whole history of 49 sessions is ~1,175
        // messages, ~40 minutes). It reads a transcript in windows of kBackfillWindowBytes (a window that holds no complete
        // line grows up to kBackfillWindowMaxBytes — one line can be a pasted image); a batch that did not finish sending
        // in kBackfillStallMs is dropped and sent again from its start.
        constexpr int64_t kBackfillGapMs = 2000;
        constexpr int64_t kBackfillWindowBytes = 1024 * 1024;
        constexpr int64_t kBackfillWindowMaxBytes = 64 * 1024 * 1024;
        // A batch whose messages have all left the outbox and that got no answer for this long is dropped and read again
        // (a throttled batch, still queued, is never "stalled").
        constexpr int64_t kBackfillStallMs = 30 * 60 * 1000;
        // The replay's share of the chat's flood limit: a history message goes only while the last minute holds fewer calls of
        // any kind than this (so ~18 a minute when nothing else runs, less while the live traffic runs), and not for this
        // long after a 429. Measured (§21.9's first run, dev, 2026-10-16): ~27 messages a minute held for ~11 minutes earned
        // a 429 (retry after 40 s), then went on for 5 more without one — a budget that drains, near the documented group
        // limit of 20 a minute rather than the 1 a second of a private chat.
        constexpr size_t kHistoryCallsPerMinute = 18;
        constexpr int64_t kHistoryFloodPauseMs = 5 * 60 * 1000;
        constexpr int64_t kCallWindowMs = 60 * 1000;
        // How long a history message sent AS THE USER is remembered by the echo guard: it may wait in the outbox behind the
        // live traffic and the flood budget for a long time, and an old prompt Telegram hands back must never be queued.
        constexpr int64_t kHistoryEchoMs = 90 * 60 * 1000;
        // The topic list's re-sort (§21.9): at most once a minute, only after half a minute with nothing posted anywhere,
        // and at most this many cards re-posted (a longer fix waits for a recreation or /recreate).
        constexpr int64_t kResortEveryMs = 60 * 1000;
        constexpr int64_t kResortQuietMs = 30 * 1000;
        constexpr size_t kResortMaxTopics = 24;
        // The replay waits for a RECREATION's new list (the old topics deleted, the new ones made — a minute or two), but not
        // longer than this after the recreation started: a topic Telegram keeps refusing to make must not hold every history.
        constexpr int64_t kRecreateListWaitMs = 5 * 60 * 1000;
        // How often the tick loop logs the outbound call census (`[msg] traffic (10 min): …`).
        constexpr int64_t kTrafficLogMs = 10 * 60 * 1000;
        // How much transcript a tick reads per session at most (a busy session writes kilobytes a second;
        // a catch-up after a pause must not stall the tick).
        constexpr size_t kMaxReadPerTick = 4u * 1024u * 1024u;
        // A transcript path that did not resolve is tried again this often — every ~2 s while the session is busy
        // (a NEW session or fork has its topic before its first message, §21.6, and that first prompt is what the
        // file starts with: found late, it would still be read from the start, but the thread would lag), and
        // every 20 s while it rests (nothing will appear until the user types).
        constexpr int64_t kResolveRetryMs = 20 * 1000;
        constexpr int64_t kResolveRetryBusyMs = 2 * 1000;
        // The run marker (§4.3, §21.18) is rewritten this often; the marker found at start is the previous run's last minute
        // alive, and with no run.closed stamp at or after it, that run ended without closing (a crash, a forced stop).
        constexpr int64_t kMarkerMs = 60 * 1000;
        // §21.18: after the app's last window closed (NoteAppClosing), the marker is not rewritten for this long — the process
        // is about to end, and a marker written between the close stamp and the end would make the stamp look older than the
        // run's last sign of life, so a clean quit would read as a crash. Past it the app evidently goes on (the last tab was
        // torn out into a new window), the marker is written again, and the stamp is outdated as it should be.
        constexpr int64_t kAppClosingMarkerHoldMs = 30 * 1000;
        // §21.18, the START NOTE's life (MessagingService::_startNoteState): not gathered yet, an attempt queued in the
        // outbox, due (its first send, or the next after a failed one, at _startNoteRetryAtMs), done (delivered or given up).
        constexpr int kStartNoteNone = 0;
        constexpr int kStartNoteQueued = 1;
        constexpr int kStartNoteDue = 2;
        constexpr int kStartNoteDone = 3;
        // A start note whose send failed AFTER the bot reached Telegram (getMe answered: before that the note simply waits)
        // goes again after 30 s, the wait doubling up to 10 minutes, 12 sends in all (~1.5 h). The note is the user's one
        // sign that Agentmaster came back, so a network that drops right after the connect must not cost it.
        constexpr int64_t kStartNoteRetryFirstMs = 30 * 1000;
        constexpr int64_t kStartNoteRetryMaxMs = 10 * 60 * 1000;
        constexpr int kStartNoteMaxAttempts = 12;
        // A Search topic whose creation was refused is asked for again this much later (_ensureSearchTopic), not every tick.
        constexpr int64_t kSearchCreateRetryMs = 60 * 1000;
        // The state file is saved at most this often while a STRUCTURAL change is dirty (and at once on Stop, at the topic
        // recreation and when the app's last window closes; StateSaveDue).
        constexpr int64_t kStateSaveGapMs = 2000;
        // ... and at most this often while only PROGRESS is dirty: the live mirror's cursors, recap facts and prompt lines
        // (CopyBackTopicProgress), a work log's new id, a topic's posted clock (_noteTopicPosted) and its card's stored hash.
        // Progress moves on every tick while any session works, and each save rewrites the whole file (465 KB with
        // production's 441 topics, flushed). Saved as a structural change, it was a save every ~3.2 s — with the backup copy
        // before each, the measured 279 KB/s, ~24 GB a day (UI_THREAD.md §13d). The cost of the gap: after an UNEXPECTED end,
        // up to this much of the mirrored lines is posted again at the next start (a normal quit flushes first,
        // NoteAppClosing), a card may be edited once more, and a card may stay above newer posts until the topic's next post.
        constexpr int64_t kStateProgressSaveGapMs = 10 * 1000;
        // The backup copy (state.json.prev) is refreshed at most this often (StatePrevCopyDue): it is the last resort for an
        // unreadable state.json, and copying it before every save doubled every save's writes.
        constexpr int64_t kStatePrevCopyGapMs = 60 * 1000;
        // The app-close flush (NoteAppClosing) waits at most this long for each of the save lock and the service's lock. It runs
        // under the engine's window mutex, so it must never wait without a bound: a bot thread holding `_mtx` might itself wait
        // for something that needs that mutex. The bot's threads hold `_mtx` only briefly (a few percent of the time with 441
        // topics) and a running save holds the save lock for one write, so the wait almost always ends within milliseconds.
        constexpr int64_t kAppCloseSaveLockWaitMs = 250;

        /// <summary>
        /// Takes a DEFERRED lock, waiting at most `waitMs`. Use the bounded form only where a caller must not depend on the lock's
        /// holder making progress (the app-close flush, which holds the engine's window mutex); everywhere else pass a negative
        /// wait, which is a plain blocking lock.
        /// </summary>
        /// <remarks>
        /// The bounded form polls try_lock about every millisecond (std::mutex has no timed lock; Sleep(1) may last up to a
        /// timer tick, ~15.6 ms), so it can give up although the lock was free for a moment between two polls — the caller must
        /// treat "not acquired" as an ordinary outcome. The wait is rare and short: it costs nothing on the tick's paths.
        /// </remarks>
        /// <typeparam name="Lock">A std::unique_lock over a mutex, not yet owning it.</typeparam>
        /// <param name="lock">The deferred lock; it owns the mutex on a true return and does not on a false one.</param>
        /// <param name="waitMs">The longest wait in milliseconds; negative = wait as long as it takes.</param>
        /// <returns>Whether the lock is now held (always true for a negative wait).</returns>
        /// <exception cref="std::system_error">From std::unique_lock when it already owns its mutex or has none (a caller bug).</exception>
        template<typename Lock>
        bool AcquireLockWithin(Lock& lock, int64_t waitMs)
        {
            if (waitMs < 0)
            {
                lock.lock();
                return true;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
            for (;;)
            {
                if (lock.try_lock())
                {
                    return true;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        // A Search agent idle this long is parked (its claude.exe holds ~400 MB).
        constexpr int64_t kSearchParkMs = 10 * 60 * 1000;
        // Action tokens live this long (M5: a stale button is refused).
        constexpr int64_t kTokenTtlMs = 2 * 60 * 60 * 1000;
        // How many topics a tick creates or deletes at most (each is an API call on the tick thread).
        constexpr size_t kTopicOpsPerTick = 6;
        // The topic names' state glyphs (§21.11): a rename goes while the last minute's calls leave room for its two (the edit,
        // and the delete of the "topic renamed" notice Telegram posts) under kHistoryCallsPerMinute, and the history replay
        // waits for it (MessagingModels.h NameRenameFits / HistoryYieldsToRename). Until 2026-10-17 a rename needed the window
        // under 12 calls — a level the replay never let it fall to, so no topic name followed its session for hours.
        // §21.12: a prompt's status follows a session state only once it has held this long (a recon synth undone a pass later
        // must not edit the line twice); an Error shows at once.
        constexpr int64_t kPromptStatusSettleMs = 3 * 1000;
        // A rich message's parts each take a block of this many sub-sequence slots in the outbox (§21.11): part j at
        // j * stride, its HTML fallback's chunks after it (MessagingService::_htmlItems, two slots per chunk).
        constexpr uint32_t kRichPartSubSeqStride = 4096;
        // §21.15: the SLOW FORM UPGRADE re-edits one unchanged card per this gap, across all topics (_takeCardFormatSlot), so
        // turning title chips on converts the cards one at a time — dev's ~48 cards in ~8 minutes — within the call budget.
        constexpr int64_t kCardFormatGapMs = 10 * 1000;
        // The START-UP HOLD on "closed" topic deletes (§21.4): at an app start every session reads live=false
        // until its window re-homes its tabs (the registry loads sessions.json as archived; each reopened window
        // flips its own tabs live, one by one, in its first layout; a second window registers only when the
        // first one's restore has released the shared UI thread). A "closed" delete of a topic whose session
        // has NOT been seen live in this run is held (a) for kStartupCloseHoldMinMs after the start — the
        // windows that have not registered yet — and (b) while any registered window is still restoring
        // (ServiceHooks::windowsRestoring, Engine::WindowsStillRestoring), up to kStartupCloseHoldCapMs so a
        // restore that never ends cannot hold forever. Measured 2026-10-16 without it: the first tick after a
        // dev swap deleted all 47 topics as "closed" and the next minute re-created 38; with a fixed 120 s
        // hold, one 56-tab window took 72 s of fleet load + 68 s of re-home, and the 16 tabs re-homed after
        // the hold churned again — a restart must never churn the chat.
        constexpr int64_t kStartupCloseHoldMinMs = 120 * 1000;
        constexpr int64_t kStartupCloseHoldCapMs = 30 * 60 * 1000;

        int64_t WallMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        // When THIS process started, as Unix ms (0 when Windows does not say). The start note (§21.18) tells the app's launch
        // from a start of the bot in an app that was already running by it (ClassifyStartNote), and names it in the second case.
        // (Not ProcessInspect's ProcessStartUnixMs(pid), which reads another process by its pid.)
        int64_t ThisProcessStartUnixMs() noexcept
        {
            FILETIME created{};
            FILETIME exited{};
            FILETIME kernel{};
            FILETIME user{};
            if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user))
            {
                return 0;
            }
            ULARGE_INTEGER t{};
            t.LowPart = created.dwLowDateTime;
            t.HighPart = created.dwHighDateTime;
            constexpr unsigned long long kUnixEpoch100ns = 116444736000000000ULL; // 1601-01-01 to 1970-01-01 in FILETIME ticks
            if (t.QuadPart <= kUnixEpoch100ns)
            {
                return 0;
            }
            return static_cast<int64_t>((t.QuadPart - kUnixEpoch100ns) / 10000);
        }

        // ----- Credential Manager: the one place a bot token is stored (M4) -----

        // Reads a generic credential's blob as a token (trailing NULs, spaces and line breaks dropped); "" when absent or
        // refused. Throws std::bad_alloc only (the callers contain it).
        std::wstring ReadCredentialToken(const std::wstring& target)
        {
            PCREDENTIALW cred = nullptr;
            if (!::CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred) || !cred)
            {
                return {};
            }
            std::wstring token;
            if (cred->CredentialBlob && cred->CredentialBlobSize >= sizeof(wchar_t))
            {
                token.assign(reinterpret_cast<const wchar_t*>(cred->CredentialBlob), cred->CredentialBlobSize / sizeof(wchar_t));
            }
            ::CredFree(cred);
            while (!token.empty() && (token.back() == L'\0' || token.back() == L' ' || token.back() == L'\r' || token.back() == L'\n'))
            {
                token.pop_back();
            }
            return token;
        }

        // Writes (replaces) a token as a generic credential, trimmed; an all-blank token is refused (false). A refusal of
        // Credential Manager is logged to hooks.log with `what` ("bot token", "secretary bot token") — never the token.
        // Throws std::bad_alloc only.
        bool StoreCredentialToken(std::wstring target, std::wstring_view token, const wchar_t* what)
        {
            std::wstring value{ token };
            const size_t a = value.find_first_not_of(L" \t\r\n");
            if (a == std::wstring::npos)
            {
                return false;
            }
            const size_t b = value.find_last_not_of(L" \t\r\n");
            value = value.substr(a, b - a + 1);
            std::wstring user = L"bot";
            CREDENTIALW cred{};
            cred.Type = CRED_TYPE_GENERIC;
            cred.TargetName = target.data();
            cred.UserName = user.data();
            cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
            cred.CredentialBlobSize = static_cast<DWORD>(value.size() * sizeof(wchar_t));
            cred.CredentialBlob = reinterpret_cast<LPBYTE>(value.data());
            const bool ok = ::CredWriteW(&cred, 0) != FALSE;
            if (!ok)
            {
                AppendStateLog(L"hooks.log", std::wstring{ L"[msg] could not store the " } + what + L" in Credential Manager (error " + std::to_wstring(::GetLastError()) + L")\n");
            }
            return ok;
        }

        // Deletes a generic credential; true when deleted or absent.
        bool ClearCredentialToken(const std::wstring& target) noexcept
        {
            if (::CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0))
            {
                return true;
            }
            return ::GetLastError() == ERROR_NOT_FOUND;
        }

        // Whether a refused RICH send should go again as HTML (§21.11): a 400 about the message itself (its Markdown, its
        // size, a block Telegram does not take) or a 404 (a Bot API without the method). Not a missing chat or thread (the
        // HTML would be refused the same), not a 429 (the send loop already waited and retried), not a transport failure.
        bool RichRefusalWantsFallback(const TgResult& r) noexcept
        {
            if (r.ok || r.transportFailed)
            {
                return false;
            }
            if (r.httpStatus == 404)
            {
                return true;
            }
            return r.httpStatus == 400 && !r.ThreadNotFound() && r.description.find(L"chat not found") == std::wstring::npos;
        }

        // Whether a refused RICH EDIT should be made again with the classic render (§21.15): RichRefusalWantsFallback's rule,
        // minus two answers about the message itself — "message is not modified" (it already shows this content) and "message
        // to edit not found" (it is gone; the caller's own handling must see that answer, not a second refusal).
        bool RichEditRefusalWantsFallback(const TgResult& r) noexcept
        {
            if (!RichRefusalWantsFallback(r) || r.NotModified())
            {
                return false;
            }
            return r.description.find(L"message to edit not found") == std::wstring::npos;
        }

        // Whether Telegram's answer to a send or an edit is a RICH message (§21.15): the Message it returns carries `rich_message`
        // for a rich one and `text` for a classic one — the truth about the form a card is shown in.
        bool ResultIsRichMessage(const TgResult& r) noexcept
        {
            return r.ok && r.result.type == json::Value::Type::Obj && r.result.Find(L"rich_message") != nullptr;
        }

        /// <summary>
        /// §21.42: runs a card's post or edit EXACTLY ONCE with its picture, whichever path settles it first - the hosting window's
        /// answer, the picture worker, the guard of a read no window answered, or a failure in MessagingService::_withCardPicture.
        /// A second Run is a no-op, so every path may call it without knowing what the others did; and the send never runs twice
        /// (a card posted twice would leave two cards in the topic).
        /// </summary>
        class CardPictureOnce
        {
        public:
            /// <summary>Holds the send until the first Run.</summary>
            /// <param name="then">The card's post or edit, given the picture (null = none).</param>
            explicit CardPictureOnce(std::function<void(std::shared_ptr<const CardPicture>)> then) noexcept :
                _then{ std::move(then) }
            {
            }

            CardPictureOnce(const CardPictureOnce&) = delete;
            CardPictureOnce& operator=(const CardPictureOnce&) = delete;

            /// <summary>Runs the send with `picture` the first time; does nothing after. Contains what the send throws (Rule #18).</summary>
            /// <param name="picture">The picture (null = the card goes without one).</param>
            void Run(std::shared_ptr<const CardPicture> picture) noexcept
            {
                if (_ran.exchange(true))
                {
                    return;
                }
                try
                {
                    if (_then)
                    {
                        _then(std::move(picture));
                    }
                }
                catch (...)
                {
                    // Rule #18. Recovery: this card change is lost (a post's in-flight mark then stays until the next start, which
                    // clears it, or until a "message to edit not found" answer resets the card); the next update tries again.
                    LogSwallowedException(L"CardPictureOnce (the card's post or edit)");
                }
            }

            /// <summary>Whether the send ran (or is running): the wait for the picture is over.</summary>
            /// <returns>True after the first Run.</returns>
            bool Ran() const noexcept
            {
                return _ran.load();
            }

        private:
            std::atomic<bool> _ran{ false }; // the send ran (or is running): every later Run is a no-op
            std::function<void(std::shared_ptr<const CardPicture>)> _then;
        };

        /// <summary>
        /// §21.42: the ONE answer of a card's screen read (the "screen-snapshot" session action, sent to every window). Shared by
        /// every window's copy of the reply: the hosting window's reply Claim()s it; when the LAST copy goes unclaimed - no window
        /// hosts the tab (it closed, a claude outside Agentmaster's tabs), or the windows dropped the work - the destructor runs
        /// the card's send with the stored picture (or none), so a card change is never lost to a read that nobody answered.
        /// </summary>
        /// <remarks>
        /// The destructor runs on whichever thread drops the last copy: a window's UI thread, or the calling thread itself when no
        /// window is registered (then before _withCardPicture's request returns). CardPictureOnce::Run never throws.
        /// </remarks>
        class CardPictureReadGuard
        {
        public:
            /// <summary>Arms the guard.</summary>
            /// <param name="once">The card's one send.</param>
            /// <param name="fallback">What it goes with when no window answers (the stored picture, or null).</param>
            /// <param name="taps">§21.28: the tapped buttons that wait for this card change (the send runs inside a scope of them).</param>
            CardPictureReadGuard(std::shared_ptr<CardPictureOnce> once, std::shared_ptr<const CardPicture> fallback, std::vector<TapHoldRef> taps) noexcept :
                _once{ std::move(once) }, _fallback{ std::move(fallback) }, _taps{ std::move(taps) }
            {
            }

            /// <summary>Sends the card with the fallback when no window claimed the read.</summary>
            ~CardPictureReadGuard()
            {
                if (_claimed.exchange(true) || !_once)
                {
                    return; // a window answered: its reply settles the picture
                }
                const TapScope tapScope{ _taps }; // never throws
                _once->Run(_fallback);
            }

            CardPictureReadGuard(const CardPictureReadGuard&) = delete;
            CardPictureReadGuard& operator=(const CardPictureReadGuard&) = delete;

            /// <summary>Claims the read's one answer for the calling reply.</summary>
            /// <returns>True for the first caller (it settles the picture); false for any later one.</returns>
            bool Claim() noexcept
            {
                return !_claimed.exchange(true);
            }

        private:
            std::atomic<bool> _claimed{ false };
            std::shared_ptr<CardPictureOnce> _once;
            std::shared_ptr<const CardPicture> _fallback;
            std::vector<TapHoldRef> _taps;
        };

        // FNV-1a of a token, for the one-poller-per-token mutex name (never the token itself).
        std::wstring TokenHash(std::wstring_view token)
        {
            uint64_t h = 1469598103934665603ull;
            for (const wchar_t c : token)
            {
                h ^= static_cast<uint64_t>(c);
                h *= 1099511628211ull;
            }
            wchar_t buf[24];
            swprintf_s(buf, L"%016llx", static_cast<unsigned long long>(h));
            return buf;
        }

        std::wstring Leaf(std::wstring_view path)
        {
            std::wstring p{ path };
            while (!p.empty() && (p.back() == L'\\' || p.back() == L'/'))
            {
                p.pop_back();
            }
            const size_t cut = p.find_last_of(L"\\/");
            return cut == std::wstring::npos ? p : p.substr(cut + 1);
        }

        // An atomic UTF-8 file write: a temp sibling, flushed, then moved over the target (the engine's
        // WriteAllUtf8 recipe; Persistence.cpp's is file-local).
        bool WriteFileAtomic(const std::wstring& path, const std::string& bytes)
        {
            const std::wstring tmp = path + L".tmp";
            HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            DWORD written = 0;
            const BOOL ok = ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
            ::FlushFileBuffers(h);
            ::CloseHandle(h);
            if (!ok)
            {
                ::DeleteFileW(tmp.c_str());
                return false;
            }
            return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        }

        std::string ReadFileBytes(const std::wstring& path)
        {
            HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return {};
            }
            std::string out;
            LARGE_INTEGER size{};
            if (::GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (16ll << 20))
            {
                out.resize(static_cast<size_t>(size.QuadPart));
                DWORD read = 0;
                if (!::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr))
                {
                    out.clear();
                }
                else
                {
                    out.resize(read);
                }
            }
            ::CloseHandle(h);
            return out;
        }

        /// <summary>
        /// Reads [cursor, size) of a transcript (at most kMaxReadPerTick bytes), returning the bytes and the file's size; an
        /// unreadable file returns an empty string with size -1. Pass an infinite cursor (0x7FFF...) to learn only the size.
        /// </summary>
        /// <remarks>
        /// The size comes from a STAT first, and the file is opened only when it holds bytes past the cursor: the tick asks
        /// every topic's transcript on every tick (441 topics every 1.5 s on production, ~294 opens a second, nearly all of
        /// files that had not grown; UI_THREAD.md §13d), and the "size only" callers pass an infinite cursor. The stat
        /// (GetFileAttributesExW, a query-open of the file's metadata, not the directory listing's cached size) reports the
        /// size the file system holds, as GetFileSizeEx on an open handle does; a file that grows between the stat and the
        /// open is read up to its size at the open, and whatever it gains after that is read at the next tick. One
        /// difference from the open-first form: a file that exists but cannot be opened right now (a sharing violation) and
        /// holds nothing past the cursor reports its real size instead of -1, so the caller keeps its path rather than
        /// resolving it again.
        /// </remarks>
        /// <param name="path">The transcript.</param>
        /// <param name="cursor">The byte offset already read; a value at or past the end reads nothing.</param>
        /// <param name="sizeOut">Receives the file's size, or -1 when it is gone or could not be opened for a read it needed.</param>
        /// <returns>The bytes from the cursor (empty when there are none, or on a failed read).</returns>
        /// <exception cref="std::bad_alloc">When the read buffer (up to kMaxReadPerTick bytes) cannot be allocated.</exception>
        std::string ReadTranscriptTail(const std::wstring& path, int64_t cursor, int64_t& sizeOut)
        {
            sizeOut = -1;
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
            {
                return {}; // gone or unreadable: the same answer the open used to give (size -1)
            }
            const int64_t statSize = static_cast<int64_t>((static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow);
            if (statSize <= cursor)
            {
                // Nothing past the cursor (or the file shrank, which the caller sees in the size): no open needed.
                sizeOut = statSize;
                return {};
            }
            HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return {};
            }
            std::string out;
            LARGE_INTEGER size{};
            if (::GetFileSizeEx(h, &size))
            {
                sizeOut = size.QuadPart;
                if (size.QuadPart > cursor)
                {
                    LARGE_INTEGER pos{};
                    pos.QuadPart = cursor;
                    if (::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN))
                    {
                        const size_t want = static_cast<size_t>(std::min<int64_t>(size.QuadPart - cursor, static_cast<int64_t>(kMaxReadPerTick)));
                        out.resize(want);
                        DWORD read = 0;
                        if (!::ReadFile(h, out.data(), static_cast<DWORD>(want), &read, nullptr))
                        {
                            out.clear();
                        }
                        else
                        {
                            out.resize(read);
                        }
                    }
                }
            }
            ::CloseHandle(h);
            return out;
        }

        // What a new topic learns from the tail of a transcript (§21.6, _seedFromTranscript).
        struct TranscriptTailScan
        {
            int64_t size{ -1 }; // the file's size when read (-1 = unreadable / absent)
            int64_t lastPromptLineAt{ -1 }; // where the newest turn-starting prompt's line begins (-1 = none in the window)
            std::wstring recapText; // the newest away_summary in the window (FoldRecapFacts)
            int64_t recapAtMs{ 0 };
            int64_t lastMessageAtMs{ 0 }; // the newest conversation item's time in the window
            int64_t completeTo{ -1 }; // where the last complete line read ends (-1 = none read): a line end, never inside a torn last line
        };

        // Reads the last `windowBytes` of a transcript and folds every COMPLETE line of it: the recap facts and the
        // byte offset where the newest turn-starting prompt's line begins (a line torn by the window's start is
        // dropped; a torn last line — still being written — is not read). Lines are parsed one at a time because the
        // offset of each is needed. Throws std::bad_alloc only (the caller contains it).
        TranscriptTailScan ScanTranscriptTail(const std::wstring& path, int64_t windowBytes)
        {
            TranscriptTailScan scan;
            if (path.empty())
            {
                return scan;
            }
            int64_t size = -1;
            ReadTranscriptTail(path, 0x7FFFFFFFFFFFFFFFll, size); // the size only
            scan.size = size;
            if (size <= 0)
            {
                return scan;
            }
            const int64_t from = size > windowBytes ? size - windowBytes : 0;
            int64_t sizeNow = -1;
            const std::string bytes = ReadTranscriptTail(path, from, sizeNow);
            size_t pos = 0;
            if (from > 0)
            {
                const size_t nl = bytes.find('\n');
                if (nl == std::string::npos)
                {
                    return scan; // one line longer than the window: nothing complete to read
                }
                pos = nl + 1;
            }
            while (pos < bytes.size())
            {
                const size_t nl = bytes.find('\n', pos);
                if (nl == std::string::npos)
                {
                    break; // the last line is still being written
                }
                const int64_t lineAt = from + static_cast<int64_t>(pos);
                auto parsed = ParseTranscriptJsonl(bytes.substr(pos, nl - pos));
                pos = nl + 1;
                if (parsed.empty())
                {
                    continue;
                }
                const auto items = ClassifyTranscriptLine(parsed.front());
                for (const auto& it : items)
                {
                    if (it.kind == TopicItemKind::Prompt && !it.midTurn)
                    {
                        scan.lastPromptLineAt = lineAt;
                    }
                }
                FoldRecapFacts(items, scan.recapText, scan.recapAtMs, scan.lastMessageAtMs);
            }
            scan.completeTo = from + static_cast<int64_t>(pos); // after the loop `pos` is the first byte not read as a complete line
            return scan;
        }

        // Reads exactly [from, to) of a file, or less when the file is shorter now. Throws std::bad_alloc only.
        std::string ReadFileRange(const std::wstring& path, int64_t from, int64_t to)
        {
            std::string out;
            if (to <= from)
            {
                return out;
            }
            HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                return out;
            }
            LARGE_INTEGER pos{};
            pos.QuadPart = from;
            if (::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN))
            {
                out.resize(static_cast<size_t>(to - from));
                size_t got = 0;
                while (got < out.size())
                {
                    DWORD read = 0;
                    const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - got, 16u * 1024u * 1024u));
                    if (!::ReadFile(h, out.data() + got, want, &read, nullptr) || read == 0)
                    {
                        break;
                    }
                    got += read;
                }
                out.resize(got);
            }
            ::CloseHandle(h);
            return out;
        }

        // Reads the next window of a HISTORY replay (§21.9) from [pos, end) of a transcript and splits it
        // (SplitHistoryWindow: complete turns only). A window without one complete line grows (1 -> 4 -> 16 -> 64 MB; a
        // pasted image can make one line megabytes long); a line longer than the largest window is skipped (the next
        // window starts inside it, and its torn rest is dropped as unparsable). consumedTo -1 = the file is gone or shorter
        // than `pos`. Throws std::bad_alloc only (the caller contains it).
        HistoryWindow ReadHistoryWindow(const std::wstring& path, int64_t pos, int64_t end)
        {
            for (int64_t want = kBackfillWindowBytes;; want *= 4)
            {
                const int64_t to = std::min(end, pos + want);
                const std::string bytes = ReadFileRange(path, pos, to);
                if (bytes.size() < static_cast<size_t>(to - pos))
                {
                    return HistoryWindow{}; // shrunk or gone
                }
                HistoryWindow w = SplitHistoryWindow(bytes, pos, to >= end);
                if (!w.needMore)
                {
                    return w;
                }
                if (want >= kBackfillWindowMaxBytes)
                {
                    HistoryWindow skip;
                    skip.consumedTo = to;
                    return skip;
                }
            }
        }

        // The topic an outbox item posts into (its message_thread_id), 0 = none (the bot's General chat, or not a post).
        int64_t ThreadIdOf(const json::Value& params) noexcept
        {
            const auto* th = params.Find(L"message_thread_id");
            return th && th->type == json::Value::Type::Num ? static_cast<int64_t>(th->num) : 0;
        }

        // FNV-1a 32 of a text with its surrounding whitespace dropped: the identity the secretary echo guard
        // compares (Telegram may trim what it stores).
        uint32_t EchoHash(std::wstring_view text)
        {
            const size_t a = text.find_first_not_of(L" \t\r\n");
            if (a == std::wstring_view::npos)
            {
                return CardTextHash(L"");
            }
            const size_t b = text.find_last_not_of(L" \t\r\n");
            return CardTextHash(text.substr(a, b - a + 1));
        }

        std::wstring StateWord(SessionState s)
        {
            switch (s)
            {
            case SessionState::Running:
                return L"running";
            case SessionState::WaitingForInput:
                return L"waiting";
            case SessionState::NeedsApproval:
                return L"needs-approval";
            case SessionState::Error:
                return L"error";
            case SessionState::Done:
                return L"done";
            default:
                return L"idle";
            }
        }

        // A 0xRRGGBB color as "#RRGGBB" — a topic icon color (kTelegramTopicColors) in the form the color helpers parse (the
        // directory's chip of a dedicated topic takes its look from it, §21.16). Throws std::bad_alloc only.
        std::wstring RgbHex(uint32_t rgb)
        {
            wchar_t buf[16];
            swprintf_s(buf, L"#%06X", static_cast<unsigned>(rgb & 0xFFFFFFu));
            return buf;
        }
    }

    // ===== construction / lifecycle ==========================================================

    MessagingService::MessagingService(std::shared_ptr<SessionRegistry> registry, std::shared_ptr<Scheduler> scheduler, std::wstring profileDir, ServiceHooks hooks) :
        _registry(std::move(registry)), _scheduler(std::move(scheduler)), _profile(std::move(profileDir)), _hooks(std::move(hooks))
    {
        _stateFile = _profile + L"\\messaging\\telegram\\state.json";
        _status = L"Off.";
        // §21.23: a host that gives only the ANSWERED launch still gets a plain one, for the code that only asks whether launching
        // is available (`!_hooks.launch`) and for any caller that needs no answer. Its answers are dropped: the service's own
        // launches go through _askLaunch, which takes launchAnswered itself and routes the answer to the tick.
        if (!_hooks.launch && _hooks.launchAnswered)
        {
            _hooks.launch = [answered = _hooks.launchAnswered](const RemoteLaunchRequest& req) { return answered(req, RemoteLaunchReply{}); };
        }
    }

    MessagingService::~MessagingService()
    {
        Stop();
    }

    int64_t MessagingService::_nowMs() const noexcept
    {
        return WallMs();
    }

    void MessagingService::_log(const std::wstring& line) const noexcept
    {
        try
        {
            AppendStateLog(L"hooks.log", line + L"\n");
        }
        catch (...)
        {
            // ⚠ DELIBERATELY BARE: the logger itself failed; nothing left to report through.
        }
    }

    void MessagingService::_setStatus(std::wstring text)
    {
        std::lock_guard lk{ _mtx };
        _status = std::move(text);
    }

    bool MessagingService::Running() const noexcept
    {
        return _running.load();
    }

    std::wstring MessagingService::StatusText() const
    {
        std::lock_guard lk{ _mtx };
        if (!_running.load())
        {
            return _status;
        }
        // The SECRETARY line (§21.5, §21.11): who sends the user's turns as the user, or what is missing for it.
        std::wstring line;
        const int64_t secretaryId = _secretaryBotId.load();
        const bool viaSecretary = _state.businessBotId != 0 && _state.businessBotId == secretaryId;
        const std::wstring secretaryName = _secretaryUsername.empty() ? std::wstring{ L"the secretary bot" } : L"@" + _secretaryUsername;
        if (!_secretaryProblem.empty())
        {
            line = L"Secretary bot: " + _secretaryProblem + L".";
        }
        else if (_state.businessMode == 1)
        {
            line = L"Secretary: your prompts appear as your own messages (sent by " + (viaSecretary ? secretaryName : std::wstring{ L"this bot" }) + L").";
        }
        else if (_state.businessMode == -1)
        {
            line = L"Secretary: Telegram refused to send as you (" + _state.businessNote + L")" +
                   (_secretaryOn.load() ? std::wstring{ L"." } : std::wstring{ L"; a second bot as the secretary can (the steps below)." });
        }
        else if (!_state.businessConnectionId.empty() && _state.businessEnabled && !_state.businessCanReply)
        {
            line = L"Secretary: connected without the right to reply \x2014 allow it under Telegram \x2192 Settings \x2192 Chat Automation.";
        }
        else if (!_state.businessConnectionId.empty() && _state.businessEnabled)
        {
            line = L"Secretary: connected; checking with one message in the \U0001F50E Search topic.";
        }
        else if (_secretaryOn.load())
        {
            line = L"Secretary: " + secretaryName + L" is running \x2014 connect it under Telegram \x2192 Settings \x2192 Chat Automation (allow it to reply).";
        }
        else
        {
            line = L"Secretary: off (optional \x2014 your prompts show as the bot's \x2328 lines).";
        }
        return _status + L"\n" + line;
    }

    void MessagingService::SetSearchFactory(SearchBridgeFactory factory) noexcept
    {
        try
        {
            std::lock_guard lk{ _mtx };
            _searchFactory = std::move(factory);
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::SetSearchFactory");
        }
    }

    void MessagingService::SetLaunchMenuSource(LaunchMenuSource source) noexcept
    {
        try
        {
            std::lock_guard lk{ _mtx };
            _launchMenuSource = std::move(source);
        }
        catch (...)
        {
            // Rule #18. Recovery: the model step lists Default and a typed id only (the menu's models and presets are missing).
            LogSwallowedException(L"MessagingService::SetLaunchMenuSource");
        }
    }

    void MessagingService::Apply(const AppSettings& settings) noexcept
    {
        try
        {
            _idleGraceMs.store(static_cast<int64_t>(ClampMessagingIdleGraceMinutes(settings.messagingIdleGraceMinutes)) * 60 * 1000);
            // §21.11: the default mode of the topics and the rich-message switch apply to the next message (live).
            _defaultVerbose.store(settings.messagingVerbose);
            _richMessages.store(settings.messagingRichMessages);
            // §21.39: 📜 All Screen's default level, live: the next bare /allscreen draws it, and the next render of a card's level
            // picker marks it (an empty or unknown word reads as Full).
            _allScreenLevel.store(static_cast<int>(AllScreenLevelSetting(settings.messagingAllScreenLevel)));
            // §21.42: whether the cards carry the terminal's screen, live: the next card update reads (or no longer reads) it.
            _cardPictures.store(settings.messagingCardScreenshots);
            int64_t owner = 0;
            for (const wchar_t c : settings.messagingTelegramOwnerId)
            {
                if (c >= L'0' && c <= L'9' && owner < 100000000000000ll)
                {
                    owner = owner * 10 + (c - L'0');
                }
            }
            {
                std::lock_guard lk{ _mtx };
                _ownerId = owner;
                // §21.25: the new-thread wizard's model step lists the launch models of THESE settings (a cog Save reaches the
                // next wizard that enters the step), and its Default names the settings model.
                _launchModelsSpec = settings.launchModels;
                _settingsModel = settings.model;
                _searchThinkingWord = settings.messagingSearchThinking.empty() ? L"high" : settings.messagingSearchThinking;
                // The Search conversation is the process's (§21.7): made once, it is never made again with a new
                // level, so a Save must reach it live. Under `_mtx`, which also orders it after the poll thread's
                // creation of the bridge; the agent calls no listener back into this service for it (its StateChanged
                // event is ignored by the bridge), so holding the lock is safe.
                if (_search)
                {
                    _search->SetThinkingLevel(_searchThinkingWord);
                }
            }
            // §21.32.4: the FILE LINKS' settings, before any return below — so a later start of the bot finds them, and a
            // running bot's server follows a changed mode or port at once (Messaging.Files.cpp).
            _filesApply(settings);
            // §21.43: the VOICE NOTES' settings, before any return below too — a later start configures its transcriber with
            // them, and a running one takes a changed engine at its next note (Messaging.Voice.cpp).
            _voiceApply(settings);
            if (!settings.messagingEnabled)
            {
                if (_running.load())
                {
                    _log(L"[msg] messaging switched off: the bot stops");
                }
                Stop();
                _setStatus(L"Off.");
                return;
            }
            const std::wstring token = ReadStoredToken(_profile);
            if (token.empty())
            {
                Stop();
                _setStatus(L"On, but no bot token is stored: paste the token from @BotFather and Save.");
                return;
            }
            if (owner == 0)
            {
                Stop();
                _setStatus(L"On, but nobody is paired: enter your Telegram user id (numeric) and Save.");
                return;
            }
            if (_running.load())
            {
                // A live reconfiguration: the owner / grace / thinking word / topic mode / rich switch were taken above; a
                // token change needs a restart of the poller (the token is baked into the client) — the SECRETARY bot's
                // too (§21.11): a Save that stored, replaced or forgot it restarts the service here.
                const std::wstring secretary = ReadStoredSecretaryToken(_profile);
                if ((secretary.empty() ? std::wstring{} : TokenHash(secretary)) != _secretaryTokenHash)
                {
                    _log(L"[msg] the secretary bot token changed: the service restarts to pick it up");
                    Stop();
                    _start(token);
                    return;
                }
                _tickCv.notify_all();
                return;
            }
            _start(token);
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::Apply");
        }
    }

    void MessagingService::_start(std::wstring token) noexcept
    {
        try
        {
            // M7 — one poller per token, machine-wide: Telegram answers a second concurrent getUpdates
            // with 409 and the two readers steal each other's updates. The mutex is named by a hash of the
            // token (never the token).
            const std::wstring mainHash = TokenHash(token);
            const std::wstring mutexName = L"Global\\Agentmaster-telegram-" + mainHash;
            HANDLE m = ::CreateMutexW(nullptr, TRUE, mutexName.c_str());
            if (m && ::GetLastError() == ERROR_ALREADY_EXISTS)
            {
                ::CloseHandle(m);
                _setStatus(L"Stopped: another Agentmaster instance (or program) is already polling this bot. One bot per install.");
                _log(L"[msg] not started: another process holds the poller mutex for this bot token");
                return;
            }
            if (!m)
            {
                _log(L"[msg] poller mutex could not be created (error " + std::to_wstring(::GetLastError()) + L"); starting anyway");
            }
            _tokenMutex = m;

            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path{ _stateFile }.parent_path(), ec);
            _loadState();
            {
                std::lock_guard lk{ _mtx };
                for (auto& kv : _state.topics)
                {
                    // An "in flight" marker (-1) that a stop mid-send left in the file would wait for an answer that
                    // never comes: no send is in flight at a start, so a card or work log marked so is simply absent.
                    if (kv.second.cardMessageId < 0)
                    {
                        kv.second.cardMessageId = 0;
                    }
                    if (kv.second.workLogMessageId < 0)
                    {
                        kv.second.workLogMessageId = 0;
                    }
                }
                // A business connection stored without a verdict (the app closed before the probe ran) is probed
                // once the Search topic exists.
                _probeWanted.store(_state.businessEnabled && _state.businessCanReply && _state.businessMode == 0 && !_state.businessConnectionId.empty());
            }
            _secretaryFailures.store(0);
            _stop.store(false);
            _api = std::make_unique<TelegramApi>(std::move(token));
            // The SECRETARY bot (§21.11), when its token is stored: a client and a poller of its own (one poller per token,
            // machine-wide, like the topics bot's), started with the other threads below.
            {
                std::wstring secretary = ReadStoredSecretaryToken(_profile);
                _secretaryTokenHash = secretary.empty() ? std::wstring{} : TokenHash(secretary);
                _secretaryBotId.store(0);
                {
                    std::lock_guard lk{ _mtx };
                    _secretaryUsername.clear();
                    _secretaryProblem.clear();
                }
                if (!secretary.empty() && _secretaryTokenHash == mainHash)
                {
                    _log(L"[msg] the secretary bot token is the topics bot's own token: no second bot is started");
                    std::lock_guard lk{ _mtx };
                    _secretaryProblem = L"its token is the topics bot's own \x2014 the secretary must be a second bot";
                }
                else if (!secretary.empty())
                {
                    const std::wstring secretaryMutex = L"Global\\Agentmaster-telegram-" + _secretaryTokenHash;
                    HANDLE sm = ::CreateMutexW(nullptr, TRUE, secretaryMutex.c_str());
                    if (sm && ::GetLastError() == ERROR_ALREADY_EXISTS)
                    {
                        ::CloseHandle(sm);
                        _log(L"[msg] secretary bot not started: another process holds its poller mutex");
                        std::lock_guard lk{ _mtx };
                        _secretaryProblem = L"another Agentmaster instance (or program) already polls it";
                    }
                    else
                    {
                        _secretaryMutex = sm;
                        _secretaryApi = std::make_unique<TelegramApi>(std::move(secretary));
                        _secretaryOn.store(true);
                    }
                }
            }
            _topicsEnabled.store(false);
            _botId.store(0); // re-arms the poll loop's identity gate: this token may name another bot than the last run's
            {
                // §21.17: no title links until this token's getMe names its bot (the old username could name another bot).
                std::lock_guard linkLock{ _linkBotMtx };
                _linkBotName.clear();
            }
            _lastGetMeMs = 0;
            _startNotePosted = false;
            _stateIconRefused.store(false); // §21.22: every start tries the state icons again (one refused call when Telegram still refuses)
            _startedMs = WallMs(); // the start-up hold on "closed" deletes counts from here
            _startupHoldLogged = false;
            // §21.18: count this start (only the first one of the process, at its launch, rings — ClassifyStartNote), re-arm
            // the start note, and read the app's version word for the note and the run marker HERE, before the threads run,
            // so the tick and send threads read it without a lock. A hook that throws costs the version, never the start.
            ++_botStarts;
            _startNoteState.store(kStartNoteNone);
            _startNoteAttempts.store(0);
            _startNoteRetryAtMs.store(0);
            _appClosingAtMs.store(0);
            _searchCreateRetryAtMs = 0;
            _versionWord.clear();
            if (_hooks.versionWord)
            {
                try
                {
                    _versionWord = _hooks.versionWord();
                }
                catch (...)
                {
                    LogSwallowedException(L"MessagingService::_start (versionWord)"); // Rule #18; recovery: no version word
                }
            }
            {
                std::lock_guard lk{ _mtx };
                _mirrors.clear();
                _cards.clear();
                _reacted.clear();
                _steers.clear(); // §21.33: a watch of the last run names a send this run never made
                _menuWatches.clear(); // §21.34: a menu watch of the last run names a card this run never posted
                _outbox.clear();
                _dirty.clear();
                _seenLive.clear();
                _secretaryEchoes.clear();
                // The HISTORY replay (§21.9): no batch is in flight at a start (the outbox is empty), so a batch of the last
                // run that was not committed is read again. A replay found pending is finished — and, when it was a
                // recreation's (or any replay: the list is in replay order then), the list is re-sorted after it.
                _backfill = BackfillBatch{};
                _backfillAfterRecreation = false;
                for (const auto& kv : _state.topics)
                {
                    _backfillAfterRecreation = _backfillAfterRecreation || TopicBackfillPending(kv.second);
                }
                _backfillPostsBy.clear();
                _replayPosts = 0;
                _replayTopics = 0;
                _resortFull = false;
                _lastResortMs = 0;
                _resortTooLongLogged = false;
                _resortQueue.clear();
                _runningIds.clear();
                // §21.16.7: a dot's hold and its pending requests are this run's (the order keys are rebuilt by the first sync).
                _frontHeld.clear();
                _frontRequests.clear();
                _frontTapMs.clear();
                // §21.25: the new threads' wizards and launch binds come back from the state file (MessagingState::newThreads), so a
                // restart — of the bot or of the app — resumes each wizard where it stood, its picker's buttons still mapping to
                // what they showed, and a launch still binds its thread when its session comes up. The file is this token's bot's
                // as far as the start knows: the identity gate's reset (a different bot at the first getMe) clears them with the
                // topics before any update or tick can use them. A restored bind's clock starts now (NewThreadsFromJson), and the
                // window steps that waited for a census ask again once the windows have restored (_newThreadsTick). Before §21.25
                // both were cleared here: a picker's thread got a new picker with its next message, and a bind's session became an
                // ordinary new tab with a topic of its own.
                _newThreads.clear();
                _threadBinds.clear();
                try
                {
                    NewThreadsFromJson(_state.newThreads, _newThreads, _threadBinds, WallMs());
                }
                catch (...)
                {
                    // Rule #18. Recovery: no wizard is restored (their threads get new pickers with their next messages; a bind's
                    // session becomes an ordinary new tab) — the old behavior.
                    LogSwallowedException(L"MessagingService::_start (restore the new-thread wizards)");
                    _newThreads.clear();
                    _threadBinds.clear();
                }
                for (auto& [threadId, nt] : _newThreads)
                {
                    nt.pickerStale = false; // no post is in flight at a start
                    nt.wizard.windowsSerial = 0; // no census is in flight either: _newThreadsTick asks again
                }
                _newThreadsResumePending = !_newThreads.empty();
                if (!_newThreads.empty() || !_threadBinds.empty())
                {
                    // A log line under `_mtx` is avoided elsewhere (a file write); here none of the service's threads runs yet
                    // (they start below), so nothing waits on the lock meanwhile.
                    _log(L"[msg] new threads: " + std::to_wstring(_newThreads.size()) + L" wizard(s) and " + std::to_wstring(_threadBinds.size()) +
                         L" launch(es) restored from the state file");
                }
                _recreateStartedMs = 0;
                // The DIRECTORY (§21.16): the outbox was cleared above, so no send of the last run is in flight; a new serial makes
                // any answer still on its way stale. What the message shows stays in the state file (no edit for an unchanged
                // content), and a pin the last run could not make is tried once more.
                _directoryCheckedMs = 0;
                _directoryInFlight = false;
                _directoryInFlightSinceMs = 0;
                ++_directorySerial;
                _directoryLastSendMs = 0;
                _directoryNotBeforeMs = 0;
                _directoryRichRefusedHash = 0;
                _directoryPinTried = false;
                _directoryEditLogged = false;
                // §21.28: a new thread's tap holds are the last run's (no launch of this run has a token yet). Letting them go here,
                // under `_mtx`, only queues their taps for the send loop (OnTapHoldsGone takes the leaf `_tapMtx`), which the next
                // step forgets.
                _bindTapHolds.clear();
            }
            {
                // §21.28: the last run's tapped buttons are past Telegram's limit by now, and the send thread that would have answered
                // them is gone: each is marked answered — so a hold that goes later (a window's reply still on its way) queues no
                // answer Telegram would refuse — and forgotten, with the ones the outbox's clear above just let go.
                std::lock_guard lk{ _tapMtx };
                for (const auto& tap : _openTaps)
                {
                    tap->TryClaimAnswer();
                }
                _openTaps.clear();
                _tapsDue.clear();
                _tapAnswersDue.store(false);
            }
            // The registry notifies every change of every session; the service only marks the session
            // dirty and wakes the tick (never any work on the notifying thread).
            {
                std::weak_ptr<MessagingService> weak = weak_from_this();
                _registryObserver = _registry->AddObserver([weak](const SessionInfo& s, HookEvent) {
                    if (auto self = weak.lock())
                    {
                        self->_onRegistryNotify(s);
                    }
                });
            }
            // §21.30: the question registry's acceptor (Messaging.Questions.cpp) — before `_running` turns true, so it refuses
            // until the bot runs, and before the threads, so the tables it clears are not in use yet.
            _questionsStart();
            // §21.32.4: the FILE LINKS — the linked set read once per process, the secret and the server when they are on
            // (Messaging.Files.cpp). Before the threads, so the mirror's first pass already rewrites links; never throws.
            _filesStart();
            // §21.43: the VOICE NOTES' transcriber (the factory's) and its thread — before the poll thread, so the first update can
            // already hand a voice note over (Messaging.Voice.cpp); never throws.
            _voiceStart();
            _running.store(true);
            _setStatus(L"Starting\x2026");
            _pollThread = std::thread([this]() noexcept { _pollLoop(); });
            _sendThread = std::thread([this]() noexcept { _sendLoop(); });
            _tickThread = std::thread([this]() noexcept { _tickLoop(); });
            if (_secretaryApi)
            {
                _secretaryThread = std::thread([this]() noexcept { _secretaryPollLoop(); });
            }
            _log(L"[msg] messaging started (owner " + std::to_wstring(_ownerId) + L", idle grace " + std::to_wstring(_idleGraceMs.load() / 60000) + L" min, topics " +
                 (_defaultVerbose.load() ? L"verbose" : L"conversation only") + L" by default, " + (_richMessages.load() ? L"rich messages" : L"HTML messages") +
                 (_secretaryApi ? L", a secretary bot" : L"") + L")");
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_start");
            _running.store(false);
            _setStatus(L"Failed to start (see hooks.log).");
        }
    }

    void MessagingService::Stop() noexcept
    {
        try
        {
            if (!_running.exchange(false))
            {
                // Threads are down; a token mutex from a refused start is already released.
                return;
            }
            _stop.store(true);
            _tickCv.notify_all();
            _sendCv.notify_all();
            if (_api)
            {
                _api->Abort(); // the long poll returns at once
            }
            if (_secretaryApi)
            {
                _secretaryApi->Abort(); // the secretary's long poll too (§21.11)
            }
            // §21.43: the voice thread goes before the other joins — its engine is killed first, so a transcription in flight
            // fails fast instead of holding the stop (Messaging.Voice.cpp); it uses `_api`, which goes only at the very end.
            _voiceStop();
            if (_registryObserver)
            {
                _registry->RemoveObserver(_registryObserver);
                _registryObserver = 0;
            }
            if (_pollThread.joinable())
            {
                _pollThread.join();
            }
            if (_sendThread.joinable())
            {
                _sendThread.join();
            }
            if (_tickThread.joinable())
            {
                _tickThread.join();
            }
            if (_secretaryThread.joinable())
            {
                _secretaryThread.join();
            }
            // §21.30: no acceptor any more, and every open question released ("{}"): the PC keeps each dialog while the bot is off.
            _questionsStop();
            // §21.32.4: the file links' server stops with the bot, and the linked set is saved.
            _filesStop();
            _saveStateIfDirty(true);
            // A clean stop removes the run marker (a marker found at the next start means a crash).
            ::DeleteFileW((_profile + L"\\messaging\\run.marker").c_str());
            if (_tokenMutex)
            {
                ::ReleaseMutex(static_cast<HANDLE>(_tokenMutex));
                ::CloseHandle(static_cast<HANDLE>(_tokenMutex));
                _tokenMutex = nullptr;
            }
            {
                std::lock_guard lk{ _mtx };
                if (_search)
                {
                    _search->Park(0);
                }
                // Detaches this start's sink only: the Search conversation is the process's and lives on (parked), so
                // the next start of the bot continues it (§21.7).
                _search.reset();
            }
            _api.reset();
            // The secretary's client goes after every thread that could use it is joined (the send loop uses it).
            _secretaryOn.store(false);
            _secretaryApi.reset();
            _secretaryBotId.store(0);
            if (_secretaryMutex)
            {
                ::ReleaseMutex(static_cast<HANDLE>(_secretaryMutex));
                ::CloseHandle(static_cast<HANDLE>(_secretaryMutex));
                _secretaryMutex = nullptr;
            }
            _log(L"[msg] messaging stopped");
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::Stop");
        }
    }

    void MessagingService::_onRegistryNotify(const SessionInfo& s) noexcept
    {
        try
        {
            {
                std::lock_guard lk{ _mtx };
                _dirty[s.id] = WallMs();
                _tickRequested = true;
            }
            _tickCv.notify_all();
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_onRegistryNotify");
        }
    }

    // ===== the token (Credential Manager) ==================================================

    std::wstring MessagingService::TokenCredentialTarget(std::wstring_view profileDir)
    {
        std::wstring leaf = Leaf(profileDir);
        if (leaf.empty())
        {
            leaf = L"default";
        }
        return L"Agentmaster:messaging:telegram:" + leaf;
    }

    std::wstring MessagingService::ReadStoredToken(std::wstring_view profileDir) noexcept
    {
        try
        {
            return ReadCredentialToken(TokenCredentialTarget(profileDir));
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::ReadStoredToken");
            return {};
        }
    }

    bool MessagingService::StoreToken(std::wstring_view profileDir, std::wstring_view token) noexcept
    {
        try
        {
            return StoreCredentialToken(TokenCredentialTarget(profileDir), token, L"bot token");
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::StoreToken");
            return false;
        }
    }

    bool MessagingService::ClearToken(std::wstring_view profileDir) noexcept
    {
        try
        {
            return ClearCredentialToken(TokenCredentialTarget(profileDir));
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::ClearToken");
            return false;
        }
    }

    std::wstring MessagingService::SecretaryTokenCredentialTarget(std::wstring_view profileDir)
    {
        std::wstring leaf = Leaf(profileDir);
        if (leaf.empty())
        {
            leaf = L"default";
        }
        return L"Agentmaster:messaging:telegram-secretary:" + leaf;
    }

    std::wstring MessagingService::ReadStoredSecretaryToken(std::wstring_view profileDir) noexcept
    {
        try
        {
            return ReadCredentialToken(SecretaryTokenCredentialTarget(profileDir));
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::ReadStoredSecretaryToken");
            return {};
        }
    }

    bool MessagingService::StoreSecretaryToken(std::wstring_view profileDir, std::wstring_view token) noexcept
    {
        try
        {
            return StoreCredentialToken(SecretaryTokenCredentialTarget(profileDir), token, L"secretary bot token");
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::StoreSecretaryToken");
            return false;
        }
    }

    bool MessagingService::ClearSecretaryToken(std::wstring_view profileDir) noexcept
    {
        try
        {
            return ClearCredentialToken(SecretaryTokenCredentialTarget(profileDir));
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::ClearSecretaryToken");
            return false;
        }
    }

    MessagingService::BotCheck MessagingService::CheckBot(std::wstring_view token) noexcept
    {
        BotCheck c;
        try
        {
            std::wstring t{ token };
            const size_t a = t.find_first_not_of(L" \t\r\n");
            if (a == std::wstring::npos)
            {
                c.error = L"no token";
                return c;
            }
            const size_t b = t.find_last_not_of(L" \t\r\n");
            TelegramApi api{ t.substr(a, b - a + 1) };
            const TgResult r = api.Call(L"getMe", json::Value::MkObj(), 15000);
            if (!r.ok)
            {
                c.error = r.httpStatus == 401 ? std::wstring{ L"Telegram refused the token (401 Unauthorized)" } : (r.description.empty() ? std::wstring{ L"no answer" } : r.description);
                return c;
            }
            c.ok = true;
            c.username = L"@" + r.result.StrAt(L"username");
            c.topicsEnabled = r.result.BoolAt(L"has_topics_enabled", false);
            c.usersMayCreateTopics = r.result.BoolAt(L"allows_users_to_create_topics", false);
            return c;
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::CheckBot");
            c.error = L"exception";
            return c;
        }
    }

    // ===== the state file ====================================================================

    MessagingStateFileLoad LoadMessagingStateFile(const std::wstring& stateFile)
    {
        MessagingStateFileLoad out;
        // The startup budget: the topic map is read once per bot start, and every topic the bot keeps depends on it.
        const StateFileRead read = ReadStateFileUtf8(stateFile, kStartupStateReadRetryBudgetMs);
        if (read.status == StateFileStatus::Missing)
        {
            return out; // the bot's first start in this profile: no topics yet, nothing to keep
        }
        std::wstring why;
        if (read.status == StateFileStatus::Read)
        {
            if (const auto doc = json::Parse(read.text))
            {
                if (auto s = StateFromJson(*doc))
                {
                    out.state = std::move(*s);
                    out.fromFile = true;
                    // The save's no-op gate compares against these bytes. Re-encoded from the text (a BOM the reader dropped
                    // is not part of it): for every file this service wrote they are the file's own bytes; for any other,
                    // the worst case is one save that writes what the file already says.
                    out.fileBytes = Profiles::detail::WideToUtf8(read.text);
                    // Loaded: the guard's first replace of it needs no second read (MarkStateFileSafeToReplace's contract).
                    MarkStateFileSafeToReplace(stateFile);
                    return out;
                }
                why = L"it is JSON but not a state object this build reads";
            }
            else
            {
                why = L"its content is not valid JSON (a torn or damaged write)";
            }
            // Read but not usable: the saves would write a map that did not come from it, so its first replace keeps a copy
            // of it ("state.json.could-not-read-<stamp>"). (An UNREADABLE read was marked by the reader itself.)
            MarkStateFileKeepBeforeReplace(stateFile);
        }
        else
        {
            why = L"it could not be opened or read (Windows error " + std::to_wstring(read.error) + L")";
        }
        // state.json is there but this start cannot use it: the previous copy (refreshed at most once a minute by the saves,
        // from a state.json this service could read — StatePrevCopyDue) is the topic map to run with. Without it every topic
        // would become an orphan the bot cannot find (§10.5), a new one would be made for each tab, and the first save would
        // write that empty map over state.json — which the keep mark now copies aside first in every case.
        const StateFileRead prev = ReadStateFileUtf8(stateFile + L".prev", kStartupStateReadRetryBudgetMs);
        if (prev.status == StateFileStatus::Read)
        {
            if (const auto doc2 = json::Parse(prev.text))
            {
                if (auto s = StateFromJson(*doc2))
                {
                    out.state = std::move(*s);
                    out.fromPrevious = true;
                    out.note = L"[msg] state.json could not be used (" + why +
                               L"); the previous copy (state.json.prev) was used, and state.json is kept aside before it is next written";
                    return out;
                }
            }
        }
        out.note = L"[msg] state.json could not be used (" + why +
                   L") and no previous copy could be read: starting with no topic map; state.json is kept aside before it is next written";
        return out;
    }

    void MessagingService::_loadState()
    {
        // PERSISTENCE.md §16: the guarded load (LoadMessagingStateFile) — it tells a missing state.json from one that cannot be
        // read, parsed or understood, falls back to the backup for all three (it used to for a torn file only, and a LOCKED
        // file read as "no topics" and was then saved over with an empty map), and marks a file it could not use so the guard
        // keeps a copy of it before the first save replaces it.
        MessagingStateFileLoad load = LoadMessagingStateFile(_stateFile);
        if (!load.note.empty())
        {
            _log(load.note);
        }
        MessagingState loaded = std::move(load.state);
        const bool loadedFromFile = load.fromFile; // the state came from state.json itself (not the backup, not empty)
        const std::string bytes = std::move(load.fileBytes); // state.json's bytes when loadedFromFile, else empty
        {
            // What the file holds now: a save that would write exactly these bytes writes nothing (_saveStateIfDirty). A state
            // taken from the backup, or a fresh one, has no such bytes, so its first save always writes. A new bot start also
            // refreshes the backup at its first save (StatePrevCopyDue's 0) — but only from a state.json this run could READ:
            // copying a torn (or unknown) file over the backup would destroy the one readable version left, so after such a
            // load the backup waits until a save has written a good file.
            std::lock_guard saveLk{ _stateSaveMtx };
            _lastSavedStateBytes = loadedFromFile ? bytes : std::string{};
            _lastPrevCopyMs = 0;
            _stateFileReadable = loadedFromFile;
        }
        std::lock_guard lk{ _mtx };
        _state = std::move(loaded);
        _stateDirty = false;
        _stateProgressDirty = false;
    }

    void MessagingService::_saveStateIfDirty(bool force, int64_t lockWaitMs)
    {
        // ONE WRITER at a time (`_stateSaveMtx`, taken before `_mtx`): the tick, Stop, the topic recreation and the app's
        // last window (NoteAppClosing, on another thread) all save through here, and two of them writing state.json.tmp at
        // once would make one fail. The app-close flush runs under the engine's window mutex and must never wait on the
        // service's locks without a bound: it passes kAppCloseSaveLockWaitMs and gives up when a lock stays held that long
        // (logged; the state then keeps what the last save wrote).
        std::unique_lock saveLk{ _stateSaveMtx, std::defer_lock };
        if (!AcquireLockWithin(saveLk, lockWaitMs))
        {
            _log(L"[msg] state.json was not flushed: another save held the file for " + std::to_wstring(lockWaitMs) + L" ms");
            return;
        }
        json::Value doc;
        int64_t now = 0;
        {
            std::unique_lock lk{ _mtx, std::defer_lock };
            if (!AcquireLockWithin(lk, lockWaitMs))
            {
                _log(L"[msg] state.json was not flushed: the service's lock stayed held for " + std::to_wstring(lockWaitMs) + L" ms");
                return;
            }
            now = WallMs();
            // A structural change goes within kStateSaveGapMs, the mirror's progress alone within kStateProgressSaveGapMs.
            if (!StateSaveDue(_stateDirty, _stateProgressDirty, now, _lastStateSaveMs, force, kStateSaveGapMs, kStateProgressSaveGapMs))
            {
                return;
            }
            // §21.25: the new-thread wizards and launches go into the file with the topics (one atomic write: a crash leaves
            // either the old file or the new one, never a topic map from one moment and the wizards from another). An empty set
            // writes no key (NewThreadsToJson's object is dropped), so a state file without wizards reads as before §21.25.
            _state.newThreads = (_newThreads.empty() && _threadBinds.empty()) ? json::Value{} : NewThreadsToJson(_newThreads, _threadBinds);
            doc = StateToJson(_state);
            _stateDirty = false;
            _stateProgressDirty = false;
            _lastStateSaveMs = now;
        }
        std::string bytes = Profiles::detail::WideToUtf8(json::Dump(doc));
        // A save of the bytes the file already holds writes nothing — no copy, no write, no flush. Several paths mark the state
        // dirty with values that did not change (a stamp written again, a flag set again); this gate makes them cost a
        // serialization at most, never a disk write.
        if (bytes == _lastSavedStateBytes)
        {
            return;
        }
        // Keep a previous copy: the Bot API cannot list topics, so this file is their only record. Refreshed at most once a
        // minute (StatePrevCopyDue): it only has to be a recent readable version for a state.json that cannot be read. Never
        // from a file this run could not read (`_stateFileReadable`, set by _loadState and by every good write below).
        if (_stateFileReadable && StatePrevCopyDue(now, _lastPrevCopyMs, kStatePrevCopyGapMs))
        {
            if (::CopyFileW(_stateFile.c_str(), (_stateFile + L".prev").c_str(), FALSE))
            {
                _lastPrevCopyMs = now;
            }
        }
        // PERSISTENCE.md §16: the state-file guard before the replace, as for every other state file — it refuses a state.json
        // it cannot read, and keeps a copy of one this start could not use (LoadMessagingStateFile marked it) or that does not
        // parse, before the map in memory replaces it. A refusal is a failed write: the state stays dirty and the next tick
        // tries again (the guard logged the refusal, once per quiet window).
        const bool guardAllows = PrepareStateFileReplace(_stateFile);
        if (!guardAllows || !WriteFileAtomic(_stateFile, bytes))
        {
            if (guardAllows)
            {
                _log(L"[msg] state.json could not be written (error " + std::to_wstring(::GetLastError()) + L")");
            }
            // Dirty again, so the next tick tries again (a STRUCTURAL mark, so it retries within kStateSaveGapMs). The bounded
            // wait of the app-close flush applies here too: the process ends next, and the mark only matters if it does not.
            // `_lastSavedStateBytes` keeps the old file's bytes, which is what a failed atomic write leaves on disk.
            std::unique_lock lk{ _mtx, std::defer_lock };
            if (AcquireLockWithin(lk, lockWaitMs))
            {
                _stateDirty = true;
            }
            return;
        }
        _lastSavedStateBytes = std::move(bytes);
        _stateFileReadable = true;
        // The file holds what this service just wrote: stamp it, so the guard's next check of it is one stat until someone
        // else changes it (and a keep mark from the load is spent: the copy was made before this write).
        MarkStateFileSafeToReplace(_stateFile);
    }

    // ===== the threads =======================================================================

    void MessagingService::_pollLoop() noexcept
    {
        int64_t backoffMs = 1000;
        while (!_stop.load())
        {
            try
            {
                // The IDENTITY GATE: no getUpdates before getMe has named the bot (the tick loop asks it first
                // thing). Update ids are per bot, and an offset carried over from ANOTHER bot's state file
                // would confirm — silently drop — every update of this bot below it, for good. The tick loop
                // resets the state when the bot id changed and only then sets _botId, which opens this gate.
                if (_botId.load() == 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    continue;
                }
                auto params = json::Value::MkObj();
                int64_t offset = 0;
                {
                    std::lock_guard lk{ _mtx };
                    offset = _state.updateOffset;
                }
                if (offset > 0)
                {
                    params.Set(L"offset", json::Value::MkNum(static_cast<double>(offset)));
                }
                params.Set(L"timeout", json::Value::MkNum(static_cast<double>(kPollTimeoutSec)));
                params.Set(L"limit", json::Value::MkNum(100));
                auto allowed = json::Value::MkArr();
                allowed.Push(json::Value::MkStr(L"message"));
                allowed.Push(json::Value::MkStr(L"edited_message"));
                allowed.Push(json::Value::MkStr(L"callback_query"));
                // The user's "Secretary mode" (Telegram Business → Chatbots): the connection and its rights arrive
                // ONLY as this update, when the user adds, edits or removes the bot there (§21.5).
                allowed.Push(json::Value::MkStr(L"business_connection"));
                params.Set(L"allowed_updates", std::move(allowed));
                const TgResult r = _api->Call(L"getUpdates", params, kPollReceiveTimeoutMs);
                if (_stop.load())
                {
                    break;
                }
                if (!r.ok)
                {
                    if (r.httpStatus == 409)
                    {
                        _setStatus(L"Stopped: Telegram says another poller reads this bot (409). One bot per install.");
                        _log(L"[msg] getUpdates 409 Conflict: another reader polls this bot; the poll loop ends");
                        return;
                    }
                    if (r.httpStatus == 401)
                    {
                        _setStatus(L"Stopped: Telegram refused the bot token (401). Paste a new token from @BotFather.");
                        _log(L"[msg] getUpdates 401: the token was refused; the poll loop ends");
                        return;
                    }
                    // Network loss, sleep, a 5xx: back off 1, 2, 4 … 60 s.
                    const int64_t wait = r.retryAfterSec > 0 ? static_cast<int64_t>(r.retryAfterSec) * 1000 : backoffMs;
                    backoffMs = std::min<int64_t>(backoffMs * 2, 60000);
                    for (int64_t slept = 0; slept < wait && !_stop.load(); slept += 250)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                    continue;
                }
                backoffMs = 1000;
                if (r.result.type == json::Value::Type::Arr)
                {
                    for (const auto& u : r.result.arr)
                    {
                        _handleUpdate(u);
                        if (_stop.load())
                        {
                            break;
                        }
                    }
                }
            }
            catch (...)
            {
                LogSwallowedException(L"MessagingService::_pollLoop");
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
    }

    void MessagingService::_secretaryPollLoop() noexcept
    {
        int64_t backoffMs = 1000;
        bool known = false;
        while (!_stop.load())
        {
            try
            {
                if (!known)
                {
                    // getMe first: the bot's id (its update ids are its own — a new secretary bot starts from offset 0) and
                    // whether Secretary Mode is on for it in @BotFather (getMe's can_connect_to_business).
                    const TgResult me = _secretaryApi->Call(L"getMe", json::Value::MkObj(), 15000);
                    if (_stop.load())
                    {
                        break;
                    }
                    if (!me.ok)
                    {
                        if (me.httpStatus == 401)
                        {
                            _log(L"[msg] secretary bot: Telegram refused its token (401); sends as the user stay off");
                            std::lock_guard lk{ _mtx };
                            _secretaryProblem = L"Telegram refused its token (401) \x2014 paste a new one";
                            return;
                        }
                        const int64_t wait = backoffMs;
                        backoffMs = std::min<int64_t>(backoffMs * 2, 60000);
                        for (int64_t slept = 0; slept < wait && !_stop.load(); slept += 250)
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(250));
                        }
                        continue;
                    }
                    const int64_t id = me.result.I64At(L"id", 0);
                    const std::wstring user = me.result.StrAt(L"username");
                    const bool canConnect = me.result.BoolAt(L"can_connect_to_business", false);
                    {
                        std::lock_guard lk{ _mtx };
                        if (id != 0 && _state.secretaryBotId != id)
                        {
                            _state.secretaryBotId = id;
                            _state.secretaryUpdateOffset = 0;
                            _stateDirty = true;
                        }
                        _secretaryUsername = user;
                        _secretaryProblem = canConnect ? std::wstring{} : L"Secretary Mode is off for @" + user + L" \x2014 turn it on in @BotFather (Bot Settings)";
                    }
                    _secretaryBotId.store(id);
                    _log(L"[msg] secretary bot @" + user + L" ready (Secretary Mode " + (canConnect ? L"on" : L"OFF \x2014 turn it on in @BotFather") + L")");
                    known = true;
                    backoffMs = 1000;
                }
                auto params = json::Value::MkObj();
                int64_t offset = 0;
                {
                    std::lock_guard lk{ _mtx };
                    offset = _state.secretaryUpdateOffset;
                }
                if (offset > 0)
                {
                    params.Set(L"offset", json::Value::MkNum(static_cast<double>(offset)));
                }
                params.Set(L"timeout", json::Value::MkNum(static_cast<double>(kPollTimeoutSec)));
                params.Set(L"limit", json::Value::MkNum(100));
                // ONLY the connection: a business bot would otherwise get every message of every chat it may access.
                auto allowed = json::Value::MkArr();
                allowed.Push(json::Value::MkStr(L"business_connection"));
                params.Set(L"allowed_updates", std::move(allowed));
                const TgResult r = _secretaryApi->Call(L"getUpdates", params, kPollReceiveTimeoutMs);
                if (_stop.load())
                {
                    break;
                }
                if (!r.ok)
                {
                    if (r.httpStatus == 409 || r.httpStatus == 401)
                    {
                        _log(L"[msg] secretary bot getUpdates " + std::to_wstring(r.httpStatus) + L": " + r.description + L"; its poll loop ends");
                        std::lock_guard lk{ _mtx };
                        _secretaryProblem = r.httpStatus == 409 ? L"another program polls it (409)" : L"Telegram refused its token (401)";
                        return;
                    }
                    const int64_t wait = r.retryAfterSec > 0 ? static_cast<int64_t>(r.retryAfterSec) * 1000 : backoffMs;
                    backoffMs = std::min<int64_t>(backoffMs * 2, 60000);
                    for (int64_t slept = 0; slept < wait && !_stop.load(); slept += 250)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                    continue;
                }
                backoffMs = 1000;
                if (r.result.type == json::Value::Type::Arr)
                {
                    for (const auto& u : r.result.arr)
                    {
                        const int64_t updateId = u.I64At(L"update_id", 0);
                        {
                            std::lock_guard lk{ _mtx };
                            if (updateId + 1 > _state.secretaryUpdateOffset)
                            {
                                _state.secretaryUpdateOffset = updateId + 1;
                                _stateDirty = true;
                            }
                        }
                        if (const auto* bc = u.Find(L"business_connection"); bc && bc->type == json::Value::Type::Obj)
                        {
                            _handleBusinessConnection(*bc, true);
                        }
                        if (_stop.load())
                        {
                            break;
                        }
                    }
                }
            }
            catch (...)
            {
                LogSwallowedException(L"MessagingService::_secretaryPollLoop");
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
    }

    void MessagingService::_sendLoop() noexcept
    {
        while (!_stop.load())
        {
            try
            {
                // §21.28: the tapped buttons whose work is done (their last hold went) and the ones past their hold's limit get their
                // answers queued first — every pass, so a limit is kept within a pass (0.5 s while the outbox is empty, 1.2 s while
                // it paces), and outside `_mtx`, which the answer's _enqueue takes.
                _answerDueTaps();
                // §21.42: a card that waited too long for its picture goes now without a new one (a frozen window must not stop it).
                _expireCardPictureWaits();
                OutItem item;
                int64_t waitMs = 0;
                {
                    std::unique_lock lk{ _mtx };
                    // §21.28: a tap whose last hold went wakes the wait too (OnTapHoldsGone): with an empty outbox the pass ends at
                    // once, and the next one queues the answer.
                    _sendCv.wait_for(lk, std::chrono::milliseconds(500), [this] { return _stop.load() || !_outbox.empty() || _tapAnswersDue.load(); });
                    if (_stop.load())
                    {
                        break;
                    }
                    if (_outbox.empty())
                    {
                        continue;
                    }
                    // The best item: lowest priority value, then the oldest — among the items whose method no long 429
                    // parks (_floodParking; the secretary bot's sends have their own): a parked method's items stay queued
                    // in their places until its deadline while every other method goes on.
                    const int64_t now = static_cast<int64_t>(::GetTickCount64());
                    auto best = _outbox.end();
                    int64_t parkedUntil = 0; // the earliest deadline among the skipped items: how long to wait when all are parked
                    for (auto it = _outbox.begin(); it != _outbox.end(); ++it)
                    {
                        if (const int64_t until = FloodParkedUntil(it->asSecretary ? _secretaryFloodParking : _floodParking, _outItemMethod(*it), now); until > 0)
                        {
                            parkedUntil = parkedUntil == 0 ? until : (std::min)(parkedUntil, until); // (std::min): the <windows.h> macro
                            continue;
                        }
                        if (best == _outbox.end() || static_cast<int>(it->prio) < static_cast<int>(best->prio) ||
                            (it->prio == best->prio && (it->seq < best->seq || (it->seq == best->seq && it->subSeq < best->subSeq))))
                        {
                            best = it;
                        }
                    }
                    if (best == _outbox.end())
                    {
                        // Every queued item waits for Telegram's flood control: sleep (in slices, below) until the first deadline.
                        waitMs = (std::max<int64_t>)(1, parkedUntil - now);
                    }
                    // A history message (§21.9) is only ever the best item when nothing else is queued; it waits the longer
                    // gap, goes only while the last minute holds fewer than kHistoryCallsPerMinute calls (the chat's flood
                    // limit is near 20 a minute, live traffic included), not for kHistoryFloodPauseMs after a 429, and not
                    // while a due topic RENAME waits for room (the rename goes first: HistoryYieldsToRename).
                    const bool history = best != _outbox.end() && best->prio == SendPriority::History;
                    const int64_t gap = best == _outbox.end() ? 0 : (history ? kBackfillGapMs : (best->paced ? kSendGapMs : kLightGapMs));
                    _noteCallLocked(0); // prunes the window only
                    if (best == _outbox.end())
                    {
                        // (waitMs set above)
                    }
                    else if (history && _floodAtMs != 0 && now - _floodAtMs < kHistoryFloodPauseMs)
                    {
                        waitMs = kHistoryFloodPauseMs - (now - _floodAtMs);
                    }
                    else if (history && HistoryYieldsToRename(_renameWaitingSinceMs, now))
                    {
                        // The next free call is the rename's: _stateNameTick tries again every kTickMs and clears the stamp.
                        waitMs = 500;
                    }
                    else if (history && _recentCallsMs.size() >= kHistoryCallsPerMinute)
                    {
                        waitMs = std::max<int64_t>(1, _recentCallsMs.front() + kCallWindowMs - now);
                    }
                    else if (_lastSendMs != 0 && now - _lastSendMs < gap)
                    {
                        waitMs = gap - (now - _lastSendMs);
                    }
                    else
                    {
                        item = std::move(*best);
                        _outbox.erase(best);
                        _lastSendMs = now;
                        _noteCallLocked(now);
                    }
                }
                if (waitMs > 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(waitMs, 1200)));
                    continue;
                }
                // §21.12: a final reply points at its turn's prompt here — the prompt was queued before it and has answered by now.
                if (item.beforeSend)
                {
                    try
                    {
                        item.beforeSend(item.params);
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"MessagingService::_sendLoop (beforeSend)"); // the item goes as it is
                    }
                }
                // One call with the client the item names: a send as the user through the SECRETARY bot (§21.11) uses its
                // client, which exists for the whole run when it was made (it is reset only after this thread is joined).
                const auto call = [this, &item]() {
                    if (!item.uploadField.empty())
                    {
                        // §21.42: a rich card post or edit with its NEW screenshot attached (the parameters name it
                        // "attach://<field>"): one multipart call, so the picture costs no call of its own.
                        return _api->CallWithFile(item.method, item.params, Profiles::detail::WideToUtf8(item.uploadField), "image/png", item.fileName, item.fileBytes, 60000);
                    }
                    if (item.document)
                    {
                        // §21.24: a picture goes up as a photo (the apps show it inline and zoomable); any other file as a document.
                        return item.photo ? _api->SendPhoto(item.params, item.fileName, item.fileBytes, 60000) :
                                            _api->SendDocument(item.params, item.fileName, item.fileBytes, 60000);
                    }
                    if (item.asSecretary)
                    {
                        if (!_secretaryApi)
                        {
                            TgResult none;
                            none.description = L"the secretary bot is not running";
                            return none;
                        }
                        return _secretaryApi->Call(item.method, item.params, 30000);
                    }
                    return _api->Call(item.method, item.params, 30000);
                };
                TgResult r = call();
                const std::wstring method = _outItemMethod(item);
                {
                    // The traffic census the tick loop logs every 10 minutes: one count per method, and the
                    // pacing refusals under "429" — the chat's real call rate, which no other log shows. Edits also by
                    // what they edit (_editsByKind), since an edit flood earned a 6542 s wait with no other trace.
                    std::lock_guard lk{ _mtx };
                    ++_trafficByMethod[method];
                    if (method == L"editMessageText")
                    {
                        ++_editsByKind[EditKindOfKey(item.key)];
                    }
                    if (!r.ok && r.httpStatus == 429)
                    {
                        ++_trafficByMethod[L"429"];
                    }
                }
                if (!r.ok && r.httpStatus == 429 && r.retryAfterSec > kFloodParkAfterSec)
                {
                    // A LONG wait (6542 s once, 2026-10-17): park only this method and put the item back in its place; every
                    // other method goes on, and a second method refused meanwhile parks them all (_parkForFlood). Not answered,
                    // so no onDone yet: the item goes again after the deadline. An edit of the same message queued while
                    // this one was in flight replaced it already (the key), and then this stale one is dropped.
                    _noteFlood(method, r.retryAfterSec);
                    _parkForFlood(method, r.retryAfterSec, item.asSecretary);
                    {
                        std::lock_guard lk{ _mtx };
                        const auto newer = item.key.empty() ? _outbox.end() :
                                                              std::find_if(_outbox.begin(), _outbox.end(), [&item](const OutItem& o) { return o.key == item.key; });
                        const bool superseded = newer != _outbox.end();
                        if (!superseded)
                        {
                            _outbox.push_back(std::move(item));
                        }
                        else
                        {
                            // §21.28: the newer item carries the change a tapped button waits for (the same message, a newer
                            // text), so it takes over this stale one's holds instead of letting the tap answer before it.
                            MoveTapHolds(item.tapHolds, newer->tapHolds);
                        }
                    }
                    continue;
                }
                if (!r.ok && r.httpStatus == 429)
                {
                    // A SHORT wait (up to kFloodParkAfterSec): Telegram's pacing verdict is waited out here, then the same
                    // item is retried once — the outbox's order is kept, and everything waits a moment.
                    const int64_t wait = std::max<int64_t>(1, r.retryAfterSec) * 1000;
                    _log(L"[msg] pacing: 429 on " + item.method + L", waiting " + std::to_wstring(wait / 1000) + L" s");
                    _noteFlood(item.method, r.retryAfterSec);
                    for (int64_t slept = 0; slept < wait && !_stop.load(); slept += 250)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                    {
                        std::lock_guard lk{ _mtx };
                        _lastSendMs = static_cast<int64_t>(::GetTickCount64());
                        _noteCallLocked(_lastSendMs); // the retry is a call into the chat too
                    }
                    r = call();
                }
                if (item.onDone)
                {
                    try
                    {
                        // §21.28: what the callback queues — a refused rich message sent again as HTML, a refused photo sent as
                        // a document, the next step of a chain — holds the same tapped buttons as this item (a scope never
                        // throws); the item's own holds go when it does, at the end of this pass.
                        const TapScope tapScope{ item.tapHolds };
                        item.onDone(r);
                    }
                    catch (...)
                    {
                        LogSwallowedException(L"MessagingService::_sendLoop (onDone)");
                    }
                }
            }
            catch (...)
            {
                LogSwallowedException(L"MessagingService::_sendLoop");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }

    void MessagingService::_tickLoop() noexcept
    {
        while (!_stop.load())
        {
            try
            {
                {
                    std::unique_lock lk{ _mtx };
                    // §21.23: a launch answer wakes the tick too (_onLaunchAnswered sets the flag without `_mtx`, so the phone hears
                    // a refusal at once instead of after the tick's period).
                    _tickCv.wait_for(lk, std::chrono::milliseconds(kTickMs), [this] { return _stop.load() || _tickRequested || _launchAnswersPending.load(); });
                    _tickRequested = false;
                }
                if (_stop.load())
                {
                    break;
                }
                // §21.23: the windows' answers to this service's launches first, every tick — before the sync, so a bind or a
                // revive the answer settles is seen settled by everything this tick does.
                _settleLaunchAnswers();
                const int64_t now = WallMs();
                // The run marker (§4.3, §21.18): rewritten every minute with the time and the version word — the previous
                // run's last copy is when that run was last seen alive and which version it was (the start note). Held for
                // kAppClosingMarkerHoldMs once the app's last window closed (NoteAppClosing), so the close stamp stays the
                // newest sign of this run.
                const int64_t closingAt = _appClosingAtMs.load();
                const bool appClosing = closingAt != 0 && now - closingAt < kAppClosingMarkerHoldMs;
                if (!appClosing && now - _lastMarkerMs >= kMarkerMs)
                {
                    const std::wstring marker = _profile + L"\\messaging\\run.marker";
                    if (_lastMarkerMs == 0)
                    {
                        // The first write of this run: a marker still on disk is the previous run's last minute alive (a
                        // clean Stop deletes it; an app quit never runs Stop — the app ends with TerminateProcess — so a quit
                        // leaves it too, and run.closed tells the two apart). Keep its content as `.last` for the start note
                        // the Search topic posts (_ensureSearchTopic reads and removes it).
                        const std::string prevText = ReadFileBytes(marker);
                        if (!prevText.empty())
                        {
                            WriteFileAtomic(marker + L".last", prevText);
                        }
                    }
                    WriteFileAtomic(marker, FormatRunMarker(now, _versionWord));
                    _lastMarkerMs = now;
                }
                // The traffic census: every call the send loop made since the last line, by method. Written
                // even when nothing was sent (a "quiet" line is evidence too); the first line comes 10 minutes
                // after the start.
                if (_lastTrafficLogMs == 0)
                {
                    _lastTrafficLogMs = now;
                }
                else if (now - _lastTrafficLogMs >= kTrafficLogMs)
                {
                    _lastTrafficLogMs = now;
                    std::map<std::wstring, uint32_t> census;
                    std::map<std::wstring, uint32_t> edits;
                    std::map<std::wstring, uint32_t> pictures; // §21.42: the card picture decisions of the window
                    uint64_t pictureBytes = 0;
                    {
                        std::lock_guard lk{ _mtx };
                        census.swap(_trafficByMethod);
                        edits.swap(_editsByKind);
                        pictures.swap(_cardPicturesByKind);
                        pictureBytes = _cardPictureBytes;
                        _cardPictureBytes = 0;
                    }
                    std::wstring line = L"[msg] traffic (" + std::to_wstring(kTrafficLogMs / 60000) + L" min):";
                    uint32_t total = 0;
                    for (const auto& [method, count] : census)
                    {
                        line += L" " + method + L"=" + std::to_wstring(count);
                        total += count;
                    }
                    line += census.empty() ? std::wstring{ L" nothing sent" } : L" (" + std::to_wstring(total) + L" calls)";
                    if (!edits.empty())
                    {
                        // The editMessageText calls above, by what they edited (not counted again in the total).
                        std::wstring parts;
                        for (const auto& [kind, count] : edits)
                        {
                            parts += (parts.empty() ? L"" : L" ") + kind + L"=" + std::to_wstring(count);
                        }
                        line += L" edits(" + parts + L")";
                    }
                    if (!pictures.empty())
                    {
                        // §21.42: what the cards' screenshots cost - "drawn" pictures were uploaded with their card's own call (their
                        // KB is the bandwidth), "reused" ones were an unchanged screen, "kept" ones an unreadable screen or a page flip.
                        std::wstring parts;
                        for (const auto& [kind, count] : pictures)
                        {
                            parts += (parts.empty() ? L"" : L" ") + kind + L"=" + std::to_wstring(count);
                        }
                        line += L" pictures(" + parts + L", " + std::to_wstring(pictureBytes / 1024) + L" KB drawn)";
                    }
                    _log(line);
                }
                // §21.32.4: the file links' housekeeping (the linked set saved and pruned, a server that could not start tried
                // again, the summary line); quick unless one of those is due.
                _filesTick(now);
                // §21.43: the voice lanes — a transcribed note read as its text, the messages that waited behind it handed over
                // in order (Messaging.Voice.cpp); quick when nothing is ready.
                _voiceTick();
                // Threaded Mode: topics need it; until it is on, getMe is re-asked every minute and the owner
                // gets one note with the switch's location.
                if (!_topicsEnabled.load())
                {
                    if (now - _lastGetMeMs >= kGetMeRetryMs)
                    {
                        _lastGetMeMs = now;
                        const TgResult r = _api->Call(L"getMe", json::Value::MkObj(), 15000);
                        if (r.ok)
                        {
                            const std::wstring user = r.result.StrAt(L"username");
                            const int64_t id = r.result.I64At(L"id", 0);
                            std::wstring previousBot; // non-empty = the state file belonged to another bot
                            {
                                std::lock_guard lk{ _mtx };
                                _botUsername = user;
                                {
                                    // §21.17: the title links' copy, behind its own leaf lock (_linkBot): the send paths read it
                                    // whatever their callers hold. A different bot's username replaces it with the rest below.
                                    std::lock_guard linkLock{ _linkBotMtx };
                                    _linkBotName = user;
                                }
                                // A DIFFERENT bot than the state file's (the token was replaced in the cog): the
                                // update offset, the Search topic and the topic map are the OLD bot's — update ids
                                // are per bot (a stale higher offset would drop every update of the new one), and
                                // the thread ids name topics that exist only in the old bot's chat. The per-session
                                // mirrors and reactions point into that chat too. The new bot starts from nothing.
                                if (_state.botId != 0 && id != 0 && _state.botId != id)
                                {
                                    previousBot = _state.botUsername.empty() ? std::to_wstring(_state.botId) : _state.botUsername;
                                    _state.updateOffset = 0;
                                    _state.searchThreadId = 0;
                                    _state.setupNoticeMs = 0;
                                    _state.topics.clear();
                                    // A business connection (secretary mode) is made to ONE bot: the new bot has none
                                    // until the user adds it under Telegram Business → Chatbots.
                                    _state.businessConnectionId.clear();
                                    _state.businessEnabled = false;
                                    _state.businessCanReply = false;
                                    _state.businessMode = 0;
                                    _state.businessNote.clear();
                                    _probeWanted.store(false);
                                    _mirrors.clear();
                                    _cards.clear();
                                    _reacted.clear();
                                    _steers.clear(); // §21.33: its watches name messages of the OLD bot's chat
                                    _menuWatches.clear(); // §21.34: and so do the menu watches and their cards
                                    // §21.16.7: the dots' holds and requests name the OLD bot's topics (this runs on the tick
                                    // thread, which owns _frontHeld).
                                    _frontHeld.clear();
                                    _frontRequests.clear();
                                    _frontTapMs.clear();
                                    _backfill = BackfillBatch{}; // its topics are the old bot's
                                    // §21.25: the new-thread wizards and their launches name threads of the OLD bot's chat.
                                    _newThreads.clear();
                                    _threadBinds.clear();
                                    // §21.28: and so do the holds of the taps that started those launches (letting them go here
                                    // only queues their taps for the send loop: OnTapHoldsGone takes the leaf `_tapMtx`).
                                    _bindTapHolds.clear();
                                    _state.newThreads = json::Value{};
                                    _newThreadsResumePending = false;
                                    // §21.16: the directory message is a message of the OLD bot's chat; the new bot posts its own.
                                    _state.directoryMessageId = 0;
                                    _state.directoryHash = 0;
                                    _state.directoryRich = false;
                                    _state.directoryPinned = false;
                                    _directoryInFlight = false;
                                    ++_directorySerial;
                                    _directoryNotBeforeMs = 0;
                                    _directoryRichRefusedHash = 0;
                                    _directoryPinTried = false;
                                    _stateDirty = true;
                                }
                                if (id != 0 && _state.botId != id)
                                {
                                    _state.botId = id;
                                    _stateDirty = true;
                                }
                                if (_state.botUsername != user)
                                {
                                    _state.botUsername = user;
                                    _stateDirty = true;
                                }
                            }
                            if (!previousBot.empty())
                            {
                                _log(L"[msg] the bot changed (@" + previousBot + L" -> @" + user + L"): the update offset, the Search topic and the topic map of the old bot were reset");
                            }
                            // Opens the poll loop's identity gate — AFTER the reset above, never before it.
                            if (id != 0)
                            {
                                _botId.store(id);
                            }
                            if (r.result.BoolAt(L"has_topics_enabled", false))
                            {
                                _topicsEnabled.store(true);
                                _setStatus(L"Connected as @" + user + L".");
                                _log(L"[msg] connected as @" + user + L" (topics enabled)");
                            }
                            else
                            {
                                // ⚠ The switch exists ONLY in the BotFather MINI APP (tap Open on @BotFather); the classic /mybots
                                // chat menu does not show Threads Settings (the user flipped Secretary Mode there on 2026-10-16,
                                // which is the business-bot setting, and topics stayed off).
                                _setStatus(L"Connected as @" + user + L", waiting for Threaded Mode: open the @BotFather MINI APP (tap Open on @BotFather) \x2192 My bots \x2192 @" + user + L" \x2192 Bot Settings \x2192 Threads Settings \x2192 Threaded Mode \x2192 On (the classic /mybots menu has no such setting). Checked again every minute.");
                                bool sendNotice = false;
                                {
                                    std::lock_guard lk{ _mtx };
                                    // One notice per day at most: the user may take a while, and every
                                    // Agentmaster start must not repeat it.
                                    if (now - _state.setupNoticeMs > 24ll * 3600 * 1000)
                                    {
                                        _state.setupNoticeMs = now;
                                        _stateDirty = true;
                                        sendNotice = true;
                                    }
                                }
                                if (sendNotice)
                                {
                                    _sendText(0,
                                              L"<b>Agentmaster is connected to this bot</b>, but topics are off, so there is nothing to show yet.\n\n"
                                              L"Turn <b>Threaded Mode</b> on in the BotFather <b>Mini App</b> (search botfather, tap <b>Open</b> \x2014 the classic /mybots chat menu does not have this setting): My bots \x2192 @" +
                                                  TgEscapeHtml(user) + L" \x2192 Bot Settings \x2192 <b>Threads Settings</b> \x2192 Threaded Mode \x2192 Turn on. "
                                                                        L"Agentmaster checks again every minute and then creates one topic per working session.",
                                              false,
                                              SendPriority::Final,
                                              L"setup-notice",
                                              json::Value{},
                                              nullptr);
                                    _log(L"[msg] topics are off for @" + user + L": the setup notice was sent");
                                }
                            }
                        }
                        else if (r.httpStatus == 401)
                        {
                            _setStatus(L"Stopped: Telegram refused the bot token (401). Paste a new token from @BotFather.");
                        }
                        else
                        {
                            _setStatus(L"Cannot reach Telegram: " + (r.description.empty() ? std::wstring{ L"no answer" } : r.description) + L". Retrying.");
                        }
                    }
                    _saveStateIfDirty(false);
                    continue;
                }

                _ensureSearchTopic();
                if (_probeWanted.load())
                {
                    int64_t search = 0;
                    {
                        std::lock_guard lk{ _mtx };
                        search = _state.searchThreadId;
                    }
                    if (search != 0)
                    {
                        _probeWanted.store(false);
                        _probeSecretary();
                    }
                }
                _syncTopics();
                _flushIdleNotices(WallMs()); // §21.31: the main view names the sessions whose topics just left as idle, once they settled
                _stateNameTick(); // §21.11 + §21.13: at most one topic name follows its session's state and color per tick, within the budget
                _directoryTick(); // §21.16: the pinned directory follows the fleet (a check every 15 s, an edit at most every 2 min)

                // The mirror and the cards, per topic. The topic set is copied under the lock; each
                // session's work reads the registry and the transcript outside it.
                std::vector<std::wstring> ids;
                {
                    std::lock_guard lk{ _mtx };
                    ids.reserve(_state.topics.size());
                    for (const auto& [sid, t] : _state.topics)
                    {
                        if (t.threadId != 0)
                        {
                            ids.push_back(sid);
                        }
                    }
                    // The open-turn count that stretches the work log's edit gap (_flushWorkLog): a pass-old
                    // value is fine, the gap only has to follow the fleet's load, not each turn's edge.
                    size_t open = 0;
                    for (const auto& kv : _mirrors)
                    {
                        if (kv.second.turnOpen)
                        {
                            ++open;
                        }
                    }
                    _openTurns.store(open);
                }
                // The topic list's ORDER (§21.8): Telegram lists topics by their last message, newest on top, so the
                // posts this pass queues (replies, card re-posts) go out lowest-ranked topic first — the topic that
                // ranks highest in MOST RECENT order is posted last and stays on top. A session the sync did not see
                // (closed) ranks lowest.
                std::stable_sort(ids.begin(), ids.end(), [this](const std::wstring& a, const std::wstring& b) {
                    const auto ka = _orderKeys.find(a);
                    const auto kb = _orderKeys.find(b);
                    const int64_t keyA = ka == _orderKeys.end() ? 0 : ka->second.first;
                    const int64_t keyB = kb == _orderKeys.end() ? 0 : kb->second.first;
                    const std::wstring& titleA = ka == _orderKeys.end() ? a : ka->second.second;
                    const std::wstring& titleB = kb == _orderKeys.end() ? b : kb->second.second;
                    return TopicRanksAbove(keyB, titleB, keyA, titleA); // a first when b ranks above it
                });
                for (const auto& sid : ids)
                {
                    if (_stop.load())
                    {
                        break;
                    }
                    const auto s = _session(sid);
                    if (!s)
                    {
                        continue;
                    }
                    TopicRecord topic;
                    Mirror mirror;
                    {
                        std::lock_guard lk{ _mtx };
                        const auto it = _state.topics.find(sid);
                        if (it == _state.topics.end())
                        {
                            continue;
                        }
                        topic = it->second;
                        mirror = _mirrors[sid];
                    }
                    _onStateEdge(sid, topic, mirror, *s);
                    _followTranscript(sid, topic, mirror, *s);
                    _flushWorkLog(sid, topic, mirror, false);
                    _syncReactions(*s);
                    _sweepSteers(*s); // §21.33: after the reactions, so a steer the window put back gets its ✍ last
                    _updatePromptStatus(topic, mirror, *s); // §21.12: after the reactions (a phone prompt's 👨‍💻 goes first)
                    _sweepRevives(*s);
                    {
                        std::lock_guard lk{ _mtx };
                        if (const auto it = _state.topics.find(sid); it != _state.topics.end())
                        {
                            // Keep the fields the mirror advanced; the sync may have changed the others (never the
                            // card clocks: _noteTopicPosted and the card's callbacks write those under the lock).
                            auto& rec = it->second;
                            // A work-log create whose answer landed while this session was processed: the copy still
                            // says "in flight" (-1), the record already holds the answer (the new message's id, or 0
                            // for a refusal; _flushWorkLog marks the record -1 when it sends) — keep it, or the next
                            // tick waits for an answer that already came and the work log stops for the turn.
                            const bool workLogAnswerLanded = topic.workLogMessageId < 0 && rec.workLogMessageId >= 0;
                            // Only a REAL change marks the file dirty, and as PROGRESS (saved within kStateProgressSaveGapMs):
                            // marking it on every tick for every topic rewrote the whole file every ~3 s even when nothing
                            // had changed (UI_THREAD.md §13d).
                            if (CopyBackTopicProgress(rec, topic, workLogAnswerLanded))
                            {
                                _stateProgressDirty = true;
                            }
                            auto& cur = _mirrors[sid];
                            if (workLogAnswerLanded && cur.workLogGen == mirror.workLogGen && cur.workLogMessageId > 0)
                            {
                                mirror.workLogMessageId = cur.workLogMessageId;
                                mirror.workLogDirty = true; // what arrived meanwhile goes out as an edit
                            }
                            cur = std::move(mirror);
                        }
                    }
                    // The session copy this pass already holds: a second full Get per topic per tick (the queue and every
                    // text copied again) cost as much as the first. A change the pass made to the session in the registry
                    // meanwhile reaches the card at the next tick.
                    _refreshCard(sid, false, false, false, &*s);
                }
                // §21.30: Claude's questions — new requests become cards, every card reads its dialog's result, and the questions no
                // hook offered within kAskPairWaitMs are posted show-only. After the per-topic pass: the mirror's question items of
                // this tick are known by then, and no copy of a mirror is out (a new card marks its session's mirror).
                _questionsTick();
                // §21.34: the /add-dir commands the topics queued — their dialogs found on the screen become cards, a card's default
                // is pressed by itself a minute after its dialog opened, and each command's result ends its watch.
                _menusTick();
                // The HISTORY replay's next batch (§21.9), then the list's re-sort: both after the per-topic pass, so the
                // live posts of this tick are queued first (they go first anyway: the replay has the lowest priority).
                _pumpBackfill();
                _resortTick();
                // Revives for sessions that have no topic yet (a resumed session that is still Idle). §21.23: a revive settles on
                // the session its window's ANSWER named (Revive::openedId) — a new session or a fork has no id of its own until the
                // answer, and the request's own id is the fork's SOURCE (or the topic a /newsession was typed in), which a live
                // match used to settle on. A revive with no answer yet waits; the ones that waited too long are told so.
                {
                    std::vector<std::wstring> reviveIds;
                    {
                        std::lock_guard lk{ _mtx };
                        for (const auto& rv : _revives)
                        {
                            if (rv.answered && !rv.openedId.empty())
                            {
                                reviveIds.push_back(rv.openedId);
                            }
                        }
                    }
                    for (const auto& rid : reviveIds)
                    {
                        if (const auto s = _session(rid))
                        {
                            _sweepRevives(*s);
                        }
                    }
                    _expireRevives();
                }
                // §21.25: abandoned new-thread wizards are aborted (their threads deleted), and after a start the restored ones
                // that waited for their windows ask again once the start-up hold is over (the sync above updated it).
                _newThreadsTick();
                _sweepTokens();
                // Park an idle Search agent.
                {
                    std::lock_guard lk{ _mtx };
                    if (_search && !_searchParked && now - _searchLastUsedMs > kSearchParkMs)
                    {
                        _search->Park(0);
                        _searchParked = true;
                    }
                }
                _saveStateIfDirty(false);
            }
            catch (...)
            {
                LogSwallowedException(L"MessagingService::_tickLoop");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    std::optional<SessionInfo> MessagingService::_session(const std::wstring& sessionId) const
    {
        return _registry->Get(sessionId);
    }

    bool MessagingService::_awaitCallSlot(const wchar_t* method)
    {
        // The SAME slot the send thread takes for every outbox item (_lastSendMs, GetTickCount64 ms): whoever calls
        // next waits until kSendGapMs passed since the last call of either thread, so the two together keep the chat's
        // ~1 call/s pace (§21.4's 429 cost 21 minutes).
        for (;;)
        {
            if (_stop.load())
            {
                // Stop() aborts only the request in flight: a call started now would hold Stop's join for its whole
                // timeout. The caller skips it.
                return false;
            }
            int64_t waitMs = 0;
            {
                std::lock_guard lk{ _mtx };
                const int64_t now = static_cast<int64_t>(::GetTickCount64());
                if (FloodParkedUntil(_floodParking, method, now) > 0)
                {
                    // A long 429 holds this method (or the whole chat): the call would only collect another refusal. The
                    // caller leaves the work (a topic to create / delete / rename) for a tick after the deadline.
                    return false;
                }
                if (_lastSendMs == 0 || now - _lastSendMs >= kSendGapMs)
                {
                    _lastSendMs = now;
                    _noteCallLocked(now); // the history replay's budget counts the direct calls too
                    ++_trafficByMethod[method];
                    return true;
                }
                waitMs = kSendGapMs - (now - _lastSendMs);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(waitMs, 1200)));
        }
    }

    void MessagingService::_noteCallLocked(int64_t tickMs)
    {
        const int64_t now = static_cast<int64_t>(::GetTickCount64());
        while (!_recentCallsMs.empty() && now - _recentCallsMs.front() >= kCallWindowMs)
        {
            _recentCallsMs.pop_front();
        }
        if (tickMs > 0)
        {
            _recentCallsMs.push_back(tickMs);
        }
    }

    void MessagingService::_noteFlood(const std::wstring& method, int retryAfterSec)
    {
        {
            std::lock_guard lk{ _mtx };
            _floodAtMs = static_cast<int64_t>(::GetTickCount64());
        }
        // What else waits is the caller's line: a short wait holds the thread that met it, a long one parks the method
        // (_parkForFlood says which). Until 2026-10-17 this line said "live messages go on" while the send loop slept.
        _log(L"[msg] history replay paused " + std::to_wstring(kHistoryFloodPauseMs / 60000) + L" min: Telegram refused a " + method + L" (429, retry after " +
             std::to_wstring(retryAfterSec) + L" s)");
    }

    void MessagingService::_parkForFlood(const std::wstring& method, int retryAfterSec, bool secretary)
    {
        bool all = false;
        int64_t remainingMs = 0;
        {
            std::lock_guard lk{ _mtx };
            const int64_t now = static_cast<int64_t>(::GetTickCount64());
            FloodParking& parking = secretary ? _secretaryFloodParking : _floodParking;
            all = ParkForFlood(parking, method, retryAfterSec, now);
            remainingMs = (std::max<int64_t>)(0, FloodParkedUntil(parking, method, now) - now);
        }
        const int64_t wall = WallMs();
        const std::wstring who = secretary ? std::wstring{ L"the secretary bot's " } : std::wstring{};
        _log(L"[msg] pacing: 429 on " + who + method + L" (retry after " + std::to_wstring(retryAfterSec) + L" s): " +
             (all ? L"a second method refused \x2014 every " + who + L"call waits until " + TgClockText(wall + remainingMs, wall)
                  : who + method + L" waits until " + TgClockText(wall + remainingMs, wall) + L"; the other calls go on"));
    }

    bool MessagingService::_handleTickFlood(const std::wstring& method, const TgResult& r)
    {
        if (r.ok || r.httpStatus != 429)
        {
            return false;
        }
        _noteFlood(method, r.retryAfterSec);
        if (r.retryAfterSec > kFloodParkAfterSec)
        {
            // A long wait parks the method: sleeping it out here would freeze the tick (every mirror, card and rename) for
            // the whole retry_after — and createForumTopic's old sleep did not even end for Stop.
            _parkForFlood(method, r.retryAfterSec);
            return true;
        }
        const int64_t waitMs = static_cast<int64_t>((std::max)(1, r.retryAfterSec)) * 1000;
        _log(L"[msg] pacing: 429 on " + method + L", waiting " + std::to_wstring(waitMs / 1000) + L" s");
        for (int64_t slept = 0; slept < waitMs && !_stop.load(); slept += 250)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return true;
    }

    // ===== the outbox ========================================================================

    void MessagingService::_enqueue(OutItem item)
    {
        // §21.15 belt: a title token never reaches Telegram as text (its private-use characters would show as boxes). Every path
        // that sends a builder's text renders the tokens (_sendText, _editText); an item that still carries one — a path that
        // bypasses both — is rendered here, classic, and the first such item of the run is logged so the path can be found.
        for (auto& [name, value] : item.params.members)
        {
            if ((name == L"text" || name == L"caption") && value.type == json::Value::Type::Str && TgHasTitleTokens(value.str))
            {
                value.str = TgRenderTitleTokensClassic(value.str);
                if (!_titleTokenLeakLogged.exchange(true))
                {
                    _log(L"[msg] a title token reached the outbox unrendered (" + _outItemMethod(item) + L"); it went as the classic title");
                }
            }
        }
        // §21.28: an item queued inside a TAP SCOPE — while a tapped button's handler runs, or a continuation of its work — holds
        // that tap's answer until Telegram has answered this call (the send loop lets the item's holds go after its callback), so
        // the button keeps its loading indicator for as long as the work it asked for. An answer itself holds nothing. Outside a
        // scope (the fleet's own traffic, nearly every call) this is one thread-local read.
        if (TapScope::Active() && item.method != L"answerCallbackQuery")
        {
            std::vector<TapHoldRef> holds = _holdTaps();
            MoveTapHolds(holds, item.tapHolds);
        }
        {
            std::lock_guard lk{ _mtx };
            if (!item.key.empty())
            {
                // A newer item with the same key replaces the queued one (an edit of the same message). §21.28: it takes over the
                // replaced item's tap holds — the newer edit carries the change that tap waits for (a newer text still).
                for (auto it = _outbox.begin(); it != _outbox.end();)
                {
                    if (it->key == item.key)
                    {
                        MoveTapHolds(it->tapHolds, item.tapHolds);
                        it = _outbox.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            if (item.seq == 0)
            {
                item.seq = ++_outSeq; // a chunk or a fallback brings the seq of its message (its place in the order)
            }
            _outbox.push_back(std::move(item));
        }
        _sendCv.notify_one();
    }

    uint64_t MessagingService::_takeOutSeq()
    {
        std::lock_guard lk{ _mtx };
        return ++_outSeq;
    }

    // ===== §21.28: a tapped button's answer, held while its work runs ========================================================

    void MessagingService::OnTapHoldsGone(const std::shared_ptr<TapAnswer>& tap) noexcept
    {
        try
        {
            {
                // The LEAF lock only: this thread may hold `_mtx` (an outbox item dropped under it); the answer itself is queued
                // by the send loop (_answerDueTaps), outside every lock.
                std::lock_guard lk{ _tapMtx };
                _tapsDue.push_back(tap);
            }
            _tapAnswersDue.store(true);
            _sendCv.notify_one();
        }
        catch (...)
        {
            // Rule #18. Recovery: the tap stays in `_openTaps`, so its hold's limit answers it — a few seconds late, never lost.
            LogSwallowedException(L"MessagingService::OnTapHoldsGone");
        }
    }

    std::vector<TapHoldRef> MessagingService::_holdTaps()
    {
        // The holds tell this service (weakly: a hold that outlives the service — a window's reply after a quit — tells nobody).
        return HoldTapsOfScope(std::weak_ptr<TapReleaseSink>{ weak_from_this() });
    }

    void MessagingService::_awaitCardChange(const std::wstring& sessionId)
    {
        if (!TapScope::Active())
        {
            return; // the fleet's own refreshes and the typed commands: no button waits
        }
        std::vector<TapHoldRef> holds = _holdTaps();
        if (holds.empty())
        {
            return; // every tap in scope was answered already (its hold's limit passed)
        }
        std::lock_guard lk{ _mtx };
        const auto it = _state.topics.find(sessionId);
        if (it == _state.topics.end() || it->second.threadId == 0)
        {
            // No topic, so no card that could change: the holds go with this function (after the lock), and the tap is answered
            // as soon as its other holds go.
            return;
        }
        MoveTapHolds(holds, _cards[sessionId].tapHolds);
    }

    void MessagingService::_answerDueTaps() noexcept
    {
        try
        {
            const int64_t now = static_cast<int64_t>(::GetTickCount64());
            std::vector<std::pair<std::shared_ptr<TapAnswer>, bool>> due; // (the tap, answered by its hold's limit)
            {
                std::lock_guard lk{ _tapMtx };
                // The flag before the take: a hold that goes after the take sets it again and wakes the next pass.
                _tapAnswersDue.store(false);
                // The taps whose last hold went come first: a tap that also reached its limit in the same moment is answered as
                // done (its work is in), and the second claim below finds it answered.
                for (auto& tap : _tapsDue)
                {
                    due.emplace_back(std::move(tap), false);
                }
                _tapsDue.clear();
                for (auto it = _openTaps.begin(); it != _openTaps.end();)
                {
                    if ((*it)->Answered())
                    {
                        it = _openTaps.erase(it); // answered by its last hold: nothing more to watch
                    }
                    else if (now >= (*it)->deadlineMs)
                    {
                        due.emplace_back(*it, true);
                        it = _openTaps.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            for (const auto& [tap, byLimit] : due)
            {
                _answerTap(tap, byLimit, now);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the taps taken in this pass go unanswered (their buttons stop at Telegram's own limit); the open
            // ones that were not taken are looked at again by the next pass.
            LogSwallowedException(L"MessagingService::_answerDueTaps");
        }
    }

    void MessagingService::_answerTap(const std::shared_ptr<TapAnswer>& tap, bool byLimit, int64_t nowTickMs)
    {
        if (!tap || !tap->TryClaimAnswer())
        {
            return; // answered already: by its last hold, or by its limit a pass ago
        }
        const bool workGoesOn = byLimit && tap->holds.load() > 0;
        const std::wstring text = TapAnswerText(tap->Toast(), workGoesOn);
        OutItem item;
        item.prio = SendPriority::Final;
        // Ahead of every other item of its priority (the outbox serves by priority, then by seq): an answer is no message of the
        // chat, so it can come before none of them out of order, the work its tap waited for has been answered already (or its
        // limit is up), and the button spins until the answer lands.
        item.seq = kTapAnswerOutSeq;
        item.method = L"answerCallbackQuery";
        item.paced = false;
        item.params = json::Value::MkObj();
        item.params.Set(L"callback_query_id", json::Value::MkStr(tap->queryId));
        if (!text.empty())
        {
            item.params.Set(L"text", json::Value::MkStr(text));
        }
        auto self = weak_from_this();
        const std::wstring label = tap->logLabel;
        item.onDone = [self, label](const TgResult& r) {
            if (r.ok)
            {
                return;
            }
            // Mostly "query is too old" — the answer came after Telegram stopped waiting (the button had stopped spinning by
            // then); the work itself was done or goes on all the same.
            if (const auto me = self.lock())
            {
                me->_log(L"[msg] tap " + label + L": Telegram refused its answer (" + (r.description.empty() ? std::wstring{ L"no response" } : r.description) + L")");
            }
        };
        _log(L"[msg] tap " + tap->logLabel + L": answer queued " + std::to_wstring(nowTickMs - tap->receivedMs) + L" ms after the tap" +
             (workGoesOn ? std::wstring{ L" (the hold's limit; its work goes on)" } : (byLimit ? std::wstring{ L" (the hold's limit)" } : std::wstring{ L" (its work is in)" })));
        _enqueue(std::move(item));
    }

    json::Value MessagingService::_buttons(const std::vector<std::vector<std::pair<std::wstring, std::wstring>>>& rows) const
    {
        auto kb = json::Value::MkArr();
        for (const auto& row : rows)
        {
            auto r = json::Value::MkArr();
            for (const auto& [text, data] : row)
            {
                auto b = json::Value::MkObj();
                b.Set(L"text", json::Value::MkStr(text));
                if (data.rfind(L"copy:", 0) == 0)
                {
                    auto ct = json::Value::MkObj();
                    ct.Set(L"text", json::Value::MkStr(data.substr(5)));
                    b.Set(L"copy_text", std::move(ct));
                }
                else
                {
                    b.Set(L"callback_data", json::Value::MkStr(data));
                }
                r.Push(std::move(b));
            }
            kb.Push(std::move(r));
        }
        auto markup = json::Value::MkObj();
        markup.Set(L"inline_keyboard", std::move(kb));
        return markup;
    }

    void MessagingService::_applyReplyRef(json::Value& params, const PromptRef& ref)
    {
        const int64_t id = ref.id.load();
        if (id <= 0 || params.Find(L"reply_parameters"))
        {
            return; // not known (refused, or a send as the user whose echo has not come): the reply goes without a quote
        }
        auto rp = json::Value::MkObj();
        rp.Set(L"message_id", json::Value::MkNum(static_cast<double>(id)));
        rp.Set(L"allow_sending_without_reply", json::Value::MkBool(true)); // the prompt deleted meanwhile must not cost the reply
        params.Set(L"reply_parameters", std::move(rp));
    }

    std::vector<MessagingService::OutItem> MessagingService::_htmlItems(int64_t threadId, const std::wstring& html, bool silent, SendPriority prio, const std::wstring& key, json::Value replyMarkup, std::function<void(const TgResult&)> onDone, int64_t replyToMessageId, uint64_t seq, uint32_t subSeqBase, std::shared_ptr<PromptRef> replyRef)
    {
        // A text past Telegram's limit goes out as several messages; only the LAST carries the buttons and the callback,
        // and only the first the reply reference. Every chunk keeps the message's place: (seq, subSeqBase + 2i), and its
        // plain-text fallback the slot right after it.
        std::vector<OutItem> items;
        const auto chunks = TgChunkHtml(html);
        for (size_t i = 0; i < chunks.size(); ++i)
        {
            OutItem item;
            item.prio = prio;
            item.seq = seq;
            item.subSeq = subSeqBase + static_cast<uint32_t>(2 * i);
            item.key = (i + 1 == chunks.size()) ? key : std::wstring{};
            item.method = L"sendMessage";
            item.params = json::Value::MkObj();
            item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            if (threadId > 0)
            {
                item.params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(threadId)));
            }
            item.params.Set(L"text", json::Value::MkStr(chunks[i]));
            item.params.Set(L"parse_mode", json::Value::MkStr(L"HTML"));
            item.params.Set(L"disable_notification", json::Value::MkBool(silent || i + 1 < chunks.size()));
            {
                auto lp = json::Value::MkObj();
                lp.Set(L"is_disabled", json::Value::MkBool(true));
                item.params.Set(L"link_preview_options", std::move(lp));
            }
            if (i == 0 && replyToMessageId > 0)
            {
                auto rp = json::Value::MkObj();
                rp.Set(L"message_id", json::Value::MkNum(static_cast<double>(replyToMessageId)));
                rp.Set(L"allow_sending_without_reply", json::Value::MkBool(true));
                item.params.Set(L"reply_parameters", std::move(rp));
            }
            else if (i == 0 && replyRef)
            {
                // §21.12: the reply to the turn's prompt, resolved when the item goes (the prompt has answered by then).
                item.beforeSend = [replyRef](json::Value& p) { _applyReplyRef(p, *replyRef); };
            }
            if (i + 1 == chunks.size())
            {
                if (replyMarkup.type == json::Value::Type::Obj)
                {
                    item.params.Set(L"reply_markup", replyMarkup);
                }
                item.onDone = onDone;
            }
            // Telegram refuses HTML it cannot parse (a 400): fall back to the escaped plain text, so a
            // reply is never lost to a formatting edge case. The fallback takes this chunk's place in the order.
            const std::wstring plainText = chunks[i];
            const int64_t thread = threadId;
            const uint64_t plainSeq = seq;
            const uint32_t plainSub = item.subSeq + 1;
            auto self = weak_from_this();
            auto inner = std::move(item.onDone);
            item.onDone = [self, plainText, thread, inner, prio, plainSeq, plainSub](const TgResult& r) {
                if (!r.ok && r.httpStatus == 400 && r.description.find(L"can't parse entities") != std::wstring::npos)
                {
                    if (auto me = self.lock())
                    {
                        me->_log(L"[msg] Telegram refused the HTML of a message; sending it as plain text");
                        OutItem plain;
                        plain.prio = prio;
                        plain.seq = plainSeq;
                        plain.subSeq = plainSub;
                        plain.method = L"sendMessage";
                        plain.params = json::Value::MkObj();
                        plain.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(me->_ownerId)));
                        if (thread > 0)
                        {
                            plain.params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(thread)));
                        }
                        // Strip the tags this header emits; entities stay escaped, which plain mode shows as
                        // typed ("&lt;"): a readable degradation.
                        std::wstring stripped;
                        bool inTag = false;
                        for (const wchar_t c : plainText)
                        {
                            if (c == L'<')
                            {
                                inTag = true;
                            }
                            else if (c == L'>' && inTag)
                            {
                                inTag = false;
                            }
                            else if (!inTag)
                            {
                                stripped += c;
                            }
                        }
                        plain.params.Set(L"text", json::Value::MkStr(stripped));
                        plain.params.Set(L"disable_notification", json::Value::MkBool(true));
                        plain.onDone = inner;
                        me->_enqueue(std::move(plain));
                    }
                    return;
                }
                if (inner)
                {
                    inner(r);
                }
            };
            items.push_back(std::move(item));
        }
        return items;
    }

    void MessagingService::_sendText(int64_t threadId, const std::wstring& html, bool silent, SendPriority prio, const std::wstring& key, json::Value replyMarkup, std::function<void(const TgResult&)> onDone, int64_t replyToMessageId, bool movesCard, std::shared_ptr<PromptRef> replyRef, std::shared_ptr<const CardPicture> picture)
    {
        // §21.15: a message that names a session carries title tokens. They never go out as they are (private-use characters):
        // with title chips wanted the message goes RICH, its titles as chips, and its classic render is the fallback; else the
        // classic render goes the HTML way. A rich form past the rich limit (never for our titled messages, which are a card, a
        // reply or a Search answer) goes classic too, chunked as usual.
        const std::wstring* text = &html;
        std::wstring classic;
        if (TgHasTitleTokens(html))
        {
            // §21.17: with the bot's username known, a title that names its topic links to it in both forms (a URL chip, an
            // <a> around the title); before the first getMe answers the titles go as before (copy chips, plain mentions).
            const std::wstring bot = _linkBot();
            classic = TgRenderTitleTokensClassic(html, bot);
            text = &classic;
            if (_titleChipsWanted())
            {
                if (std::wstring rich = TgRichHtmlFromClassic(html, bot); !rich.empty() && rich.size() <= kRichMessageMaxChars)
                {
                    // §21.42: the card's screenshot rides the rich form only (a classic message cannot hold a picture block).
                    _sendTitledRich(threadId, rich, classic, silent, prio, key, std::move(replyMarkup), std::move(onDone), replyToMessageId, std::move(replyRef), std::move(picture));
                    if (movesCard && threadId > 0)
                    {
                        _noteTopicPosted(threadId);
                    }
                    return;
                }
            }
        }
        for (auto& item : _htmlItems(threadId, *text, silent, prio, key, std::move(replyMarkup), std::move(onDone), replyToMessageId, _takeOutSeq(), 0, std::move(replyRef)))
        {
            _enqueue(std::move(item));
        }
        if (movesCard && threadId > 0)
        {
            _noteTopicPosted(threadId);
        }
    }

    bool MessagingService::_titleChipsWanted() const noexcept
    {
        return _richMessages.load() && !_richUnavailable.load();
    }

    std::wstring MessagingService::_linkBot() const
    {
        std::lock_guard lk{ _linkBotMtx }; // a leaf lock: callers may hold _mtx
        return _linkBotName;
    }

    int64_t MessagingService::_threadOf(const std::wstring& sessionId) const
    {
        std::lock_guard lk{ _mtx };
        const auto it = _state.topics.find(sessionId);
        return it == _state.topics.end() ? 0 : it->second.threadId;
    }

    MessagingService::OutItem MessagingService::_titledRichItem(int64_t threadId, const std::wstring& richHtml, bool silent, SendPriority prio, const std::wstring& key, const json::Value& replyMarkup, int64_t replyToMessageId, const std::shared_ptr<PromptRef>& replyRef, const CardPicture* picture, uint64_t seq, uint32_t subSeq) const
    {
        OutItem item;
        item.prio = prio;
        item.seq = seq;
        item.subSeq = subSeq;
        item.key = key;
        item.method = L"sendRichMessage";
        item.params = json::Value::MkObj();
        item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        if (threadId > 0)
        {
            item.params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(threadId)));
        }
        {
            auto rich = json::Value::MkObj();
            // §21.42: the card's screenshot is the message's LAST block (after the text, before the buttons): rich HTML takes media
            // only as a block of its own, and the photo itself goes in the media list the block's tg://photo?id= names.
            rich.Set(L"html", json::Value::MkStr(picture ? richHtml + CardPictureFigureHtml(picture->shotAtMs, WallMs()) : richHtml));
            if (picture)
            {
                rich.Set(L"media", CardPictureMediaJson(*picture));
            }
            item.params.Set(L"rich_message", std::move(rich));
        }
        if (picture && !picture->png.empty())
        {
            // A NEW picture goes up in this very call ("attach://screen" in the media list names the part): no call of its own on
            // the chat's budget. A COPY of the bytes: the shared picture may be carried by another item too (it is never changed).
            item.uploadField = std::wstring{ kCardPictureMediaId };
            item.fileName = std::wstring{ kCardPictureFileName };
            item.fileBytes = picture->png;
        }
        item.params.Set(L"disable_notification", json::Value::MkBool(silent));
        if (replyToMessageId > 0)
        {
            auto rp = json::Value::MkObj();
            rp.Set(L"message_id", json::Value::MkNum(static_cast<double>(replyToMessageId)));
            rp.Set(L"allow_sending_without_reply", json::Value::MkBool(true));
            item.params.Set(L"reply_parameters", std::move(rp));
        }
        else if (replyRef)
        {
            // §21.12: the reply to the turn's prompt, resolved when the item goes (the prompt has answered by then).
            item.beforeSend = [replyRef](json::Value& p) { _applyReplyRef(p, *replyRef); };
        }
        if (replyMarkup.type == json::Value::Type::Obj)
        {
            item.params.Set(L"reply_markup", replyMarkup);
        }
        return item;
    }

    void MessagingService::_sendTitledRich(int64_t threadId, const std::wstring& richHtml, const std::wstring& classicHtml, bool silent, SendPriority prio, const std::wstring& key, json::Value replyMarkup, std::function<void(const TgResult&)> onDone, int64_t replyToMessageId, std::shared_ptr<PromptRef> replyRef, std::shared_ptr<const CardPicture> picture)
    {
        const uint64_t seq = _takeOutSeq(); // the rich message and its fallback keep one place in the order
        OutItem item = _titledRichItem(threadId, richHtml, silent, prio, key, replyMarkup, replyToMessageId, replyRef, picture.get(), seq, 0);
        // A refusal sends the classic render in this message's place: the same buttons, the same reply, the same onDone, so a
        // caller (a card post that waits for its message id) sees one answer either way. The fallback carries NO key: queued
        // from this answer, a key would erase a NEWER item with the same key queued meanwhile (the stale text would win); its
        // sequence number already puts it before any such item. `classicSub` is the slot it takes: 1 right after the rich message,
        // 2 after the §21.42 retry without the picture (which takes 1).
        auto self = weak_from_this();
        const auto plainDone = [self, threadId, classicHtml, silent, prio, replyMarkup, onDone, replyToMessageId, replyRef, seq](uint32_t classicSub) -> std::function<void(const TgResult&)> {
            return [self, threadId, classicHtml, silent, prio, replyMarkup, onDone, replyToMessageId, replyRef, seq, classicSub](const TgResult& r) {
                if (RichRefusalWantsFallback(r))
                {
                    if (auto me = self.lock())
                    {
                        if (r.httpStatus == 404 && !me->_richUnavailable.exchange(true))
                        {
                            me->_log(L"[msg] the Bot API has no sendRichMessage (404: " + r.description + L"): titles go with their color square, as HTML, for the rest of this run");
                        }
                        else if (r.httpStatus != 404)
                        {
                            me->_log(L"[msg] Telegram refused a message with a title chip (" + r.description + L"); it goes as HTML with the color square in its place");
                        }
                        for (auto& fb : me->_htmlItems(threadId, classicHtml, silent, prio, L"", replyMarkup, onDone, replyToMessageId, seq, classicSub, replyRef))
                        {
                            me->_enqueue(std::move(fb));
                        }
                    }
                    return;
                }
                if (onDone)
                {
                    onDone(r);
                }
            };
        };
        if (!picture)
        {
            item.onDone = plainDone(1);
        }
        else
        {
            // §21.42: a refusal of the message WITH its picture may be the picture's: the same rich message goes once more WITHOUT it,
            // in the next slot and with no key (the rule above), so the card's text and its title chip still land; only that
            // retry's refusal falls back to the classic render. A 404 is no picture's fault (no rich messages at all): straight to
            // the classic fallback, as before.
            item.onDone = [self, threadId, richHtml, silent, prio, replyMarkup, replyToMessageId, replyRef, seq, plainDone](const TgResult& r) {
                if (RichRefusalWantsFallback(r) && r.httpStatus != 404)
                {
                    if (auto me = self.lock())
                    {
                        me->_noteCardPictureRefused(r);
                        OutItem plain = me->_titledRichItem(threadId, richHtml, silent, prio, L"", replyMarkup, replyToMessageId, replyRef, nullptr, seq, 1);
                        plain.onDone = plainDone(2);
                        me->_enqueue(std::move(plain));
                    }
                    return;
                }
                plainDone(1)(r);
            };
        }
        _enqueue(std::move(item));
    }

    void MessagingService::_sendRich(int64_t threadId, const std::wstring& markdown, const std::wstring& fallbackHtml, bool silent, SendPriority prio, std::function<void(const TgResult&)> onDone, int64_t replyToMessageId, bool movesCard, std::shared_ptr<PromptRef> replyRef)
    {
        const bool blank = markdown.find_first_not_of(L" \t\r\n") == std::wstring::npos;
        if (blank || !_richMessages.load() || _richUnavailable.load())
        {
            _sendText(threadId, fallbackHtml.empty() ? TgMarkdownToHtml(markdown) : fallbackHtml, silent, prio, L"", json::Value{}, std::move(onDone), replyToMessageId, movesCard, std::move(replyRef));
            return;
        }
        const auto parts = TgChunkMarkdown(markdown);
        const uint64_t seq = _takeOutSeq(); // every part, and every fallback, keeps this message's place in the order
        auto self = weak_from_this();
        for (size_t j = 0; j < parts.size(); ++j)
        {
            const bool last = j + 1 == parts.size();
            const uint32_t sub = static_cast<uint32_t>(j) * kRichPartSubSeqStride;
            // What this part falls back to: the caller's HTML for a message of one part, else this part converted.
            const std::wstring partHtml = parts.size() == 1 && !fallbackHtml.empty() ? fallbackHtml : TgMarkdownToHtml(parts[j]);
            std::function<void(const TgResult&)> partDone = last ? onDone : nullptr;
            const int64_t replyTo = j == 0 ? replyToMessageId : 0;
            const std::shared_ptr<PromptRef> partRef = j == 0 ? replyRef : nullptr; // §21.12: the first part quotes the prompt
            const bool quiet = silent || !last; // only the last part rings
            std::wstring prepared = TgPrepareRichMarkdown(parts[j]);
            if (prepared.size() > kRichMessageMaxChars || prepared.find_first_not_of(L" \t\r\n") == std::wstring::npos)
            {
                // Grown past the rich limit by its escapes (thousands of '<'), or nothing left to show: HTML in its place.
                for (auto& item : _htmlItems(threadId, partHtml, quiet, prio, L"", json::Value{}, partDone, replyTo, seq, sub + 1, partRef))
                {
                    _enqueue(std::move(item));
                }
                continue;
            }
            OutItem item;
            item.prio = prio;
            item.seq = seq;
            item.subSeq = sub;
            item.method = L"sendRichMessage";
            item.params = json::Value::MkObj();
            item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            if (threadId > 0)
            {
                item.params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(threadId)));
            }
            {
                auto rich = json::Value::MkObj();
                rich.Set(L"markdown", json::Value::MkStr(prepared));
                item.params.Set(L"rich_message", std::move(rich));
            }
            item.params.Set(L"disable_notification", json::Value::MkBool(quiet));
            if (replyTo > 0)
            {
                auto rp = json::Value::MkObj();
                rp.Set(L"message_id", json::Value::MkNum(static_cast<double>(replyTo)));
                rp.Set(L"allow_sending_without_reply", json::Value::MkBool(true));
                item.params.Set(L"reply_parameters", std::move(rp));
            }
            else if (partRef)
            {
                item.beforeSend = [partRef](json::Value& p) { _applyReplyRef(p, *partRef); };
            }
            item.onDone = [self, threadId, partHtml, quiet, prio, seq, sub, replyTo, partDone, partRef](const TgResult& r) {
                if (RichRefusalWantsFallback(r))
                {
                    if (auto me = self.lock())
                    {
                        if (r.httpStatus == 404 && !me->_richUnavailable.exchange(true))
                        {
                            me->_log(L"[msg] the Bot API has no sendRichMessage (404: " + r.description + L"): Claude's Markdown goes as HTML for the rest of this run");
                        }
                        else if (r.httpStatus != 404)
                        {
                            me->_log(L"[msg] Telegram refused a rich message (" + r.description + L"); it goes as HTML in its place");
                        }
                        for (auto& fb : me->_htmlItems(threadId, partHtml, quiet, prio, L"", json::Value{}, partDone, replyTo, seq, sub + 1, partRef))
                        {
                            me->_enqueue(std::move(fb));
                        }
                    }
                    return;
                }
                if (partDone)
                {
                    partDone(r);
                }
            };
            _enqueue(std::move(item));
        }
        if (movesCard && threadId > 0)
        {
            _noteTopicPosted(threadId);
        }
    }

    void MessagingService::_noteTopicPosted(int64_t threadId)
    {
        if (threadId <= 0)
        {
            return;
        }
        std::lock_guard lk{ _mtx };
        for (auto& kv : _state.topics)
        {
            if (kv.second.threadId == threadId)
            {
                kv.second.lastPostedMs = WallMs();
                // PROGRESS, not a structural change (§21.35): every message a topic gets stamps this, and a structural mark
                // saved the whole state file within kStateSaveGapMs of each one while sessions worked. Lost after an
                // unexpected end, it only delays the card's move below the newer posts until the topic's next post.
                _stateProgressDirty = true;
                return;
            }
        }
    }

    void MessagingService::_sendAsUser(int64_t threadId, const std::wstring& text, std::function<void(const TgResult&)> onDone, SendPriority prio)
    {
        std::wstring connection;
        int64_t holder = 0;
        {
            std::lock_guard lk{ _mtx };
            connection = _state.businessConnectionId;
            holder = _state.businessBotId;
        }
        const int64_t botId = _botId.load();
        // Which bot sends (§21.11): the one that holds the business connection — the SECRETARY bot (a second bot the user
        // connected under Chat Automation, with its own client), or the topics bot itself (0, or its own id).
        const int64_t secretaryId = _secretaryBotId.load();
        const bool viaSecretary = holder != 0 && holder != botId && holder == secretaryId;
        const bool holderGone = holder != 0 && holder != botId && !viaSecretary;
        if (connection.empty() || botId == 0 || holderGone)
        {
            TgResult none;
            none.ok = false;
            none.httpStatus = 0;
            none.description = connection.empty() ? L"no business connection" : (botId == 0 ? L"the bot's id is not known yet" : L"the bot that holds the business connection is not running");
            if (onDone)
            {
                onDone(none);
            }
            return;
        }
        // Plain text (the user's own words carry no markup), split at Telegram's limit on line ends.
        std::vector<std::wstring> parts;
        {
            std::wstring rest = text;
            constexpr size_t kPart = 4000;
            while (rest.size() > kPart)
            {
                size_t cut = rest.rfind(L'\n', kPart);
                if (cut == std::wstring::npos || cut < kPart / 2)
                {
                    cut = kPart;
                }
                parts.push_back(rest.substr(0, cut));
                rest.erase(0, cut == kPart ? cut : cut + 1);
            }
            parts.push_back(rest);
        }
        {
            // The echo guard's memory (_takeSecretaryEcho): if Telegram hands the message back to the bot as a
            // message from the owner in this topic, it must never be queued as a new prompt. A HISTORY message (§21.9)
            // may wait in the outbox for a long time behind the live traffic and the flood budget, so its echo is expected
            // that much later (kHistoryEchoMs) — an old prompt of the history taken for a new one would be SENT to the
            // session again.
            std::lock_guard lk{ _mtx };
            const int64_t now = WallMs();
            const int64_t expires = now + (prio == SendPriority::History ? kHistoryEchoMs : kSecretaryEchoMs);
            std::erase_if(_secretaryEchoes, [now](const SecretaryEcho& e) { return e.expiresMs < now; });
            for (const auto& part : parts)
            {
                _secretaryEchoes.push_back(SecretaryEcho{ threadId, EchoHash(part), expires });
            }
        }
        for (size_t i = 0; i < parts.size(); ++i)
        {
            OutItem item;
            item.prio = prio;
            item.method = L"sendMessage";
            item.asSecretary = viaSecretary;
            item.params = json::Value::MkObj();
            item.params.Set(L"business_connection_id", json::Value::MkStr(connection));
            // Seen from the business account (the user), its chat with this bot is "the private chat with user
            // <bot id>": that is the chat_id a send on the user's behalf names. Whether Telegram allows the
            // bot's own chat as a target is exactly what the probe establishes (§21.5).
            item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(botId)));
            if (threadId > 0)
            {
                item.params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(threadId)));
            }
            item.params.Set(L"text", json::Value::MkStr(parts[i]));
            item.params.Set(L"disable_notification", json::Value::MkBool(true));
            if (i + 1 == parts.size())
            {
                item.onDone = onDone;
            }
            _enqueue(std::move(item));
        }
        _noteTopicPosted(threadId);
    }

    bool MessagingService::_takeSecretaryEcho(int64_t threadId, const std::wstring& text)
    {
        const uint32_t hash = EchoHash(text);
        std::lock_guard lk{ _mtx };
        const int64_t now = WallMs();
        for (auto it = _secretaryEchoes.begin(); it != _secretaryEchoes.end(); ++it)
        {
            if (it->threadId == threadId && it->hash == hash && it->expiresMs >= now)
            {
                _secretaryEchoes.erase(it); // one echo per send: the same words typed again later are a new message
                return true;
            }
        }
        return false;
    }

    void MessagingService::_notePromptEcho(int64_t threadId, const std::wstring& text, int64_t messageId)
    {
        if (threadId == 0 || messageId <= 0)
        {
            return;
        }
        const uint32_t hash = EchoHash(text);
        std::lock_guard lk{ _mtx };
        for (const auto& [sid, t] : _state.topics)
        {
            if (t.threadId != threadId)
            {
                continue;
            }
            // The mirror's PromptRef is the object every copy shares: the id lands there whatever the tick is doing.
            if (const auto mit = _mirrors.find(sid); mit != _mirrors.end() && mit->second.promptRef)
            {
                auto& ref = *mit->second.promptRef;
                if (ref.id.load() == 0 && ref.echoHash.load() == hash)
                {
                    ref.botLine.store(false);
                    ref.id.store(messageId);
                }
            }
            return;
        }
    }

    MessagingService::OutItem MessagingService::_classicEditItem(int64_t messageId, const std::wstring& html, SendPriority prio, const std::wstring& key, const json::Value& replyMarkup) const
    {
        OutItem item;
        item.prio = prio;
        item.key = key;
        item.method = L"editMessageText";
        item.params = json::Value::MkObj();
        item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        item.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
        // An edit cannot be split: cut at the limit (the work log rolls over before it, the card is short).
        std::wstring text = html;
        if (text.size() > kTelegramMessageMaxChars)
        {
            text = TgChunkHtml(text).front();
        }
        item.params.Set(L"text", json::Value::MkStr(text));
        item.params.Set(L"parse_mode", json::Value::MkStr(L"HTML"));
        {
            auto lp = json::Value::MkObj();
            lp.Set(L"is_disabled", json::Value::MkBool(true));
            item.params.Set(L"link_preview_options", std::move(lp));
        }
        if (replyMarkup.type == json::Value::Type::Obj)
        {
            item.params.Set(L"reply_markup", replyMarkup);
        }
        return item;
    }

    MessagingService::OutItem MessagingService::_richEditItem(int64_t messageId, const std::wstring& richHtml, SendPriority prio, const std::wstring& key, const json::Value& replyMarkup, const CardPicture* picture, uint64_t seq, uint32_t subSeq) const
    {
        OutItem item;
        item.prio = prio;
        item.seq = seq;
        item.subSeq = subSeq;
        item.key = key;
        item.method = L"editMessageText";
        item.params = json::Value::MkObj();
        item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        item.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
        {
            auto richMessage = json::Value::MkObj();
            // §21.42: the card's screenshot as the message's last block, its photo in the media list (see _titledRichItem). An edit
            // with a NEW photo replaces the old one in place; one without a picture drops it (both measured on the dev bot).
            richMessage.Set(L"html", json::Value::MkStr(picture ? richHtml + CardPictureFigureHtml(picture->shotAtMs, WallMs()) : richHtml));
            if (picture)
            {
                richMessage.Set(L"media", CardPictureMediaJson(*picture));
            }
            item.params.Set(L"rich_message", std::move(richMessage));
        }
        if (picture && !picture->png.empty())
        {
            // The new picture goes up in this very edit (editMessageText takes a direct upload for a message that is not inline).
            item.uploadField = std::wstring{ kCardPictureMediaId };
            item.fileName = std::wstring{ kCardPictureFileName };
            item.fileBytes = picture->png;
        }
        if (replyMarkup.type == json::Value::Type::Obj)
        {
            item.params.Set(L"reply_markup", replyMarkup);
        }
        return item;
    }

    void MessagingService::_editText(int64_t messageId, const std::wstring& html, SendPriority prio, const std::wstring& key, json::Value replyMarkup, std::function<void(const TgResult&)> onDone, std::function<void(bool)> onForm, std::shared_ptr<const CardPicture> picture)
    {
        if (messageId <= 0)
        {
            return;
        }
        // §21.15: the answer to a classic edit reports the classic form (the Message Telegram returns says which form it shows;
        // "not modified" means the message shows this very text already), then goes to the caller.
        const auto classicDone = [](std::function<void(const TgResult&)> done, std::function<void(bool)> form) -> std::function<void(const TgResult&)> {
            if (!form)
            {
                return done;
            }
            return [done = std::move(done), form = std::move(form)](const TgResult& r) {
                if (r.ok || r.NotModified())
                {
                    form(r.ok ? ResultIsRichMessage(r) : false);
                }
                if (done)
                {
                    done(r);
                }
            };
        };
        if (!TgHasTitleTokens(html))
        {
            OutItem item = _classicEditItem(messageId, html, prio, key, replyMarkup);
            item.onDone = classicDone(std::move(onDone), std::move(onForm));
            _enqueue(std::move(item));
            return;
        }
        // A text that names a session: a RICH edit whose titles are chips while they are wanted (editMessageText turns a classic
        // message into a rich one and back — measured, thread 26785, messages 909 / 910), else the classic render.
        // §21.17: the titles link to their topics once the bot's username is known (_sendText's rule).
        const std::wstring bot = _linkBot();
        std::wstring classic = TgRenderTitleTokensClassic(html, bot);
        std::wstring rich = _titleChipsWanted() ? TgRichHtmlFromClassic(html, bot) : std::wstring{};
        if (rich.empty() || rich.size() > kRichMessageMaxChars)
        {
            OutItem item = _classicEditItem(messageId, classic, prio, key, replyMarkup);
            item.onDone = classicDone(std::move(onDone), std::move(onForm));
            _enqueue(std::move(item));
            return;
        }
        const uint64_t seq = _takeOutSeq(); // the rich edit and its classic fallback keep one place in the order
        OutItem item = _richEditItem(messageId, rich, prio, key, replyMarkup, picture.get(), seq, 0);
        // A refused rich edit is made again with the classic render, right after it in the order and WITHOUT a key: queued from
        // this answer, a key would erase a newer edit of the same message queued meanwhile, and the stale text would win.
        // `classicSub` is the slot it takes: 1 right after the rich edit, 2 after the §21.42 retry without the picture (slot 1).
        auto self = weak_from_this();
        const auto plainDone = [self, messageId, classic = std::move(classic), prio, replyMarkup, onDone, onForm, seq, classicDone](uint32_t classicSub) -> std::function<void(const TgResult&)> {
            return [self, messageId, classic, prio, replyMarkup, onDone, onForm, seq, classicDone, classicSub](const TgResult& r) {
                if (RichEditRefusalWantsFallback(r))
                {
                    if (auto me = self.lock())
                    {
                        if (r.httpStatus == 404 && !me->_richUnavailable.exchange(true))
                        {
                            me->_log(L"[msg] the Bot API has no rich messages (404: " + r.description + L"): titles go with their color square, as HTML, for the rest of this run");
                        }
                        else if (r.httpStatus != 404)
                        {
                            me->_log(L"[msg] Telegram refused an edit with a title chip (" + r.description + L"); the message is edited as HTML with the color square");
                        }
                        OutItem fb = me->_classicEditItem(messageId, classic, prio, L"", replyMarkup);
                        fb.seq = seq;
                        fb.subSeq = classicSub;
                        fb.onDone = classicDone(onDone, onForm);
                        me->_enqueue(std::move(fb));
                    }
                    return;
                }
                if (onForm && (r.ok || r.NotModified()))
                {
                    onForm(r.ok ? ResultIsRichMessage(r) : true); // not modified: it already shows this rich text
                }
                if (onDone)
                {
                    onDone(r);
                }
            };
        };
        if (!picture)
        {
            item.onDone = plainDone(1);
        }
        else
        {
            // §21.42: a refused edit WITH its picture is made once more WITHOUT it (rich, the next slot, no key), so the card's text
            // still lands with its title chip; only that retry's refusal goes classic. "Message is not modified" and "message to edit
            // not found" are no refusals here (RichEditRefusalWantsFallback): the caller's own handling sees them. A 404 means no
            // rich messages at all: straight to the classic fallback.
            item.onDone = [self, messageId, rich, prio, replyMarkup, seq, plainDone](const TgResult& r) {
                if (RichEditRefusalWantsFallback(r) && r.httpStatus != 404)
                {
                    if (auto me = self.lock())
                    {
                        me->_noteCardPictureRefused(r);
                        OutItem plain = me->_richEditItem(messageId, rich, prio, L"", replyMarkup, nullptr, seq, 1);
                        plain.onDone = plainDone(2);
                        me->_enqueue(std::move(plain));
                    }
                    return;
                }
                plainDone(1)(r);
            };
        }
        _enqueue(std::move(item));
    }

    void MessagingService::_react(int64_t messageId, std::wstring_view emoji)
    {
        if (messageId <= 0)
        {
            return;
        }
        OutItem item;
        item.prio = SendPriority::Backfill;
        item.key = L"react:" + std::to_wstring(messageId);
        item.method = L"setMessageReaction";
        item.paced = false;
        item.params = json::Value::MkObj();
        item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        item.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
        auto arr = json::Value::MkArr();
        if (!emoji.empty())
        {
            auto re = json::Value::MkObj();
            re.Set(L"type", json::Value::MkStr(L"emoji"));
            re.Set(L"emoji", json::Value::MkStr(std::wstring{ emoji }));
            arr.Push(std::move(re));
        }
        item.params.Set(L"reaction", std::move(arr));
        _enqueue(std::move(item));
    }

    void MessagingService::_sendDocument(int64_t threadId, std::wstring_view fileName, std::string bytes, const std::wstring& caption, SendPriority prio)
    {
        OutItem item;
        item.prio = prio;
        item.document = true;
        item.fileName = std::wstring{ fileName };
        item.fileBytes = std::move(bytes);
        item.params = json::Value::MkObj();
        item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        if (threadId > 0)
        {
            item.params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(threadId)));
        }
        if (!caption.empty())
        {
            item.params.Set(L"caption", json::Value::MkStr(caption.size() > 1000 ? caption.substr(0, 1000) : caption));
        }
        item.params.Set(L"disable_notification", json::Value::MkBool(true));
        _enqueue(std::move(item));
    }

    void MessagingService::_sendPhoto(int64_t threadId, std::wstring fileName, std::string bytes, std::wstring caption, SendPriority prio)
    {
        OutItem item;
        item.prio = prio;
        item.document = true;
        item.photo = true;
        item.fileName = fileName;
        // A copy for the fallback: a photo Telegram refuses goes again as a document. A very wide terminal can pass a photo's
        // limits (width + height at most 10,000 pixels, one side at most 20 times the other); a document has none of them.
        auto kept = std::make_shared<std::string>(bytes);
        item.fileBytes = std::move(bytes);
        item.params = json::Value::MkObj();
        item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        if (threadId > 0)
        {
            item.params.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(threadId)));
        }
        if (!caption.empty())
        {
            item.params.Set(L"caption", json::Value::MkStr(caption.size() > 1000 ? caption.substr(0, 1000) : caption));
        }
        item.params.Set(L"disable_notification", json::Value::MkBool(true)); // an answer to the user's own tap: silent (D5)
        auto self = weak_from_this();
        item.onDone = [self, threadId, fileName, kept, caption, prio](const TgResult& r) {
            // Only a refusal OF THE PHOTO falls back: a deleted topic would refuse the document too, and a transport failure or a
            // 429 is the send loop's (a fallback there would send the picture twice once the first one gets through).
            if (r.ok || r.httpStatus != 400 || r.ThreadNotFound())
            {
                return;
            }
            auto me = self.lock();
            if (!me)
            {
                return;
            }
            me->_log(L"[msg] screen picture: Telegram refused it as a photo (" + r.description + L"); it goes as a document");
            me->_sendDocument(threadId, fileName, std::move(*kept), caption, prio);
        };
        _enqueue(std::move(item));
    }

    std::wstring MessagingService::_cardMarkupFor(const std::wstring& sessionId, const CardButtonFacts& facts, CardPage page, json::Value& markupOut) const
    {
        // Card buttons use STABLE callback data ("c:<verb>:<session id>", at most 49 bytes of the 64 allowed): each verb acts
        // on the session's CURRENT state (a toggle reads it first), so a stale button cannot do something old (M5 is about
        // answers and queue items, which use tokens). §21.24: two pages - BuildCardButtonRows lays them out. §21.39: and the ALL
        // SCREEN page, whose ⭐ marks the cog's default level - a setting, not a fact of the session, so it is set here for every
        // caller (the card's hash covers the buttons: a changed default re-edits a card that shows the picker, and no other).
        CardButtonFacts withLevel = facts;
        withLevel.allScreenLevel = static_cast<TurnsLevel>(_allScreenLevel.load());
        markupOut = _buttons(BuildCardButtonRows(sessionId, withLevel, page));
        return {};
    }

    // ===== topics = tabs =====================================================================

    void MessagingService::_ensureSearchTopic()
    {
        int64_t thread = 0;
        {
            std::lock_guard lk{ _mtx };
            thread = _state.searchThreadId;
        }
        if (thread == 0)
        {
            // §21.18: the start note makes a NEW Search topic when the user deleted the old one, so this call can now meet a
            // chat under a flood verdict: it takes its turn and respects a parked method like the sync's topic calls
            // (_awaitCallSlot, _handleTickFlood), and a refusal is tried again kSearchCreateRetryMs later — not on every 1.5 s
            // tick, which is how a flood verdict grows.
            if (WallMs() < _searchCreateRetryAtMs || !_awaitCallSlot(L"createForumTopic"))
            {
                return;
            }
            auto p = json::Value::MkObj();
            p.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            p.Set(L"name", json::Value::MkStr(L"\U0001F50E Search"));
            p.Set(L"icon_color", json::Value::MkNum(static_cast<double>(kTelegramTopicColors[0])));
            const TgResult r = _api->Call(L"createForumTopic", p, 20000);
            if (!r.ok)
            {
                _handleTickFlood(L"createForumTopic", r); // a 429: a short verdict waited out here, a long one parks the method
                _searchCreateRetryAtMs = WallMs() + kSearchCreateRetryMs;
                _setStatus(L"Connected, but the Search topic could not be created: " + r.description);
                return;
            }
            thread = r.result.I64At(L"message_thread_id", 0);
            {
                std::lock_guard lk{ _mtx };
                _state.searchThreadId = thread;
                _stateDirty = true;
            }
            _log(L"[msg] topic created: Search (thread " + std::to_wstring(thread) + L")");
            _sendText(thread,
                      L"<b>\U0001F50E Search</b> \x2014 ask for a session in plain words (\x201Cthe session where I fixed the tab drag crash\x201D) and the Sessions page's search agent finds it. Tap a result's number to pick it: Resume a closed one into a tab, Activate a deactivated tab where it is, Wake an idle one, or Fork it.\n\n"
                      L"This is one conversation for the whole app: it carries on after a restart, and /clear (or \x2298 Clear under an answer) starts it over.\n\n"
                      L"Fleet commands: /idle (idle tabs), /board (counts), /newsession &lt;folder&gt;, /pauseall, /resumeall, /status, /help.\n\n"
                      L"Pin this topic at the top of the list (Telegram has no way for a bot to do it).",
                      true,
                      SendPriority::Final,
                      L"",
                      json::Value{},
                      nullptr);
        }
        if (!_startNotePosted)
        {
            _startNotePosted = true;
            // The chat's "/" menu: one list for the whole chat (setMyCommands with the chat scope). Sent once per start
            // (one call), not only when the Search topic is first made: a build that adds a command (§21.8's /recreate)
            // must reach a chat whose menu an older build set.
            {
                auto cmds = json::Value::MkArr();
                const std::pair<const wchar_t*, const wchar_t*> list[] = {
                    { L"queue", L"the queued prompts (session topic)" },
                    { L"sendnow", L"send the next queued prompt now" },
                    { L"steer", L"send a message NOW, into a running turn too: /steer <text>" }, // §21.33
                    { L"runner", L"runner auto|paused" },
                    { L"pin", L"never idle: keep this topic" },
                    { L"unpin", L"let the session idle again" },
                    { L"wake", L"Idle -> Waiting for you" },
                    { L"transcript", L"the session as a .md file [minimal|compact|default|full|max]" },
                    { L"summary", L"the session's summary" },
                    { L"screen", L"the terminal's visible text" },
                    { L"screenimage", L"the terminal's screen as a picture" }, // §21.24 (the card's 🖼 Screen Image)
                    { L"allscreen", L"the whole conversation as tall pictures [minimal|compact|default|full|max] [diffs] (bare: the default level)" }, // §21.39 (the card's 📜 All Screen ▸)
                    { L"download", L"a file's download link: /download <path>" }, // §21.32.4 (the file links)
                    { L"status", L"Claude Code's status and plan usage (after the session's card / the bot's line)" }, // §21.40
                    { L"search", L"ask the \U0001F50E Search agent from any topic: /search <words>" }, // §21.19
                    // §21.19: a menu command's name holds only a-z, 0-9 and "_" (Telegram refuses the WHOLE list otherwise), so
                    // the user's /search-tab is listed in the form a tap sends; typed, /search-tab works the same.
                    { L"search_tab", L"jump to the \U0001F50E Search topic (from any topic)" },
                    { L"rename", L"rename the tab and the topic" },
                    { L"restartsession", L"restart the session (asks while running)" },
                    { L"deactivate", L"park the tab (asks while running)" },
                    { L"activate", L"start a parked tab's claude" },
                    { L"reactivate", L"resume a parked tab or a cut-off / failed session" },
                    { L"forksession", L"fork into a new tab" },
                    { L"newsession", L"a new session in this folder (or <folder> in Search)" },
                    { L"interrupt", L"stop the running turn (asks first)" },
                    { L"idle", L"Search: the idle tabs" },
                    { L"board", L"Search: counts per state" },
                    { L"recreate", L"Search: make the topics again in MOST RECENT order" },
                    { L"pauseall", L"pause every runner" },
                    { L"resumeall", L"resume every runner" },
                    { L"help", L"what you can do here" },
                };
                for (const auto& [c, d] : list)
                {
                    auto o = json::Value::MkObj();
                    o.Set(L"command", json::Value::MkStr(c));
                    o.Set(L"description", json::Value::MkStr(d));
                    cmds.Push(std::move(o));
                }
                auto sp = json::Value::MkObj();
                sp.Set(L"commands", std::move(cmds));
                auto scope = json::Value::MkObj();
                scope.Set(L"type", json::Value::MkStr(L"chat"));
                scope.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
                sp.Set(L"scope", std::move(scope));
                OutItem item;
                item.prio = SendPriority::Backfill;
                item.method = L"setMyCommands";
                item.params = std::move(sp);
                item.paced = false;
                _enqueue(std::move(item));
            }
            // §21.18, the START NOTE (the user's request of 2026-10-18: a message in Search at every launch, "to notice
            // restarts or regain accessibility"). Its facts are gathered once per start of the bot: the previous run's last
            // marker (run.marker.last — when that run was last seen alive, and its version) and the close stamp
            // (run.closed — whether the app closed or died), both read AND removed here, so no later start reports them
            // again. The note itself goes through _pumpStartNote (below, and on later ticks until it has arrived).
            // ⚠ The v1 note (2026-10-16..18) read every end as a crash — the app quits with TerminateProcess and never runs
            // Stop, so the marker was always there — and put its time in an EMPTY <tg-time>, which Telegram shows as nothing.
            const std::wstring marker = _profile + L"\\messaging\\run.marker";
            const std::wstring closedFile = _profile + L"\\messaging\\run.closed";
            StartNoteFacts facts;
            try
            {
                facts.previous = ParseRunMarker(ReadFileBytes(marker + L".last"));
                facts.closedMs = ParseRunClosed(ReadFileBytes(closedFile));
            }
            catch (...)
            {
                // Rule #18. Recovery: no history line — a file that cannot be read costs the note its history, never the note.
                LogSwallowedException(L"MessagingService::_ensureSearchTopic (start note files)");
                facts.previous = RunMarker{};
                facts.closedMs = 0;
            }
            ::DeleteFileW((marker + L".last").c_str());
            ::DeleteFileW(closedFile.c_str());
            const int64_t processStartedMs = ThisProcessStartUnixMs();
            facts.kind = ClassifyStartNote(_botStarts, processStartedMs, _startedMs);
            facts.version = _versionWord;
            facts.processStartedMs = processStartedMs;
            facts.botStartedMs = _startedMs;
            {
                std::lock_guard lk{ _mtx };
                facts.topics = _state.topics.size();
            }
            const wchar_t* kindWord = facts.kind == StartNoteKind::AppLaunch ? L"the app's launch" : (facts.kind == StartNoteKind::BotStarted ? L"the bot started in a running app" : L"the bot restarted in a running app");
            std::wstring previousWord = L"no previous run known";
            if (facts.previous.aliveMs > 0)
            {
                previousWord = std::wstring{ StartNoteRunClosedCleanly(facts) ? L"the previous run closed" : L"the previous run did not close" } + L" (last seen alive " +
                               TgClockText(facts.previous.aliveMs, WallMs()) + (facts.previous.version.empty() ? std::wstring{} : L", " + facts.previous.version) + L")";
            }
            _log(L"[msg] start note: " + std::wstring{ kindWord } + L", version " + (facts.version.empty() ? std::wstring{ L"unknown" } : facts.version) + L", " + previousWord + L", " +
                 std::to_wstring(facts.topics) + L" topic(s) kept");
            _startNote = std::move(facts);
            _startNoteRetryAtMs.store(0);
            _startNoteState.store(kStartNoteDue);
        }
        _pumpStartNote();
    }

    void MessagingService::_appNote(const std::wstring& html, bool sound)
    {
        int64_t thread = 0;
        {
            std::lock_guard lk{ _mtx };
            thread = _state.searchThreadId;
        }
        _sendText(thread, html, !sound, SendPriority::Final, L"", json::Value{}, nullptr);
    }

    void MessagingService::_pumpStartNote()
    {
        // The retry time is stored BEFORE the state (both seq_cst, _onStartNoteSent's order), so a "due" seen here comes
        // with the wait that belongs to it.
        if (_startNoteState.load() != kStartNoteDue)
        {
            return;
        }
        const int64_t now = WallMs();
        if (now < _startNoteRetryAtMs.load())
        {
            return;
        }
        int64_t thread = 0;
        {
            std::lock_guard lk{ _mtx };
            thread = _state.searchThreadId;
        }
        if (thread == 0)
        {
            return; // no Search topic (yet, or again after it was deleted): _ensureSearchTopic makes it, a later tick sends
        }
        _startNoteState.store(kStartNoteQueued);
        const bool rings = StartNoteRings(_startNote);
        const int attempt = _startNoteAttempts.load() + 1;
        // Built with THIS attempt's clock: a note that goes out late (a failed first send, a slow connect) says so itself.
        const std::wstring html = BuildStartNoteText(_startNote, now);
        _log(L"[msg] start note queued for the Search topic (thread " + std::to_wstring(thread) + L", " + (rings ? L"with sound" : L"silent") +
             (attempt > 1 ? L", send " + std::to_wstring(attempt) : std::wstring{}) + L")");
        auto self = weak_from_this();
        _sendText(thread, html, !rings, SendPriority::Final, L"", json::Value{}, [self, thread](const TgResult& r) {
            if (auto me = self.lock())
            {
                me->_onStartNoteSent(thread, r);
            }
        });
    }

    void MessagingService::_onStartNoteSent(int64_t threadId, const TgResult& r)
    {
        const int attempt = _startNoteAttempts.fetch_add(1) + 1;
        if (r.ok)
        {
            _startNoteState.store(kStartNoteDone);
            _log(L"[msg] start note posted in the Search topic (message " + std::to_wstring(r.result.I64At(L"message_id", 0)) + L")");
            return;
        }
        const std::wstring why = r.description.empty() ? (r.transportFailed ? std::wstring{ L"no answer from Telegram" } : L"HTTP " + std::to_wstring(r.httpStatus)) : r.description;
        if (attempt >= kStartNoteMaxAttempts)
        {
            _startNoteState.store(kStartNoteDone);
            _log(L"[msg] start note NOT posted after " + std::to_wstring(attempt) + L" sends (" + why + L"): given up");
            return;
        }
        if (r.ThreadNotFound())
        {
            // The user deleted the Search topic in Telegram: forget it (only if it is still the one this attempt went to — a
            // newer topic may exist already), so the next tick's _ensureSearchTopic makes a new one with its intro, and the
            // note goes there. The Search topic is the user's way in from the phone — the note's whole point.
            bool forgot = false;
            {
                std::lock_guard lk{ _mtx };
                if (_state.searchThreadId == threadId)
                {
                    _state.searchThreadId = 0;
                    _stateDirty = true;
                    forgot = true;
                }
            }
            _log(L"[msg] start note: the Search topic (thread " + std::to_wstring(threadId) + L") is gone in Telegram" +
                 (forgot ? std::wstring{ L"; a new one is made and the note goes there" } : std::wstring{ L"; it was replaced already" }));
            _startNoteRetryAtMs.store(0);
            _startNoteState.store(kStartNoteDue);
            return;
        }
        // Passing failures: no answer at all (no network, a timeout), Telegram's own trouble (5xx), or a flood verdict the send
        // loop's one wait did not outlast. Anything else is about the note itself and would be refused again.
        const bool passing = r.transportFailed || r.httpStatus >= 500 || r.httpStatus == 429;
        if (!passing)
        {
            _startNoteState.store(kStartNoteDone);
            _log(L"[msg] start note NOT posted: Telegram refused it (" + why + L"); not sent again");
            return;
        }
        int64_t wait = kStartNoteRetryFirstMs;
        for (int i = 1; i < attempt && wait < kStartNoteRetryMaxMs; ++i)
        {
            wait *= 2;
        }
        wait = (std::min)(wait, kStartNoteRetryMaxMs); // (std::min): the <windows.h> macro
        _startNoteRetryAtMs.store(WallMs() + wait);
        _startNoteState.store(kStartNoteDue);
        _log(L"[msg] start note not posted (" + why + L"): sent again in " + TgShortSpan(wait) + L" (send " + std::to_wstring(attempt) + L" of " +
             std::to_wstring(kStartNoteMaxAttempts) + L")");
    }

    void MessagingService::NoteAppClosing() noexcept
    {
        try
        {
            if (!_running.load())
            {
                return; // no marker of this run: a stamp would be compared with an older run's marker and prove nothing
            }
            const int64_t now = WallMs();
            // First the hold (the tick must not write a marker after the stamp), then the stamp itself.
            _appClosingAtMs.store(now);
            const std::wstring closedFile = _profile + L"\\messaging\\run.closed";
            if (WriteFileAtomic(closedFile, std::to_string(static_cast<long long>(now))))
            {
                _log(L"[msg] the app's last window closed: run.closed stamped, so the next start note says this run closed");
            }
            else
            {
                _log(L"[msg] the app's last window closed, but run.closed could not be written (error " + std::to_wstring(::GetLastError()) +
                     L"): the next start note will say this run did not close normally");
            }
            // The STATE FILE as well: the app ends with TerminateProcess, so Stop never saves it, and the mirror's progress since
            // the last save (up to kStateProgressSaveGapMs) would be mirrored a second time at the next start. Forced, and AFTER
            // the stamp (a slow or refused save must not cost the clean-end record). This runs under the engine's window mutex,
            // so the wait for the service's locks is bounded (kAppCloseSaveLockWaitMs): a save the tick is making right now is
            // waited for, then this one writes whatever that save did not include.
            _saveStateIfDirty(true, kAppCloseSaveLockWaitMs);
            // §21.32.4: the linked set too (its lock is never held across I/O, so the wait is short): links made in the last
            // seconds would otherwise answer 404 after the restart.
            _filesSaveStore(true);
        }
        catch (...)
        {
            // Rule #18; recovery: no stamp (the safe side), or no state flush (the next start then mirrors the last few
            // seconds again).
            LogSwallowedException(L"MessagingService::NoteAppClosing");
        }
    }

    void MessagingService::_syncTopics()
    {
        // The facts: every live session, lean (SnapshotLiveForCards carries title / state / tabToken /
        // kind / pinned — no queue).
        std::vector<TabFact> facts;
        std::unordered_map<std::wstring, SessionInfo> byId;
        const TabColorMode colorMode = LoadAppSettings().tabColorMode;
        // §21.13: the colors the topic names, cards and title mentions show — resolved like the tab, from ONE dir-colors.json
        // snapshot for the pass (the per-session form stats the file and copies its map per call). A mode switch is stamped:
        // the re-colors it causes change the topics quietly for kColorModeSettleMs (NoticeOldSquare).
        const int64_t colorNowMs = WallMs();
        if (_colorModeSeen >= 0 && _colorModeSeen != static_cast<int>(colorMode))
        {
            _colorModeChangedMs = colorNowMs;
            _log(L"[msg] tab color mode changed (" + std::to_wstring(_colorModeSeen) + L" -> " + std::to_wstring(static_cast<int>(colorMode)) +
                 L"): the topic names take the new colors quietly");
        }
        _colorModeSeen = static_cast<int>(colorMode);
        _colorMode.store(_colorModeSeen);
        DirColorIndex dirColors;
        try
        {
            dirColors = LoadDirColorIndex();
        }
        catch (...)
        {
            // Rule #18. Recovery: an empty index — every folder resolves to its automatic color this pass.
            LogSwallowedException(L"MessagingService::_syncTopics (dir colors)");
        }
        _orderKeys.clear();
        _runningIds.clear(); // the quiet re-sort (§21.9) waits while any session runs
        for (auto& s : _registry->SnapshotLiveForCards())
        {
            TabFact f;
            f.sessionId = s.id;
            f.title = s.title;
            f.state = s.state;
            f.live = s.live;
            f.inTab = !s.tabToken.empty();
            f.claude = (s.kind == AgentKind::Claude);
            f.pinned = s.pinned;
            f.lastActivityMs = s.lastActivityUnixMs; // a new tab's creation instant: it is listed before its first message
            f.tabToken = s.tabToken; // per tab: a topic follows its tab to the next conversation (Rekey)
            f.continuedFromId = s.continuedFromId; // ...also to a /handover-here successor, which takes the tab on a new connection
            f.mostRecentMs = MostRecentSortKeyMs(s); // the side panel's MOST RECENT key: the creates' order (§21.8)
            f.stateSinceMs = s.stateChangedAtUnixMs; // how long the state has held: the topic name's glyph waits for it (§21.11)
            try
            {
                f.colorHex = ResolveSessionColorHex(colorMode, s, dirColors); // §21.13: the tab's color, as the tab resolves it
            }
            catch (...)
            {
                // Rule #18. Recovery: the color the last pass resolved (none on a first sight), so a failed resolve never
                // reads as a change of the tab's color.
                LogSwallowedException(L"MessagingService::_syncTopics (color)");
                if (const auto c = _colorSince.find(s.id); c != _colorSince.end())
                {
                    f.colorHex = c->second.first;
                }
            }
            // Since when the color holds: a first sight in this run is settled (0); a change restarts the clock, so a name
            // follows a color only once it has held (an inferred folder that flips back and forth renames nothing).
            if (const auto c = _colorSince.find(s.id); c == _colorSince.end())
            {
                _colorSince.emplace(s.id, std::make_pair(f.colorHex, int64_t{ 0 }));
            }
            else if (c->second.first != f.colorHex)
            {
                c->second = { f.colorHex, colorNowMs };
            }
            f.colorSinceMs = _colorSince[s.id].second;
            _orderKeys[s.id] = { f.mostRecentMs, s.title }; // ...and the tick's posting order
            if (s.state == SessionState::Running)
            {
                _runningIds.insert(s.id);
            }
            facts.push_back(std::move(f));
            byId.emplace(s.id, std::move(s));
        }
        _lastFacts = facts; // the state glyph renames read them after the sync (_stateNameTick)
        // §21.13: a session that left the live set forgets its color clock (it starts settled when it comes back).
        for (auto c = _colorSince.begin(); c != _colorSince.end();)
        {
            c = byId.count(c->first) ? std::next(c) : _colorSince.erase(c);
        }
        const bool colorModeSettled = _colorModeChangedMs <= 0 || colorNowMs - _colorModeChangedMs >= kColorModeSettleMs;
        {
            // The tab each topic's session is in NOW (a restart gives every tab a new token, and the topic's tab is
            // what a later Rekey matches): kept in step here, before the plan reads the records.
            std::lock_guard lk{ _mtx };
            for (auto& [sid, t] : _state.topics)
            {
                if (const auto it = byId.find(sid); it != byId.end() && it->second.live && !it->second.tabToken.empty() && it->second.tabToken != t.tabToken)
                {
                    t.tabToken = it->second.tabToken;
                    _stateDirty = true;
                }
                // §21.13: a name whose square already IS the tab's, recorded under another mode (a topic named before the
                // square in the "Remove colors" mode, a mode switch that left its color as it was), takes the mode SILENTLY —
                // no rename is needed. Only once the mode and the color have held: an Individual switch deals new colors
                // moments after it, and a topic stamped before the deal would tell the deal as the user's recolor.
                const auto c = _colorSince.find(sid);
                if (c != _colorSince.end() && colorModeSettled && t.nameColorMode != static_cast<int>(colorMode) &&
                    (c->second.second <= 0 || colorNowMs - c->second.second >= kNameStateSettleMs) && t.nameColor == TgTitleColorSquare(c->second.first))
                {
                    t.nameColorMode = static_cast<int>(colorMode);
                    _stateDirty = true;
                }
            }
        }
        // The start-up hold's evidence: a session seen live in this run has re-homed, so its topic follows the
        // plan from then on (a tab the user closes 30 s after a start loses its topic at once). The set is
        // only consulted inside the hold and is dropped when the hold ends.
        const int64_t nowMs = WallMs();
        const int64_t sinceStartMs = nowMs - _startedMs;
        size_t restoring = 0;
        if (_hooks.windowsRestoring && sinceStartMs < kStartupCloseHoldCapMs)
        {
            try
            {
                restoring = _hooks.windowsRestoring();
            }
            catch (...)
            {
                LogSwallowedException(L"MessagingService::_syncTopics (windowsRestoring)");
            }
        }
        const bool inStartupHold = sinceStartMs < kStartupCloseHoldMinMs || restoring > 0;
        if (inStartupHold)
        {
            _startupHoldOpen = true;
            for (const auto& f : facts)
            {
                if (f.live)
                {
                    _seenLive.insert(f.sessionId);
                }
            }
        }
        else
        {
            if (_startupHoldOpen)
            {
                // Once per start, the moment the last restoring window is done (or the cap ran out).
                _startupHoldOpen = false;
                _log(L"[msg] start-up hold ended " + std::to_wstring(sinceStartMs / 1000) + L" s after the start (" + (sinceStartMs < kStartupCloseHoldCapMs ? L"every window restored" : L"the cap ran out") + L")");
            }
            if (!_seenLive.empty())
            {
                _seenLive.clear();
            }
        }
        // The RECREATION (§21.8): topics made under an older layout are made again ONCE, after the start-up hold — when
        // every window has re-homed its tabs, so every tab that should have a topic is live and gets one. Until then the
        // old topics are left as they are (no create, no delete); a state with no session topic just records the layout.
        {
            bool recreate = false;
            bool waitForHold = false;
            bool busy = false;
            int oldLayout = kTopicLayoutVersion;
            {
                std::lock_guard lk{ _mtx };
                busy = _recreating || !_state.orphanThreads.empty();
                oldLayout = _state.topicLayout;
                if (_state.topicLayout < kTopicLayoutVersion)
                {
                    if (!NeedsTopicRecreation(_state))
                    {
                        _state.topicLayout = kTopicLayoutVersion;
                        _stateDirty = true;
                    }
                    else if (inStartupHold)
                    {
                        waitForHold = true;
                    }
                    else
                    {
                        recreate = true;
                    }
                }
            }
            if (waitForHold)
            {
                return;
            }
            if (recreate)
            {
                // Layout 1 (§21.8) made the list in MOST RECENT order; layout 2 (§21.9) puts every session's whole history in
                // its topic.
                _recreateTopics(L"topics of layout " + std::to_wstring(oldLayout) + L", this build makes layout " + std::to_wstring(kTopicLayoutVersion) +
                                L" (" + (oldLayout < 1 ? L"the MOST RECENT order and " : L"") + L"every session's whole history)");
            }
            else if (_recreateRequested.load() && !inStartupHold)
            {
                // The user's /recreate (confirmed in the Search topic). One under way already answers it.
                _recreateRequested.store(false);
                if (busy)
                {
                    _log(L"[msg] /recreate: a recreation is under way already");
                }
                else
                {
                    _recreateTopics(L"the user's /recreate");
                }
            }
        }
        // A recreation's old topics are deleted FIRST: no other topic change until they are all gone, so the new list is
        // built from nothing and comes out in order (a stop midway resumes here at the next start: the list is persisted).
        {
            bool orphans = false;
            {
                std::lock_guard lk{ _mtx };
                orphans = !_state.orphanThreads.empty();
            }
            if (orphans)
            {
                if (!_recreating)
                {
                    // A recreation a stop interrupted: it goes on (and still reports when the new list is complete).
                    _recreating = true;
                    _recreatedCount = 0;
                    _recreateStartedMs = nowMs;
                    _backfillAfterRecreation = true; // the new list's history replay is re-sorted after it, as in the first run
                    _log(L"[msg] recreation resumed: old topics still to delete from the last run");
                }
                _deleteOrphanTopics(kTopicOpsPerTick);
                return;
            }
            if (_recreating && inStartupHold)
            {
                return; // a resumed recreation builds its new list after the hold too, when every tab is live
            }
        }
        // A topic the user asked to make again in a mode (§21.11, /recreate in the topic): one per tick, before the plan (the
        // plan then sees the new record and leaves it alone).
        {
            std::pair<std::wstring, int> one;
            {
                std::lock_guard lk{ _mtx };
                if (!_recreateOne.empty())
                {
                    one = std::move(_recreateOne.front());
                    _recreateOne.pop_front();
                }
            }
            if (!one.first.empty())
            {
                _recreateOneTopic(one.first, one.second);
            }
        }
        // §21.17: a session started from a thread the user opened becomes THAT thread's topic — bound before the plan, so the plan
        // finds its record and makes no topic of its own for it. §21.25: inside the start-up hold no launch times out (a restored
        // one's session may only be waiting for its window to re-home it).
        _adoptBoundThreads(byId, colorMode, inStartupHold);
        std::vector<TopicSyncOp> ops;
        {
            std::lock_guard lk{ _mtx };
            ops = PlanTopicSync(facts, _state.topics, nowMs, _idleGraceMs.load());
        }
        size_t apiOps = 0;
        size_t heldCloses = 0;
        size_t windowHeldCloses = 0; // PERSISTENCE.md §16: "closed" deletes kept because a window holds the session's tab
        bool createsPlanned = false;
        for (const auto& op : ops)
        {
            if (_stop.load())
            {
                return;
            }
            switch (op.kind)
            {
            case TopicSyncOp::Kind::Delete:
                if (op.reason == L"closed" && inStartupHold && _seenLive.find(op.sessionId) == _seenLive.end())
                {
                    // The start-up hold: "closed" here most likely means "its window has not re-homed its tabs
                    // yet". The plan re-emits the delete every tick; once the session comes back live the plan
                    // stops asking, and a session still closed when the hold ends is deleted then.
                    ++heldCloses;
                    break;
                }
                if (op.reason == L"closed" && _sessionHeldByAWindow(op.sessionId))
                {
                    // The WINDOW HOLD (PERSISTENCE.md §16): a window keeps this session's saved tab although its restore
                    // could not make it (Claude Code not found at this start, a preset that cannot launch). The user closed
                    // nothing, so the topic and its history stay; the plan re-asks every tick, and the delete happens only
                    // once no window holds the ref any more (the tab came back — then the plan stops asking — or its
                    // window was closed).
                    ++windowHeldCloses;
                    break;
                }
                if (apiOps++ < kTopicOpsPerTick)
                {
                    _deleteTopic(op.sessionId, op.reason);
                }
                break;
            case TopicSyncOp::Kind::Create:
                // The plan lists the creates lowest-ranked first (§21.8), so a tick's budget makes the oldest ones and
                // the next tick goes on from there: the topic that ranks highest is made last and lands on top.
                createsPlanned = true;
                if (apiOps++ < kTopicOpsPerTick)
                {
                    std::wstring color;
                    if (const auto it = byId.find(op.sessionId); it != byId.end())
                    {
                        try
                        {
                            color = ResolveSessionColorHex(colorMode, it->second);
                        }
                        catch (...)
                        {
                            LogSwallowedException(L"MessagingService::_syncTopics (color)");
                        }
                    }
                    _createTopic(op.sessionId, op.title, color, op.state);
                    if (_recreating)
                    {
                        std::lock_guard lk{ _mtx };
                        _recreatedCount += _state.topics.count(op.sessionId);
                    }
                }
                break;
            case TopicSyncOp::Kind::Rename:
                if (apiOps++ < kTopicOpsPerTick)
                {
                    _renameTopic(op.sessionId, op.title, op.state);
                }
                break;
            case TopicSyncOp::Kind::Rekey:
                // The tab's conversation changed (/clear, /resume, a handover in place): the topic follows the tab.
                // A rename may be one API call; past the tick's budget the next tick plans it again.
                if (apiOps++ < kTopicOpsPerTick)
                {
                    _rekeyTopic(op.fromSessionId, op.sessionId, op.title);
                }
                break;
            case TopicSyncOp::Kind::IdleCard:
            {
                int64_t leavesAt = 0;
                {
                    std::lock_guard lk{ _mtx };
                    if (const auto it = _state.topics.find(op.sessionId); it != _state.topics.end())
                    {
                        it->second.idleSinceMs = nowMs;
                        _stateDirty = true;
                    }
                }
                if (const auto it = byId.find(op.sessionId); it != byId.end())
                {
                    leavesAt = IdleTopicLeavesAtMs(nowMs, it->second.lastActivityUnixMs, _idleGraceMs.load());
                }
                _log(L"[msg] topic idle " + ShortId(op.sessionId) + L" (leaves the list in " + std::to_wstring(std::max<int64_t>(0, leavesAt - nowMs) / 60000) + L" min unless pinned or woken)");
            }
                _refreshCard(op.sessionId, true);
                break;
            case TopicSyncOp::Kind::UnidleCard:
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _state.topics.find(op.sessionId); it != _state.topics.end())
                {
                    it->second.idleSinceMs = 0;
                    _stateDirty = true;
                }
            }
                _refreshCard(op.sessionId, true);
                break;
            }
        }
        if (heldCloses > 0 && !_startupHoldLogged)
        {
            // Once per start: the plan re-asks every tick, and the count only shrinks as the windows re-home.
            _startupHoldLogged = true;
            _log(L"[msg] start-up hold: " + std::to_wstring(heldCloses) + L" topic(s) kept until their sessions re-home (" + std::to_wstring(restoring) + L" window(s) still restoring; at least " + std::to_wstring(kStartupCloseHoldMinMs / 1000) + L" s after the start)");
        }
        if (windowHeldCloses != _windowHeldClosesLogged)
        {
            // Written when the count changes (the hold has no end of its own, and the plan re-asks every tick): the log
            // then shows when the window hold began, how far it shrank as tabs came back, and when it ended (0).
            _windowHeldClosesLogged = windowHeldCloses;
            _log(L"[msg] window hold: " + std::to_wstring(windowHeldCloses) +
                 L" topic(s) kept because a window holds their session's saved tab, which its restore could not make (PERSISTENCE.md 16)");
        }
        if (_recreating && !createsPlanned)
        {
            // Every tab that should have a topic has one again (a create Telegram refused is planned again, so this
            // line only comes once the whole list exists).
            _recreating = false;
            _log(L"[msg] recreation done: " + std::to_wstring(_recreatedCount) + L" topic(s) made, the oldest first, so the list reads in MOST RECENT order; each session's history follows, the most recent first");
            _appNote(L"✅ <b>Topics made again</b> \x2014 " + std::to_wstring(_recreatedCount) +
                         L", in MOST RECENT order. Each session's whole history is added now, the most recent sessions first (one message every 2 s, after the live ones); "
                         L"when all of it is in, the list is put back in MOST RECENT order.",
                     false);
        }
    }

    void MessagingService::_recreateTopics(const std::wstring& why)
    {
        size_t count = 0;
        {
            std::lock_guard lk{ _mtx };
            std::unordered_set<int64_t> oldThreads;
            for (const auto& [sid, t] : _state.topics)
            {
                if (t.threadId > 0 && oldThreads.insert(t.threadId).second)
                {
                    _state.orphanThreads.push_back(t.threadId);
                }
            }
            count = oldThreads.size();
            _state.topics.clear();
            _state.topicLayout = kTopicLayoutVersion;
            _mirrors.clear();
            _cards.clear();
            // §21.16.7: the old topics' dot requests and taps (thread ids that are about to be deleted); _frontHeld is cleared
            // with the re-sort's queue below.
            _frontRequests.clear();
            _frontTapMs.clear();
            _backfill = BackfillBatch{}; // a batch of an old topic is pointless (its queued posts go with the purge below)
            _stateDirty = true;
            // Queued work for the old topics is pointless: their cards' posts / edits / deletes and work-log edits (keyed
            // by a session id) and any message into one of their threads. The Search topic's items stay; a reaction on
            // a message of an old topic fails harmlessly.
            for (auto it = _outbox.begin(); it != _outbox.end();)
            {
                bool old = it->key.rfind(L"card", 0) == 0 || it->key.rfind(L"wl:", 0) == 0;
                if (!old)
                {
                    if (const auto* th = it->params.Find(L"message_thread_id"); th && th->type == json::Value::Type::Num)
                    {
                        old = oldThreads.count(static_cast<int64_t>(th->num)) > 0;
                    }
                }
                it = old ? _outbox.erase(it) : std::next(it);
            }
        }
        _saveStateIfDirty(true); // the orphan list must survive a crash in the next minute
        _recreating = true;
        _recreatedCount = 0;
        _recreateStartedMs = WallMs();
        _backfillAfterRecreation = true; // the new list's history replay ends with the full re-sort
        _backfillPostsBy.clear();
        _replayPosts = 0;
        _replayTopics = 0;
        _resortFull = false;
        _resortQueue.clear();
        _frontHeld.clear(); // §21.16.7: a recreation orders the whole list again; a dot's hold on an old topic means nothing now
        _log(L"[msg] recreation (" + why + L"): " + std::to_wstring(count) + L" old topic(s) are deleted first; then the list is made again, the oldest first, so it reads in MOST RECENT order");
        LogNav(L"remote telegram recreate-topics old=" + std::to_wstring(count));
        _appNote(L"♻ <b>Making the topics again</b> in MOST RECENT order (like the side panel): the " + std::to_wstring(count) +
                     L" old topic(s) are deleted first \x2014 their messages go with them; every conversation stays in its session \x2014 then one topic per open tab is made, the most recent last, so it is on top, "
                     L"and each gets its session's whole history.",
                 false);
    }

    size_t MessagingService::_deleteOrphanTopics(size_t budget)
    {
        size_t done = 0;
        bool emptied = false;
        while (done < budget && !_stop.load())
        {
            int64_t thread = 0;
            {
                std::lock_guard lk{ _mtx };
                if (_state.orphanThreads.empty())
                {
                    break;
                }
                thread = _state.orphanThreads.front();
            }
            if (!_awaitCallSlot(L"deleteForumTopic"))
            {
                break;
            }
            auto p = json::Value::MkObj();
            p.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            p.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(thread)));
            const TgResult r = _api->Call(L"deleteForumTopic", p, 20000);
            if (_handleTickFlood(L"deleteForumTopic", r))
            {
                // Telegram's pacing verdict: a short wait was waited out (stop-aware), a long one parks the method; either
                // way the same topic is tried again on a later tick.
                break;
            }
            if (!r.ok && !r.ThreadNotFound())
            {
                // Dropped from the list anyway: a topic Telegram will not delete would block the new list forever. The
                // user can delete it by hand.
                _log(L"[msg] recreation: an old topic could not be deleted (thread " + std::to_wstring(thread) + L"): " + r.description);
            }
            {
                std::lock_guard lk{ _mtx };
                if (!_state.orphanThreads.empty() && _state.orphanThreads.front() == thread)
                {
                    _state.orphanThreads.erase(_state.orphanThreads.begin());
                }
                emptied = _state.orphanThreads.empty();
                _stateDirty = true;
            }
            ++done;
        }
        if (emptied)
        {
            _log(L"[msg] recreation: the old topics are deleted; the new list is made now, the oldest first");
        }
        return done;
    }

    void MessagingService::_createTopic(const std::wstring& sessionId, const std::wstring& title, const std::wstring& colorHex, SessionState state, int verbose)
    {
        // §21.22: the state is the topic's ICON, and the name is the tab color's square and the title (§21.13) — a name that
        // started with the state glyph (§21.11) showed the state twice, since Telegram draws a topic with no icon of its own as
        // its name's first character in a colored bubble. While Telegram refuses icons in this run, the glyph form. The record
        // keeps the bare title, which the sync compares with the tab's (a long title is cut in the name only), the square and
        // the form.
        const std::wstring bare = title.empty() ? ShortId(sessionId) : title;
        const std::wstring square = TgTitleColorSquare(colorHex);
        const TopicStateIcon icon = StateTopicIcon(state);
        bool stateIcon = _stateIconsWanted();
        std::wstring name = TopicDisplayName(state, square, bare, stateIcon);
        // One attempt's parameters: the name, the creation color (the tab's — what Telegram shows if the icon is ever removed,
        // and the only color it lets a topic have) and the state icon, as far as the attempt keeps them.
        const auto params = [&](bool withColor, bool withIcon) {
            auto p = json::Value::MkObj();
            p.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            p.Set(L"name", json::Value::MkStr(name));
            if (withColor)
            {
                p.Set(L"icon_color", json::Value::MkNum(static_cast<double>(TgTopicIconColor(colorHex))));
            }
            if (withIcon)
            {
                p.Set(L"icon_custom_emoji_id", json::Value::MkStr(icon.customEmojiId));
            }
            return p;
        };
        if (!_awaitCallSlot(L"createForumTopic"))
        {
            return; // stopping, or a long 429 parks topic creation: the sync plans the topic again on a later tick
        }
        TgResult r = _api->Call(L"createForumTopic", params(true, stateIcon), 20000);
        std::wstring iconRefusal; // §21.22: Telegram's words, when it refused the create that carried the state icon
        if (!r.ok && stateIcon && TopicIconRefused(r.httpStatus, r.description))
        {
            // Telegram did not take the icon (or the color — the next answer tells): the glyph form, with the color.
            iconRefusal = r.description;
            stateIcon = false;
            name = TopicDisplayName(state, square, bare);
            if (!_awaitCallSlot(L"createForumTopic"))
            {
                return;
            }
            r = _api->Call(L"createForumTopic", params(true, false), 20000);
        }
        bool colorDropped = false;
        if (!r.ok && r.httpStatus == 400 && r.description.find(L"icon") != std::wstring::npos)
        {
            // A color Telegram did not take: create without one.
            colorDropped = true;
            if (!_awaitCallSlot(L"createForumTopic"))
            {
                return;
            }
            r = _api->Call(L"createForumTopic", params(false, false), 20000);
        }
        if (!r.ok)
        {
            // A 429 is waited out stop-aware when short and parks createForumTopic when long (the old sleep here ran the whole
            // retry_after on the tick thread and did not end for Stop); the sync plans the topic again on a later tick.
            _handleTickFlood(L"createForumTopic", r);
            _log(L"[msg] topic create failed for " + ShortId(sessionId) + L": " + r.description);
            return;
        }
        if (!iconRefusal.empty() && !colorDropped && !_stateIconRefused.exchange(true))
        {
            // Leaving the icon out is what made the create go through: Telegram refuses the state icons, so every topic shows the
            // glyph again for this run (PickNameRename's form rule). A create that also had to drop its color proves nothing
            // about the icon: that topic is made in the glyph form, and the name tick's next try of the icon finds out.
            _log(L"[msg] Telegram refused a topic icon (" + iconRefusal + L"): the topic names show the state glyph again until the bot starts again");
        }
        const int64_t thread = r.result.I64At(L"message_thread_id", 0);
        if (thread == 0)
        {
            return;
        }
        // The topic shows the session from now on (the backfill of §6.7 is a later phase; /transcript sends the
        // whole session as a file meanwhile): _seedFromTranscript decides where the mirror starts and what the
        // first card's recap is. Nothing but the card is posted here — the thread is the conversation (§21.6).
        const auto s = _session(sessionId);
        TopicRecord t;
        t.sessionId = sessionId;
        t.threadId = thread;
        t.title = bare;
        t.createdMs = WallMs();
        t.nameState = static_cast<int>(state);
        t.nameSetMs = t.createdMs;
        t.nameColor = square; // §21.13: the square the name shows, and the mode it was resolved under
        t.nameColorMode = static_cast<int>(_currentColorMode());
        t.stateIcon = stateIcon; // §21.22: the icon shows the state (the name has no glyph), or the glyph form after a refusal
        t.verbose = verbose; // §21.11: -1 follows the default; a topic recreated in a mode keeps that mode
        if (s)
        {
            t.tabToken = s->tabToken;
            _seedFromTranscript(t, *s);
        }
        {
            std::lock_guard lk{ _mtx };
            _state.topics[sessionId] = t;
            _mirrors.erase(sessionId);
            _cards.erase(sessionId);
            _stateDirty = true;
        }
        // The MOST RECENT key in the line (§21.8): a list made from nothing must log its creates oldest first.
        std::wstring rank;
        if (const auto ok = _orderKeys.find(sessionId); ok != _orderKeys.end() && ok->second.first > 0)
        {
            rank = L", most recent " + TgClockText(ok->second.first, WallMs());
        }
        _log(L"[msg] topic created " + ShortId(sessionId) + L" " + (stateIcon ? std::wstring{ icon.emoji } + L" " : std::wstring{}) + L"\x201C" + name + L"\x201D (thread " + std::to_wstring(thread) + L", " +
             (t.transcriptPath.empty() ? std::wstring{ L"no transcript yet: mirrored from its first line" } : L"mirrored from byte " + std::to_wstring(t.cursor)) + rank + L")");
        if (!s)
        {
            // The session left the registry between the plan and now: a bare card, the next sync deletes the topic.
            _postCard(sessionId, thread, L"\U0001F4CC " + TgEscapeHtml(name), json::Value{}, 0, false, 0);
            return;
        }
        // The card is the STICKY bottom message of the topic (§21.5): _refreshCard posts the first one (cardMessageId
        // is 0) with the recap rule, the buttons and the text hash, exactly as every later re-post.
        _refreshCard(sessionId, true);
    }

    void MessagingService::_adoptBoundThreads(const std::unordered_map<std::wstring, SessionInfo>& byId, TabColorMode colorMode, bool inStartupHold)
    {
        // The binds whose session is live now, and the ones that waited too long; the work (Telegram calls, registry writes) runs
        // after the lock. A READY bind is only COPIED here (who and where, not its messages): it stays in _threadBinds until its
        // thread is the session's topic AND everything it holds has been handed over (the drain at the end of the loop below),
        // so at every moment the poll thread finds the thread's bind, its topic, or both. A message the user sends meanwhile is
        // held by the bind and handed over after the ones before it. Until 2026-10-18 the bind left the list here and the topic
        // was recorded only after the registry write and the transcript seed: a message in between found neither, and got a
        // stray folder picker inside the new topic. An EXPIRED bind leaves the list here (its messages stay in the chat).
        std::vector<ThreadBind> ready;
        std::vector<ThreadBind> expired;
        {
            std::lock_guard lk{ _mtx };
            if (_threadBinds.empty())
            {
                _bindTapHolds.clear(); // §21.28: no launch is waiting, so no tap waits for one (a belt: every end lets its holds go)
                return;
            }
            const int64_t now = WallMs();
            for (auto it = _threadBinds.begin(); it != _threadBinds.end();)
            {
                if (const auto s = byId.find(it->sessionId); s != byId.end() && s->second.live)
                {
                    // The copy's `held` stays empty: the messages are taken in the lock scope that records the topic, so none the
                    // poll thread adds before that is missed, and none is handed over twice.
                    ThreadBind copy;
                    copy.sessionId = it->sessionId;
                    copy.threadId = it->threadId;
                    copy.dir = it->dir;
                    copy.title = it->title; // §21.25: a name the user gave the thread while the launch ran is in here too
                    copy.requestedMs = it->requestedMs;
                    ready.push_back(std::move(copy));
                    ++it;
                }
                else if (inStartupHold)
                {
                    // §21.25: while the windows restore their tabs, a launch's session may only be waiting to be re-homed (a launch
                    // restored from the state file, whose tab the window record kept): its deadline starts over when the hold ends.
                    it->requestedMs = now;
                    ++it;
                }
                else if (now - it->requestedMs > kNewThreadBindTimeoutMs)
                {
                    expired.push_back(std::move(*it));
                    it = _threadBinds.erase(it);
                    _stateDirty = true; // §21.25: the binds are persisted
                }
                else
                {
                    ++it;
                }
            }
            // §21.28: the holds of a launch whose bind is gone (expired just now, taken, or ended on a path that kept its entry) go:
            // the tap is answered long before (its hold's limit is seconds, a bind's timeout minutes), and the map must not grow.
            for (auto h = _bindTapHolds.begin(); h != _bindTapHolds.end();)
            {
                const uint64_t token = h->first;
                const bool bound = std::any_of(_threadBinds.begin(), _threadBinds.end(), [token](const ThreadBind& b) { return b.launchToken == token; });
                h = bound ? std::next(h) : _bindTapHolds.erase(h);
            }
        }
        for (const auto& b : expired)
        {
            // §21.23: a launch the window REFUSED (no claude.exe, a failed tab, a window that closed) never gets here — its answer
            // sent the thread back to a picker at once (_settleLaunchAnswers). What is left: the PC never answered within the
            // timeout (its window's UI thread was busy), or it opened the tab and the session never came up. The messages the bind
            // held wait in the thread's picker record — the next message posts the picker, and the pick hands them over — where
            // they were dropped before §21.23 (they stayed in the chat, unsent).
            size_t kept = 0;
            NewThreadStep backTo = NewThreadStep::Window;
            {
                std::lock_guard lk{ _mtx };
                // §21.25: the thread goes back into its WIZARD, with the choices the launch was made with (a record that exists
                // already — none should while a bind holds the thread — keeps its own, newer state; only the messages merge).
                const auto [rec, inserted] = _newThreads.try_emplace(b.threadId);
                auto& nt = rec->second;
                if (inserted)
                {
                    nt.wizard = b.wizard;
                    nt.topicName = b.title;
                    nt.pickerMessageId = 0; // the launch deleted the picker: the render posts a new one
                    // The window step when the user saw it (its list read again, shown even with one window), else the model
                    // step: a tap is the retry, never a launch by itself (NewThreadStepAfterRefusal).
                    backTo = NewThreadStepAfterRefusal(nt.wizard, RemoteLaunchResult::WindowClosed);
                    ReturnNewThreadToStep(nt.wizard, backTo, L"");
                }
                else
                {
                    backTo = nt.wizard.step;
                }
                nt.held.insert(nt.held.begin(), b.held.begin(), b.held.end());
                if (nt.held.size() > kNewThreadHeldMax)
                {
                    nt.held.resize(kNewThreadHeldMax); // the oldest ones: the bind's, then any the record already had
                }
                kept = nt.held.size();
                if (nt.topicName.empty())
                {
                    nt.topicName = b.title;
                }
                nt.lastUseMs = WallMs();
                _stateDirty = true;
            }
            const std::wstring minutes = std::to_wstring(kNewThreadBindTimeoutMs / 60000);
            _log(L"[msg] new thread " + std::to_wstring(b.threadId) + L": the session " + ShortId(b.sessionId) + L" in " + b.dir + L" did not come up within " + minutes + L" min (" +
                 (b.answered ? L"the window opened its tab" : L"no window answered the launch") + L"); back to its wizard's " + NewThreadStepName(backTo) + L" step, " +
                 std::to_wstring(kept) + L" held message(s) kept");
            _reply(b.threadId,
                   (b.answered ? L"The tab opened on the PC, but the new session did not come up within " + minutes + L" minutes." :
                                 L"The new session did not start: the PC did not answer within " + minutes + L" minutes (its window may be busy).") +
                       L" Choose again below, or cancel" + (kept > 0 ? L"; what you wrote here is kept for it." : L"."),
                   false);
            _showNewThreadPicker(b.threadId); // the wizard again, below the line (the launch deleted the old picker)
        }
        for (auto& b : ready)
        {
            const SessionInfo& s = byId.at(b.sessionId);
            // This bind's entry in _threadBinds (a bind is one thread-and-session pair; it stays there until the hand-over below
            // ends). Call it under `_mtx` only: the poll thread adds to the entry's `held` under that lock.
            const auto bindOf = [this, &b]() {
                return std::find_if(_threadBinds.begin(), _threadBinds.end(), [&b](const ThreadBind& x) { return x.threadId == b.threadId && x.sessionId == b.sessionId; });
            };
            // The thread must still be free and the session must have no topic: nothing else makes either while the bind waits
            // (the plan runs after this, and a user thread is in no record), so a hit here is a bug worth the line, not a crash.
            // A taken bind leaves the list here, in the scope that saw it taken, with what it holds.
            bool taken = false;
            size_t dropped = 0;
            {
                std::lock_guard lk{ _mtx };
                taken = _state.topics.count(b.sessionId) > 0;
                for (const auto& [sid, t] : _state.topics)
                {
                    taken = taken || t.threadId == b.threadId;
                }
                if (taken)
                {
                    if (const auto it = bindOf(); it != _threadBinds.end())
                    {
                        dropped = it->held.size();
                        _threadBinds.erase(it);
                        _stateDirty = true; // §21.25: the binds are persisted
                    }
                }
            }
            if (taken)
            {
                _log(L"[msg] new thread " + std::to_wstring(b.threadId) + L" not bound to " + ShortId(b.sessionId) + L": the thread or the session has a topic already (" +
                     std::to_wstring(dropped) + L" held message(s) dropped)");
                continue;
            }
            // At rest it waits for the user (like a revive): an Idle session's topic would leave the list after the idle grace,
            // and this one was just made from the phone.
            SessionState state = s.state;
            if (state == SessionState::Idle || state == SessionState::Done)
            {
                _registry->Update(s.id, [](SessionInfo& ss) {
                    if (ss.state == SessionState::Idle || ss.state == SessionState::Done)
                    {
                        ss.state = SessionState::WaitingForInput;
                        ss.lastActivityUnixMs = WallMs(); // the Waiting-for-you window starts now
                        ss.readUnixMs = 0;
                    }
                });
                state = SessionState::WaitingForInput;
            }
            // §21.25: a name the user gave the thread while the launch ran (forum_topic_edited reached the bind,
            // _onNewThreadRenamed) is the session's title — the launch was asked with the name the thread had then. Renamed
            // through the hosting window (the tab, the registry, Rule #11) and used for the topic's name right away, so the
            // rename below does not put the old title back. A bind's title the session already shows changes nothing.
            std::wstring bindTitle = b.title;
            if (!bindTitle.empty() && bindTitle != s.title && bindTitle != TgOneLineTitle(s.title))
            {
                try
                {
                    if (_hooks.sessionAction)
                    {
                        _hooks.sessionAction(s.id, L"rename", bindTitle, nullptr);
                    }
                    else
                    {
                        _registry->Update(s.id, [&bindTitle](SessionInfo& ss) { ss.title = bindTitle; });
                    }
                    _log(L"[msg] new thread " + std::to_wstring(b.threadId) + L": " + ShortId(s.id) + L" takes the thread's name \x201C" + bindTitle + L"\x201D (renamed while it launched)");
                }
                catch (...)
                {
                    // Rule #18. Recovery: the session keeps the title it launched with, and the topic is named after it.
                    LogSwallowedException(L"MessagingService::_adoptBoundThreads (rename to the thread's name)");
                    bindTitle.clear();
                }
            }
            else
            {
                bindTitle.clear();
            }
            // The record, as _createTopic makes one — but for the user's thread, and with no name recorded yet: the rename below
            // records it when Telegram takes it, and a refused rename leaves the name due for the state-name tick (nameState -1).
            const std::wstring bare = !bindTitle.empty() ? bindTitle : (s.title.empty() ? ShortId(s.id) : s.title);
            TopicRecord t;
            t.sessionId = s.id;
            t.threadId = b.threadId;
            t.title = bare;
            t.createdMs = WallMs();
            t.nameState = -1;
            t.nameSetMs = 0;
            t.nameColorMode = -1;
            t.tabToken = s.tabToken;
            _seedFromTranscript(t, s); // a fresh session: no transcript yet, so no history to replay, mirrored from its first line
            // The thread BECOMES the topic: the record goes in and the bind's messages come out in ONE lock scope. The bind stays,
            // empty, as the thread's hand-over queue: the poll thread adds to it what the user writes now (_handleMessage sends
            // a message of a thread that has a topic and a bind to _holdForNewThread), until the drain below finds it empty.
            std::vector<json::Value> held;
            {
                std::lock_guard lk{ _mtx };
                _state.topics[s.id] = t;
                _mirrors.erase(s.id);
                _cards.erase(s.id);
                _stateDirty = true;
                if (const auto it = bindOf(); it != _threadBinds.end())
                {
                    held.swap(it->held); // leaves the bind's own vector empty for the poll thread's next message
                }
            }
            // §21.13: the color of the topic's FIRST name — resolved fresh from the session's record as it is now (as _createTopic
            // resolves a new topic's color) and made the sync's fact for this session, settled, before the rename reads it
            // (ChooseNameColor reads _lastFact). The sync's facts come from ONE dir-colors.json snapshot taken before its
            // registry copy, and a session launched from the phone is often in that copy before its tab's first paint has given
            // its folder a color (_LaunchClaudeSession records the session, then paints the tab, which persists the color — or,
            // in the Individual mode, the session's own color): the fact then held the folder's PREVIEW color (AutoDirColorHex).
            // The first name took that square, and the next pass saw the persisted color as a change, so the name tick
            // re-squared the topic (2026-10-18 on dev: a green square, then a purple one 49 s later). The color clock entry keeps
            // the next pass from reading the fresh color as a change.
            std::wstring color;
            bool colorResolved = false;
            try
            {
                const auto current = _registry->GetWithoutQueue(s.id); // the color reads no queue
                color = ResolveSessionColorHex(colorMode, current ? *current : s);
                colorResolved = true;
            }
            catch (...)
            {
                // Rule #18. Recovery: no square in the log line, and the rename takes the sync's fact as it is.
                LogSwallowedException(L"MessagingService::_adoptBoundThreads (color)");
            }
            if (colorResolved)
            {
                for (auto& f : _lastFacts)
                {
                    if (f.sessionId == s.id)
                    {
                        f.colorHex = color;
                        f.colorSinceMs = 0; // settled: a new topic's name takes the color the tab has now, as a created one does
                        break;
                    }
                }
                _colorSince[s.id] = { color, int64_t{ 0 } };
            }
            _log(L"[msg] new thread " + std::to_wstring(b.threadId) + L" is now the topic of " + ShortId(s.id) + L" \x201C" + bare + L"\x201D in " + b.dir +
                 (color.empty() ? std::wstring{} : L" " + std::wstring{ TgTitleColorSquare(color) }) + L" (" + std::to_wstring(held.size()) + L" held message(s) handed over)");
            LogNav(L"remote telegram new-thread bound " + ShortId(s.id) + L" thread=" + std::to_wstring(b.threadId));
            // The thread's name like every topic's (state glyph, color square, title); the card follows the rename. A refused rename
            // (a flood pause) leaves both to the next ticks: the name is due (nameState -1) and the card is posted below anyway.
            // Guarded: whatever fails here, the messages below must still reach the session, and the bind must leave the list.
            try
            {
                _renameTopic(s.id, bare, state);
                _refreshCard(s.id, true);
            }
            catch (...)
            {
                // Rule #18. Recovery: the name stays due for the state-name tick (nameState -1 until a rename is recorded), and
                // the hand-over below goes on.
                LogSwallowedException(L"MessagingService::_adoptBoundThreads (name and card)");
            }
            // The tab opened in the background and has not started: start its claude now, so the first prompt does not wait for a
            // visit at the PC (_queueRemotePrompt would start it too, but held commands alone would not).
            if (!s.started && !s.external && _hooks.sessionAction)
            {
                try
                {
                    _hooks.sessionAction(s.id, L"start", L"", nullptr);
                }
                catch (...)
                {
                    LogSwallowedException(L"MessagingService::_adoptBoundThreads (start)");
                }
            }
            // What the user wrote in the thread, oldest first, as if it had been written in the topic (it was read live: no
            // backlog question) — then what the bind took while this ran, batch by batch, until a look under the lock finds it
            // empty and removes it. From then on the poll thread sends the thread's messages straight to the topic, and every
            // earlier one was handed over before that look: the session gets them in the order the user wrote them (straight
            // to the topic, a message sent during the rename would be queued ahead of the held ones).
            size_t later = 0; // the messages the bind took after the topic was recorded
            // §21.17.4: one held message's hand-over. Telegram tells a bot nothing when the user DELETES a message, so a held
            // message the user deleted while it waited was delivered anyway; and its 👀 (the hold's "seen" mark) stayed on a held
            // COMMAND for good — only a queued prompt's ✍ replaced it. One call settles both: the reaction call that takes the 👀
            // off, made directly (not through the outbox) because its ANSWER is the only sign of a deletion — "message to react
            // not found" (HeldMessageGone) leaves the message out. Any other answer hands it over, and so does a call that could
            // not be made (stopping, or a flood pause on reactions): a lost prompt costs more than a stray one. The same removal
            // is then QUEUED under the reaction's own outbox key, so a 👀 still waiting in the outbox (it is replaced) or in flight
            // (it is sent first) can never come back after it; a prompt's ✍ (_queueRemotePrompt) replaces the removal in turn.
            const auto handOver = [&](const json::Value& msg) {
                const int64_t msgId = msg.I64At(L"message_id", 0);
                if (msgId > 0 && _awaitCallSlot(L"setMessageReaction"))
                {
                    auto p = json::Value::MkObj();
                    p.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
                    p.Set(L"message_id", json::Value::MkNum(static_cast<double>(msgId)));
                    p.Set(L"reaction", json::Value::MkArr()); // an empty list takes the bot's reaction off
                    const TgResult r = _api->Call(L"setMessageReaction", p, 15000);
                    if (HeldMessageGone(r.ok, r.httpStatus, r.description))
                    {
                        _log(L"[msg] new thread " + std::to_wstring(b.threadId) + L": held message " + std::to_wstring(msgId) + L" was deleted in Telegram before " +
                             ShortId(s.id) + L" took it; left out (" + r.description + L")");
                        return;
                    }
                    _handleTickFlood(L"setMessageReaction", r); // a 429 paces the next direct calls; the message goes either way
                }
                _react(msgId, L""); // the queued removal (above); a no-op without an id
                // §21.43: a held VOICE NOTE (writing one in "All" opens the thread with it) is transcribed now that the session —
                // and so its folder's vocabulary — exists: it goes to the voice lane, and a held message behind it waits there,
                // in order, still read as held (_voiceTake's heldByNewThread). Any other message goes to the topic as before.
                if (!_voiceTake(msg, s.id, b.threadId, false, false, true))
                {
                    _handleTopicMessage(s.id, msg, b.threadId, true);
                }
            };
            for (;;)
            {
                for (const auto& msg : held)
                {
                    try
                    {
                        handOver(msg);
                    }
                    catch (...)
                    {
                        // Rule #18. Recovery: the next held message still goes; this one stays in the chat for the user to send again.
                        LogSwallowedException(L"MessagingService::_adoptBoundThreads (a held message)");
                    }
                }
                held.clear();
                std::lock_guard lk{ _mtx };
                const auto it = bindOf();
                if (it == _threadBinds.end())
                {
                    break; // nothing else removes a bind whose topic is recorded: there is nothing left to hand over
                }
                if (it->held.empty())
                {
                    _threadBinds.erase(it); // in the scope that saw it empty: no message can slip in between
                    _stateDirty = true; // §21.25: the binds are persisted
                    break;
                }
                held.swap(it->held); // the next batch; the lock ends with this pass of the loop, before the hand-over
                later += held.size();
            }
            if (later > 0)
            {
                _log(L"[msg] new thread " + std::to_wstring(b.threadId) + L": " + std::to_wstring(later) +
                     L" more message(s), sent while it was being bound, handed over after the held ones");
            }
        }
    }

    void MessagingService::_newThreadsTick()
    {
        try
        {
            const int64_t now = WallMs();
            std::vector<int64_t> idle; // wizards nobody used for kNewThreadPickerIdleMs: aborted (their threads deleted)
            std::vector<int64_t> resume; // restored window steps whose census died with the last run: asked again
            {
                std::lock_guard lk{ _mtx };
                for (const auto& [threadId, nt] : _newThreads)
                {
                    if (nt.lastUseMs > 0 && now - nt.lastUseMs > kNewThreadPickerIdleMs)
                    {
                        idle.push_back(threadId);
                    }
                }
                // Once per start, after the start-up hold (every window has registered and re-homed its tabs, so the census sees
                // the whole set): a window step that waits for a census nobody runs any more. `_startupHoldOpen` is this
                // thread's own (the sync above set it this tick).
                if (_newThreadsResumePending && !_startupHoldOpen)
                {
                    _newThreadsResumePending = false;
                    for (const auto& [threadId, nt] : _newThreads)
                    {
                        if (nt.wizard.step == NewThreadStep::Window && !nt.wizard.windowsCollected && nt.wizard.windowsSerial == 0)
                        {
                            resume.push_back(threadId);
                        }
                    }
                }
            }
            for (const int64_t threadId : idle)
            {
                // The user's rule: an aborted creation deletes its new topic — and a wizard left alone for a day is aborted.
                _abortNewThread(threadId, L"nothing was chosen in it for " + std::to_wstring(kNewThreadPickerIdleMs / 3600000) + L" hours", false);
            }
            for (const int64_t threadId : resume)
            {
                _log(L"[msg] new thread " + std::to_wstring(threadId) + L": its window step was waiting when the bot stopped; the windows are asked again");
                _collectNewThreadWindows(threadId, 0);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the next tick sweeps again (an idle wizard stays one more tick; a lost resume leaves its window
            // step at "looking at the windows…" until the user taps ◀ Back or sends a message).
            LogSwallowedException(L"MessagingService::_newThreadsTick");
        }
    }

    void MessagingService::_postCard(const std::wstring& sessionId, int64_t threadId, const std::wstring& text, json::Value markup, int64_t deleteFirst, bool ping, uint32_t textHash, SendPriority prio, std::shared_ptr<const CardPicture> picture)
    {
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
            {
                it->second.cardMessageId = -1; // in flight: _refreshCard waits for the answer below
                it->second.cardTextHash = textHash; // what the topic will show (a refusal re-posts it anyway)
                _stateDirty = true;
            }
        }
        if (deleteFirst > 0)
        {
            // Same priority as the send that follows and queued first, so the old card is gone before the new
            // one appears; a card already gone (the user deleted it) is a harmless 400. §21.28: `prio` is the cards' own unless
            // a tapped button waits for this post.
            OutItem del;
            del.prio = prio;
            del.key = L"card-del:" + sessionId;
            del.method = L"deleteMessage";
            del.params = json::Value::MkObj();
            del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(deleteFirst)));
            _enqueue(std::move(del));
        }
        auto self = weak_from_this();
        // §21.15: the form this post asks for — rich with a title chip when chips are wanted (a card's text always names its
        // session). Telegram's answer says which form went (a refused rich post went as HTML in its place).
        const bool wantRich = _titleChipsWanted() && TgHasTitleTokens(text);
        // §21.17: the post's title links to its topic when the username is known now (_sendText reads the same value).
        const bool linked = !_linkBot().empty() && TgHasTitleTokens(text);
        // `!ping` = silent: a card is a status message, and only a state the user must know about rings (§21.6).
        _sendText(threadId, text, !ping, prio, L"card-post:" + sessionId, std::move(markup), [self, sessionId, threadId, textHash, wantRich, linked, picture](const TgResult& r) {
            auto me = self.lock();
            if (!me)
            {
                return;
            }
            const int64_t id = r.ok ? r.result.I64At(L"message_id", 0) : 0;
            {
                // By THREAD, not by session: a Rekey (§21.6) may have moved the topic to another session's key while
                // this post flew, and the record must still learn its card (else it would wait for it forever).
                std::lock_guard lk{ me->_mtx };
                for (auto& [sid, t] : me->_state.topics)
                {
                    if (t.threadId == threadId)
                    {
                        t.cardMessageId = id;
                        t.cardPostedMs = WallMs(); // on a refusal too: the retry waits kCardRepostAfterFailureMs
                        if (r.ok)
                        {
                            // §21.15: the form the card shows now; a rich post that went as HTML is not retried for this text
                            // by the slow form upgrade (a changed text tries the rich form again).
                            t.cardRich = ResultIsRichMessage(r);
                            t.cardLinked = linked; // §21.17
                            if (wantRich && !t.cardRich)
                            {
                                me->_cards[sid].richRefusedHash = textHash;
                            }
                        }
                        me->_stateDirty = true;
                        break;
                    }
                }
            }
            if (!r.ok)
            {
                me->_log(L"[msg] card post refused for " + ShortId(sessionId) + L": " + r.description);
            }
            else if (picture)
            {
                // §21.42: the photo the new card shows (its file_id, for the next update to reuse), recorded on the record that now
                // holds this card's id (set just above, by thread).
                me->_noteCardPictureShown(threadId, id, *picture, r);
            }
        }, 0, false, nullptr, picture); // a COPY: the callback above captured it in this same call (argument order is unspecified)
    }

    void MessagingService::_seedFromTranscript(TopicRecord& t, const SessionInfo& s) noexcept
    {
        try
        {
            t.transcriptPath = ResolveClaudeTranscriptPath(s.id);
            t.cursor = 0; // no transcript yet (a new session, a fork before its first message): its first line is news
            t.recapText.clear();
            t.recapAtMs = 0;
            t.lastMessageAtMs = 0;
            // The HISTORY replay (§21.9) is decided below: the conversation so far, up to where the live mirror starts.
            t.backfillPath.clear();
            t.backfillPos = 0;
            t.backfillEnd = 0;
            t.backfillDayKey = 0;
            // The recap facts come from the transcript the session SHOWS: its own, or — for a fork with no message of
            // its own yet — the one it was forked from (its conversation so far IS the source's). Never cached: the
            // fork's first message creates its own transcript, and the mirror reads that from its first line.
            std::wstring recapPath = t.transcriptPath;
            if (recapPath.empty() && !s.forkParentId.empty())
            {
                recapPath = ResolveClaudeDisplayTranscript(s.id, s.forkParentId).path;
            }
            TranscriptTailScan recapScan;
            if (!recapPath.empty())
            {
                recapScan = ScanTranscriptTail(recapPath, kRecapSeedBytes);
                t.recapText = recapScan.recapText;
                t.recapAtMs = recapScan.recapAtMs;
                t.lastMessageAtMs = recapScan.lastMessageAtMs;
            }
            if (t.transcriptPath.empty())
            {
                // A fork with no message of its own yet: its conversation so far IS the source's, so the source's transcript
                // is its history (up to the last complete line now; the fork's own transcript, once it exists, is mirrored
                // live from its first line — its copies of the source's lines are skipped there).
                if (!recapPath.empty() && recapScan.completeTo > 0)
                {
                    t.backfillPath = recapPath;
                    t.backfillEnd = recapScan.completeTo;
                }
                return;
            }
            // Its own transcript exists: the topic shows the session from now on — from the END of the file for a
            // session at rest, and from the start of the turn's own prompt line for one that is mid-turn (a resting tab
            // that the user prompts at the PC gets its topic a tick after the prompt line was written; starting at the
            // end lost exactly that prompt — 2026-10-16). A turn longer than the window starts at the end. The END is the
            // last COMPLETE line's end when the scan read this file (a line still being written belongs to the mirror).
            int64_t size = -1;
            if (recapPath == t.transcriptPath)
            {
                size = recapScan.completeTo >= 0 ? recapScan.completeTo : recapScan.size; // the scan above read this very file
            }
            else
            {
                ReadTranscriptTail(t.transcriptPath, 0x7FFFFFFFFFFFFFFFll, size); // the size only
            }
            t.cursor = size > 0 ? size : 0;
            const bool midTurn = s.state == SessionState::Running || s.state == SessionState::NeedsApproval;
            if (midTurn && recapPath == t.transcriptPath && recapScan.lastPromptLineAt >= 0 && recapScan.lastPromptLineAt < t.cursor)
            {
                t.cursor = recapScan.lastPromptLineAt;
            }
            // Everything before the mirror's start is the session's HISTORY (§21.9, the user's "a thread is a reflection of
            // the entire session"): the pump replays it first — a fork's copied lines included, so a fork's topic shows the
            // conversation it was forked from — and the mirror waits for it, then goes on from exactly here.
            if (t.cursor > 0)
            {
                t.backfillPath = t.transcriptPath;
                t.backfillEnd = t.cursor;
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the mirror starts at the file's end (or waits for the file), the card has no recap.
            LogSwallowedException(L"MessagingService::_seedFromTranscript");
            try
            {
                int64_t size = -1;
                if (!t.transcriptPath.empty())
                {
                    ReadTranscriptTail(t.transcriptPath, 0x7FFFFFFFFFFFFFFFll, size);
                }
                t.cursor = size > 0 ? size : 0;
            }
            catch (...)
            {
                LogSwallowedException(L"MessagingService::_seedFromTranscript (recovery)");
            }
        }
    }

    void MessagingService::_pumpBackfill() noexcept
    {
        try
        {
            const int64_t now = WallMs();
            // Ranks a topic above another the way the list is ordered (§21.8): the sync's MOST RECENT keys; a session the sync
            // did not see ranks lowest.
            const auto ranksAbove = [this](const std::wstring& a, const std::wstring& b) {
                const auto ka = _orderKeys.find(a);
                const auto kb = _orderKeys.find(b);
                return TopicRanksAbove(ka == _orderKeys.end() ? 0 : ka->second.first, ka == _orderKeys.end() ? a : ka->second.second,
                                       kb == _orderKeys.end() ? 0 : kb->second.first, kb == _orderKeys.end() ? b : kb->second.second);
            };
            // A topic whose history just came in: one line (its card moves to the bottom at its next refresh, once).
            const auto reportDone = [this, now](const std::wstring& sid, int64_t bytes, int64_t createdMs) {
                size_t posts = 0;
                if (const auto it = _backfillPostsBy.find(sid); it != _backfillPostsBy.end())
                {
                    posts = it->second;
                    _backfillPostsBy.erase(it);
                }
                ++_replayTopics;
                _log(L"[msg] history in for " + ShortId(sid) + L": " + std::to_wstring(posts) + L" message(s) from " + std::to_wstring(bytes / 1024) + L" KB of transcript, " +
                     std::to_wstring(createdMs > 0 ? std::max<int64_t>(0, now - createdMs) / 1000 : 0) + L" s after the topic was made; the live conversation follows");
            };

            // 1. The batch going out: ONE in the whole outbox at a time. Its window's end is committed to the topic only when
            //    the last of its messages was answered, so a stop midway re-sends at most this batch and never skips one.
            {
                std::wstring doneSid;
                int64_t doneBytes = 0;
                int64_t doneCreatedMs = 0;
                std::wstring note;
                {
                    std::lock_guard lk{ _mtx };
                    if (_backfill.active)
                    {
                        if (_backfill.inFlight > 0)
                        {
                            // Still going out while any of its messages is QUEUED (the flood budget and the live traffic
                            // decide when) or an answer came lately; stalled only when every message left the outbox and
                            // none was answered for kBackfillStallMs (a send that never answered).
                            const int64_t thread = _backfill.threadId;
                            const bool queued = std::any_of(_outbox.begin(), _outbox.end(), [thread](const OutItem& o) { return o.prio == SendPriority::History && ThreadIdOf(o.params) == thread; });
                            if (queued || now - std::max(_backfill.startedMs, _backfill.lastAnswerMs) < kBackfillStallMs)
                            {
                                return;
                            }
                            // Dropped and read again from the uncommitted position: at worst some of its messages show twice;
                            // none is lost.
                            note = L"[msg] history of " + ShortId(_backfill.sessionId) + L": a batch of " + std::to_wstring(_backfill.posts) + L" message(s) had " +
                                   std::to_wstring(_backfill.inFlight) + L" unanswered for " + std::to_wstring(kBackfillStallMs / 60000) + L" min; it is sent again";
                            _backfill = BackfillBatch{};
                        }
                        else
                        {
                            if (_backfill.failed > 0)
                            {
                                note = L"[msg] history of " + ShortId(_backfill.sessionId) + L": " + std::to_wstring(_backfill.failed) + L" of " + std::to_wstring(_backfill.posts) +
                                       L" message(s) were refused (" + _backfill.firstFailure + L"); the replay goes on";
                            }
                            if (const auto it = _state.topics.find(_backfill.sessionId); it != _state.topics.end() && it->second.threadId == _backfill.threadId)
                            {
                                auto& rec = it->second;
                                rec.backfillPos = std::min(_backfill.consumedTo, rec.backfillEnd);
                                rec.backfillDayKey = _backfill.dayKey;
                                // The batch's messages are IN the topic now: the topic's last post is now, not when they were
                                // queued. A post is stamped at enqueue time and a card at its answer, so a history queued
                                // before its card went out (a new topic: both in one tick) read as older than the card, and the
                                // card never moved below the history (live, 2026-10-16 23:49).
                                rec.lastPostedMs = std::max(rec.lastPostedMs, now);
                                _stateDirty = true;
                                if (!TopicBackfillPending(rec))
                                {
                                    doneSid = it->first;
                                    doneBytes = rec.backfillEnd;
                                    doneCreatedMs = rec.createdMs;
                                }
                            }
                            _backfill = BackfillBatch{};
                        }
                    }
                }
                if (!note.empty())
                {
                    _log(note);
                }
                if (!doneSid.empty())
                {
                    reportDone(doneSid, doneBytes, doneCreatedMs);
                }
            }

            // 1b. §21.29: the CHANGE LINES held while a topic's history came in (_renameTopic, HoldTopicChangeLine) are told once
            //     nothing of the replay is left. The replay's position moves on only when its last message was answered, so the
            //     line lands below the history; a replay cut short, or a rekey's new conversation that has no history, lets its
            //     line go too. Before step 2, so neither a recreation nor the start-up hold delays a live line. One silent line
            //     per topic, like the change line it stands for; a scan of booleans under the lock, sent outside it.
            {
                struct HeldLine
                {
                    std::wstring sessionId;
                    int64_t threadId{ 0 };
                    std::wstring html; // "" = the held changes undid each other: nothing to tell
                };
                std::vector<HeldLine> released;
                {
                    std::lock_guard lk{ _mtx };
                    for (auto& [id, rec] : _state.topics)
                    {
                        if (auto line = TakeHeldTopicChangeLine(rec))
                        {
                            _stateDirty = true;
                            released.push_back(HeldLine{ id, rec.threadId, std::move(*line) });
                        }
                    }
                }
                for (const auto& h : released)
                {
                    if (h.html.empty() || h.threadId <= 0)
                    {
                        _log(L"[msg] topic " + ShortId(h.sessionId) + L": the changes held while its history came in undid each other; no line");
                        continue;
                    }
                    _sendText(h.threadId, h.html, true, SendPriority::New, L"", json::Value{}, nullptr, 0, false);
                    _log(L"[msg] topic " + ShortId(h.sessionId) + L" told: the change held while its history came in");
                }
            }

            // 2. Nothing is replayed while a RECREATION deletes the old topics or makes the new list (a minute or two; at most
            //    kRecreateListWaitMs): the replay then goes most recent first over the whole list.
            {
                bool orphans = false;
                {
                    std::lock_guard lk{ _mtx };
                    orphans = !_state.orphanThreads.empty();
                }
                // ...and nothing while the START-UP HOLD lasts: the order keys come from the sessions the windows have
                // re-homed, so before that every topic ranks the same and the replay would start anywhere (the 00:07
                // restart of the first live run began with an old session).
                if (orphans || (_recreating && now - _recreateStartedMs < kRecreateListWaitMs) || _startupHoldOpen)
                {
                    return;
                }
            }

            // 3. The next window: of the highest-ranked topic with history pending (the sessions the user cares about now). A
            //    window with nothing to show is committed at once and the next one read, a few per tick.
            for (int window = 0; window < 4 && !_stop.load(); ++window)
            {
                std::wstring sid;
                TopicRecord t;
                {
                    std::lock_guard lk{ _mtx };
                    for (const auto& [id, rec] : _state.topics)
                    {
                        if (rec.threadId > 0 && TopicBackfillPending(rec) && (sid.empty() || ranksAbove(id, sid)))
                        {
                            sid = id;
                        }
                    }
                    if (!sid.empty())
                    {
                        t = _state.topics[sid];
                    }
                }
                if (sid.empty())
                {
                    if (_backfillAfterRecreation)
                    {
                        // The recreation's replay is complete. The list is in REPLAY order (the most recent sessions went
                        // first, so they sit lowest): the full re-sort puts it back in MOST RECENT order.
                        _backfillAfterRecreation = false;
                        _resortFull = true;
                        _log(L"[msg] history replay done: " + std::to_wstring(_replayPosts) + L" message(s) into " + std::to_wstring(_replayTopics) +
                             L" topic(s); the list is re-sorted in MOST RECENT order now");
                    }
                    return;
                }
                if (_backfillPostsBy.find(sid) == _backfillPostsBy.end())
                {
                    _log(L"[msg] history replay of " + ShortId(sid) + L" " + (t.backfillPos == 0 ? L"started" : L"goes on") + L": bytes " + std::to_wstring(t.backfillPos) +
                         L" to " + std::to_wstring(t.backfillEnd) + L" of its transcript into thread " + std::to_wstring(t.threadId));
                    _backfillPostsBy.emplace(sid, 0);
                }
                HistoryWindow w = ReadHistoryWindow(t.backfillPath, t.backfillPos, t.backfillEnd);
                // §21.32.4: the history shows the same links the live mirror would (a file still on disk is linked; a no-op with
                // file links off).
                _filesRewriteItems(sid, w.items);
                if (w.consumedTo < 0)
                {
                    // The file is gone, or shorter than where the replay stands (replaced): what is left cannot be read, so the
                    // replay of this topic ends here and the live mirror goes on from its own position.
                    {
                        std::lock_guard lk{ _mtx };
                        if (const auto it = _state.topics.find(sid); it != _state.topics.end() && it->second.threadId == t.threadId)
                        {
                            it->second.backfillPos = it->second.backfillEnd;
                            _stateDirty = true;
                        }
                    }
                    _log(L"[msg] history of " + ShortId(sid) + L" stops at byte " + std::to_wstring(t.backfillPos) + L" of " + std::to_wstring(t.backfillEnd) +
                         L": the transcript is gone or shorter now");
                    reportDone(sid, t.backfillPos, t.createdMs);
                    continue;
                }
                int64_t day = t.backfillDayKey;
                std::vector<HistoryPlanPost> posts;
                if (!w.items.empty())
                {
                    // Where a prompt came from (§21.12: "⚙ runner", "📱 from the phone", else "typed at the PC" in the user's line):
                    // the session's queue knows the newest of them.
                    std::unordered_set<std::wstring> runner;
                    std::unordered_set<std::wstring> phone;
                    if (const auto s = _session(sid))
                    {
                        for (const auto& q : s->queue)
                        {
                            if (!q.remoteSource.empty())
                            {
                                phone.insert(NormalizePromptTextForCompare(q.text));
                            }
                            else if (q.origin == PromptOrigin::Autorun)
                            {
                                runner.insert(NormalizePromptTextForCompare(q.text));
                            }
                        }
                    }
                    // The topic's mode (§21.11): verbose replays the narrative too, the conversation only the user's posts and the
                    // turns' last messages.
                    posts = PlanHistoryPosts(
                        w,
                        day,
                        [&runner, &phone](const std::wstring& text) {
                            if (runner.empty() && phone.empty())
                            {
                                return PromptSource::Pc;
                            }
                            const std::wstring key = NormalizePromptTextForCompare(text);
                            return phone.count(key) ? PromptSource::Phone : (runner.count(key) ? PromptSource::Runner : PromptSource::Pc);
                        },
                        _topicVerbose(t),
                        UserLineTitleRow(t.title, sid, t.nameColor, t.threadId)); // §21.17: the session's title under every "👤 You" header
                }
                if (posts.empty())
                {
                    bool done = false;
                    {
                        std::lock_guard lk{ _mtx };
                        if (const auto it = _state.topics.find(sid); it != _state.topics.end() && it->second.threadId == t.threadId)
                        {
                            it->second.backfillPos = std::min(w.consumedTo, it->second.backfillEnd);
                            it->second.backfillDayKey = day;
                            _stateDirty = true;
                            done = !TopicBackfillPending(it->second);
                        }
                    }
                    if (done)
                    {
                        reportDone(sid, t.backfillEnd, t.createdMs);
                    }
                    continue;
                }
                // The batch: every post queued now, in order, at the History priority; the answers count it down. The user's
                // turns go as the user only while secretary mode works AND can send (a connection and the bot's id): a send
                // that fails at once would put its bot line ahead of the batch's earlier posts.
                bool asUser = false;
                const uint64_t seq = ++_backfillSeq;
                {
                    std::lock_guard lk{ _mtx };
                    asUser = _state.businessMode == 1 && !_state.businessConnectionId.empty() && _botId.load() != 0;
                    _backfill = BackfillBatch{};
                    _backfill.active = true;
                    _backfill.seq = seq;
                    _backfill.sessionId = sid;
                    _backfill.threadId = t.threadId;
                    _backfill.consumedTo = w.consumedTo;
                    _backfill.dayKey = day;
                    _backfill.inFlight = posts.size();
                    _backfill.posts = posts.size();
                    _backfill.startedMs = now;
                }
                _backfillPostsBy[sid] += posts.size();
                _replayPosts += posts.size();
                auto self = weak_from_this();
                const int64_t thread = t.threadId;
                // Each post's answer counts the batch down; a turn's LAST post also commits the turn (its end and day), so a
                // stop midway re-sends at most the turn that was going out. A refused post commits too: the replay goes on.
                const auto answeredFor = [self, seq, thread, sid](int64_t commitTo, int64_t dayAfter) {
                    return std::function<void(const TgResult&)>{ [self, seq, thread, sid, commitTo, dayAfter](const TgResult& r) {
                        auto me = self.lock();
                        if (!me)
                        {
                            return;
                        }
                        bool last = false;
                        {
                            std::lock_guard lk{ me->_mtx };
                            // An answer of a dropped batch (stalled, or its topic deleted or carried over) never counts down the next one.
                            if (me->_backfill.active && me->_backfill.seq == seq && me->_backfill.inFlight > 0)
                            {
                                const int64_t answeredAt = WallMs();
                                me->_backfill.lastAnswerMs = answeredAt;
                                if (!r.ok && me->_backfill.failed++ == 0)
                                {
                                    me->_backfill.firstFailure = r.description.empty() ? L"HTTP " + std::to_wstring(r.httpStatus) : r.description;
                                }
                                if (commitTo > 0)
                                {
                                    if (const auto it = me->_state.topics.find(sid); it != me->_state.topics.end() && it->second.threadId == thread)
                                    {
                                        it->second.backfillPos = std::max(it->second.backfillPos, std::min(commitTo, it->second.backfillEnd));
                                        it->second.backfillDayKey = dayAfter;
                                        it->second.lastPostedMs = std::max(it->second.lastPostedMs, answeredAt); // in the topic now (see the batch commit)
                                        me->_stateDirty = true;
                                    }
                                }
                                last = --me->_backfill.inFlight == 0;
                                if (last)
                                {
                                    me->_tickRequested = true; // the next window goes out without waiting for the tick's period
                                }
                            }
                        }
                        if (last)
                        {
                            me->_tickCv.notify_one();
                        }
                    } };
                };
                std::shared_ptr<PromptRef> turnRef; // §21.12: the current turn's prompt line (its reply quotes it)
                // §21.17: the CHAIN of turns — each turn's prompt line replies to the previous turn's, so the user can jump from
                // turn to turn. Its last link is the record's (a replay a restart interrupted goes on with its chain); a line sent
                // as the user (secretary mode) is no link: the bot cannot make the user's own message a reply.
                std::shared_ptr<PromptRef> lastTurnPrompt;
                if (t.historyPromptMessageId > 0)
                {
                    lastTurnPrompt = std::make_shared<PromptRef>();
                    lastTurnPrompt->id.store(t.historyPromptMessageId);
                }
                for (const auto& planned : posts)
                {
                    const HistoryPost& p = planned.post;
                    std::function<void(const TgResult&)> answered = answeredFor(planned.commitTo, planned.dayAfter);
                    std::shared_ptr<PromptRef> previousTurn; // §21.17: what this turn's prompt line replies to
                    if (p.turnPrompt)
                    {
                        // The turn's prompt line: its answer names its message (lineAnswered below) - the bot's line; a send as
                        // the user is in the user's view, so a history reply to it goes without a quote.
                        previousTurn = lastTurnPrompt;
                        turnRef = std::make_shared<PromptRef>();
                        if (!(p.fromUser && asUser))
                        {
                            lastTurnPrompt = turnRef;
                        }
                    }
                    const std::shared_ptr<PromptRef> promptLineRef = p.turnPrompt ? turnRef : nullptr;
                    const auto lineAnswered = [answered, promptLineRef, self, sid, thread](const TgResult& r) {
                        if (promptLineRef && r.ok)
                        {
                            if (const int64_t id = r.result.I64At(L"message_id", 0); id > 0)
                            {
                                promptLineRef->botLine.store(true);
                                promptLineRef->id.store(id);
                                // §21.17: the chain's last link, kept on the record (the first live turn after the replay quotes
                                // it, and a replay that a restart interrupts goes on from it). Released before `answered`, which
                                // takes the same lock.
                                if (auto me = self.lock())
                                {
                                    std::lock_guard lk{ me->_mtx };
                                    if (const auto it = me->_state.topics.find(sid); it != me->_state.topics.end() && it->second.threadId == thread)
                                    {
                                        it->second.historyPromptMessageId = id;
                                        me->_stateDirty = true;
                                    }
                                }
                            }
                        }
                        if (answered)
                        {
                            answered(r);
                        }
                    };
                    // A turn's reply quotes its prompt (§21.12); a turn's prompt line quotes the previous turn's (§21.17).
                    const std::shared_ptr<PromptRef> replyRef = p.turnReply ? turnRef : (p.turnPrompt ? previousTurn : nullptr);
                    if (p.fromUser && asUser)
                    {
                        // SECRETARY MODE (§21.5): the user's turn is the user's own bubble, as it was in the conversation. A
                        // refused send puts the bot's line in its place AT ONCE (the New priority goes before the rest of the
                        // batch, so the order holds), and the batch counts the post when THAT is answered.
                        const std::wstring fallback = p.html;
                        _sendAsUser(
                            thread,
                            p.userText,
                            [self, thread, fallback, answered, lineAnswered](const TgResult& r) {
                                auto me = self.lock();
                                if (!me)
                                {
                                    return;
                                }
                                if (r.ok)
                                {
                                    me->_secretaryFailures.store(0);
                                    answered(r);
                                    return;
                                }
                                const int failures = me->_secretaryFailures.fetch_add(1) + 1;
                                me->_log(L"[msg] secretary: a history turn could not be sent as the user (" + r.description + L"); the bot's line goes in its place" +
                                         (failures >= 3 ? L"; secretary mode is off until the bot is connected again" : L""));
                                if (failures >= 3)
                                {
                                    std::lock_guard lk{ me->_mtx };
                                    me->_state.businessMode = -1;
                                    me->_state.businessNote = r.description;
                                    me->_stateDirty = true;
                                }
                                me->_sendText(thread, fallback, true, SendPriority::New, L"", json::Value{}, lineAnswered, 0, true);
                            },
                            SendPriority::History);
                    }
                    else if (!p.markdown.empty())
                    {
                        // Claude's Markdown as a rich message (§21.11); a part Telegram refuses goes as the HTML in its place. The
                        // turn's reply quotes its prompt (§21.12).
                        _sendRich(thread, p.markdown, p.html, true, SendPriority::History, answered, 0, true, replyRef);
                    }
                    else
                    {
                        _sendText(thread, p.html, true, SendPriority::History, L"", json::Value{}, lineAnswered, 0, true, replyRef);
                    }
                }
                return; // one batch at a time
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the batch state stays as it was (a batch already queued is committed when answered, or
            // dropped by the stall guard); the next tick reads the window again.
            LogSwallowedException(L"MessagingService::_pumpBackfill");
        }
    }

    void MessagingService::_resortTick() noexcept
    {
        try
        {
            const int64_t now = WallMs();
            const bool full = _resortFull;
            // §21.16.7: the topics a directory DOT put first since the last pass. The poll thread only records the tap
            // (_bringTopicToFront); the order key that ends the hold is the tick thread's (`_orderKeys`), so the fold happens here,
            // before any of the returns below. A held topic whose topic is gone is dropped in the same pass.
            {
                std::unordered_map<std::wstring, int64_t> requests;
                {
                    std::lock_guard lk{ _mtx };
                    requests.swap(_frontRequests);
                    for (auto it = _frontHeld.begin(); it != _frontHeld.end();)
                    {
                        it = _state.topics.count(it->first) ? std::next(it) : _frontHeld.erase(it);
                    }
                }
                for (const auto& kv : requests)
                {
                    const auto key = _orderKeys.find(kv.first);
                    _frontHeld[kv.first] = key == _orderKeys.end() ? 0 : key->second.first;
                    // A re-sort in progress re-posts other topics after the tap, which would push this one down again: it is
                    // re-posted LAST instead (moved to the end of the queue), so it ends on top of what the re-sort builds.
                    if (!_resortQueue.empty())
                    {
                        _resortQueue.erase(std::remove(_resortQueue.begin(), _resortQueue.end(), kv.first), _resortQueue.end());
                        _resortQueue.push_back(kv.first);
                    }
                }
            }
            if (_startupHoldOpen || _recreating)
            {
                return; // the order keys are incomplete until every window has re-homed its tabs, and a recreation orders the list itself
            }
            // A re-sort in progress: its next re-post, one per tick, while the call window has room for its two calls (the
            // old card's delete and the new card's send) under the history's share of the budget — the live traffic keeps
            // the rest. The per-topic pass of the next tick carries it out (_refreshCard: a silent re-post).
            if (!_resortQueue.empty())
            {
                bool room = false;
                {
                    std::lock_guard lk{ _mtx };
                    _noteCallLocked(0);
                    room = _recentCallsMs.size() + 2 <= kHistoryCallsPerMinute && !_backfill.active;
                }
                if (!room)
                {
                    return;
                }
                const std::wstring next = _resortQueue.front();
                _resortQueue.pop_front();
                {
                    std::lock_guard lk{ _mtx };
                    _cards[next].repostWanted = true;
                }
                if (_resortQueue.empty())
                {
                    _log(L"[msg] re-sort: the last card is re-posted now; the list reads in MOST RECENT order");
                }
                return;
            }
            if (!full && (now - _lastResortMs < kResortEveryMs || !_runningIds.empty()))
            {
                return;
            }
            // The list as it stands: by each topic's last message (the card's post or the newest other post), newest on top,
            // which is what Telegram orders by. Only topics with a card (one that cannot be re-posted cannot be moved) and whose
            // session the sync saw (the order key).
            std::vector<std::pair<int64_t, std::wstring>> last;
            {
                std::lock_guard lk{ _mtx };
                if (_backfill.active)
                {
                    return;
                }
                for (const auto& [sid, t] : _state.topics)
                {
                    if (TopicBackfillPending(t))
                    {
                        return; // a replay is still running: its end asks for the re-sort
                    }
                }
                if (!full && !_outbox.empty())
                {
                    return;
                }
                for (const auto& [sid, t] : _state.topics)
                {
                    if (t.threadId <= 0 || t.cardMessageId <= 0 || _orderKeys.find(sid) == _orderKeys.end())
                    {
                        continue;
                    }
                    // §21.16.7: a topic the user put first with a directory dot stays OUT of the order — neither moved down nor
                    // counted as "not quiet" by its own re-post — while its session's order key is the one it had at the tap.
                    // Its next turn changes the key; from then on it is ordered like any other topic.
                    if (const auto held = _frontHeld.find(sid); held != _frontHeld.end())
                    {
                        if (_orderKeys.find(sid)->second.first == held->second)
                        {
                            continue;
                        }
                        _frontHeld.erase(held);
                    }
                    const int64_t lastMs = std::max(t.cardPostedMs, t.lastPostedMs);
                    if (!full && now - lastMs < kResortQuietMs)
                    {
                        return; // something was posted in the last half minute: not quiet
                    }
                    last.emplace_back(lastMs, sid);
                }
            }
            _lastResortMs = now;
            _resortFull = false;
            std::sort(last.begin(), last.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
            std::vector<std::wstring> current;
            current.reserve(last.size());
            for (const auto& entry : last)
            {
                current.push_back(entry.second);
            }
            std::vector<std::wstring> desired = current;
            std::stable_sort(desired.begin(), desired.end(), [this](const std::wstring& a, const std::wstring& b) {
                const auto& ka = _orderKeys.find(a)->second;
                const auto& kb = _orderKeys.find(b)->second;
                return TopicRanksAbove(ka.first, ka.second, kb.first, kb.second);
            });
            const auto plan = PlanTopicResort(current, desired);
            if (plan.empty())
            {
                if (full)
                {
                    _log(L"[msg] re-sort after the history replay: the list is in MOST RECENT order already");
                    _appNote(L"\x2705 <b>History added</b> to " + std::to_wstring(_replayTopics) + L" topic(s) (" + std::to_wstring(_replayPosts) +
                                 L" message(s)); the list reads in MOST RECENT order.",
                             false);
                }
                return;
            }
            if (!full && plan.size() > kResortMaxTopics)
            {
                if (!_resortTooLongLogged)
                {
                    _resortTooLongLogged = true;
                    _log(L"[msg] re-sort skipped: putting the list back in MOST RECENT order would re-post " + std::to_wstring(plan.size()) + L" card(s) (more than " +
                         std::to_wstring(kResortMaxTopics) + L"); /recreate in the Search topic makes it again in order");
                }
                return;
            }
            _resortQueue.assign(plan.begin(), plan.end());
            _log(L"[msg] re-sort " + std::wstring{ full ? L"after the history replay" : L"(the list drifted)" } + L": " + std::to_wstring(plan.size()) + L" of " +
                 std::to_wstring(current.size()) + L" card(s) to re-post, the lowest-ranked first and one at a time within the flood budget, so the list reads in MOST RECENT order");
            if (full)
            {
                _appNote(L"\x2705 <b>History added</b> to " + std::to_wstring(_replayTopics) + L" topic(s) (" + std::to_wstring(_replayPosts) + L" message(s)); " +
                             std::to_wstring(plan.size()) + L" card(s) are re-posted now to put the list back in MOST RECENT order.",
                         false);
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: no re-sort this time; the next quiet minute plans again.
            LogSwallowedException(L"MessagingService::_resortTick");
        }
    }

    void MessagingService::_rekeyTopic(const std::wstring& fromSessionId, const std::wstring& toSessionId, const std::wstring& title)
    {
        const auto s = _session(toSessionId);
        if (!s)
        {
            return; // gone between the plan and now: the next sync decides again
        }
        TopicRecord t;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.topics.find(fromSessionId);
            if (it == _state.topics.end() || _state.topics.count(toSessionId))
            {
                return; // the plan is stale (deleted meanwhile, or the heir got a topic of its own)
            }
            t = it->second;
        }
        // Everything that belonged to the OLD conversation goes: its mirror position and transcript, its work log, its
        // recap, its Error note, its last prompt (whose status is removed: §21.12). What belongs to the TOPIC stays: the
        // thread, the card message, the sticky clocks.
        _showPromptStatus(t, PromptTurnStatus::None);
        t.promptMessageId = 0;
        t.promptBotLine = false;
        t.promptTsMs = 0;
        t.promptSource = 0;
        t.promptText.clear();
        t.promptInterrupted = false;
        t.sessionId = toSessionId;
        t.tabToken = s->tabToken;
        t.workLogMessageId = 0;
        t.idleSinceMs = 0;
        t.errorAnnounced = false;
        t.cardTextHash = 0; // the card describes another session now: edited at once
        _seedFromTranscript(t, *s);
        const std::wstring oldTitle = t.title;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.topics.find(fromSessionId);
            if (it == _state.topics.end() || _state.topics.count(toSessionId))
            {
                return;
            }
            t.cardMessageId = it->second.cardMessageId; // a card post may have answered while the seed read the file
            t.cardPostedMs = it->second.cardPostedMs;
            t.lastPostedMs = it->second.lastPostedMs;
            _state.topics.erase(it);
            _state.topics[toSessionId] = t;
            _mirrors.erase(fromSessionId);
            _mirrors.erase(toSessionId);
            _cards.erase(fromSessionId);
            _cards.erase(toSessionId);
            _stateDirty = true;
            // Queued edits keyed by the old session (its work log, its card edit) belong to a conversation the topic no
            // longer shows, and so does the rest of its HISTORY replay (§21.9); a card post / delete in flight keeps going
            // (its answer is matched by thread).
            for (auto o = _outbox.begin(); o != _outbox.end();)
            {
                const bool old = o->key == L"wl:" + fromSessionId || o->key == L"card:" + fromSessionId || (o->prio == SendPriority::History && ThreadIdOf(o->params) == t.threadId);
                o = old ? _outbox.erase(o) : std::next(o);
            }
            if (_backfill.active && _backfill.threadId == t.threadId)
            {
                _backfill = BackfillBatch{}; // the new conversation's history (seeded above) is read from its own start
            }
        }
        _backfillPostsBy.erase(fromSessionId);
        _log(L"[msg] topic carried over " + ShortId(fromSessionId) + L" -> " + ShortId(toSessionId) + L" (the same tab, thread " + std::to_wstring(t.threadId) + L"; " +
             (t.transcriptPath.empty() ? std::wstring{ L"no transcript yet" } : L"mirrored from byte " + std::to_wstring(t.cursor)) + L")");
        LogNav(L"remote telegram topic-rekey " + ShortId(fromSessionId) + L" -> " + ShortId(toSessionId));
        // One silent line marks where the new conversation starts (the thread is the conversation, and this is its
        // boundary: a /clear, a /resume or a handover the user ran at the PC).
        _sendText(t.threadId, L"<i>\x2014 a new conversation in this tab (" + TgEscapeHtml(ShortId(toSessionId)) + L") \x2014</i>", true, SendPriority::New, L"", json::Value{}, nullptr);
        if (!title.empty() && title != oldTitle)
        {
            _renameTopic(toSessionId, title, s->state); // refreshes the card on success
        }
        _refreshCard(toSessionId, true);
    }

    bool MessagingService::_sessionHeldByAWindow(const std::wstring& sessionId) const noexcept
    {
        if (!_hooks.sessionHeld)
        {
            return false; // the harness, or an app without windows: nothing can be held
        }
        try
        {
            return _hooks.sessionHeld(sessionId);
        }
        catch (...)
        {
            // Rule #18. Recovery: "held" — the sync keeps the topic for this tick and asks again at the next one, where a
            // wrong "not held" would delete the topic and its history for good.
            LogSwallowedException(L"MessagingService::_sessionHeldByAWindow");
            return true;
        }
    }

    void MessagingService::_deleteTopic(const std::wstring& sessionId, const std::wstring& reason)
    {
        int64_t thread = 0;
        // §21.31: what the topic's name showed (the idle notice names the session the way the topic list did).
        std::wstring shownTitle;
        std::wstring shownSquare;
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.topics.find(sessionId);
            if (it == _state.topics.end())
            {
                return;
            }
            thread = it->second.threadId;
            shownTitle = it->second.title;
            shownSquare = it->second.nameColor;
        }
        auto p = json::Value::MkObj();
        p.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        p.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(thread)));
        if (!_awaitCallSlot(L"deleteForumTopic"))
        {
            return; // stopping, or a long 429 parks topic deletes: the record stays, and a later tick (or run) deletes it
        }
        const TgResult r = _api->Call(L"deleteForumTopic", p, 20000);
        if (_handleTickFlood(L"deleteForumTopic", r))
        {
            // Telegram's flood control refused it: the record stays (dropping it, as any other failure does, would leave the
            // topic in the chat with nothing that knows it), and the sync plans the delete again after the pause.
            return;
        }
        if (!r.ok && !r.ThreadNotFound())
        {
            _log(L"[msg] topic delete failed for " + ShortId(sessionId) + L": " + r.description + L" (dropping the record anyway)");
        }
        {
            std::lock_guard lk{ _mtx };
            const bool erased = _state.topics.erase(sessionId) > 0;
            _mirrors.erase(sessionId);
            _cards.erase(sessionId);
            _stateDirty = true;
            // Queued edits and posts for the dead topic are pointless (a history post carries no session key: its thread).
            for (auto it = _outbox.begin(); it != _outbox.end();)
            {
                const bool mine = it->key.find(sessionId) != std::wstring::npos || (thread > 0 && ThreadIdOf(it->params) == thread);
                it = mine ? _outbox.erase(it) : std::next(it);
            }
            if (_backfill.active && _backfill.threadId == thread)
            {
                _backfill = BackfillBatch{}; // its answers no longer count (the seq is gone)
            }
            // §21.31: a topic that left because its session went idle is named in the main view's idle notice. Only when THIS call
            // erased the record, so a delete that raced another one is never told twice; the other reasons (a closed tab, …) are
            // not the user's request and stay quiet.
            if (erased && reason == L"idle")
            {
                _idleNoticePending.push_back(IdleNoticeEntry{ sessionId, std::move(shownTitle), std::move(shownSquare), WallMs() });
            }
        }
        _backfillPostsBy.erase(sessionId);
        _log(L"[msg] topic deleted " + ShortId(sessionId) + L" (" + reason + L")");
    }

    void MessagingService::_flushIdleNotices(int64_t nowMs)
    {
        std::vector<IdleNoticeEntry> due;
        {
            std::lock_guard lk{ _mtx };
            if (!IdleNoticeDue(_idleNoticePending, nowMs))
            {
                return; // nothing waits, or a burst is still coming in (the line names it whole once it has settled)
            }
            due.swap(_idleNoticePending);
        }
        // The chat's main view ("All": no thread), where the pinned directory lives; the topics themselves are gone. Silent (D5:
        // only a turn's final message rings), at the New priority like a topic's change line, and no card moves (no topic).
        _sendText(0, BuildIdleNoticeHtml(due), true, SendPriority::New, L"", json::Value{}, nullptr, 0, false);
        std::wstring ids;
        for (const auto& e : due)
        {
            ids += (ids.empty() ? L"" : L", ") + ShortId(e.sessionId);
        }
        _log(L"[msg] idle notice in the main view: " + std::to_wstring(due.size()) + L" session(s) went idle (" + ids + L")");
    }

    void MessagingService::_renameTopic(const std::wstring& sessionId, const std::wstring& title, SessionState state)
    {
        int64_t thread = 0;
        TopicRecord before; // §21.13: the name as it was (its title and square), for the change line
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.topics.find(sessionId);
            if (it == _state.topics.end())
            {
                return;
            }
            thread = it->second.threadId;
            before = it->second;
        }
        // §21.22: the state is the topic's ICON while Telegram takes the icons, and the name is the tab color's square (once
        // that color has held — §21.13: a rename for the state or the title never carries a color that is still changing;
        // ChooseNameColor keeps the old square then) and the title; a state change then changes only the icon. After a refused
        // icon in this run, the glyph form (§21.11), and a state icon the topic still shows is removed ("" removes it).
        const int64_t now = WallMs();
        const NameColorChoice color = ChooseNameColor(_lastFact(sessionId), before, now, _currentColorMode());
        const bool wantIcon = _stateIconsWanted();
        const TopicStateIcon icon = StateTopicIcon(state);
        const std::wstring name = TopicDisplayName(state, color.square, title, wantIcon);
        const auto params = [&](bool touchIcon) {
            auto p = json::Value::MkObj();
            p.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
            p.Set(L"message_thread_id", json::Value::MkNum(static_cast<double>(thread)));
            p.Set(L"name", json::Value::MkStr(name));
            if (touchIcon)
            {
                p.Set(L"icon_custom_emoji_id", json::Value::MkStr(wantIcon ? std::wstring{ icon.customEmojiId } : std::wstring{}));
            }
            return p;
        };
        if (!_awaitCallSlot(L"editForumTopic"))
        {
            return; // stopping, or a long 429 parks renames: the name stays due, and a later tick (or run) renames it
        }
        // The icon is touched when it should show the state, or when it shows one that must go (the glyph form after a refusal).
        TgResult r = _api->Call(L"editForumTopic", params(wantIcon || before.stateIcon), 20000);
        const auto floodRefused = [&]() {
            if (!_handleTickFlood(L"editForumTopic", r))
            {
                return false;
            }
            // Telegram's flood control refused it: NOT recorded, so the name stays due and is made after the pause (the
            // record used to take the new state "either way", and a topic refused this way kept its old glyph until the
            // session's next change).
            _log(L"[msg] topic rename of " + ShortId(sessionId) + L" refused by Telegram's flood control (429); it is made again after the pause");
            return true;
        };
        if (floodRefused())
        {
            return;
        }
        if (!r.ok && (wantIcon || before.stateIcon) && TopicIconRefused(r.httpStatus, r.description))
        {
            // §21.22: Telegram refused the topic ICON. The glyph form for the rest of the run: every topic that shows a state
            // icon becomes due for it (PickNameRename's form rule), and the next try of the icons is the bot's next start.
            if (!_stateIconRefused.exchange(true))
            {
                _log(L"[msg] Telegram refused a topic icon (" + r.description + L"): the topic names show the state glyph again until the bot starts again");
            }
            if (wantIcon)
            {
                // Nothing changed on Telegram's side: NOT recorded, so the topic stays due and the next name tick renames it
                // in the glyph form.
                return;
            }
            // The REMOVAL of a state icon was refused: the glyph-form name without touching the icon (its old state icon stays
            // beside the glyph), recorded as the glyph form so it is not tried again every tick.
            if (!_awaitCallSlot(L"editForumTopic"))
            {
                return;
            }
            r = _api->Call(L"editForumTopic", params(false), 20000);
            if (floodRefused())
            {
                return;
            }
        }
        // §21.29: NO ANSWER at all (DNS, TLS, a timeout, an abort: transportFailed) is not a refusal, because Telegram may never
        // have seen the call. It is NOT recorded, so the name stays due and the next tick makes it again. Recorded "either way"
        // (below), as it was before, a network blip lost the new name and its change line for good, and since the change line is
        // a rename's only answer (the user's rule of 2026-10-19) that left a /rename unanswered. A rename Telegram applied
        // before its answer was lost comes back as TOPIC_NOT_MODIFIED at the retry, which counts as done.
        if (r.transportFailed)
        {
            _log(L"[msg] topic rename of " + ShortId(sessionId) + L" got no answer (" + r.description + L"); it is made again at the next tick");
            return;
        }
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
            {
                // Remember the title and the state either way (a refusal other than the flood control's or an icon's — a name
                // Telegram will not take — must not be retried every tick; a call with no answer at all returned above, §21.29).
                it->second.title = title;
                it->second.nameState = static_cast<int>(state);
                it->second.nameSetMs = WallMs();
                it->second.nameColor = color.square; // §21.13: and the square, with the mode it stands for
                it->second.nameColorMode = color.mode;
                it->second.stateIcon = wantIcon; // §21.22: the icon shows the state, or the glyph form (after a refused icon)
                _stateDirty = true;
            }
        }
        // §21.29: TOPIC_NOT_MODIFIED means the topic shows this name and icon already: a rename whose lost answer was made
        // again (above), or a title change that only the cut part of a long name holds. That is a rename that took, for the line
        // and the card too. Before §21.29 it was logged as a failed rename, and its change line was never told.
        const bool took = r.ok || r.TopicNotModified();
        if (took)
        {
            if (r.ok)
            {
                _log(L"[msg] topic renamed " + ShortId(sessionId) + L" \x2192 " + (wantIcon ? std::wstring{ icon.emoji } + L" " : std::wstring{}) + L"\x201C" + name + L"\x201D");
            }
            else
            {
                _log(L"[msg] topic " + ShortId(sessionId) + L" shows its name already (TOPIC_NOT_MODIFIED); taken as renamed");
            }
            // §21.13, the CHANGE LINE (the user's "a message like whatsapp does for group chat name changed"): a new title, or
            // the tab's color changed from one square to another under a settled mode. Silent, and it does not move the sticky
            // card (one call, not three: the next real post moves the card below it). Telegram's own "topic renamed" notice is
            // deleted when it arrives (_handleMessage). §21.29: the line is a rename's ONLY answer since the user's rule of
            // 2026-10-19 (a /rename in the topic posts no reply of its own), so it is never dropped. While the topic's history is
            // being replayed, where it would land among the old turns, the line is HELD (HoldTopicChangeLine), and _pumpBackfill
            // tells it once the replay is done.
            const std::wstring oldSquare = NoticeOldSquare(before, color, now, _colorModeChangedMs, kColorModeSettleMs);
            const std::wstring notice = BuildTopicChangeNoticeHtml(before.title, oldSquare, title, color.square, before.threadId);
            // Which change the line tells, by the line's own rule (the titles as it PRINTS them, 2026-10-18): titles that differ
            // only in a line break or past the cut tell their color, if anything, so the log never says "title" for a line that
            // names none. Read only for a line that has something to tell.
            const auto what = [&before, &title]() -> const wchar_t* { return TopicChangeShowsTitle(before.title, title) ? L"title" : L"color"; };
            if (TopicBackfillPending(before))
            {
                // On the record itself, under the lock: the tick's copy write-back never touches the held fields. A change with
                // nothing to tell still moves a held line's quietly changed square along (HoldTopicChangeLine's remarks).
                {
                    std::lock_guard lk{ _mtx };
                    if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
                    {
                        HoldTopicChangeLine(it->second, !notice.empty(), before.title, oldSquare, before.nameColor, color.square);
                        _stateDirty = true;
                    }
                }
                if (!notice.empty())
                {
                    _log(L"[msg] topic " + ShortId(sessionId) + L" changed (" + what() + L"): the line is held until its history is in");
                }
            }
            else if (!notice.empty())
            {
                _sendText(thread, notice, true, SendPriority::New, L"", json::Value{}, nullptr, 0, false);
                _log(L"[msg] topic " + ShortId(sessionId) + L" told: its " + what() + L" changed");
            }
            _refreshCard(sessionId, true);
        }
        else if (!r.NotModified())
        {
            _log(L"[msg] topic rename failed for " + ShortId(sessionId) + L": " + r.description);
        }
    }

    void MessagingService::_stateNameTick() noexcept
    {
        try
        {
            if (_startupHoldOpen || _recreating || _lastFacts.empty())
            {
                return; // the facts are not complete in the hold; a recreation makes every name anew
            }
            std::wstring sid;
            {
                std::lock_guard lk{ _mtx };
                if (!_state.orphanThreads.empty())
                {
                    _renameWaitingSinceMs = 0; // a recreation's deletes come first; the replay must not hold meanwhile
                    return;
                }
                // Not for kHistoryFloodPauseMs after a 429, and not while a long one parks the rename's method.
                const int64_t tick = static_cast<int64_t>(::GetTickCount64());
                _noteCallLocked(0); // prunes the window only
                if ((_floodAtMs != 0 && tick - _floodAtMs < kHistoryFloodPauseMs) || FloodParkedUntil(_floodParking, L"editForumTopic", tick) > 0)
                {
                    _renameWaitingSinceMs = 0; // the replay must not hold for a rename that cannot go
                    return;
                }
                // §21.13: the state glyph AND the color square (PickNameRename: PickStateRename's rule, then the tabs whose
                // color changed, then the quiet re-squares of topics named before the square or under another mode). §21.22:
                // the state is the topic's ICON while Telegram takes the icons, and a topic that still shows its state the other
                // way (a glyph-form name made before §21.22, or an icon after a refusal in this run) is a quiet re-form.
                sid = PickNameRename(_lastFacts, _state.topics, WallMs(), _currentColorMode(), _stateIconsWanted()).sessionId;
                if (sid.empty())
                {
                    _renameWaitingSinceMs = 0;
                    return;
                }
                // The live traffic goes first by itself (it never checks the window); the RENAME goes before the history
                // replay, which held the window at its cap for hours and starved every rename (2026-10-17: 1 of ~411 topics
                // renamed in five hours). No room now: the replay holds its next message (HistoryYieldsToRename) until the
                // window has drained enough, a few seconds, and this tick's successor makes the rename.
                if (!NameRenameFits(_recentCallsMs.size(), kHistoryCallsPerMinute))
                {
                    if (_renameWaitingSinceMs == 0)
                    {
                        _renameWaitingSinceMs = tick;
                    }
                    return;
                }
                _renameWaitingSinceMs = 0;
            }
            const auto f = std::find_if(_lastFacts.begin(), _lastFacts.end(), [&sid](const TabFact& x) { return x.sessionId == sid; });
            if (f == _lastFacts.end())
            {
                return;
            }
            std::wstring title = f->title;
            if (title.empty())
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _state.topics.find(sid); it != _state.topics.end())
                {
                    title = it->second.title;
                }
            }
            _renameTopic(sid, title, f->state);
        }
        catch (...)
        {
            // Rule #18. Recovery: no rename this tick; the next tick picks again.
            LogSwallowedException(L"MessagingService::_stateNameTick");
        }
    }

    // ===== the directory (§21.16) ===========================================================

    std::vector<DirectoryTopic> MessagingService::_directoryTopics() const
    {
        // The dedicated topics, in the order the directory lists them. 🔎 Search is the only one today; a new dedicated topic
        // (one the service makes for a purpose, not for a tab) adds its entry here, with its label, its thread and its icon color.
        std::vector<DirectoryTopic> topics;
        DirectoryTopic search;
        search.label = L"\U0001F50E Search";
        {
            std::lock_guard lk{ _mtx };
            search.threadId = _state.searchThreadId; // 0 until _ensureSearchTopic made it: a chip that opens nothing
        }
        search.colorHex = RgbHex(kTelegramTopicColors[0]); // the icon color _ensureSearchTopic gives it (blue): a blue chip
        topics.push_back(std::move(search));
        return topics;
    }

    void MessagingService::_directoryTick() noexcept
    {
        try
        {
            const int64_t now = WallMs();
            if (_directoryCheckedMs != 0 && now - _directoryCheckedMs < kDirectoryCheckMs)
            {
                return;
            }
            // The facts are incomplete during the start-up hold (every session reads closed until its window re-homes it) and a
            // recreation (every topic is made anew): a directory built then would list half the fleet, then edit itself again.
            if (_startupHoldOpen || _recreating || _lastFacts.empty())
            {
                return;
            }
            // The session store's FIRST whole read of a run opens every record file (seconds on a profile of thousands); the
            // engine makes it on a background thread at start-up, and the directory waits for it instead of making it here.
            if (!SessionStoreScanIsWarm())
            {
                return;
            }
            _directoryCheckedMs = now;
            std::wstring bot;
            DirectoryShown shown;
            bool inFlight = false;
            int64_t lastSendMs = 0;
            int64_t notBeforeMs = 0;
            bool pinNeeded = false;
            bool stallForgotten = false;
            {
                std::lock_guard lk{ _mtx };
                if (!_state.orphanThreads.empty())
                {
                    return; // a recreation's deletes come first
                }
                // Not for kHistoryFloodPauseMs after a 429, and not while a long one parks a method the directory calls: Telegram
                // then holds the whole chat (MESSAGING.md §21.14.5), and the directory is the least urgent message the bot has.
                const int64_t tick = static_cast<int64_t>(::GetTickCount64());
                if ((_floodAtMs != 0 && tick - _floodAtMs < kHistoryFloodPauseMs) || FloodParkedUntil(_floodParking, L"editMessageText", tick) > 0 ||
                    FloodParkedUntil(_floodParking, L"sendRichMessage", tick) > 0 || FloodParkedUntil(_floodParking, L"sendMessage", tick) > 0)
                {
                    return;
                }
                if (_directoryInFlight && now - _directoryInFlightSinceMs >= kDirectoryStallMs)
                {
                    // An answer that never came (an exception in the send loop before its onDone): the send is forgotten, and an
                    // answer that still comes is told apart by its serial. Every directory send carries its key, so the next one
                    // replaces a twin that is still queued.
                    _directoryInFlight = false;
                    ++_directorySerial;
                    stallForgotten = true;
                }
                bot = _botUsername;
                shown.messageId = _state.directoryMessageId;
                shown.hash = _state.directoryHash;
                shown.rich = _state.directoryRich;
                shown.richRefusedHash = _directoryRichRefusedHash;
                inFlight = _directoryInFlight;
                lastSendMs = _directoryLastSendMs;
                notBeforeMs = _directoryNotBeforeMs;
                pinNeeded = _state.directoryMessageId > 0 && !_state.directoryPinned && !_directoryPinTried;
            }
            if (stallForgotten)
            {
                _log(L"[msg] directory: a send got no answer for " + std::to_wstring(kDirectoryStallMs / 60000) + L" min; it is forgotten and sent again");
            }
            if (bot.empty())
            {
                return; // the links need the bot's username, which getMe gives
            }

            // ---- the model ----
            DirectoryModel m;
            m.botUsername = bot;
            m.topics = _directoryTopics();
            // The tags: every tag a session carries (an archived session's too) and the registry's 0-carrier ones, each with the
            // OPEN tabs that carry it. The render lists only the tags an open tab carries (the user's "Tags with 0 tabs should not
            // be shown"); the rest still pass through the join, which keeps the display casing CollectGlobalTags picks, and are
            // counted for the log. The store's reads are cached (a directory listing, one stat per small file): milliseconds.
            const auto tagsBySession = LoadAllSessionTags();
            const auto knownTags = LoadKnownTags();
            const auto tagColors = LoadAllTagColors();
            std::vector<std::pair<std::wstring, std::wstring>> universe;
            for (const auto& g : CollectGlobalTags(tagsBySession, {}, knownTags))
            {
                universe.emplace_back(g.name, ResolveTagDisplayHex(g.name, tagColors)); // the tag's color as every app surface paints it
            }
            std::vector<std::pair<DirectoryTab, std::vector<std::wstring>>> openTabs;
            {
                std::lock_guard lk{ _mtx }; // the topic map: each tab's thread
                for (const auto& f : _lastFacts)
                {
                    if (!(f.live && f.inTab))
                    {
                        continue;
                    }
                    const auto tg = tagsBySession.find(f.sessionId);
                    const bool tagged = tg != tagsBySession.end() && !tg->second.empty();
                    // §21.16.8: every open CLAUDE tab is a candidate of the Recent activity section — the stats rows' population
                    // (a Codex tab has no topic, D23) — and the render keeps the ten newest that are not stationary. A tab that is
                    // neither (a Codex tab with no tag) is not needed.
                    if (!tagged && !f.claude)
                    {
                        continue;
                    }
                    DirectoryTab t;
                    t.sessionId = f.sessionId;
                    t.title = f.title;
                    t.state = f.state;
                    t.colorHex = f.colorHex; // the sync resolved it like the tab (§21.13)
                    t.recentMs = f.mostRecentMs; // the topic list's MOST RECENT key: the Recent activity section's order (§21.16.8)
                    if (const auto topic = _state.topics.find(f.sessionId); topic != _state.topics.end())
                    {
                        t.threadId = topic->second.threadId;
                        if (t.title.empty())
                        {
                            t.title = topic->second.title;
                        }
                    }
                    if (f.claude)
                    {
                        m.recent.push_back(t);
                    }
                    if (tagged)
                    {
                        openTabs.emplace_back(std::move(t), tg->second);
                    }
                }
            }
            m.tags = JoinDirectoryTags(universe, openTabs);
            // The counts: the sync's facts for the states, the registry's queues for the backlog — the lean live copy the scanner
            // takes every pass (open prompts only), over the same tabs the states are counted on.
            size_t queued = 0;
            for (const auto& s : _registry->SnapshotLiveForScan())
            {
                if (s.kind != AgentKind::Claude || s.tabToken.empty())
                {
                    continue;
                }
                for (const auto& q : s.queue)
                {
                    queued += (q.status == PromptStatus::Pending || q.status == PromptStatus::Held) ? 1 : 0;
                }
            }
            m.stats = CountDirectoryStats(_lastFacts, queued, _scheduler ? _scheduler->GlobalPaused() : false);
            const DirectoryRender r = RenderDirectory(m, now, now);

            // ---- the decision ----
            const bool wantRich = _titleChipsWanted();
            const DirectoryAction action = DecideDirectoryAction(shown, r.hash, wantRich, inFlight, now, lastSendMs, notBeforeMs);
            if (action == DirectoryAction::None)
            {
                if (pinNeeded && !inFlight)
                {
                    _pinDirectory(shown.messageId); // the message the last run could not pin, or one posted before its pin was made
                }
                return;
            }
            uint64_t serial = 0;
            {
                std::lock_guard lk{ _mtx };
                serial = ++_directorySerial;
                _directoryInFlight = true;
                _directoryInFlightSinceMs = now;
                _directoryLastSendMs = now;
            }
            if (action == DirectoryAction::Edit)
            {
                _editDirectory(shown.messageId, r, wantRich && shown.richRefusedHash != r.hash, serial);
                return;
            }
            auto self = weak_from_this();
            const uint32_t hash = r.hash;
            std::function<void(const TgResult&)> done = [self, serial, hash, wantRich](const TgResult& res) {
                if (auto me = self.lock())
                {
                    me->_onDirectoryPosted(serial, hash, wantRich, res);
                }
            };
            // In the chat's main view (no thread): the directory belongs to no topic. Silent; keyed, so a send after a stall
            // replaces a twin still queued; at the Backfill priority, after the conversation and the cards.
            if (wantRich)
            {
                _sendTitledRich(0, r.rich, r.classic, true, SendPriority::Backfill, L"dir:post", json::Value{}, std::move(done), 0, nullptr);
            }
            else
            {
                for (auto& item : _htmlItems(0, r.classic, true, SendPriority::Backfill, L"dir:post", json::Value{}, std::move(done), 0, _takeOutSeq(), 0))
                {
                    _enqueue(std::move(item));
                }
            }
            _log(L"[msg] directory: posting it in the chat's main view (" + DescribeDirectoryRender(r) + (wantRich ? L", rich)" : L", HTML)"));
        }
        catch (...)
        {
            // Rule #18. Recovery: nothing sent on this check; the next check builds the directory again.
            LogSwallowedException(L"MessagingService::_directoryTick");
        }
    }

    void MessagingService::_onDirectoryPosted(uint64_t serial, uint32_t hash, bool wantRich, const TgResult& r)
    {
        const int64_t messageId = r.ok ? r.result.I64At(L"message_id", 0) : 0;
        const bool rich = ResultIsRichMessage(r);
        bool stale = false;
        {
            std::lock_guard lk{ _mtx };
            stale = serial != _directorySerial;
            if (!stale)
            {
                _directoryInFlight = false;
                if (messageId > 0)
                {
                    _state.directoryMessageId = messageId;
                    _state.directoryHash = hash;
                    _state.directoryRich = rich;
                    _state.directoryPinned = false;
                    _stateDirty = true;
                    _directoryNotBeforeMs = 0;
                    _directoryPinTried = false; // the new message gets its pin below
                    if (wantRich && !rich)
                    {
                        _directoryRichRefusedHash = hash; // the rich send fell back to HTML: this content stays HTML (DirectoryShown)
                    }
                }
                else
                {
                    _directoryNotBeforeMs = WallMs() + kDirectoryRetryMs;
                }
            }
        }
        if (stale)
        {
            if (messageId > 0)
            {
                // A post this run had given up on (a stall) went after all, and a newer one is, or will be, the directory: delete
                // this one, so the chat never holds two directories.
                OutItem del;
                del.prio = SendPriority::Backfill;
                del.method = L"deleteMessage";
                del.params = json::Value::MkObj();
                del.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
                del.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
                _enqueue(std::move(del));
                _log(L"[msg] directory: a post given up on went after all (message " + std::to_wstring(messageId) + L"); it is deleted");
            }
            return;
        }
        if (messageId > 0)
        {
            _log(L"[msg] directory posted (message " + std::to_wstring(messageId) + (rich ? L", rich" : L", HTML") + L"); pinning it");
            _pinDirectory(messageId);
            return;
        }
        _log(L"[msg] directory: Telegram refused the post (" + (r.description.empty() ? std::wstring{ L"no answer" } : r.description) + L"); tried again in " +
             std::to_wstring(kDirectoryRetryMs / 60000) + L" min");
    }

    void MessagingService::_editDirectory(int64_t messageId, const DirectoryRender& r, bool wantRich, uint64_t serial)
    {
        auto self = weak_from_this();
        const uint32_t hash = r.hash;
        // Every answer, whichever form went, ends in _onDirectoryEdited with the form the message shows then. What the render
        // lists rides along for the run's first edit log line (the one place the edited content can be read back on the PC).
        const auto finish = [self, serial, hash, messageId, wantRich, what = DescribeDirectoryRender(r)](const TgResult& res, bool shownRich) {
            if (auto me = self.lock())
            {
                me->_onDirectoryEdited(serial, hash, messageId, wantRich, res, shownRich, what);
            }
        };
        if (!wantRich)
        {
            OutItem item = _classicEditItem(messageId, r.classic, SendPriority::Backfill, L"dir:edit", json::Value{});
            item.onDone = [finish](const TgResult& res) { finish(res, false); };
            _enqueue(std::move(item));
            return;
        }
        const uint64_t seq = _takeOutSeq(); // the rich edit and its classic fallback keep one place in the order
        OutItem item;
        item.prio = SendPriority::Backfill;
        item.seq = seq;
        item.subSeq = 0;
        item.key = L"dir:edit";
        item.method = L"editMessageText";
        item.params = json::Value::MkObj();
        item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        item.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
        {
            auto richMessage = json::Value::MkObj();
            richMessage.Set(L"html", json::Value::MkStr(r.rich));
            item.params.Set(L"rich_message", std::move(richMessage));
        }
        item.onDone = [self, messageId, classic = r.classic, seq, finish](const TgResult& res) {
            if (RichEditRefusalWantsFallback(res))
            {
                if (auto me = self.lock())
                {
                    if (res.httpStatus == 404 && !me->_richUnavailable.exchange(true))
                    {
                        me->_log(L"[msg] the Bot API has no rich messages (404: " + res.description + L"): the directory and the titles go as HTML for the rest of this run");
                    }
                    else if (res.httpStatus != 404)
                    {
                        me->_log(L"[msg] Telegram refused the directory as a rich message (" + res.description + L"); it is edited as HTML");
                    }
                    // The classic render in the rich edit's place, WITHOUT a key: queued from this answer, a key would erase a newer
                    // edit queued meanwhile, and the stale text would win.
                    OutItem fb = me->_classicEditItem(messageId, classic, SendPriority::Backfill, L"", json::Value{});
                    fb.seq = seq;
                    fb.subSeq = 1;
                    fb.onDone = [finish](const TgResult& r2) { finish(r2, false); };
                    me->_enqueue(std::move(fb));
                }
                return;
            }
            finish(res, res.ok ? ResultIsRichMessage(res) : true); // "not modified": it shows this rich content already
        };
        _enqueue(std::move(item));
    }

    void MessagingService::_onDirectoryEdited(uint64_t serial, uint32_t hash, int64_t messageId, bool wantRich, const TgResult& r, bool shownRich,
                                              const std::wstring& what)
    {
        const bool taken = r.ok || r.NotModified();
        const bool gone = !taken && DirectoryEditFoundNoMessage(r.ok, r.httpStatus, r.description);
        bool firstLog = false;
        {
            std::lock_guard lk{ _mtx };
            if (serial != _directorySerial)
            {
                return; // an edit this run gave up on (a stall): the next check compares what the message shows anyway
            }
            _directoryInFlight = false;
            if (taken)
            {
                if (_state.directoryMessageId == messageId)
                {
                    _state.directoryHash = hash;
                    _state.directoryRich = shownRich;
                    _stateDirty = true;
                }
                if (wantRich && !shownRich)
                {
                    _directoryRichRefusedHash = hash; // the rich form was refused for this content: it stays HTML until it changes
                }
                _directoryNotBeforeMs = 0;
                firstLog = !_directoryEditLogged;
                _directoryEditLogged = true;
            }
            else if (gone)
            {
                // Deleted by the user, or an id that names no message of this chat: forget it; a new one is posted after the gap.
                if (_state.directoryMessageId == messageId)
                {
                    _state.directoryMessageId = 0;
                    _state.directoryHash = 0;
                    _state.directoryRich = false;
                    _state.directoryPinned = false;
                    _stateDirty = true;
                }
                _directoryPinTried = false;
                _directoryNotBeforeMs = WallMs() + kDirectoryRepostGapMs;
            }
            else
            {
                _directoryNotBeforeMs = WallMs() + kDirectoryRetryMs;
            }
        }
        if (gone)
        {
            const int64_t wall = WallMs();
            _log(L"[msg] directory message " + std::to_wstring(messageId) + L" is gone (" + r.description + L"); a new one is posted and pinned at " +
                 TgClockText(wall + kDirectoryRepostGapMs, wall));
        }
        else if (!taken)
        {
            _log(L"[msg] directory: Telegram refused the edit (" + (r.description.empty() ? std::wstring{ L"no answer" } : r.description) + L"); tried again in " +
                 std::to_wstring(kDirectoryRetryMs / 60000) + L" min");
        }
        else if (firstLog)
        {
            _log(L"[msg] directory edited (message " + std::to_wstring(messageId) + (shownRich ? L", rich: " : L", HTML: ") + what +
                 L"); later edits are logged only when one fails");
        }
    }

    void MessagingService::_pinDirectory(int64_t messageId)
    {
        if (messageId <= 0)
        {
            return;
        }
        {
            std::lock_guard lk{ _mtx };
            _directoryPinTried = true; // once per message and run: a refused pin is tried again at the next start
        }
        OutItem item;
        item.prio = SendPriority::Backfill;
        item.key = L"dir:pin";
        item.method = L"pinChatMessage";
        item.params = json::Value::MkObj();
        item.params.Set(L"chat_id", json::Value::MkNum(static_cast<double>(_ownerId)));
        item.params.Set(L"message_id", json::Value::MkNum(static_cast<double>(messageId)));
        item.params.Set(L"disable_notification", json::Value::MkBool(true)); // a private chat never notifies a pin anyway
        auto self = weak_from_this();
        item.onDone = [self, messageId](const TgResult& r) {
            auto me = self.lock();
            if (!me)
            {
                return;
            }
            if (r.ok)
            {
                {
                    std::lock_guard lk{ me->_mtx };
                    if (me->_state.directoryMessageId == messageId)
                    {
                        me->_state.directoryPinned = true;
                        me->_stateDirty = true;
                    }
                }
                me->_log(L"[msg] directory pinned (message " + std::to_wstring(messageId) + L")");
                return;
            }
            me->_log(L"[msg] directory: Telegram refused the pin (" + (r.description.empty() ? std::wstring{ L"no answer" } : r.description) + L"); it is tried again at the next start");
        };
        _enqueue(std::move(item));
    }

    const TabFact* MessagingService::_lastFact(const std::wstring& sessionId) const noexcept
    {
        // A linear search: one per rename, and a rename is rare (the name tick's budget allows a few a minute).
        for (const auto& f : _lastFacts)
        {
            if (f.sessionId == sessionId)
            {
                return &f;
            }
        }
        return nullptr;
    }

    TabColorMode MessagingService::_currentColorMode() const noexcept
    {
        if (const int mode = _colorMode.load(); mode >= 0 && mode <= static_cast<int>(TabColorMode::NoColor))
        {
            return static_cast<TabColorMode>(mode);
        }
        try
        {
            return LoadAppSettings().tabColorMode; // before the first sync: the settings' own (a reply that comes that early)
        }
        catch (...)
        {
            // Rule #18. Recovery: the default mode (one shared color per working directory).
            LogSwallowedException(L"MessagingService::_currentColorMode");
            return TabColorMode::WorkingDirectory;
        }
    }

    bool MessagingService::_stateIconsWanted() const noexcept
    {
        return !_stateIconRefused.load();
    }

    std::wstring MessagingService::_titleSquare(const SessionInfo* s, const std::wstring& dirFallback) const noexcept
    {
        try
        {
            const TabColorMode mode = _currentColorMode();
            if (s)
            {
                return TgTitleColorSquare(ResolveSessionColorHex(mode, *s));
            }
            if (dirFallback.empty())
            {
                return {};
            }
            // A session the registry does not know: its folder's color, like the Sessions page's chip of an on-disk session
            // (the per-folder precedence; in the Individual mode such a session has no color of its own to wear).
            SessionInfo folderOnly;
            folderOnly.workingDir = dirFallback;
            return TgTitleColorSquare(ResolveSessionColorHex(mode, folderOnly));
        }
        catch (...)
        {
            // Rule #18. Recovery: no square on this mention (the title alone).
            LogSwallowedException(L"MessagingService::_titleSquare");
            return {};
        }
    }

    void MessagingService::_recreateOneTopic(const std::wstring& sessionId, int verbose)
    {
        const auto s = _session(sessionId);
        std::wstring title;
        bool had = false;
        {
            std::lock_guard lk{ _mtx };
            if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
            {
                title = it->second.title;
                had = true;
            }
        }
        if (!s || !had)
        {
            _log(L"[msg] /recreate of " + ShortId(sessionId) + L": the topic or its session is gone; nothing to do");
            return;
        }
        if (!s->title.empty())
        {
            title = s->title;
        }
        std::wstring color;
        try
        {
            color = ResolveSessionColorHex(LoadAppSettings().tabColorMode, *s);
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_recreateOneTopic (color)");
        }
        const wchar_t* mode = verbose == 1 ? L"verbose" : L"conversation only";
        _deleteTopic(sessionId, std::wstring{ L"recreated " } + mode + L" by the user");
        _createTopic(sessionId, title, color, s->state, verbose);
        bool made = false;
        {
            std::lock_guard lk{ _mtx };
            made = _state.topics.count(sessionId) > 0;
        }
        _log(L"[msg] topic of " + ShortId(sessionId) + L" made again, " + mode +
             (made ? std::wstring{ L"; its whole history follows" } : std::wstring{ L"; Telegram refused the new topic (the sync makes it again, in the default mode)" }));
        LogNav(L"remote telegram /recreate " + ShortId(sessionId) + L" -> " + (verbose == 1 ? L"verbose" : L"conversation"));
    }

    void MessagingService::_updatePromptStatus(TopicRecord& topic, Mirror& m, const SessionInfo& s)
    {
        if (topic.promptText.empty() && topic.promptMessageId == 0)
        {
            return; // the live mirror has seen no prompt in this topic yet
        }
        // The id arrives in the shared PromptRef (the line's send, or the echo of a send as the user); after a restart the record
        // holds it and the PromptRef is made from it.
        if (!m.promptRef)
        {
            m.promptRef = std::make_shared<PromptRef>();
            m.promptRef->botLine.store(topic.promptBotLine);
            m.promptRef->id.store(topic.promptMessageId);
        }
        else if (const int64_t id = m.promptRef->id.load(); id > 0 && id != topic.promptMessageId)
        {
            topic.promptBotLine = m.promptRef->botLine.load();
            topic.promptMessageId = id;
        }
        if (topic.promptMessageId <= 0)
        {
            return; // the line's send has not answered yet: next tick
        }
        const PromptTurnStatus want = PromptTurnStatusFor(s.state, topic.promptInterrupted);
        if (static_cast<int>(want) == topic.promptStatus)
        {
            return;
        }
        if (want != PromptTurnStatus::Error && s.stateChangedAtUnixMs > 0 && WallMs() - s.stateChangedAtUnixMs < kPromptStatusSettleMs)
        {
            return; // a state that may still flicker: it shows once it has held
        }
        _showPromptStatus(topic, want);
    }

    void MessagingService::_showPromptStatus(TopicRecord& topic, PromptTurnStatus status)
    {
        if (topic.promptMessageId <= 0 || topic.promptStatus == static_cast<int>(status))
        {
            topic.promptStatus = static_cast<int>(status);
            return;
        }
        if (topic.promptBotLine)
        {
            // Our line: the same line with the new status (or none). Keyed per message, so a newer status replaces one still
            // queued; at the cards' priority, after every message.
            // §21.17: the same title row the line was posted with (the topic's title and square of now: a rename since shows).
            _editText(topic.promptMessageId,
                      BuildPromptLineHtml(topic.promptTsMs, static_cast<PromptSource>(topic.promptSource), topic.promptText, status, false,
                                          UserLineTitleRow(topic.title, topic.sessionId, topic.nameColor, topic.threadId)),
                      SendPriority::CardEdit,
                      L"pl:" + std::to_wstring(topic.promptMessageId),
                      json::Value{});
        }
        else
        {
            // The user's own message: a reaction from Telegram's fixed set (an empty one removes it).
            _react(topic.promptMessageId, PromptStatusReaction(status));
        }
        topic.promptStatus = static_cast<int>(status);
    }

    bool MessagingService::_topicVerbose(const TopicRecord& t) const noexcept
    {
        return TopicIsVerbose(t, _defaultVerbose.load());
    }

    void MessagingService::_refreshCard(const std::wstring& sessionId, bool force, bool ping, bool tapped, const SessionInfo* known)
    {
        // The tick passes the session copy its per-topic pass holds (`known`); every other caller has none and reads the
        // registry here.
        std::optional<SessionInfo> fetched;
        const SessionInfo* s = known;
        if (!s)
        {
            fetched = _session(sessionId);
            if (!fetched)
            {
                return;
            }
            s = &*fetched;
        }
        TopicRecord topic;
        bool pingWanted = false;
        bool repostWanted = false;
        // §21.28: a tapped button waits for this card's NEXT change (CardState::tapHolds): the change goes now, past the edit gap,
        // at an answer's priority, and the edit or post that carries it takes the holds.
        bool awaited = false;
        CardPage page = CardPage::Main; // §21.24: the page of buttons this card shows (CardState::page)
        {
            std::lock_guard lk{ _mtx };
            const auto it = _state.topics.find(sessionId);
            if (it == _state.topics.end() || it->second.threadId == 0)
            {
                return;
            }
            topic = it->second;
            auto& card = _cards[sessionId];
            if (ping)
            {
                card.pingWanted = true; // kept until a post can carry it (below)
                // §21.24: a card that starts to RING (a session that exited, errs or needs approval - a state CHANGE, so once
                // per episode) goes back to its Main page, where the state's own action is (🔄 Restart Session for an exit or an
                // error).
                // Only when the ring is raised: a page the user opens after that is kept, the ringing re-post included - the
                // tap is the newer word.
                card.page = CardPage::Main;
            }
            pingWanted = card.pingWanted;
            repostWanted = card.repostWanted; // the list's re-sort (_resortTick), kept until a post can carry it
            page = card.page; // §21.24: the page the user opened (Main unless ⚙ Session was tapped); a re-post keeps it
            // §21.28: a hold of a tap its hold's limit answered already waits for nothing — it goes here (under `_mtx`: its sink
            // only queues), so it can never force a later, unrelated change of this card past the edit gap.
            auto& waitingHolds = card.tapHolds;
            waitingHolds.erase(std::remove_if(waitingHolds.begin(), waitingHolds.end(), [](const TapHoldRef& h) { return !h || !h->Tap() || h->Tap()->Answered(); }),
                               waitingHolds.end());
            awaited = !waitingHolds.empty();
        }
        if (topic.cardMessageId < 0)
        {
            // §21.28: a tap that wants a change of this card now (a page flip) waits for the change that follows the answer.
            _awaitCardChange(sessionId);
            return; // a post is in flight; its answer sets the id (a wanted ping goes with the next post)
        }
        const int64_t now = WallMs();
        if (topic.cardMessageId == 0 && now - topic.cardPostedMs < kCardRepostAfterFailureMs)
        {
            _awaitCardChange(sessionId); // §21.28: the next post carries the tap (its hold's limit comes first, a minute being longer)
            return; // Telegram refused the last post: not every tick
        }
        // The idle line's clock comes from the persisted stamps (IdleTopicLeavesAtMs — the same rule the plan deletes
        // by), so it only changes when the session is used: no periodic edit.
        const int64_t idleLeaves = IdleTopicLeavesAtMs(topic.idleSinceMs, s->lastActivityUnixMs, _idleGraceMs.load());
        CardExtras extras;
        extras.recapFresh = RecapIsFresh(topic.recapText, topic.recapAtMs, topic.lastMessageAtMs);
        extras.recapText = topic.recapText;
        extras.recapAtMs = topic.recapAtMs;
        const bool historyPending = TopicBackfillPending(topic);
        extras.historyPending = historyPending;
        // §21.13: the square the topic's NAME shows, not the tab's color of this instant: the card and the list always agree,
        // and a color change edits the card in the rename's own refresh (a topic named before the square gets it then too).
        extras.colorSquare = topic.nameColor;
        extras.threadId = topic.threadId; // §21.17: the card's title links to its topic (in the chat's "All" view, the way in)
        const std::wstring text = BuildCardText(*s, now, idleLeaves, _scheduler ? _scheduler->GlobalPaused() : false, &extras);
        json::Value markup;
        _cardMarkupFor(sessionId, CardButtonFactsFor(*s), page, markup);
        // The text AND the buttons: a card whose text is unchanged but whose buttons differ is a change too. §21.15: over the
        // card's CLASSIC render (its title token as the §21.13 square and title), so the hash does not move with the form the
        // card is shown in — an unchanged card keeps the hash its record holds, and the upgrade to chips edits no card at once.
        const uint32_t hash = CardTextHash(TgRenderTitleTokensClassic(text) + L"\x1F" + json::Dump(markup));
        // §21.42: the TEXT alone (no buttons): a post or edit whose text changed since the card's last update is a status update and
        // reads the screen again; one whose text is the same (a page flip, the slow form upgrade) keeps its picture with no read.
        const uint32_t textHash = CardTextHash(TgRenderTitleTokensClassic(text));
        // §21.42: whether this post or edit carries the terminal's screen (the switch, rich cards, a window that can read screens).
        const bool pictured = _cardPicturesWanted();
        // The STICKY CARD (§21.5): posts newer than the card push it down — once they have been quiet for
        // kCardStickyLagMs the old card is deleted and a fresh one sent at the bottom (two calls per burst, not
        // per message). A missing card (the first one, or a refused post a minute ago) is posted the same way, and
        // so is a PING (§21.6): the card re-sent WITH SOUND is the notification for a state the user must know about.
        // While the topic's HISTORY is being replayed (§21.9) the card stays where it is, above it — chasing every history
        // message down would double the replay's calls; it moves to the bottom once, when the last part is in. A RE-SORT
        // of the list (_resortTick) asks for a silent re-post: the re-post is what moves the topic up in Telegram's list.
        const bool behind = !historyPending && topic.cardMessageId > 0 && topic.lastPostedMs > topic.cardPostedMs && now - topic.lastPostedMs >= kCardStickyLagMs;
        const bool resort = !historyPending && repostWanted;
        if (topic.cardMessageId == 0 || behind || pingWanted || resort)
        {
            std::vector<TapHoldRef> waiting; // §21.28: the taps that wait for this card's change, taken by the post (below)
            {
                std::lock_guard lk{ _mtx };
                auto& card = _cards[sessionId];
                card.pingWanted = false;
                card.repostWanted = false;
                card.lastEditMs = now;
                card.lastState = s->state;
                waiting.swap(card.tapHolds);
            }
            if (pingWanted)
            {
                _log(L"[msg] " + ShortId(sessionId) + L" card re-posted with sound (" + StateWord(s->state) + L")");
            }
            // §21.28: the old card's delete and the new card's send hold the waiting taps (a directory dot's re-post, a tap's change
            // that met a missing card) and, with a tap waiting or in scope, go at an answer's priority.
            const TapScope tapScope{ waiting };
            const SendPriority prio = (tapped || !waiting.empty()) ? SendPriority::Final : SendPriority::CardEdit;
            const int64_t deleteFirst = topic.cardMessageId > 0 ? topic.cardMessageId : 0;
            if (pictured)
            {
                // §21.42: the post carries the terminal's screen, which takes a window's read (and maybe a drawing) first. The card
                // goes "in flight" (-1) NOW - every refresh meanwhile waits for it, so no second post starts - and the post itself
                // (the old card's delete, then the new card) when the picture is settled. A post always reads the screen: it is the
                // first card, a re-post below newer messages, a ping or a re-sort, each a moment the screen belongs on the card.
                uint64_t pictureSeq = 0;
                {
                    std::lock_guard lk{ _mtx };
                    if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
                    {
                        it->second.cardMessageId = -1;
                        it->second.cardTextHash = hash;
                        _stateDirty = true;
                    }
                    auto& card = _cards[sessionId];
                    // A new generation overtakes any edit still waiting for its picture (it targets the card this post replaces).
                    // Within a run the post itself is never overtaken: while it is in flight no refresh posts or edits this card.
                    pictureSeq = ++card.pictureSeq;
                    card.pictureTextHash = textHash;
                }
                auto self = weak_from_this();
                const int64_t thread = topic.threadId;
                _withCardPicture(sessionId, true, [self, sessionId, pictureSeq, thread, text, markup, deleteFirst, pingWanted, hash, prio](std::shared_ptr<const CardPicture> picture) {
                    if (const auto me = self.lock())
                    {
                        {
                            // A step of a run the bot was stopped in (a cog Save that restarts it): the new run cleared the card
                            // states and the in-flight mark, and posts its own card - this late post would make a second one.
                            std::lock_guard lk{ me->_mtx };
                            const auto it = me->_cards.find(sessionId);
                            if (it == me->_cards.end() || it->second.pictureSeq != pictureSeq)
                            {
                                return;
                            }
                        }
                        // _postCard queues the old card's delete and the new card (with the picture, or without one when none
                        // could be had) and its answer clears the in-flight mark. Contained: `then` runs on a window's UI thread,
                        // the picture worker or a destructor (CardPictureOnce contains a throw and logs it).
                        me->_postCard(sessionId, thread, text, markup, deleteFirst, pingWanted, hash, prio, std::move(picture));
                    }
                });
                return;
            }
            _postCard(sessionId, topic.threadId, text, std::move(markup), deleteFirst, pingWanted, hash, prio);
            return;
        }
        const bool wantRich = _titleChipsWanted();
        // §21.17: the link is part of the FORM, not of the hash (taken without the username above): every card posted before the
        // links, or before this run's getMe answered, is linked by the slow upgrade, one card at a time.
        const bool wantLinked = !_linkBot().empty();
        bool formOnly = false;
        if (hash == topic.cardTextHash)
        {
            if (topic.cardRich == wantRich && topic.cardLinked == wantLinked)
            {
                return; // the topic shows this very card already, in the wanted form (also after a restart: both are persisted)
            }
            // §21.15: the text is unchanged and only the FORM differs — a card posted before the chips (or the links), or the
            // rich switch flipped. Re-edited only when the slow upgrade gives this card the turn: one card at a time, within the
            // budget.
            if (!_takeCardFormatSlot(sessionId, hash, wantRich, wantLinked))
            {
                return;
            }
            formOnly = true;
        }
        bool send = false;
        std::vector<TapHoldRef> waiting; // §21.28: the taps that wait for this card's change, taken by the edit (below)
        // §21.42: this edit's picture generation (CardState::pictureSeq) and whether its text changed, i.e. whether the screen is
        // read again (a page flip of the buttons and the slow form upgrade keep the card's picture without a read).
        uint64_t pictureSeq = 0;
        bool readScreen = false;
        {
            std::lock_guard lk{ _mtx };
            auto& card = _cards[sessionId];
            const bool stateChanged = card.lastState != s->state;
            if (formOnly || force || awaited || stateChanged || now - card.lastEditMs >= kCardEditGapMs)
            {
                card.lastEditMs = now;
                card.lastState = s->state;
                if (pictured)
                {
                    pictureSeq = ++card.pictureSeq; // a picture step of an older edit still running is overtaken: it is dropped
                    readScreen = !formOnly && card.pictureTextHash != textHash;
                    if (readScreen)
                    {
                        card.pictureTextHash = textHash;
                    }
                }
                if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
                {
                    it->second.cardTextHash = hash;
                    // PROGRESS (§21.35): the stored hash only spares the next start a re-edit of an unchanged card; lost after an
                    // unexpected end, that card is edited once more. A structural mark saved the file within kStateSaveGapMs of
                    // every card edit.
                    _stateProgressDirty = true;
                }
                send = true;
                if (!formOnly)
                {
                    // §21.28: a form-only edit (the slow upgrade to chips or links) is not the change a tap waits for: the taps stay.
                    waiting.swap(card.tapHolds);
                }
            }
        }
        if (send)
        {
            // A card the user deleted on the phone cannot be edited: the answer clears the record's card, and the next
            // refresh posts a new one at the bottom (cardPostedMs 0 = no failure wait).
            auto self = weak_from_this();
            const int64_t thread = topic.threadId;
            const int64_t cardId = topic.cardMessageId;
            // §21.28: the edit holds the taps that waited for this change (the tap in scope, if any, holds it anyway).
            const TapScope tapScope{ waiting };
            // §21.24: a tap on the card's own buttons (⚙ Session, ◀ Back) is answered at once: the edit takes an answer's priority.
            // §21.28: so does a change a tapped button waits for (a restart that shows on the card, a page flip that met a post).
            const SendPriority prio = (tapped || !waiting.empty()) ? SendPriority::Final : SendPriority::CardEdit;
            // §21.42 named the two answer handlers (they were inline) so the edit can also be made later, once its picture is settled.
            std::function<void(const TgResult&)> onDeleted = [self, thread, cardId](const TgResult& r) {
                if (r.ok || r.NotModified() || r.httpStatus != 400 || r.description.find(L"message to edit not found") == std::wstring::npos)
                {
                    return;
                }
                auto me = self.lock();
                if (!me)
                {
                    return;
                }
                {
                    std::lock_guard lk{ me->_mtx };
                    for (auto& [sid, t] : me->_state.topics)
                    {
                        if (t.threadId == thread && t.cardMessageId == cardId)
                        {
                            t.cardMessageId = 0;
                            t.cardPostedMs = 0;
                            t.cardTextHash = 0;
                            me->_stateDirty = true;
                            me->_tickRequested = true;
                            break;
                        }
                    }
                }
                me->_log(L"[msg] a card was deleted in Telegram: a new one is posted (thread " + std::to_wstring(thread) + L")");
                me->_tickCv.notify_one();
            };
            std::function<void(bool)> onForm = [self, thread, cardId, hash, wantRich, wantLinked](bool rich) {
                // §21.15: the form the card shows after this edit. A rich edit that went as HTML (refused, or past the rich
                // limit) marks this text refused, so the slow upgrade does not retry it until the text changes.
                auto me = self.lock();
                if (!me)
                {
                    return;
                }
                std::lock_guard lk{ me->_mtx };
                for (auto& [sid, t] : me->_state.topics)
                {
                    if (t.threadId == thread && t.cardMessageId == cardId)
                    {
                        if (t.cardRich != rich)
                        {
                            t.cardRich = rich;
                            me->_stateDirty = true;
                        }
                        // §21.17: both forms carry the link when the username was known for this edit (a rich edit that fell
                        // back to HTML included).
                        if (t.cardLinked != wantLinked)
                        {
                            t.cardLinked = wantLinked;
                            me->_stateDirty = true;
                        }
                        if (wantRich && !rich)
                        {
                            me->_cards[sid].richRefusedHash = hash;
                        }
                        break;
                    }
                }
            };
            if (pictured)
            {
                // §21.42: the edit carries the terminal's screen. Settled asynchronously (a window's read, maybe a drawing), so the
                // edit goes when the picture is - unless a newer post or edit of this card was decided meanwhile (its pictureSeq):
                // then this one is dropped, because finishing last it would put an older text back. Its taps go with it; the newer
                // change carries what they waited for.
                _withCardPicture(sessionId, readScreen, [self, sessionId, pictureSeq, cardId, thread, text, prio, markup, onDeleted, onForm](std::shared_ptr<const CardPicture> picture) {
                    const auto me = self.lock();
                    if (!me)
                    {
                        return;
                    }
                    {
                        std::lock_guard lk{ me->_mtx };
                        const auto it = me->_cards.find(sessionId);
                        if (it == me->_cards.end() || it->second.pictureSeq != pictureSeq)
                        {
                            return; // overtaken by a newer post or edit of this card
                        }
                    }
                    std::function<void(const TgResult&)> done = onDeleted;
                    if (picture)
                    {
                        // Its answer also records the photo the card shows now (for the next update to reuse).
                        done = [self, onDeleted, picture, thread, cardId](const TgResult& r) {
                            if (onDeleted)
                            {
                                onDeleted(r);
                            }
                            if (const auto m = self.lock())
                            {
                                m->_noteCardPictureShown(thread, cardId, *picture, r);
                            }
                        };
                    }
                    me->_editText(cardId, text, prio, L"card:" + sessionId, markup, std::move(done), onForm, std::move(picture));
                });
            }
            else
            {
                _editText(topic.cardMessageId, text, prio, L"card:" + sessionId, markup, std::move(onDeleted), std::move(onForm));
            }
        }
    }

    bool MessagingService::_takeCardFormatSlot(const std::wstring& sessionId, uint32_t hash, bool wantRich, bool wantLinked)
    {
        size_t waiting = 0;
        {
            std::lock_guard lk{ _mtx };
            // §21.17: whether this card lacks the link it should carry — an upgrade the classic fallback can make even when the rich
            // form is refused.
            bool linkMissing = false;
            if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
            {
                linkMissing = it->second.cardLinked != wantLinked;
            }
            if (wantRich && _cards[sessionId].richRefusedHash == hash && !linkMissing)
            {
                return false; // Telegram refused this very text in the rich form: not again until the text changes
            }
            const int64_t tick = static_cast<int64_t>(::GetTickCount64());
            if (_lastCardFormatEditMs != 0 && tick - _lastCardFormatEditMs < kCardFormatGapMs)
            {
                return false; // one form-only edit per gap, across all topics
            }
            // Not after a 429 (the pause the history replay keeps) or while a long one parks the edit method, and only while the
            // last minute's calls leave room for this one under the budget the replay is held to: the upgrade is the least
            // urgent call there is, and a burst of card edits is what earned the 21-minute 429 of 2026-10-16.
            _noteCallLocked(0); // prunes the window only
            if ((_floodAtMs != 0 && tick - _floodAtMs < kHistoryFloodPauseMs) || FloodParkedUntil(_floodParking, L"editMessageText", tick) > 0)
            {
                return false;
            }
            if (_recentCallsMs.size() + 1 > kHistoryCallsPerMinute)
            {
                return false;
            }
            _lastCardFormatEditMs = tick;
            for (const auto& [sid, t] : _state.topics)
            {
                if (sid != sessionId && t.threadId != 0 && t.cardMessageId > 0 && (t.cardRich != wantRich || t.cardLinked != wantLinked))
                {
                    ++waiting;
                }
            }
        }
        _log(L"[msg] " + ShortId(sessionId) + L" card re-edited " + std::wstring{ wantRich ? L"with a title chip" : L"as HTML" } +
             (wantLinked ? L" that links to its topic" : L"") + L" (its form only; " + std::to_wstring(waiting) + L" more card(s) to go)");
        return true;
    }

    // ===== §21.42: the card's screenshot =====================================================

    bool MessagingService::_cardPicturesWanted() const noexcept
    {
        // The picture is a block of the RICH card, so it needs titled messages to go rich; and a window must be able to read the
        // screen (the m5 harness sets no hooks).
        return _cardPictures.load() && !_cardPicturesStopped.load() && _titleChipsWanted() && static_cast<bool>(_hooks.sessionAction);
    }

    void MessagingService::_withCardPicture(const std::wstring& sessionId, bool readScreen, std::function<void(std::shared_ptr<const CardPicture>)> then) noexcept
    {
        // The card's send runs EXACTLY ONCE, whichever path below settles the picture first (CardPictureOnce); a failure anywhere
        // here still runs it, without a picture, so a card change is never lost to its picture (a post would otherwise stay "in
        // flight" until the next start).
        std::shared_ptr<CardPictureOnce> once;
        try
        {
            once = std::make_shared<CardPictureOnce>(std::move(then));
        }
        catch (...)
        {
            // Rule #18. Recovery: no memory for the wrapper - `then` is intact (the allocation failed before the move): send now.
            LogSwallowedException(L"MessagingService::_withCardPicture (the send's wrapper)");
            try
            {
                if (then)
                {
                    then(nullptr);
                }
            }
            catch (...)
            {
                // Rule #18. Recovery: this card change is lost; the next update tries again.
                LogSwallowedException(L"MessagingService::_withCardPicture (the send)");
            }
            return;
        }
        try
        {
            // The picture the card shows now (its topic record), and the generation the caller stamped for this change.
            std::wstring storedId;
            int64_t storedAt = 0;
            uint32_t storedHash = 0;
            uint64_t seq = 0;
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
                {
                    storedId = it->second.cardPictureFileId;
                    storedAt = it->second.cardPictureAtMs;
                    storedHash = it->second.cardPictureShotHash;
                }
                if (const auto it = _cards.find(sessionId); it != _cards.end())
                {
                    seq = it->second.pictureSeq;
                }
            }
            std::shared_ptr<const CardPicture> stored;
            if (!storedId.empty())
            {
                auto held = std::make_shared<CardPicture>();
                held->fileId = storedId;
                held->shotAtMs = storedAt;
                held->shotHash = storedHash;
                stored = std::move(held);
            }
            if (!readScreen)
            {
                // Not a status update (a page flip of the buttons, the slow form upgrade): the card keeps the picture it shows, with no
                // read and no drawing - a page flip must stay instant.
                _countCardPicture(stored ? L"kept" : L"none", 0);
                once->Run(stored);
                return;
            }
            // §21.28: the tapped buttons that wait for this change ride the read and the drawing (the caller's TapScope holds them).
            std::vector<TapHoldRef> taps = _holdTaps();
            // No window answering (every copy of the reply going uncalled) sends the card with the stored picture.
            const auto guard = std::make_shared<CardPictureReadGuard>(once, stored, taps);
            {
                // ...and so does a window that does not answer IN TIME (a frozen UI thread holds its copy of the request): past
                // kCardPictureWaitMs the send loop sends the card with the stored picture, and a late picture is dropped.
                PendingCardPicture wait;
                wait.deadlineTickMs = static_cast<int64_t>(::GetTickCount64()) + kCardPictureWaitMs;
                wait.sessionId = sessionId;
                wait.settled = [once]() { return once->Ran(); };
                wait.expire = [once, stored, taps]() {
                    const TapScope tapScope{ taps };
                    once->Run(stored);
                };
                std::lock_guard lk{ _pictureMtx };
                _pendingCardPictures.push_back(std::move(wait));
            }
            auto self = weak_from_this();
            // The hosting window reads the live screen with its colors on its UI thread (one pass over the viewport's cells under the
            // terminal's read lock: 0.3-0.6 ms optimized, measured for §21.24) and answers with the serialized snapshot ("" for a parked
            // tab, whose claude shows nothing, or a control that is not up).
            _hooks.sessionAction(sessionId, L"screen-snapshot", L"", [self, sessionId, seq, once, stored, storedId, storedAt, storedHash, taps, guard](std::wstring snapshot) {
                if (!guard->Claim())
                {
                    return;
                }
                const TapScope tapScope{ taps };
                const auto me = self.lock();
                if (!me)
                {
                    once->Run(nullptr); // the service is going: whatever runs now sends nothing
                    return;
                }
                try
                {
                    const int64_t now = WallMs();
                    const uint32_t shotHash = snapshot.empty() ? 0u : CardPictureShotHash(snapshot);
                    const CardPicturePlan plan = PlanCardPicture(!snapshot.empty(), shotHash, now, storedId, storedHash, storedAt);
                    if (plan.source == CardPictureSource::Upload)
                    {
                        // A screen that changed: drawn on the picture worker (never on this UI thread: ~90-185 ms), uploaded by the card's
                        // own post or edit. The stored picture stays the fallback should the drawing fail.
                        CardPictureJob job;
                        job.sessionId = sessionId;
                        job.seq = seq;
                        job.snapshot = std::move(snapshot);
                        job.shotHash = shotHash;
                        job.shotAtMs = now;
                        job.fallback = stored;
                        job.then = [once](std::shared_ptr<const CardPicture> picture) { once->Run(std::move(picture)); };
                        job.taps = taps;
                        me->_queueCardPictureRender(std::move(job));
                        return;
                    }
                    if (plan.source == CardPictureSource::Reuse)
                    {
                        // The screen is the one the card shows (read now: captioned now), or it could not be read (a parked tab: the
                        // last picture with its own moment). Either way the held photo, by file_id: no upload.
                        auto reused = std::make_shared<CardPicture>();
                        reused->fileId = storedId;
                        reused->shotAtMs = plan.shotAtMs;
                        reused->shotHash = storedHash;
                        me->_countCardPicture(shotHash != 0 ? L"reused" : L"kept", 0);
                        once->Run(std::move(reused));
                        return;
                    }
                    me->_countCardPicture(L"none", 0); // no screen and no picture yet: the card goes as text
                    once->Run(nullptr);
                }
                catch (...)
                {
                    // Rule #18. Recovery: the card goes with the picture it shows (or none); the next update reads the screen again.
                    LogSwallowedException(L"MessagingService::_withCardPicture (the window's answer)");
                    once->Run(stored);
                }
            });
        }
        catch (...)
        {
            // Rule #18. Recovery: the card goes now without a picture (a no-op when a path above already sent it).
            LogSwallowedException(L"MessagingService::_withCardPicture");
            once->Run(nullptr);
        }
    }

    void MessagingService::_expireCardPictureWaits() noexcept
    {
        try
        {
            std::vector<PendingCardPicture> late;
            {
                std::lock_guard lk{ _pictureMtx };
                if (_pendingCardPictures.empty())
                {
                    return; // the common case: nothing waits
                }
                const int64_t now = static_cast<int64_t>(::GetTickCount64());
                for (auto it = _pendingCardPictures.begin(); it != _pendingCardPictures.end();)
                {
                    if (it->settled && it->settled())
                    {
                        it = _pendingCardPictures.erase(it); // its picture settled (or a failure sent the card): nothing to do
                        continue;
                    }
                    if (now >= it->deadlineTickMs)
                    {
                        late.push_back(std::move(*it));
                        it = _pendingCardPictures.erase(it);
                        continue;
                    }
                    ++it;
                }
            }
            for (auto& wait : late)
            {
                _log(L"[msg] " + ShortId(wait.sessionId) + L" card picture not ready within " + std::to_wstring(kCardPictureWaitMs / 1000) +
                     L" s (the window's screen read or its drawing): the card goes with the picture it shows");
                if (wait.expire)
                {
                    wait.expire(); // CardPictureOnce never throws; a late picture is then dropped
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the waits stay (or are lost with the throw); a late card is then sent by its own picture's arrival.
            LogSwallowedException(L"MessagingService::_expireCardPictureWaits");
        }
    }

    void MessagingService::_queueCardPictureRender(CardPictureJob job) noexcept
    {
        try
        {
            bool start = false;
            {
                std::lock_guard lk{ _pictureMtx };
                _pictureJobs.push_back(std::move(job)); // the strong guarantee: on a throw `job` is untouched (handled below)
                if (!_pictureWorkerRunning)
                {
                    _pictureWorkerRunning = true;
                    start = true;
                }
            }
            if (!start)
            {
                return; // the running worker takes it after the jobs before it
            }
            try
            {
                // One thread for the whole queue, ending when it is empty. It holds the service only while it draws (a weak pointer
                // between jobs, locked per job inside the loop's owner call).
                std::thread([self = weak_from_this()]() noexcept {
                    if (const auto me = self.lock())
                    {
                        me->_runCardPictureWorker();
                    }
                }).detach();
            }
            catch (...)
            {
                // Rule #18. Recovery: no thread could start - every queued job goes now with its fallback (no picture drawn), so no
                // card change is lost, and the next job tries to start the worker again.
                LogSwallowedException(L"MessagingService::_queueCardPictureRender (the worker thread)");
                std::deque<CardPictureJob> left;
                {
                    std::lock_guard lk{ _pictureMtx };
                    left.swap(_pictureJobs);
                    _pictureWorkerRunning = false;
                }
                for (auto& j : left)
                {
                    const TapScope tapScope{ j.taps };
                    if (j.then)
                    {
                        j.then(j.fallback);
                    }
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the job could not be queued (no memory): its card goes now with the fallback.
            LogSwallowedException(L"MessagingService::_queueCardPictureRender");
            const TapScope tapScope{ job.taps };
            if (job.then)
            {
                job.then(job.fallback);
            }
        }
    }

    void MessagingService::_runCardPictureWorker() noexcept
    {
        for (;;)
        {
            CardPictureJob job;
            {
                std::lock_guard lk{ _pictureMtx };
                if (_pictureJobs.empty())
                {
                    _pictureWorkerRunning = false; // under the same lock as the push: no job is left behind unseen
                    return;
                }
                job = std::move(_pictureJobs.front());
                _pictureJobs.pop_front();
            }
            std::shared_ptr<const CardPicture> result = job.fallback;
            try
            {
                bool current = false;
                {
                    std::lock_guard lk{ _mtx };
                    const auto it = _cards.find(job.sessionId);
                    current = it != _cards.end() && it->second.pictureSeq == job.seq;
                }
                if (!current)
                {
                    // A newer post or edit of this card was decided while this one waited: its edit is dropped anyway, so nothing is
                    // drawn for it (the fallback lets its `then` see that and stop).
                    _countCardPicture(L"skipped", 0);
                }
                else if (const auto parsed = ParseScreenSnapshot(job.snapshot); !parsed)
                {
                    _log(L"[msg] " + ShortId(job.sessionId) + L" card picture: the screen's snapshot did not parse (" + std::to_wstring(job.snapshot.size()) + L" chars); the card keeps its picture");
                }
                else
                {
                    // The same drawing as 🖼 Screen Image (§21.24): the terminal's colors, its font, a whole-pixel grid, as a PNG.
                    ScreenImage image = RenderScreenSnapshotPng(*parsed);
                    if (image.png.empty())
                    {
                        _log(L"[msg] " + ShortId(job.sessionId) + L" card picture could not be drawn: " + image.error + L"; the card keeps its picture");
                    }
                    else
                    {
                        auto drawn = std::make_shared<CardPicture>();
                        drawn->png = std::move(image.png);
                        drawn->shotAtMs = job.shotAtMs;
                        drawn->shotHash = job.shotHash;
                        _countCardPicture(L"drawn", drawn->png.size());
                        result = std::move(drawn);
                    }
                }
            }
            catch (...)
            {
                // Rule #18. Recovery: the card goes with its fallback (the picture it shows, or none); the worker goes on.
                LogSwallowedException(L"MessagingService::_runCardPictureWorker");
            }
            // The card's post or edit, with what came of it. CardPictureOnce never throws; the scope never throws.
            const TapScope tapScope{ job.taps };
            if (job.then)
            {
                job.then(std::move(result));
            }
        }
    }

    void MessagingService::_noteCardPictureShown(int64_t threadId, int64_t cardMessageId, const CardPicture& picture, const TgResult& r) noexcept
    {
        try
        {
            if (!r.ok)
            {
                return; // refused, or "not modified": the card shows what it showed
            }
            const CardPictureShown shown = CardPictureFromMessage(r.result);
            if (shown.fileId.empty())
            {
                return; // the answer shows no photo (the retry without it went): the stored picture stays the one to reuse
            }
            _cardPictureRefusals.store(0); // a picture went through: refusals count from zero again
            std::lock_guard lk{ _mtx };
            for (auto& [sid, t] : _state.topics)
            {
                // By THREAD and the card's id: a Rekey moved the topic meanwhile (the thread stays), or a newer post replaced this card
                // (its id differs, and its own answer records its picture).
                if (t.threadId == threadId && (cardMessageId == 0 || t.cardMessageId == cardMessageId))
                {
                    t.cardPictureFileId = shown.fileId;
                    t.cardPictureAtMs = picture.shotAtMs;
                    t.cardPictureShotHash = picture.shotHash;
                    // PROGRESS (§21.35): a running card changes its picture every ~20 s; lost after an unexpected end, the next update
                    // just uploads once more.
                    _stateProgressDirty = true;
                    break;
                }
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the picture is not recorded; the next update draws and uploads one again.
            LogSwallowedException(L"MessagingService::_noteCardPictureShown");
        }
    }

    void MessagingService::_noteCardPictureRefused(const TgResult& r) noexcept
    {
        try
        {
            const int refusals = ++_cardPictureRefusals;
            _log(L"[msg] Telegram refused a card with its screen picture (" + std::to_wstring(r.httpStatus) + L": " + r.description + L"); it goes again without the picture");
            if (refusals >= kCardPictureRefusalsToStop && !_cardPicturesStopped.exchange(true))
            {
                _log(L"[msg] " + std::to_wstring(refusals) + L" card pictures refused in a row: the cards go without the screen for the rest of this run (the next start tries again)");
            }
        }
        catch (...)
        {
            // Rule #18. Recovery: the refusal is not logged; the count (atomic) was taken before anything could throw.
            LogSwallowedException(L"MessagingService::_noteCardPictureRefused");
        }
    }

    void MessagingService::_countCardPicture(const wchar_t* kind, size_t bytes) noexcept
    {
        try
        {
            std::lock_guard lk{ _mtx };
            ++_cardPicturesByKind[kind];
            _cardPictureBytes += bytes;
        }
        catch (...)
        {
            // Rule #18. Recovery: the census line misses this one decision.
            LogSwallowedException(L"MessagingService::_countCardPicture");
        }
    }

    void MessagingService::_postRemark(const std::wstring& sessionId, TopicRecord& topic, Mirror& m)
    {
        if (m.pendingText.empty())
        {
            return;
        }
        // The thread is the conversation (§21.5): what the assistant said before it reached for a tool is a
        // message of its own, silent. The work log so far is closed with its final content first, so the remark
        // lands below it, and the tools that follow start a new work-log message below the remark.
        _flushWorkLog(sessionId, topic, m, true);
        // Claude's own Markdown (§21.11): a rich message, so a table or a heading in a remark shows as one.
        _sendRich(topic.threadId, m.pendingText, TgMarkdownToHtml(m.pendingText), true, SendPriority::New, nullptr);
        m.pendingText.clear();
        m.workLog.clear();
        m.workLogMessageId = 0;
        m.workLogDirty = false;
        ++m.workLogGen; // a create still in flight for the closed segment must not become the new segment's message
        topic.workLogMessageId = 0;
        m.turnOpen = true;
    }

    // ===== the mirror ========================================================================

    void MessagingService::_appendWorkLog(Mirror& m, const std::wstring& line)
    {
        if (!m.workLog.empty())
        {
            m.workLog += L'\n';
        }
        m.workLog += line;
        m.workLogDirty = true;
        m.turnOpen = true;
    }

    void MessagingService::_flushWorkLog(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, bool force)
    {
        if (!m.workLogDirty || m.workLog.empty())
        {
            return;
        }
        const int64_t now = WallMs();
        // The gap grows with the number of turns open across the fleet (the tick loop counts them): one busy
        // session edits every 15 s, twenty busy sessions every 40 s each — together never more than ~0.5
        // edit/s, half of the chat's ~1 call/s budget (the other half is for messages and cards).
        const int64_t gapMs = std::max<int64_t>(kWorkLogEditGapMs, static_cast<int64_t>(_openTurns.load()) * 2000);
        if (!force && m.workLogMessageId != 0 && now - m.workLogLastEditMs < gapMs)
        {
            return;
        }
        // Past the roll-over size the message is closed and the next line starts a new one.
        std::wstring html = L"<blockquote expandable>" + TgEscapeHtml(m.workLog) + L"</blockquote>";
        if (m.workLog.size() > kWorkLogRolloverChars && m.workLogMessageId != 0)
        {
            _editText(m.workLogMessageId, html, SendPriority::WorkLogEdit, L"wl:" + sessionId, json::Value{});
            m.workLog.clear();
            m.workLogMessageId = 0;
            topic.workLogMessageId = 0;
            m.workLogDirty = false;
            m.workLogLastEditMs = now;
            return;
        }
        m.workLogDirty = false;
        m.workLogLastEditMs = now;
        if (m.workLogMessageId == 0)
        {
            if (topic.workLogMessageId < 0)
            {
                return; // a create is in flight; the next tick edits it
            }
            topic.workLogMessageId = -1; // in flight
            {
                // The shared record says "in flight" too, from now on: the tick's write-back then knows that a record
                // no longer at -1 holds the create's ANSWER (a message id, or 0 for a refusal) and keeps it.
                std::lock_guard lk{ _mtx };
                if (const auto it = _state.topics.find(sessionId); it != _state.topics.end())
                {
                    it->second.workLogMessageId = -1;
                }
            }
            auto self = weak_from_this();
            const int64_t thread = topic.threadId;
            const uint32_t gen = m.workLogGen;
            _sendText(thread, html, true, SendPriority::New, L"", json::Value{}, [self, sessionId, gen, thread](const TgResult& r) {
                auto me = self.lock();
                if (!me)
                {
                    return;
                }
                const int64_t id = r.ok ? r.result.I64At(L"message_id", 0) : 0;
                std::lock_guard lk{ me->_mtx };
                const auto mit = me->_mirrors.find(sessionId);
                if (mit == me->_mirrors.end())
                {
                    return; // the topic was deleted or carried over to another session meanwhile
                }
                auto& mm = mit->second;
                if (mm.workLogGen != gen)
                {
                    return; // the segment was closed (a remark, a new turn) while this create flew: its message stays as it is
                }
                const auto it = me->_state.topics.find(sessionId);
                if (it == me->_state.topics.end() || it->second.threadId != thread)
                {
                    return; // the session's topic was made again meanwhile (a recreation, §21.8): this message is the old topic's
                }
                it->second.workLogMessageId = id;
                // The new id is saved with the mirror's progress: the tick's write-back keeps this record value
                // (workLogAnswerLanded) and so reports no change of its own for it.
                me->_stateProgressDirty = true;
                mm.workLogMessageId = id;
                if (id != 0)
                {
                    mm.workLogDirty = true; // what arrived while the create was in flight goes out as an edit
                    me->_tickRequested = true;
                }
            }, 0);
        }
        else
        {
            _editText(m.workLogMessageId, html, SendPriority::WorkLogEdit, L"wl:" + sessionId, json::Value{});
        }
    }

    void MessagingService::_postFinal(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const std::wstring& html, const std::wstring& why, bool sound, const std::wstring& markdown)
    {
        _flushWorkLog(sessionId, topic, m, true);
        // The ONE message of a turn with sound (D5 / M12) — unless the caller rings something else for it (§21.6). Claude's
        // own Markdown goes as a rich message (§21.11: tables, headings), the HTML its fallback.
        // §21.12: a reply to the turn's prompt — Telegram shows a quote of the prompt on the answer, every pair visibly you →
        // Claude (no extra call: the id is the prompt send's answer, read when this message goes).
        if (!markdown.empty())
        {
            _sendRich(topic.threadId, markdown, html, !sound, SendPriority::Final, nullptr, 0, true, m.promptRef);
        }
        else
        {
            _sendText(topic.threadId, html, !sound, SendPriority::Final, L"", json::Value{}, nullptr, 0, true, m.promptRef);
        }
        _endTurnQuietly(sessionId, topic, m);
        _log(L"[msg] " + ShortId(sessionId) + L" final message (" + why + (sound ? L"" : L", silent") + L")");
    }

    void MessagingService::_endTurnQuietly(const std::wstring& sessionId, TopicRecord& topic, Mirror& m)
    {
        _flushWorkLog(sessionId, topic, m, true); // the work log's last lines (a no-op when nothing is pending)
        m.pendingText.clear();
        m.turnOpen = false;
        m.workLog.clear();
        m.workLogMessageId = 0;
        m.workLogDirty = false;
        ++m.workLogGen; // a create still in flight belongs to the closed turn
        topic.workLogMessageId = 0;
    }

    void MessagingService::_followTranscript(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const SessionInfo& s)
    {
        if (TopicBackfillPending(topic))
        {
            // The topic's HISTORY goes first (§21.9): the mirror waits, and its cursor is where the history ends, so it goes
            // on from exactly there once the last part is sent — whatever the session did meanwhile follows, in order.
            return;
        }
        const int64_t now = WallMs();
        if (topic.transcriptPath.empty())
        {
            // A new session or fork has its topic before its transcript exists (§21.6): look often while it is busy (its
            // first prompt is being written), seldom while it rests.
            const bool busy = s.state != SessionState::Idle;
            if (now - m.lastResolveMs < (busy ? kResolveRetryBusyMs : kResolveRetryMs))
            {
                return;
            }
            m.lastResolveMs = now;
            topic.transcriptPath = ResolveClaudeTranscriptPath(sessionId);
            if (topic.transcriptPath.empty())
            {
                return; // a never-prompted session has no transcript yet
            }
        }
        int64_t size = -1;
        std::string bytes = ReadTranscriptTail(topic.transcriptPath, topic.cursor, size);
        if (size < 0)
        {
            // Gone or unreadable for a moment: forget the path and resolve again later, but KEEP the cursor — reset to 0
            // it would replay the whole conversation into the topic when the same file is found again (a file that
            // came back smaller is mirrored from its new end, below).
            topic.transcriptPath.clear();
            return;
        }
        if (size < topic.cursor)
        {
            topic.cursor = size; // the file shrank (replaced): mirror from its new end
            return;
        }
        if (bytes.empty())
        {
            return;
        }
        // Complete lines only; a torn last line is read again next tick.
        const size_t lastNl = bytes.rfind('\n');
        if (lastNl == std::string::npos)
        {
            return;
        }
        bytes.resize(lastNl + 1);
        topic.cursor += static_cast<int64_t>(bytes.size());
        std::vector<TopicItem> items;
        try
        {
            const auto lines = ParseTranscriptJsonl(std::move(bytes));
            for (const auto& line : lines)
            {
                auto its = ClassifyTranscriptLine(line);
                for (auto& it : its)
                {
                    items.push_back(std::move(it));
                }
            }
        }
        catch (...)
        {
            LogSwallowedException(L"MessagingService::_followTranscript (parse)");
            return;
        }
        if (!items.empty())
        {
            // §21.32.4: Claude's links to files on this PC become download links before anything renders them (a no-op with
            // file links off; the tick thread, no lock held, so the file checks never block another thread).
            _filesRewriteItems(sessionId, items);
            _applyItems(sessionId, topic, m, items, s);
        }
    }

    void MessagingService::_applyItems(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const std::vector<TopicItem>& items, const SessionInfo& s)
    {
        // The recap facts first (§21.6): Claude Code's away_summary, and the newest conversation message that makes an
        // older one stale. The card reads them at its next refresh; a Recap item is nothing more in the thread.
        FoldRecapFacts(items, topic.recapText, topic.recapAtMs, topic.lastMessageAtMs);
        std::optional<SessionInfo> fresher; // a newer snapshot, fetched at most once, for a prompt the queue copy lacks
        // §21.11: VERBOSE shows the whole session (the remarks, a work log of the tool calls, compactions); the conversation
        // only (the default) shows the user's posts and each turn's last message — a remark before a tool call is dropped,
        // so what is left when the turn ends is its final reply. Questions, plans, interrupts and API errors show in both.
        const bool verbose = _topicVerbose(topic);
        for (const auto& it : items)
        {
            switch (it.kind)
            {
            case TopicItemKind::Prompt:
            {
                // A new turn: whatever the previous one left is closed first.
                if (!it.midTurn)
                {
                    if (!m.pendingText.empty())
                    {
                        const std::wstring text = m.pendingText; // a copy: _postFinal clears the mirror's text
                        _postFinal(sessionId, topic, m, TgMarkdownToHtml(text), L"a new prompt closed the turn", true, text);
                    }
                    else
                    {
                        _flushWorkLog(sessionId, topic, m, true);
                        m.workLog.clear();
                        m.workLogMessageId = 0;
                        topic.workLogMessageId = 0;
                        m.workLogDirty = false;
                        ++m.workLogGen;
                    }
                    m.toolNames.clear();
                    m.turnOpen = true;
                    m.awaitingAnswer = false; // a new turn: its own question / permission prompt rings again
                    m.approvalPinged = false;
                }
                // Its source: a prompt sent from this topic is the user's own message (not mirrored); one
                // the runner sent reads "⚙ runner"; one typed at the PC reads "typed at the PC" (§21.12).
                const std::wstring key = NormalizePromptTextForCompare(it.text);
                bool fromTopic = false;
                bool fromQueue = false;
                int64_t topicMessageId = 0; // a prompt sent from this topic: the user's own message (QueuedPrompt::remoteSource)
                // §21.44: the matched entry is a topic message that WAITED in the queue (PromptWaitedInQueue), whether the queue
                // had sent it when it was read, its queue id (the reaction sync's key) and when it went into the queue.
                bool waitedInQueue = false;
                bool matchedSent = false;
                std::wstring matchedPromptId;
                int64_t queuedBusyMs = 0;
                const auto matchQueue = [&](const SessionInfo& from) {
                    size_t looked = 0;
                    for (auto q = from.queue.rbegin(); q != from.queue.rend() && looked < 60; ++q, ++looked)
                    {
                        if (q->status != PromptStatus::Sent && q->status != PromptStatus::Pending)
                        {
                            continue;
                        }
                        if (NormalizePromptTextForCompare(q->text) == key)
                        {
                            fromTopic = !q->remoteSource.empty();
                            fromQueue = q->remoteSource.empty() && q->origin == PromptOrigin::Autorun;
                            if (fromTopic && q->remoteSource.rfind(L"telegram:", 0) == 0)
                            {
                                topicMessageId = static_cast<int64_t>(std::wcstoll(q->remoteSource.c_str() + 9, nullptr, 10));
                            }
                            waitedInQueue = PromptWaitedInQueue(*q);
                            matchedSent = q->status == PromptStatus::Sent;
                            matchedPromptId = q->id;
                            queuedBusyMs = q->remoteQueuedBusyUnixMs;
                            return true;
                        }
                    }
                    return false;
                };
                if (!matchQueue(s))
                {
                    // The tick's snapshot predates this line: the registry may have recorded the prompt since (a typed
                    // row, or a remote prompt the snapshot missed). One fresh read decides — a phone message must never
                    // come back as a ⌨ line.
                    if (!fresher)
                    {
                        fresher = _session(sessionId);
                    }
                    if (fresher)
                    {
                        matchQueue(*fresher);
                    }
                }
                else if (waitedInQueue && !matchedSent)
                {
                    // §21.44: a message that waited, still Pending in the tick's snapshot: the queue may have sent it after the
                    // snapshot was taken (mark-Sent comes before the words reach the transcript, Rule #4, so any read made after
                    // this line was read shows it Sent). One fresh read tells "the queue sent it now" from the same words typed at
                    // the PC while it still waits (then it stays Pending, and it gets its own line when it goes).
                    if (!fresher)
                    {
                        fresher = _session(sessionId);
                    }
                    if (fresher)
                    {
                        matchQueue(*fresher);
                    }
                }
                // §21.44: the queue sent a topic message that WAITED (a turn or a delivery was in flight when it came, or an earlier
                // message was ahead of it: RemotePromptWait): the user's own message sits above what kept it waiting, so the bot posts
                // the user's line HERE, where its turn starts — "📱 from the phone · 📤 sent from the queue" — and that line, not the
                // message, is the turn's prompt (its status, the reply's quote, the next turn's).
                const bool wentFromQueue = fromTopic && waitedInQueue && matchedSent;
                // §21.17: the PREVIOUS turn's prompt, which this turn's line replies to (the user's "should be a reply to previous
                // user message. this way we can jump between user turns"): the live mirror's last prompt — its id may still be in
                // flight, the shared PromptRef resolves it when this line goes —, else the record's (after a restart), else the
                // history replay's last line once the replay is done (before that, the line it would quote is not posted yet).
                std::shared_ptr<PromptRef> previousTurn;
                if (!it.midTurn)
                {
                    if (m.promptRef)
                    {
                        previousTurn = m.promptRef;
                    }
                    else if (topic.promptMessageId > 0 || (!TopicBackfillPending(topic) && topic.historyPromptMessageId > 0))
                    {
                        previousTurn = std::make_shared<PromptRef>();
                        previousTurn->id.store(topic.promptMessageId > 0 ? topic.promptMessageId : topic.historyPromptMessageId);
                    }
                }
                if (!it.midTurn)
                {
                    // §21.12: this is the turn's prompt — the LAST prompt the user sent. The previous one's status goes, this
                    // one carries the turn's status from now on, and the turn's final reply quotes it. A message typed while
                    // a turn ran is not the turn's prompt: it gets no status.
                    _showPromptStatus(topic, PromptTurnStatus::None);
                    auto ref = std::make_shared<PromptRef>();
                    topic.promptMessageId = 0;
                    topic.promptBotLine = false;
                    topic.promptTsMs = it.tsMs;
                    // §21.44: a message that waited is its own source (PhoneQueued): the status edits of its line keep "📤 sent
                    // from the queue".
                    topic.promptSource = static_cast<int>(wentFromQueue ? PromptSource::PhoneQueued
                                                                        : (fromTopic ? PromptSource::Phone : (fromQueue ? PromptSource::Runner : PromptSource::Pc)));
                    topic.promptText = it.text.size() > kUserLineMaxChars ? it.text.substr(0, kUserLineMaxChars) : it.text;
                    topic.promptStatus = static_cast<int>(PromptTurnStatus::Running);
                    topic.promptInterrupted = false;
                    if (fromTopic && !wentFromQueue && topicMessageId > 0)
                    {
                        // The user's own message from this topic: known at once; its status is a reaction (_syncReactions has
                        // just shown 👨‍💻 for the pickup, the running status). §21.44: not for a message that waited — the bot's
                        // line below is the turn's prompt, and its id comes with that line's send (lineSent).
                        ref->id.store(topicMessageId);
                        topic.promptMessageId = topicMessageId;
                    }
                    m.promptRef = ref;
                }
                if (fromTopic && !wentFromQueue)
                {
                    break;
                }
                // §21.12: "👤 You · 14:02 · typed at the PC · ⏳ running" over the prompt (a mid-turn message: no status). Sent at
                // the FINAL priority, the reply's: queued in the same tick (a fast turn), the reply must not overtake its prompt.
                // §21.44: "👤 You · 14:03 · 📱 from the phone · 📤 sent from the queue · ⏳ running" for a topic message that waited.
                const PromptSource source = wentFromQueue ? PromptSource::PhoneQueued : (fromQueue ? PromptSource::Runner : PromptSource::Pc);
                // §21.17: the session's title as the line's second row, a link to this topic (in the chat's "All" view it says
                // whose turn this is; _showPromptStatus edits the line with the same row).
                const std::wstring html = BuildPromptLineHtml(it.tsMs, source, it.text, it.midTurn ? PromptTurnStatus::None : PromptTurnStatus::Running, it.midTurn,
                                                              UserLineTitleRow(topic.title, topic.sessionId, topic.nameColor, topic.threadId));
                const std::shared_ptr<PromptRef> ref = it.midTurn ? nullptr : m.promptRef;
                // The bot's line answers with its id: the turn's reply quotes it, and its status is an edit of it.
                const auto lineSent = [ref](const TgResult& r) {
                    if (ref && r.ok)
                    {
                        if (const int64_t id = r.result.I64At(L"message_id", 0); id > 0)
                        {
                            ref->botLine.store(true);
                            ref->id.store(id);
                        }
                    }
                };
                int businessMode = 0;
                {
                    std::lock_guard lk{ _mtx };
                    businessMode = _state.businessMode;
                }
                if (wentFromQueue)
                {
                    // §21.44: the user's own message was sent — 🫡, its last mark: the turn's status shows on the line queued just
                    // below. Recorded as Sent, so the reaction sync neither shows 👨‍💻 for it (that reads "running" on a message whose
                    // turn shows its status elsewhere) nor takes it back to ✍ while the echo that sets `echoed` is still on its way.
                    _react(topicMessageId, kSentFromQueueReaction);
                    {
                        std::lock_guard lk{ _mtx };
                        _reacted[matchedPromptId] = PromptStatus::Sent;
                    }
                    const int64_t wentAtMs = it.tsMs > 0 ? it.tsMs : WallMs();
                    _log(L"[msg] " + ShortId(sessionId) + L" a topic message that waited in the queue went (message " + std::to_wstring(topicMessageId) + L", " +
                         TgShortSpan(wentAtMs - queuedBusyMs) + L" in the queue" + (it.midTurn ? L", mid-turn" : L"") + L"): \"sent from the queue\" line posted");
                }
                if (businessMode == 1 && !wentFromQueue)
                {
                    // SECRETARY MODE (§21.5): the prompt is the user's own bubble in the topic, sent on the user's
                    // behalf through the Telegram Business connection; a refused send falls back to the bot's line
                    // above, and three refusals in a row turn the mode off (the user may have revoked the right).
                    // Its id in THIS bot's view comes with the echo (_notePromptEcho, matched by the text's hash).
                    if (ref)
                    {
                        ref->echoHash.store(EchoHash(it.text));
                    }
                    const int64_t thread = topic.threadId;
                    const std::wstring fallback = html;
                    const std::wstring plain = it.text;
                    auto self = weak_from_this();
                    _sendAsUser(
                        thread,
                        plain,
                        [self, thread, fallback, lineSent, previousTurn](const TgResult& r) {
                            auto me = self.lock();
                            if (!me)
                            {
                                return;
                            }
                            if (r.ok)
                            {
                                me->_secretaryFailures.store(0);
                                return;
                            }
                            const int failures = me->_secretaryFailures.fetch_add(1) + 1;
                            me->_log(L"[msg] secretary: a send as the user was refused (" + r.description + L"); the prompt goes out as the bot's line" + (failures >= 3 ? L"; secretary mode is off until the bot is connected again" : L""));
                            if (failures >= 3)
                            {
                                std::lock_guard lk{ me->_mtx };
                                me->_state.businessMode = -1;
                                me->_state.businessNote = r.description;
                                me->_stateDirty = true;
                            }
                            // §21.17: the bot's line in the user's place replies to the previous turn's prompt like any other.
                            me->_sendText(thread, fallback, true, SendPriority::Final, L"", json::Value{}, lineSent, 0, true, previousTurn);
                        },
                        SendPriority::Final);
                }
                else
                {
                    // §21.17: a turn's line quotes the previous turn's prompt (a message typed mid-turn has no previousTurn).
                    // §21.44: so does a "sent from the queue" line, in every mode — even with the secretary on it is the bot's line,
                    // since its title row is what says the message went from the queue (a send as the user carries only the words).
                    _sendText(topic.threadId, html, true, SendPriority::Final, L"", json::Value{}, lineSent, 0, true, previousTurn);
                }
                break;
            }
            case TopicItemKind::Text:
                if (it.turnEnds)
                {
                    std::wstring text = m.pendingText;
                    if (!text.empty())
                    {
                        text += L"\n\n";
                    }
                    text += it.text;
                    _postFinal(sessionId, topic, m, TgMarkdownToHtml(text), L"end of turn", true, text);
                }
                else
                {
                    if (!m.pendingText.empty())
                    {
                        m.pendingText += L"\n\n";
                    }
                    m.pendingText += it.text;
                    m.turnOpen = true;
                }
                break;
            case TopicItemKind::ToolCall:
                m.toolNames[it.toolUseId] = it.toolName;
                if (verbose)
                {
                    // What the assistant said before the tool call — "why the bash is invoked" — is a message of
                    // its own (§21.5), not a line in the work log.
                    _postRemark(sessionId, topic, m);
                    _appendWorkLog(m, L"\x2022 " + it.summary);
                }
                else
                {
                    m.pendingText.clear(); // the conversation only: a remark is not the turn's last message
                    m.turnOpen = true;
                }
                break;
            case TopicItemKind::ToolResult:
                // §21.30: the result of a question card's dialog without answers — an error (Chat about this, Esc at the PC), none
                // given, or Claude Code's own idle timeout — closes the card (a no-op for every other tool's result).
                _onTranscriptAnswer(sessionId, it.toolUseId, std::wstring{},
                                    it.isError ? QuestionResult::Rejected : (it.afkTimeoutMs > 0 ? QuestionResult::TimedOut : QuestionResult::Answered), it.afkTimeoutMs);
                if (it.isError && verbose)
                {
                    std::wstring name;
                    if (const auto n = m.toolNames.find(it.toolUseId); n != m.toolNames.end())
                    {
                        name = n->second;
                    }
                    _appendWorkLog(m, L"\x2717 " + (name.empty() ? std::wstring{} : name + L": ") + it.text);
                }
                break;
            case TopicItemKind::Question:
            {
                if (verbose)
                {
                    _postRemark(sessionId, topic, m);
                }
                else
                {
                    m.pendingText.clear();
                }
                std::wstring html = L"\x2753 <b>" + TgEscapeHtml(it.text) + L"</b>";
                if (!it.options.empty())
                {
                    html += L"\n";
                    for (size_t i = 0; i < it.options.size(); ++i)
                    {
                        html += L"\n" + std::to_wstring(i + 1) + L". " + TgEscapeHtml(it.options[i]);
                    }
                }
                // ONE sound per "needs you": silent when the NeedsApproval edge already rang the card (§21.6).
                const bool sound = !m.approvalPinged;
                m.awaitingAnswer = true;
                // §21.30: the question is ANSWERED FROM HERE when its question hook reaches Agentmaster: the QUESTION CARD shows it
                // (_questionsTick), so this item only closes the turn's work log. No hook within kAskPairWaitMs (a claude that started
                // before the bot was on, or one older than 2.1.139) leaves the text above, posted show-only with the reason.
                if (_noteTranscriptQuestion(sessionId, it,
                                            html + L"\n\n<i>Answer it at the PC \x2014 this session's Claude Code did not offer it to Telegram (it started before the bot was on, "
                                                   L"or it is older than 2.1.139; \U0001F504 Restart Session gives it the question hook).</i>",
                                            sound, m.promptRef))
                {
                    _endTurnQuietly(sessionId, topic, m);
                    _log(L"[msg] " + ShortId(sessionId) + L" a question: shown by its question card (show-only if no question hook offers it within " +
                         std::to_wstring(kAskPairWaitMs / 1000) + L" s)");
                }
                else
                {
                    _postFinal(sessionId, topic, m, html + L"\n\n<i>Answer it at the PC.</i>", L"a question", sound);
                }
                break;
            }
            case TopicItemKind::Plan:
            {
                std::wstring html = L"\U0001F4CB <b>Plan ready for approval</b> \x2014 approve or reject it at the PC.";
                // The plan is Claude's Markdown (headings, lists, often a table): a rich message (§21.11).
                std::wstring markdown = L"\U0001F4CB **Plan ready for approval** \x2014 approve or reject it at the PC.";
                if (!it.text.empty())
                {
                    html += L"\n" + TgMarkdownToHtml(it.text.size() > 3500 ? it.text.substr(0, 3500) + L"\x2026" : it.text);
                    markdown += L"\n\n" + (it.text.size() > 20000 ? it.text.substr(0, 20000) + L"\x2026" : it.text);
                }
                const bool sound = !m.approvalPinged;
                m.awaitingAnswer = true;
                _postFinal(sessionId, topic, m, html, L"a plan", sound, markdown);
                break;
            }
            case TopicItemKind::Compaction:
                if (verbose)
                {
                    _appendWorkLog(m, L"\x2014 " + it.text + L" \x2014");
                }
                break;
            case TopicItemKind::Interrupt:
                // The user stopped the turn (Esc at the PC, or /interrupt here): what the assistant had said so far,
                // then the mark — silent, the user did it. A stray marker with no turn shown posts nothing.
                topic.promptInterrupted = true; // §21.12: the last prompt reads "■ interrupted" once the session is at rest
                if (m.turnOpen || !m.pendingText.empty())
                {
                    const std::wstring text = m.pendingText; // a copy: _postFinal clears the mirror's text
                    _postFinal(sessionId, topic, m, (text.empty() ? std::wstring{} : TgMarkdownToHtml(text) + L"\n\n") + L"\x25A0 <i>interrupted</i>", L"interrupted", false,
                               (text.empty() ? std::wstring{} : text + L"\n\n") + L"\x25A0 *interrupted*");
                }
                break;
            case TopicItemKind::ApiError:
            {
                // The turn died: an assistant line of the conversation (Claude Code writes it). It rings unless the
                // Error edge already rang the card; either way the edge that follows rings nothing more.
                const bool sound = !topic.errorAnnounced;
                topic.errorAnnounced = true;
                _postFinal(sessionId, topic, m, L"\x26A0 <b>API error</b>\n" + TgEscapeHtml(it.text.size() > 2000 ? it.text.substr(0, 2000) : it.text), L"an API error", sound);
                break;
            }
            case TopicItemKind::Command:
                // §21.12: "👤 You · 14:02 · command"; §21.17: the session's title row under it.
                _sendText(topic.threadId, BuildCommandLineHtml(it.tsMs, it.text, UserLineTitleRow(topic.title, topic.sessionId, topic.nameColor, topic.threadId)), true,
                          SendPriority::New, L"", json::Value{}, nullptr);
                break;
            case TopicItemKind::Answer:
            {
                // The user's answer to the question above (§21.9: the user's turns are the user's messages): the work log so
                // far is closed first, so the answer lands under the question and the tools that follow start below it.
                // As the user through secretary mode when it works, else the bot's ↩ line; silent (the user gave it).
                _flushWorkLog(sessionId, topic, m, true);
                m.workLog.clear();
                m.workLogMessageId = 0;
                topic.workLogMessageId = 0;
                m.workLogDirty = false;
                ++m.workLogGen;
                m.awaitingAnswer = false;
                // §21.30: a question card shows its dialog's answer itself ("Answered from Telegram", or "Answered at the PC" with the
                // answer): no "You ↩ answer" line under it. A show-only question still waiting for its hook is posted first, above it.
                if (_onTranscriptAnswer(sessionId, it.toolUseId, it.text, it.afkTimeoutMs > 0 ? QuestionResult::TimedOut : QuestionResult::Answered, it.afkTimeoutMs))
                {
                    _log(L"[msg] " + ShortId(sessionId) + L" a question's answer: shown on its question card");
                    break;
                }
                // §21.12: "👤 You · 14:02 · ↩ answer" over the answer; §21.17: the session's title row under the header.
                const std::wstring html = BuildAnswerLineHtml(it.tsMs, it.text, UserLineTitleRow(topic.title, topic.sessionId, topic.nameColor, topic.threadId));
                bool asUser = false;
                {
                    std::lock_guard lk{ _mtx };
                    asUser = _state.businessMode == 1;
                }
                if (asUser)
                {
                    const int64_t thread = topic.threadId;
                    auto self = weak_from_this();
                    _sendAsUser(thread, it.text, [self, thread, html](const TgResult& r) {
                        auto me = self.lock();
                        if (!me || r.ok)
                        {
                            if (me)
                            {
                                me->_secretaryFailures.store(0);
                            }
                            return;
                        }
                        const int failures = me->_secretaryFailures.fetch_add(1) + 1;
                        me->_log(L"[msg] secretary: an answer could not be sent as the user (" + r.description + L"); the bot's line goes in its place" +
                                 (failures >= 3 ? L"; secretary mode is off until the bot is connected again" : L""));
                        if (failures >= 3)
                        {
                            std::lock_guard lk{ me->_mtx };
                            me->_state.businessMode = -1;
                            me->_state.businessNote = r.description;
                            me->_stateDirty = true;
                        }
                        me->_sendText(thread, html, true, SendPriority::New, L"", json::Value{}, nullptr);
                    });
                }
                else
                {
                    _sendText(topic.threadId, html, true, SendPriority::New, L"", json::Value{}, nullptr);
                }
                break;
            }
            default:
                break;
            }
        }
    }

    void MessagingService::_onStateEdge(const std::wstring& sessionId, TopicRecord& topic, Mirror& m, const SessionInfo& s)
    {
        const int64_t now = WallMs();
        if (m.lastStateMs == 0)
        {
            // The first look at this session in this run: no edge to report.
            m.lastState = s.state;
            m.lastStateMs = now;
            return;
        }
        if (s.state == m.lastState)
        {
            return;
        }
        const SessionState from = m.lastState;
        m.lastState = s.state;
        m.lastStateMs = now;
        _log(L"[msg] " + ShortId(sessionId) + L" " + StateWord(from) + L" -> " + StateWord(s.state));
        if (s.state == SessionState::Running)
        {
            topic.errorAnnounced = false; // a new turn: a later Error is news again
            m.awaitingAnswer = false; // answered / approved: a later question or permission prompt rings again
            m.approvalPinged = false;
        }
        // What the transcript did not say: a turn that ended without a terminal line (a recon synth), an exit, an
        // error the tail did not carry, a permission prompt (no AskUserQuestion line). The thread is the conversation
        // (§21.6): a state is said by the CARD — re-sent at the bottom WITH SOUND (a ping) for the states the user
        // must know about — never by a bot message of its own.
        const bool atRest = s.state == SessionState::WaitingForInput || s.state == SessionState::Idle;
        bool ping = false;
        if (from == SessionState::Running && atRest && !m.pendingText.empty())
        {
            // The turn's last words that no terminal line marked as final: the turn's message with sound.
            const std::wstring text = m.pendingText;
            _postFinal(sessionId, topic, m, TgMarkdownToHtml(text), L"the state left Running", true, text);
        }
        else if (s.state == SessionState::Done && from != SessionState::Done)
        {
            // claude exited: what it said last closes the turn (silent), the card says it exited and rings.
            if (!m.pendingText.empty())
            {
                const std::wstring text = m.pendingText;
                _postFinal(sessionId, topic, m, TgMarkdownToHtml(text), L"exited", false, text);
            }
            else
            {
                _endTurnQuietly(sessionId, topic, m);
            }
            ping = true;
        }
        else if (s.state == SessionState::Error && from != SessionState::Error)
        {
            // An API error the mirror already posted set errorAnnounced; otherwise (a cut-off turn, a scanner-derived
            // error) the card rings — ONCE per episode: a session at rest in Error reads Idle at every app start until
            // the scanner re-asserts Error, and the topic was told the first time.
            if (!topic.errorAnnounced && (m.turnOpen || !m.pendingText.empty() || !s.errorMessage.empty()))
            {
                topic.errorAnnounced = true;
                if (!m.pendingText.empty())
                {
                    const std::wstring text = m.pendingText;
                    _postFinal(sessionId, topic, m, TgMarkdownToHtml(text), L"error state", false, text);
                }
                else
                {
                    _endTurnQuietly(sessionId, topic, m);
                }
                ping = true;
            }
        }
        else if (s.state == SessionState::NeedsApproval && from == SessionState::Running && !m.awaitingAnswer)
        {
            // A permission prompt — or a question whose line the mirror has not read yet: the card rings now, and the
            // question posted after it goes out silent (approvalPinged).
            m.approvalPinged = true;
            ping = true;
        }
        _refreshCard(sessionId, true, ping);
    }

    void MessagingService::_syncReactions(const SessionInfo& s)
    {
        // A remote prompt's status as a reaction on the user's own Telegram message: ✍ queued (set when it
        // was queued), 👨‍💻 sent and picked up, 👎 failed. §21.44: 🫡 instead of 👨‍💻 for a message that waited in the queue
        // (the mirror sets it with its "sent from the queue" line; this sync only as the fallback).
        for (const auto& q : s.queue)
        {
            if (q.remoteSource.rfind(L"telegram:", 0) != 0)
            {
                continue;
            }
            PromptStatus shown = PromptStatus::Pending;
            bool known = false;
            {
                std::lock_guard lk{ _mtx };
                if (const auto it = _reacted.find(q.id); it != _reacted.end())
                {
                    shown = it->second;
                    known = true;
                }
            }
            PromptStatus want = q.status;
            if (q.status == PromptStatus::Sent && !q.echoed)
            {
                want = PromptStatus::Pending; // sent but not yet picked up: still ✍
            }
            // §21.44: a topic message that WAITED in the queue. Its pickup is the MIRROR's to show: it posts the "📤 sent from the
            // queue" line and sets 🫡 here (recording Sent). So this sync never shows 👨‍💻 for one (its turn's status is on that
            // line), never takes a shown 🫡 back to ✍ (the line can come before the echo that sets `echoed`), and sets 🫡 itself
            // only when the line never came (SentFromQueueReactionOverdue: two minutes after the send). A failure still goes to
            // the 👎 path below; a Pending one keeps ✍ through the plain path.
            if (PromptWaitedInQueue(q) && want != PromptStatus::Failed && (want == PromptStatus::Sent || (known && shown == PromptStatus::Sent)))
            {
                if (!known)
                {
                    // Picked up before this run saw it queued (a restart): recorded, not reacted — the rule below, so a restart
                    // never re-reacts on every old message. A line this run's mirror posts sets its 🫡 all the same.
                    std::lock_guard lk{ _mtx };
                    _reacted[q.id] = PromptStatus::Sent;
                }
                else if (shown != PromptStatus::Sent && SentFromQueueReactionOverdue(q, WallMs()))
                {
                    // The message's id as the mirror reads it (PromptWaitedInQueue guarantees the "telegram:" prefix): wcstoll
                    // throws nothing, and garbage reads as 0, which _react ignores — the record is still marked, so a garbled
                    // one is looked at once, not every tick.
                    const int64_t overdueId = static_cast<int64_t>(std::wcstoll(q.remoteSource.c_str() + 9, nullptr, 10));
                    _react(overdueId, kSentFromQueueReaction);
                    {
                        std::lock_guard lk{ _mtx };
                        _reacted[q.id] = PromptStatus::Sent;
                    }
                    _log(L"[msg] " + ShortId(s.id) + L" a topic message that waited in the queue was sent " + TgShortSpan(WallMs() - q.sentAtUnixMs) +
                         L" ago without its \"sent from the queue\" line (message " + std::to_wstring(overdueId) + L"): marked \U0001FAE1 by the reaction sync");
                }
                continue;
            }
            if (want == shown)
            {
                continue;
            }
            if (!known && want == PromptStatus::Sent)
            {
                // Picked up before this run saw it queued (a restart: _reacted is per run). Its reaction is the turn's status
                // now (§21.12: 👌 answered, none once a newer prompt was sent) — re-setting 👨‍💻 on every old prompt of every
                // queue would undo those, and cost a call each. Recorded, not reacted.
                std::lock_guard lk{ _mtx };
                _reacted[q.id] = want;
                continue;
            }
            int64_t msgId = 0;
            try
            {
                msgId = std::stoll(q.remoteSource.substr(9));
            }
            catch (...)
            {
                continue;
            }
            const wchar_t* emoji = want == PromptStatus::Failed ? L"\U0001F44E" : (want == PromptStatus::Sent ? L"\U0001F468‍\U0001F4BB" : L"✍");
            _react(msgId, emoji);
            {
                std::lock_guard lk{ _mtx };
                _reacted[q.id] = want;
            }
            if (want == PromptStatus::Failed)
            {
                int64_t thread = 0;
                {
                    std::lock_guard lk{ _mtx };
                    if (const auto it = _state.topics.find(s.id); it != _state.topics.end())
                    {
                        thread = it->second.threadId;
                    }
                }
                _sendText(thread, L"\U0001F44E <b>not delivered</b> \x2014 the prompt never became a message (lost send or a refused delivery); the runner paused. Look at the terminal, then /sendnow or send it again.", true, SendPriority::Final, L"", json::Value{}, nullptr, msgId);
            }
        }
    }
}
