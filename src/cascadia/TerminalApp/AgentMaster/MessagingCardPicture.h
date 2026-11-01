// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the sticky card's SCREENSHOT (MESSAGING.md §21.42). The user's request (2026-10-27): "the running since
// message, called also i think status message that is pinned to the bottom - whenever it updates, have it produce a
// screenshot (if possible) and have it shown and so every update we produce a screenshot to show in the status message".
//
// The card stays what it is - a RICH message (its title a chip, its buttons below) - and carries the session's terminal as
// its last block: <figure><img src="tg://photo?id=screen"/><figcaption>🖥 the screen at 19:50:12</figcaption></figure>, the
// photo given in the rich message's `media` list (Bot API 10.2's InputRichMessageMedia). Measured on the dev bot
// (2026-10-27, scratchpad card_picture_probe.py, every message deleted after): sendRichMessage with an uploaded photo kept
// the paragraph, the photo, its caption and the keyboard (441 ms for 4 KB); editMessageText with a NEW upload replaced the
// photo in place, keyboard kept (1,291 ms for a 4 MB PNG); editMessageText reusing the photo's file_id took 274 ms with no
// upload; a text-only rich edit dropped the photo and a later edit by file_id put it back; an identical edit answered
// "message is not modified"; a re-post by file_id needed no upload either.
//
// So a card UPDATE (its text changed: a status, the since-clock, the context size of a running turn, the runner, the
// draft) reads the hosting window's screen (the 🖼 Screen Image snapshot) in the same moment; a screen that changed since the
// card's last picture is drawn and uploaded IN THE SAME CALL as the text (no extra call on the chat's ~20-a-minute budget);
// an unchanged screen reuses the picture Telegram already holds by its file_id (no upload). A change that is not an update -
// a page flip of the buttons, the slow form upgrade - keeps the current picture without reading the screen (a page flip
// must stay instant). A screen that cannot be read (a parked tab, no window hosts the tab) keeps the last picture, captioned
// with the moment it was taken; a card that never had one goes without.
//
// The picture FOLLOWS THE STATUS (the user's report of 2026-11-01: "When status changes, we update the message but I think we
// don't update the image - we should"). One read at the moment of a status update can miss: the hosting window answered too
// late (production, 2026-11-01 12:03: the Agent Manager page's 3-4 s rebuilds held a card's read 5-9 s, past the 4 s bound, so
// the card went with its old picture and the late one was dropped), the read came before Claude Code drew the new state (a Stop
// hook's line arrives while the hook still runs, and Claude Code ends the turn only after it exits: the spinner can still
// show), or a page flip overtook the update's read or put an older picture back. A card at rest never changes its text again
// ("since 12:03" is a clock, not a span), so before this it kept that picture for good. Now the card LOOKS at its screen again
// (CardPictureTrack): a settle look after every change into a state at rest, and looks with a backoff after a read that got no
// screen in time; a look edits the card only when the screen differs from the picture its newest post or edit carried.
//
// This header is the pure half: the media id, the picture as a message carries it, the plan (upload, reuse, none, unchanged),
// how a picture step settled (CardPictureRead), the looks' bookkeeping and rules, the rich HTML block, the media list, reading
// the photo back from Telegram's answer, and the caption's clock. The m5 suite pins every rule (tests_messaging_card.cpp,
// section 1d).

#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>

#include "Json.h"

namespace Agentmaster::Msg
{
    /// <summary>
    /// The id the card's photo goes by in its rich message: the HTML's tg://photo?id=screen and the `media` entry's id (1-64
    /// characters of A-Z a-z 0-9 _ -, Telegram's rule), and - for a new upload - the multipart field the file goes in
    /// (attach://screen).
    /// </summary>
    inline constexpr std::wstring_view kCardPictureMediaId = L"screen";

    /// <summary>The file name a new card picture is uploaded under (Telegram keeps a photo's pixels, not its name).</summary>
    inline constexpr std::wstring_view kCardPictureFileName = L"screen.png";

    /// <summary>
    /// The picture one card post or edit carries: either a NEW one to upload (`png`) or one Telegram already holds (`fileId`,
    /// reused with no upload), plus what it shows - the moment its screen was read and that screen's hash. Shared by the
    /// outbox items that carry it (a refused send goes again without it), never changed once made.
    /// </summary>
    struct CardPicture
    {
        std::string png; // the PNG to upload ("" = reuse fileId); a picture is ~0.1-0.6 MB (§21.24's measures)
        std::wstring fileId; // Telegram's file_id of a photo it holds; used only when png is empty
        int64_t shotAtMs{ 0 }; // when the screen this picture shows was read (Unix ms): the caption's clock
        uint32_t shotHash{ 0 }; // CardPictureShotHash of that screen: an unchanged screen reuses the picture
    };

    /// <summary>
    /// The identity of a screen snapshot (ScreenSnapshot.h's serialized form, as the window answers it): FNV-1a 32 over its
    /// UTF-16 code units, never 0 (0 = "no picture recorded"). A collision only shows the previous picture once more: the next
    /// change of the screen draws a new one.
    /// </summary>
    /// <param name="snapshot">The serialized snapshot.</param>
    /// <returns>The hash, 1..2^32-1.</returns>
    inline uint32_t CardPictureShotHash(std::wstring_view snapshot) noexcept
    {
        uint32_t h = 2166136261u;
        for (const wchar_t c : snapshot)
        {
            h ^= static_cast<uint32_t>(c);
            h *= 16777619u;
        }
        return h == 0 ? 1u : h;
    }

    /// <summary>What a card post or edit does about its picture (PlanCardPicture).</summary>
    enum class CardPictureSource
    {
        None, // no picture: the card goes as text (no screen could be read and the card never had a picture)
        Reuse, // the picture Telegram already holds, by its file_id (no upload)
        Upload, // a new picture: the screen is drawn and uploaded in the same call
        Unchanged, // a LOOK only: the screen read now is the one the card's newest post or edit carried - nothing to draw or send
    };

    /// <summary>
    /// How a card's picture step settled - what the step's send learns besides the picture itself (MessagingService::_withCardPicture
    /// hands both over). Only Stale owes the card a look (CardPictureStepSettled), so "the window did not answer in time" is never
    /// mistaken for "there is no screen to read", which would retry a parked tab forever.
    /// </summary>
    enum class CardPictureRead
    {
        Fresh, // the hosting window answered in time and the screen was read for this step: the picture (drawn, or the held
               // photo of that very screen) shows this moment
        NoScreen, // the hosting window answered in time, but its tab shows no screen (a parked tab, a control not up): nothing
                  // newer can be had, so the card keeps its last picture with that picture's own moment, or goes without
        Unchanged, // a LOOK only: the screen read now is the one the card's newest post or edit carried - nothing to send
        Kept, // no read was asked for (a page flip of the buttons, the slow form upgrade): the card keeps the picture it shows
        Stale, // no screen in time: the window did not answer within the step's wait (a busy or frozen UI thread), no window
               // answered, or the read or the drawing failed - the card goes with its held picture at best and looks again later
    };

    /// <summary>The decision for one card post or edit: where its picture comes from, and the moment its caption names.</summary>
    struct CardPicturePlan
    {
        CardPictureSource source{ CardPictureSource::None };
        int64_t shotAtMs{ 0 }; // the caption's moment: now for a screen read now (drawn, or found unchanged), else the stored picture's
    };

    /// <summary>
    /// Plans a card's picture from what was read and what the card shows: a screen read NOW that differs from the stored
    /// picture's (or with no stored picture) is drawn and uploaded; a screen read now that is the stored picture's own reuses
    /// it, captioned with NOW (the screen was looked at and is still that); no screen (a parked tab, no window, a read that
    /// failed) keeps the stored picture with its OWN moment ("the screen at 19:50" stays true), or none at all. A LOOK passes
    /// the screen hash of the picture the card's newest post or edit carried: a screen read now that is that one is Unchanged,
    /// so nothing is drawn or sent. Pure.
    /// </summary>
    /// <remarks>
    /// The look compares with what the card CARRIES (its newest post or edit, maybe still in the outbox), not with the stored
    /// picture (what Telegram last confirmed): a look that runs while a new upload is on its way would otherwise draw and upload
    /// that same screen a second time.
    /// </remarks>
    /// <param name="readable">The window answered with a screen (a non-empty snapshot).</param>
    /// <param name="shotHash">That screen's CardPictureShotHash (ignored when not readable).</param>
    /// <param name="nowMs">Now (Unix ms).</param>
    /// <param name="storedFileId">The file_id of the picture the card last showed ("" = none).</param>
    /// <param name="storedShotHash">That picture's screen hash (0 = unknown: never reused for a read screen).</param>
    /// <param name="storedAtMs">The moment that picture's screen was read.</param>
    /// <param name="unchangedShotHash">A look's basis: the screen hash of the picture the card carries (0 = no basis - every
    /// post and edit, and a look at a card that carries no picture).</param>
    /// <returns>The plan.</returns>
    inline CardPicturePlan PlanCardPicture(bool readable, uint32_t shotHash, int64_t nowMs, std::wstring_view storedFileId, uint32_t storedShotHash, int64_t storedAtMs, uint32_t unchangedShotHash = 0) noexcept
    {
        CardPicturePlan plan;
        if (readable)
        {
            if (unchangedShotHash != 0 && shotHash == unchangedShotHash)
            {
                // A look at a screen the card already carries: the caption keeps its own moment (nothing is sent to change it).
                plan.source = CardPictureSource::Unchanged;
                plan.shotAtMs = nowMs;
                return plan;
            }
            if (!storedFileId.empty() && storedShotHash != 0 && storedShotHash == shotHash)
            {
                plan.source = CardPictureSource::Reuse;
            }
            else
            {
                plan.source = CardPictureSource::Upload;
            }
            plan.shotAtMs = nowMs;
            return plan;
        }
        if (!storedFileId.empty())
        {
            plan.source = CardPictureSource::Reuse;
            plan.shotAtMs = storedAtMs;
        }
        return plan;
    }

    /// <summary>
    /// A moment as a LOCAL clock time with seconds, for the picture's caption: "19:50:12" on the same local day as `nowMs`,
    /// else "16 Oct 19:50:12". With seconds because a running card takes a new picture every ~20 s (its edit gap): two
    /// pictures of one minute must not read alike. The caption is outside the card's hash (MessagingService::_refreshCard
    /// hashes the classic text and the buttons), so a ticking clock here never causes an edit by itself.
    /// </summary>
    /// <param name="unixMs">The moment (Unix ms).</param>
    /// <param name="nowMs">The clock the "same day" test is made against.</param>
    /// <returns>"HH:MM:SS" or "D Mon HH:MM:SS"; "?" when the CRT cannot convert the moment.</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring CardPictureClockText(int64_t unixMs, int64_t nowMs)
    {
        const auto toLocal = [](int64_t ms, std::tm& out) noexcept -> bool {
            const time_t t = static_cast<time_t>(ms / 1000);
            return localtime_s(&out, &t) == 0; // the CRT's thread-safe form; it writes nothing on failure
        };
        std::tm when{};
        std::tm now{};
        if (unixMs <= 0 || !toLocal(unixMs, when))
        {
            return L"?";
        }
        const auto two = [](int v) { return (v < 10 ? L"0" : L"") + std::to_wstring(v); };
        std::wstring clock = two(when.tm_hour) + L":" + two(when.tm_min) + L":" + two(when.tm_sec);
        if (toLocal(nowMs, now) && now.tm_year == when.tm_year && now.tm_yday == when.tm_yday)
        {
            return clock;
        }
        static constexpr const wchar_t* kMonths[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
        const int mon = when.tm_mon >= 0 && when.tm_mon < 12 ? when.tm_mon : 0;
        return std::to_wstring(when.tm_mday) + L" " + kMonths[mon] + L" " + clock;
    }

    /// <summary>
    /// The picture's block in the card's rich HTML: a figure of the photo (tg://photo?id=screen, given in the `media` list) and
    /// its caption "🖥 the screen at 19:50:12". It goes LAST, after the card's text and before its buttons: the card leads with
    /// its state (§21.26), and the screen shows what that state looks like. Rich HTML takes media only as a block of its own
    /// (Telegram's formatting rules), which a figure is.
    /// </summary>
    /// <param name="shotAtMs">The moment the screen was read (CardPicturePlan::shotAtMs).</param>
    /// <param name="nowMs">Now (the caption's "same day" test).</param>
    /// <returns>The HTML block (plain text in it: nothing to escape).</returns>
    /// <exception cref="std::bad_alloc">If the HTML cannot be allocated.</exception>
    inline std::wstring CardPictureFigureHtml(int64_t shotAtMs, int64_t nowMs)
    {
        std::wstring html = L"<figure><img src=\"tg://photo?id=";
        html += kCardPictureMediaId;
        html += L"\"/><figcaption>\U0001F5A5 the screen at ";
        html += CardPictureClockText(shotAtMs, nowMs);
        html += L"</figcaption></figure>";
        return html;
    }

    /// <summary>
    /// The rich message's `media` list for a card picture: one InputRichMessageMedia whose id is kCardPictureMediaId and whose
    /// media is a photo - "attach://screen" for a new upload (the file then goes in the multipart field "screen"), else the
    /// held photo's file_id.
    /// </summary>
    /// <param name="picture">The picture (png set = an upload; else its fileId).</param>
    /// <returns>The JSON array.</returns>
    /// <exception cref="std::bad_alloc">If the JSON cannot be allocated.</exception>
    inline json::Value CardPictureMediaJson(const CardPicture& picture)
    {
        auto photo = json::Value::MkObj();
        photo.Set(L"type", json::Value::MkStr(L"photo"));
        photo.Set(L"media", json::Value::MkStr(picture.png.empty() ? picture.fileId : L"attach://" + std::wstring{ kCardPictureMediaId }));
        auto entry = json::Value::MkObj();
        entry.Set(L"id", json::Value::MkStr(std::wstring{ kCardPictureMediaId }));
        entry.Set(L"media", std::move(photo));
        auto list = json::Value::MkArr();
        list.Push(std::move(entry));
        return list;
    }

    /// <summary>The photo a sent or edited card shows, read back from Telegram's answer (CardPictureFromMessage).</summary>
    struct CardPictureShown
    {
        std::wstring fileId; // the largest size's file_id: what the next post or edit reuses ("" = the message shows no photo)
        std::wstring uniqueId; // its file_unique_id (the same picture across sizes and messages; for the log)
        int width{ 0 }; // the largest size's width in pixels
        int height{ 0 }; // and height
    };

    /// <summary>
    /// The photo a rich message shows, from the Message Telegram returned for a post or an edit: its rich_message's first photo
    /// block, the LARGEST of the sizes Telegram made of it (reusing that size's file_id keeps the full resolution).
    /// </summary>
    /// <param name="message">The returned Message (TgResult::result); anything else reads as no photo.</param>
    /// <returns>The photo, or an empty fileId when the message shows none (a text-only card, a refused picture, a classic form).</returns>
    /// <exception cref="std::bad_alloc">If the ids cannot be copied.</exception>
    inline CardPictureShown CardPictureFromMessage(const json::Value& message)
    {
        CardPictureShown shown;
        if (message.type != json::Value::Type::Obj)
        {
            return shown;
        }
        const auto* rich = message.Find(L"rich_message");
        const auto* blocks = rich && rich->type == json::Value::Type::Obj ? rich->Find(L"blocks") : nullptr;
        if (!blocks || blocks->type != json::Value::Type::Arr)
        {
            return shown;
        }
        for (const auto& block : blocks->arr)
        {
            if (block.type != json::Value::Type::Obj || block.StrAt(L"type") != L"photo")
            {
                continue;
            }
            const auto* sizes = block.Find(L"photo");
            if (!sizes || sizes->type != json::Value::Type::Arr)
            {
                continue;
            }
            int64_t bestArea = -1;
            for (const auto& size : sizes->arr)
            {
                if (size.type != json::Value::Type::Obj || size.StrAt(L"file_id").empty())
                {
                    continue;
                }
                const int64_t w = size.I64At(L"width", 0);
                const int64_t h = size.I64At(L"height", 0);
                if (w * h > bestArea)
                {
                    bestArea = w * h;
                    shown.fileId = size.StrAt(L"file_id");
                    shown.uniqueId = size.StrAt(L"file_unique_id");
                    shown.width = static_cast<int>(w);
                    shown.height = static_cast<int>(h);
                }
            }
            if (!shown.fileId.empty())
            {
                return shown; // the first photo block is the card's picture (the card has no other)
            }
        }
        return shown;
    }

    /// <summary>
    /// How many refusals of a card WITH a picture in a row switch the pictures off for the rest of the run: a refusal is retried
    /// once without the picture (the card's text must go), and three in a row mean the chat or this Bot API will not take them -
    /// the cards then go as text, and a log line says so. A picture that goes through resets the count.
    /// </summary>
    inline constexpr int kCardPictureRefusalsToStop = 3;

    /// <summary>
    /// The longest a card post or edit waits for its picture - the hosting window's screen read plus the drawing - before it goes
    /// with the picture it shows (or none). A window answers in well under a second (the read itself is 0.3-0.6 ms, the drawing
    /// ~90-185 ms), but a FROZEN window (a UI stall) would hold its copy of the request unanswered: without this bound the card,
    /// whose new text is already recorded, would not change for as long as the freeze lasts - and that is when the phone is
    /// needed most. Past it the late picture is dropped (the card's send runs once). The send loop checks the deadlines
    /// (MessagingService::_expireCardPictureWaits, every pass: at least every 0.5 s). Since 2026-11-01 a step that misses the
    /// bound owes the card a LOOK (CardPictureRead::Stale), so a late window delays the picture instead of losing it.
    /// </summary>
    inline constexpr int64_t kCardPictureWaitMs = 4 * 1000;

    // ===== The picture follows the status: the card's LOOKS at its screen (2026-11-01) =====

    /// <summary>
    /// How long after a change of the session's state into a state AT REST (anything but Running) the card looks at its screen
    /// once more - the SETTLE look. The status update reads the screen at the moment it learns of the change, and that moment
    /// can come before Claude Code drew the new state: a Stop hook's line reaches the engine while the hook still runs, and Claude
    /// Code ends the turn (and draws its idle prompt) only once its Stop hooks exited - one may block the stop - so the turn's
    /// spinner can still be on the screen read at that line. A card at rest is never edited again on its own
    /// (its text names clock times, never spans), so without this look that early picture would stay. 3 s covers the hook's
    /// exit and Claude Code's next frame with room to spare; a later look only costs latency.
    /// </summary>
    inline constexpr int64_t kCardPictureSettleMs = 3 * 1000;

    /// <summary>
    /// The longest a LOOK waits for the hosting window's answer before it counts as Stale. Much longer than kCardPictureWaitMs:
    /// a look carries no text change, so nothing on the card waits for it, and a busy window that answers in 9 s (measured
    /// 2026-11-01) must still bring its picture. Bounded so a frozen window cannot stop the looks for good: past it the next look
    /// is scheduled (CardPictureLookDelayMs) and asks again.
    /// </summary>
    inline constexpr int64_t kCardPictureLookWaitMs = 30 * 1000;

    /// <summary>The pause before the first look after a read that got no screen in time; each further miss doubles it.</summary>
    inline constexpr int64_t kCardPictureLookFirstDelayMs = 5 * 1000;

    /// <summary>The longest pause between two looks (the doubling stops here).</summary>
    inline constexpr int64_t kCardPictureLookMaxDelayMs = 60 * 1000;

    /// <summary>
    /// How many looks in a row may get no screen in time before the card stops looking until its next status update: 8 looks
    /// spread over ~9 minutes (each waits up to kCardPictureLookWaitMs; the pauses before them 5, 10, 20, 40, then 60 s). A window
    /// that answers nothing for that long is hung or gone, and without a cap it would be asked every minute forever - each ask
    /// one more item on a dispatcher queue that is not being served.
    /// </summary>
    inline constexpr int kCardPictureLookTriesMax = 8;

    /// <summary>
    /// The pause before the next look after `tries` looks in a row got no screen in time: kCardPictureLookFirstDelayMs doubled
    /// `tries` times, at most kCardPictureLookMaxDelayMs (0 tries = 5 s, then 10, 20, 40, 60, 60, ...). Pure.
    /// </summary>
    /// <param name="tries">The looks in a row that missed so far (0 for the first look after a status update's own miss);
    /// a negative count reads as 0.</param>
    /// <returns>The pause in ms, 5,000 to 60,000.</returns>
    inline int64_t CardPictureLookDelayMs(int tries) noexcept
    {
        int64_t delay = kCardPictureLookFirstDelayMs;
        // Doubling until the cap, never past it: the loop ends at the cap whatever `tries` says, so no count can overflow it.
        for (int i = 0; i < tries && delay < kCardPictureLookMaxDelayMs; ++i)
        {
            delay *= 2;
        }
        return delay < kCardPictureLookMaxDelayMs ? delay : kCardPictureLookMaxDelayMs;
    }

    /// <summary>
    /// The earlier of a look already scheduled and a new one, so a rule that wants a look soon never pushes an earlier one back.
    /// </summary>
    /// <param name="scheduledAtMs">The look scheduled now (0 = none).</param>
    /// <param name="atMs">The moment the new rule wants a look.</param>
    /// <returns>The moment of the next look.</returns>
    inline int64_t EarlierCardPictureLook(int64_t scheduledAtMs, int64_t atMs) noexcept
    {
        return scheduledAtMs != 0 && scheduledAtMs < atMs ? scheduledAtMs : atMs;
    }

    /// <summary>
    /// A card's LOOKS at its screen: what MessagingService::CardState keeps between the card's posts and edits so that a status
    /// update's picture that missed is taken again - a window that answered too late, a screen read before Claude Code drew the
    /// new state, a page flip that overtook the update's read or put an older picture back. Plain data, changed only by the rules
    /// below (pure, so the m5 suite pins them); the service holds it under `_mtx`. The generation of the card's newest post or
    /// edit itself stays CardState::pictureSeq.
    /// </summary>
    /// <remarks>
    /// A look never overtakes a post or edit: it starts only while none waits for its picture (stepPending), and a post or edit
    /// decided while it runs wins - the look's result is dropped (lookBase), because the newer one carries its own read and an
    /// older text must never be edited back over it.
    /// </remarks>
    struct CardPictureTrack
    {
        bool stepPending{ false }; // the card's newest post or edit WITH a picture step waits for its picture (its send has not run)
        bool stepReads{ false }; // ...and that step reads the screen (a status update), rather than keeping the picture (a page flip)
        uint32_t handedShotHash{ 0 }; // the screen hash of the picture the newest post or edit carried (0 = none): what the card
                                      // shows once the outbox delivered it - a look's basis (PlanCardPicture's unchangedShotHash)
        int64_t lookAtMs{ 0 }; // when the card looks at its screen again (WallMs; 0 = no look owed)
        int lookTries{ 0 }; // looks in a row that got no screen in time (the backoff; every status update starts it over)
        bool lookPending{ false }; // a look's read or drawing runs (one look at a time)
        uint64_t lookSeq{ 0 }; // the running look's generation (service-wide unique, like CardState::pictureSeq)
        uint64_t lookBase{ 0 }; // CardState::pictureSeq when that look started: a post or edit decided since makes it stale
    };

    /// <summary>
    /// Records a post or edit WITH a picture step that was just decided - the caller stamped CardState::pictureSeq for it under
    /// the same lock. A step that READS the screen (a status update) starts the look backoff over, and a change of the state
    /// schedules the settle look (kCardPictureSettleMs) when the new state is at rest, or cancels a look still owed when the
    /// session runs (a running card is edited with a fresh read whenever its text changes). A step that only KEEPS the picture (a
    /// page flip) and overtakes a read that has not settled - a status update's or a look's - owes the card the look that read
    /// was: the overtaken read's picture is dropped with it. Pure.
    /// </summary>
    /// <param name="t">The card's looks.</param>
    /// <param name="reads">The step reads the screen (a status update) rather than keeping the picture it shows.</param>
    /// <param name="stateChanged">The session's state differs from the one the card showed last (CardState::lastState).</param>
    /// <param name="atRest">The new state is not Running (Waiting, Idle, Needs approval, Error, Done).</param>
    /// <param name="nowMs">Now (WallMs).</param>
    inline void CardPictureStepDecided(CardPictureTrack& t, bool reads, bool stateChanged, bool atRest, int64_t nowMs) noexcept
    {
        if (!reads && (t.lookPending || (t.stepPending && t.stepReads)))
        {
            // The page flip goes at once with the picture the card shows, and the read it overtook is dropped when it settles: look
            // again once this step settled, so the screen the dropped read was for still reaches the card.
            t.lookAtMs = EarlierCardPictureLook(t.lookAtMs, nowMs + kCardPictureSettleMs);
        }
        t.stepPending = true;
        t.stepReads = reads;
        if (reads)
        {
            t.lookTries = 0;
            if (stateChanged)
            {
                t.lookAtMs = atRest ? nowMs + kCardPictureSettleMs : 0;
            }
        }
    }

    /// <summary>
    /// Records how the card's NEWEST post or edit settled its picture (the caller checked that the step is still the newest:
    /// CardState::pictureSeq) - the picture it carries is now what the card shows, or will once the outbox delivered it. A step
    /// that got no screen in time (Stale) owes the card a look after CardPictureLookDelayMs; a step that kept an OLDER picture
    /// than the one handed before (a page flip whose stored picture predates an upload still on its way, or that lands after
    /// it) owes one after kCardPictureSettleMs. Pure.
    /// </summary>
    /// <param name="t">The card's looks.</param>
    /// <param name="handedShotHash">The screen hash of the picture the step carries (0 = it goes without one).</param>
    /// <param name="read">How the step's picture settled.</param>
    /// <param name="nowMs">Now (WallMs).</param>
    inline void CardPictureStepSettled(CardPictureTrack& t, uint32_t handedShotHash, CardPictureRead read, int64_t nowMs) noexcept
    {
        t.stepPending = false;
        const uint32_t before = t.handedShotHash;
        t.handedShotHash = handedShotHash;
        if (read == CardPictureRead::Stale)
        {
            t.lookAtMs = EarlierCardPictureLook(t.lookAtMs, nowMs + CardPictureLookDelayMs(t.lookTries));
        }
        else if (read == CardPictureRead::Kept && before != 0 && handedShotHash != before)
        {
            t.lookAtMs = EarlierCardPictureLook(t.lookAtMs, nowMs + kCardPictureSettleMs);
        }
    }

    /// <summary>
    /// Whether the card should look at its screen now: a look is owed and due, no post or edit of the card waits for its picture
    /// (a look never overtakes one), and no other look runs. The caller also requires the card to show its current text and
    /// buttons (MessagingService::_refreshCard asks only then), so the look's edit changes the picture alone. Pure.
    /// </summary>
    /// <param name="t">The card's looks.</param>
    /// <param name="nowMs">Now (WallMs).</param>
    /// <returns>True when the look should start.</returns>
    inline bool CardPictureLookDue(const CardPictureTrack& t, int64_t nowMs) noexcept
    {
        return t.lookAtMs != 0 && nowMs >= t.lookAtMs && !t.stepPending && !t.lookPending;
    }

    /// <summary>Records a look that starts (CardPictureLookDue said so): one at a time, owed no more, stale once a post or edit is decided.</summary>
    /// <param name="t">The card's looks.</param>
    /// <param name="lookSeq">The look's generation (service-wide unique).</param>
    /// <param name="pictureSeq">CardState::pictureSeq now: the newest post or edit the look builds on.</param>
    inline void CardPictureLookStarted(CardPictureTrack& t, uint64_t lookSeq, uint64_t pictureSeq) noexcept
    {
        t.lookPending = true;
        t.lookAtMs = 0;
        t.lookSeq = lookSeq;
        t.lookBase = pictureSeq;
    }

    /// <summary>What a card's look does once its read settled (CardPictureLookSettled).</summary>
    enum class CardPictureLookStep
    {
        Dropped, // a post or edit was decided while the look ran (it carries its own picture), or this is not the card's look any more
        Send, // the screen changed: edit the card - its same text and buttons - with the new picture
        Done, // nothing to send: the screen is the one the card carries, or the tab shows none
        Again, // no screen in time: the next look is scheduled (CardPictureTrack::lookAtMs)
        GiveUp, // no screen in time for kCardPictureLookTriesMax looks in a row: no more looks until the next status update
    };

    /// <summary>
    /// Records how a look settled and says what to do with it. A look that a post or edit overtook (CardState::pictureSeq moved
    /// since it started) is dropped. A changed screen (Fresh, with a picture) is sent, and the picture it carries becomes the next
    /// look's basis at once; an unchanged screen or none ends the looks; a look that got no screen in time schedules the next one
    /// after CardPictureLookDelayMs - until kCardPictureLookTriesMax of them missed in a row. Pure.
    /// </summary>
    /// <param name="t">The card's looks.</param>
    /// <param name="lookSeq">The look's generation (CardPictureLookStarted's).</param>
    /// <param name="pictureSeq">CardState::pictureSeq now.</param>
    /// <param name="hasPicture">The look's read brought a picture (a drawing, or the held photo of the screen read now).</param>
    /// <param name="shotHash">That picture's screen hash.</param>
    /// <param name="read">How the look's read settled.</param>
    /// <param name="nowMs">Now (WallMs).</param>
    /// <returns>What the look does.</returns>
    inline CardPictureLookStep CardPictureLookSettled(CardPictureTrack& t, uint64_t lookSeq, uint64_t pictureSeq, bool hasPicture, uint32_t shotHash, CardPictureRead read, int64_t nowMs) noexcept
    {
        if (!t.lookPending || t.lookSeq != lookSeq)
        {
            return CardPictureLookStep::Dropped; // not the look that runs (the card state was made anew since it started)
        }
        t.lookPending = false;
        if (pictureSeq != t.lookBase)
        {
            // A post or edit decided meanwhile owns the card now; it scheduled whatever look it owes (CardPictureStepDecided).
            return CardPictureLookStep::Dropped;
        }
        if (read == CardPictureRead::Stale)
        {
            if (++t.lookTries >= kCardPictureLookTriesMax)
            {
                return CardPictureLookStep::GiveUp;
            }
            t.lookAtMs = nowMs + CardPictureLookDelayMs(t.lookTries);
            return CardPictureLookStep::Again;
        }
        t.lookTries = 0;
        if (read == CardPictureRead::Fresh && hasPicture)
        {
            // The edit is queued right after this: a look (or a page flip's regression check) from now on compares with it.
            t.handedShotHash = shotHash;
            return CardPictureLookStep::Send;
        }
        return CardPictureLookStep::Done;
    }
}
