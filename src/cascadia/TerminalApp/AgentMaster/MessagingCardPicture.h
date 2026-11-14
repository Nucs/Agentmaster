// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the sticky card's SCREENSHOT (MESSAGING.md §21.42), taken only when the card's 🖼️ is tapped (§21.49). The
// user's request (2026-10-27): "the running since message, called also i think status message that is pinned to the
// bottom - whenever it updates, have it produce a screenshot (if possible) and have it shown and so every update we produce a
// screenshot to show in the status message".
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
// WHEN the screen is read changed with §21.49. Until then every update of the card's text read the hosting window's screen
// (a changed one drawn and uploaded in the card's own edit or post), and since 2026-11-01 the card also LOOKED at its screen
// again - 3 s after a change into a state at rest, with a backoff after a read that came too late, and every 1.5 s while a
// starting tab's claude drew (the launch watch). Production paid for it: the 6-hour flood hold the owner's chat sat in (a 429
// asking for 21,890 s, every method parked) came after 2.5 hours of 58-166 calls per 10 minutes, nearly all card edits
// uploading a new picture (43-128 drawn per 10 minutes), and the windows left ~35,000 screen reads unanswered within the
// 4 s and 30 s bounds, worst right after the restarts. The user's call (2026-11-10): "I think we abuse sending screenshots.
// to the right of Session button, add a picture emoji button [...] When user clicks on that emoji, it updates the bottom pinned
// tab with a picture like we do now automatically" - and the picture stays until the next tap.
//
// So the screen is read for a 🖼️ tap (MessagingService::_cardPictureFromTap) and - since §21.55, the user's request of 2026-11-14
// ("I want the bottom pinned messages to have a picture when the turn finishes [...] once a turn is done or there's an error or if
// there is an approval required [...] Otherwise we just keep it") - by itself after the state edges the user looks at: a turn's
// end, an Error, a Needs approval (MessagingService::_noteCardPictureEdge). Those are rare next to the old cadence - production's
// 21.6 days before the change saw 0.5 such edges per 10 minutes on average and 17 at most, against the 43-128 uploads per 10
// minutes that preceded the flood hold - and a chat-wide gap between two such reads still bounds a burst. Either way the hosting
// window reads the screen once; a screen that changed since the card's picture is drawn on the worker and uploaded by the card's
// next post or edit; an unchanged one reuses the photo Telegram holds, captioned with the moment it was read again. Every other
// post and edit of the card CARRIES the picture it has (CardPictureToCarry: by its file_id - no upload, no read), since a rich
// edit without it would drop the photo; a card that never had one goes as text until its first tap or edge.
//
// This header is the pure half: the media id, the picture as a message carries it, the plan (upload, reuse, none), the picture
// a post or edit carries, how a read settled (CardPictureRead), the tap's bounds and its toasts, the rich HTML block, the media
// list, reading the photo back from Telegram's answer, the caption's clock, and §21.55's edge rules (which edges take a picture,
// whether the session still shows that state, the settle, the chat-wide gap, how long the card's ring waits for its picture).
// The m5 suite pins every rule (tests_messaging_card.cpp, section 1d).

#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>

#include "Json.h"
#include "ScreenSnapshot.h" // ScreenSnapshotShowsNothing: a blank screen (a tab whose claude is still starting) is never drawn
#include "SessionModels.h" // SessionState: §21.55's edges are read from the session's state changes

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
    /// The picture one card post or edit carries: either a NEW one to upload (`png`: a 🖼️ tap's drawing) or one Telegram already
    /// holds (`fileId`, reused with no upload), plus what it shows - the moment its screen was read and that screen's hash.
    /// Shared by the outbox items that carry it (a refused send goes again without it) and, while a tap's picture waits for the
    /// send that delivers it, by the card's state (MessagingService::CardState::tapPicture, which a send's answer matches by
    /// IDENTITY - the same object). Never changed once made.
    /// </summary>
    struct CardPicture
    {
        std::string png; // the PNG to upload ("" = reuse fileId); a picture is ~0.1-0.6 MB (§21.24's measures)
        std::wstring fileId; // Telegram's file_id of a photo it holds; used only when png is empty
        int64_t shotAtMs{ 0 }; // when the screen this picture shows was read (Unix ms): the caption's clock
        uint32_t shotHash{ 0 }; // CardPictureShotHash of that screen: a tap on an unchanged screen reuses the picture
    };

    /// <summary>
    /// The identity of a screen snapshot (ScreenSnapshot.h's serialized form, as the window answers it): FNV-1a 32 over its
    /// UTF-16 code units, never 0 (0 = "no picture recorded"). A collision only makes one tap reuse the previous picture: the
    /// next tap on a changed screen draws a new one.
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

    /// <summary>What a screen read - a 🖼️ tap's, or a state edge's (§21.55) - does about the card's picture (PlanCardPicture).</summary>
    enum class CardPictureSource
    {
        None, // no picture: no screen could be read and the card never had a picture
        Reuse, // the picture Telegram already holds, by its file_id (no upload)
        Upload, // a new picture: the screen is drawn and uploaded with the card's next edit
    };

    /// <summary>
    /// How a screen read settled - a 🖼️ tap's, or a state edge's (§21.55) - what the read's continuation learns besides the
    /// picture itself (MessagingService::_withCardPicture hands both over). Only Fresh brings a picture for the card; every other
    /// outcome picks the tap's toast (CardPictureTapReadText) - an edge's read has no tap and only logs it (CardPictureReadWord) -
    /// so "the window did not answer" is never told as "there is no screen to read".
    /// </summary>
    enum class CardPictureRead
    {
        Fresh, // the hosting window answered in time and the screen was read: the picture (drawn, or the held photo of that very
               // screen) shows this moment
        NoScreen, // the hosting window answered, but its tab shows no screen: a parked tab, or a terminal not up yet
        Stale, // no picture in time: the window did not answer within kCardPictureTapWaitMs (a busy or frozen UI thread), no window
               // answered (none hosts the tab), or the read or the drawing failed
        Blank, // the hosting window answered with a screen that shows NOTHING (ScreenSnapshotShowsNothing: a tab whose claude is
               // still starting) - never drawn: a black picture would replace the card's last real one
    };

    /// <summary>The decision for one 🖼️ tap's read: where its picture comes from, and the moment its caption names.</summary>
    struct CardPicturePlan
    {
        CardPictureSource source{ CardPictureSource::None };
        int64_t shotAtMs{ 0 }; // the caption's moment: now for a screen read now (drawn, or found unchanged), else the stored picture's
    };

    /// <summary>
    /// Plans a 🖼️ tap's picture from what was read and what the card shows: a screen read NOW that differs from the stored
    /// picture's (or with no stored picture) is drawn and uploaded; a screen read now that is the stored picture's own reuses
    /// it, captioned with NOW (the screen was looked at and is still that); no screen (a parked tab, no window, a read that
    /// failed) keeps the stored picture with its OWN moment ("the screen at 19:50" stays true), or none at all. Pure.
    /// </summary>
    /// <param name="readable">The window answered with a screen (a non-empty snapshot that shows something).</param>
    /// <param name="shotHash">That screen's CardPictureShotHash (ignored when not readable).</param>
    /// <param name="nowMs">Now (Unix ms).</param>
    /// <param name="storedFileId">The file_id of the picture the card last showed ("" = none).</param>
    /// <param name="storedShotHash">That picture's screen hash (0 = unknown: never reused for a read screen).</param>
    /// <param name="storedAtMs">The moment that picture's screen was read.</param>
    /// <returns>The plan.</returns>
    inline CardPicturePlan PlanCardPicture(bool readable, uint32_t shotHash, int64_t nowMs, std::wstring_view storedFileId, uint32_t storedShotHash, int64_t storedAtMs) noexcept
    {
        CardPicturePlan plan;
        if (readable)
        {
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
    /// The picture a card post or edit carries (§21.49): the picture a 🖼️ tap - or, since §21.55, a state edge - took, while it
    /// waits for the send that delivers it; else the photo the card shows, reused by its file_id (no upload, no screen read) and
    /// captioned with ITS moment; else none. Every post and edit of a card that has a picture carries one, because a rich edit
    /// without it drops the photo (measured, §21.42) - so a status update, a page flip or a re-post below newer messages keeps the
    /// picture until the next tap or edge. Pure.
    /// </summary>
    /// <param name="tapPicture">The tap's (or edge's) picture waiting for its send (null = none).</param>
    /// <param name="storedFileId">The file_id of the photo the card shows (TopicRecord::cardPictureFileId; "" = none).</param>
    /// <param name="storedAtMs">The moment that photo's screen was read (its caption's clock).</param>
    /// <param name="storedShotHash">That screen's hash (kept with the photo, so the next tap reuses it when the screen is the same).</param>
    /// <returns>The picture to carry: `tapPicture` itself (the same object, so the send's answer can match it), a held picture, or
    /// null when the card has none.</returns>
    /// <exception cref="std::bad_alloc">If the held picture cannot be allocated.</exception>
    inline std::shared_ptr<const CardPicture> CardPictureToCarry(const std::shared_ptr<const CardPicture>& tapPicture, std::wstring_view storedFileId, int64_t storedAtMs, uint32_t storedShotHash)
    {
        if (tapPicture)
        {
            return tapPicture;
        }
        if (storedFileId.empty())
        {
            return nullptr;
        }
        auto held = std::make_shared<CardPicture>();
        held->fileId = std::wstring{ storedFileId };
        held->shotAtMs = storedAtMs;
        held->shotHash = storedShotHash;
        return held;
    }

    /// <summary>
    /// A moment as a LOCAL clock time with seconds, for the picture's caption: "19:50:12" on the same local day as `nowMs`,
    /// else "16 Oct 19:50:12". With seconds because two taps of one minute must not read alike. The caption is outside the card's
    /// hash (MessagingService::_refreshCard hashes the classic text and the buttons), so a new picture's caption never causes an
    /// edit by itself.
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
    /// the cards then go as text, a 🖼️ tap says why (CardPictureTapRefusal::PicturesStopped), and a log line says so. A picture
    /// that goes through resets the count.
    /// </summary>
    inline constexpr int kCardPictureRefusalsToStop = 3;

    // ===== The 🖼️ tap (§21.49) ==================================================================

    /// <summary>
    /// The longest a 🖼️ tap waits for its picture - the hosting window's screen read plus the drawing - before it settles as
    /// Stale. Longer than the tap's own hold (kTapAnswerHoldMaxMs, 9 s, MessagingTaps.h) on purpose: a busy window answered 5-9 s
    /// late in production (2026-11-01, the Agent Manager page's rebuilds), and past the hold the tap's answer says the work goes
    /// on (kTapStillWorkingToast) while the picture still reaches the card when it comes. Bounded so a frozen window's read is
    /// forgotten: a picture later than this is dropped (one log line), and the next tap asks again. The send loop checks the
    /// deadline every pass (MessagingService::_expireCardPictureWaits).
    /// </summary>
    inline constexpr int64_t kCardPictureTapWaitMs = 30 * 1000;

    /// <summary>
    /// The longest a card counts as BUSY with a 🖼️ tap - its read, drawing and the card edit that carries the picture - so a
    /// second tap meanwhile is answered "already taking it" instead of reading and uploading the same screen twice. A tap's work
    /// clears it as soon as it ends (no picture, or the carrying send answered); the bound is the belt for a send that is never
    /// answered (an item the outbox dropped), which would otherwise keep the button refusing until the next start.
    /// </summary>
    inline constexpr int64_t kCardPictureTapBusyMs = 60 * 1000;

    /// <summary>Whether a card is still busy with a 🖼️ tap that began at `sinceTickMs` (kCardPictureTapBusyMs). Pure.</summary>
    /// <param name="sinceTickMs">When the tap's read began (GetTickCount64 ms; 0 = no tap's work runs).</param>
    /// <param name="nowTickMs">Now, on the same monotonic clock.</param>
    /// <returns>True while a tap's work may still run.</returns>
    inline bool CardPictureTapBusy(int64_t sinceTickMs, int64_t nowTickMs) noexcept
    {
        return sinceTickMs != 0 && nowTickMs - sinceTickMs < kCardPictureTapBusyMs;
    }

    /// <summary>Why a 🖼️ tap asks no window for the screen (MessagingService::_cardPictureFromTap's own checks, in this order).</summary>
    enum class CardPictureTapRefusal
    {
        Closed, // the session is closed: its tab is gone, nothing shows its screen
        PicturesOff, // the cog's "Show the screen in each session's card" is off
        PicturesStopped, // Telegram refused kCardPictureRefusalsToStop card pictures in a row: none for the rest of this run
        NoRich, // the cards go as classic HTML (rich messages off, or a Bot API without them): no classic message holds a picture
        Unavailable, // no window can read screens (a host without the window hooks: the m5 harness)
        Parked, // the tab is parked (its claude not started): it shows nothing until ▶ Reactivate starts it
        NoCard, // the session has no topic, or its topic no card, to put the picture on
        Busy, // a tap's read, drawing or delivery for this card runs already (kCardPictureTapBusyMs at most)
    };

    // The PicturesStopped toast names the count in words of its own (a string_view constant cannot be built from the number).
    static_assert(kCardPictureRefusalsToStop == 3, "CardPictureTapRefusalText says \"3 card pictures in a row\"");

    /// <summary>The toast of a 🖼️ tap that asks no window (CardPictureTapRefusal): why, and what to do. Pure.</summary>
    /// <param name="why">The refusal.</param>
    /// <returns>The toast (a static string; within kTapToastMaxChars).</returns>
    inline std::wstring_view CardPictureTapRefusalText(CardPictureTapRefusal why) noexcept
    {
        switch (why)
        {
        case CardPictureTapRefusal::Closed:
            return L"This session is closed. Resume it from the \U0001F50E Search topic.";
        case CardPictureTapRefusal::PicturesOff:
            return L"The cards' screen pictures are off (the cog's Messaging tab: \x201CShow the screen in each session's card\x201D).";
        case CardPictureTapRefusal::PicturesStopped:
            return L"Telegram refused 3 card pictures in a row, so the cards go without them until Agentmaster starts again.";
        case CardPictureTapRefusal::NoRich:
            return L"The cards go as plain HTML (rich messages are off), and a plain card cannot hold a picture.";
        case CardPictureTapRefusal::Parked:
            return L"This tab is parked: its screen shows nothing until \x25B6 Reactivate starts it.";
        case CardPictureTapRefusal::NoCard:
            return L"This session has no card to put the picture on.";
        case CardPictureTapRefusal::Busy:
            return L"Taking the screen already \x2014 it shows on the card in a moment.";
        case CardPictureTapRefusal::Unavailable:
        default:
            return L"Not available here.";
        }
    }

    /// <summary>A refusal's word in hooks.log's "[nav] remote telegram cardpic" line. Pure.</summary>
    /// <param name="why">The refusal.</param>
    /// <returns>The word (a static string).</returns>
    inline const wchar_t* CardPictureTapRefusalWord(CardPictureTapRefusal why) noexcept
    {
        switch (why)
        {
        case CardPictureTapRefusal::Closed:
            return L"closed";
        case CardPictureTapRefusal::PicturesOff:
            return L"pictures-off";
        case CardPictureTapRefusal::PicturesStopped:
            return L"pictures-stopped";
        case CardPictureTapRefusal::NoRich:
            return L"classic-cards";
        case CardPictureTapRefusal::Parked:
            return L"parked";
        case CardPictureTapRefusal::NoCard:
            return L"no-card";
        case CardPictureTapRefusal::Busy:
            return L"busy";
        case CardPictureTapRefusal::Unavailable:
        default:
            return L"unavailable";
        }
    }

    /// <summary>
    /// The toast of a 🖼️ tap whose read brought no picture: why, and what to do. "" for Fresh - the picture appearing on the card
    /// is the answer (the tapped button keeps spinning until the edit that carries it is in, MessagingTaps.h). Pure.
    /// </summary>
    /// <param name="read">How the read settled.</param>
    /// <param name="tabStarted">The session's tab runs its claude (SessionInfo live and started): NoScreen then means the terminal
    /// is not up yet; otherwise the tab is parked.</param>
    /// <returns>The toast (a static string; "" = none).</returns>
    inline std::wstring_view CardPictureTapReadText(CardPictureRead read, bool tabStarted) noexcept
    {
        switch (read)
        {
        case CardPictureRead::NoScreen:
            return tabStarted ? std::wstring_view{ L"The tab's screen is not up yet \x2014 tap \U0001F5BC\xFE0F again in a moment." } :
                                CardPictureTapRefusalText(CardPictureTapRefusal::Parked);
        case CardPictureRead::Blank:
            return L"Claude is still starting: its screen shows nothing yet \x2014 tap \U0001F5BC\xFE0F again in a moment.";
        case CardPictureRead::Stale:
            return L"No picture this time: the tab's window did not answer, or the screen could not be drawn \x2014 tap \U0001F5BC\xFE0F again.";
        case CardPictureRead::Fresh:
        default:
            return {};
        }
    }

    /// <summary>A read's word in hooks.log's "[nav] remote telegram cardpic" line ("fresh", "noscreen", "stale", "blank"). Pure.</summary>
    /// <param name="read">How the read settled.</param>
    /// <returns>The word (a static string).</returns>
    inline const wchar_t* CardPictureReadWord(CardPictureRead read) noexcept
    {
        switch (read)
        {
        case CardPictureRead::NoScreen:
            return L"noscreen";
        case CardPictureRead::Stale:
            return L"stale";
        case CardPictureRead::Blank:
            return L"blank";
        case CardPictureRead::Fresh:
        default:
            return L"fresh";
        }
    }

    /// <summary>
    /// The toast after the send that carried a 🖼️ tap's picture was answered: "" when the card shows it; else why it does not -
    /// Telegram refused the picture (the card's text went again without it), or the call failed. The caller leaves out a card
    /// the user deleted ("message to edit not found": the card posted in its place carries the picture). Pure.
    /// </summary>
    /// <param name="answered">Telegram answered ok, or "message is not modified" (the card shows this very content already).</param>
    /// <param name="photoShown">The card shows the photo: the answer's message has one (CardPictureFromMessage), or it was not
    /// modified.</param>
    /// <param name="httpStatus">The call's HTTP status (0 = no answer at all: a transport failure).</param>
    /// <returns>The toast ("" = none).</returns>
    /// <exception cref="std::bad_alloc">If the text cannot be allocated.</exception>
    inline std::wstring CardPictureDeliveryText(bool answered, bool photoShown, int httpStatus)
    {
        if (answered && photoShown)
        {
            return {};
        }
        if (answered)
        {
            return L"Telegram refused the picture; the card's text went on without it.";
        }
        if (httpStatus <= 0)
        {
            return L"The picture did not reach Telegram (no answer) \x2014 tap \U0001F5BC\xFE0F again.";
        }
        return L"The picture did not reach Telegram (HTTP " + std::to_wstring(httpStatus) + L") \x2014 tap \U0001F5BC\xFE0F again.";
    }

    // ===== The picture after a state edge (§21.55) ====================================================

    /// <summary>
    /// The state edges after which a card takes its picture BY ITSELF (§21.55, the user's request of 2026-11-14: "every individual
    /// session once a turn is done or there's an error or if there is an approval required [...] we should bring a picture").
    /// CardPictureEdgeFor names an edge; every other edge leaves the card's picture as it is ("Otherwise we just keep it").
    /// </summary>
    enum class CardPictureEdge
    {
        None, // no picture: a new turn, an approval answered, claude exiting, a decay, a restart's restore
        TurnEnded, // the turn's work ended: Running (or its dialog, Needs approval) -> Waiting for you / Idle
        Error, // the turn ended in an Error: Running or Needs approval -> Error
        NeedsApproval, // the turn waits for the user: Running -> Needs approval (a permission prompt, a question)
    };

    /// <summary>
    /// The edge a state change is, for the card's picture (§21.55). Only an edge OUT OF A TURN'S WORK counts - from Running, or
    /// from Needs approval (a dialog the user denied or interrupted, which ends the turn: 28 + 7 such edges in production's 21.6
    /// days before the change) - into a turn's end (Waiting for you, Idle), an Error, or (from Running) a Needs approval. Never
    /// from Idle, Waiting for you, Error or Done: those edges are a restart's restore (the scanner re-asserting Error on a session
    /// that read Idle at the start - 101 "idle -> error" in the same days, in bursts after each start), a decay (Waiting for you ->
    /// Idle) or a session resumed onto an old question, none of them a moment the user asked to see. Done (claude exited) was not
    /// asked for. Pure.
    /// </summary>
    /// <param name="from">The state the service last saw (the mirror's lastState).</param>
    /// <param name="to">The state now.</param>
    /// <returns>The edge; None for every change that takes no picture (and for no change).</returns>
    inline CardPictureEdge CardPictureEdgeFor(SessionState from, SessionState to) noexcept
    {
        if (from == to)
        {
            return CardPictureEdge::None;
        }
        const bool fromRunning = from == SessionState::Running;
        if (!fromRunning && from != SessionState::NeedsApproval)
        {
            return CardPictureEdge::None;
        }
        switch (to)
        {
        case SessionState::WaitingForInput:
        case SessionState::Idle:
            return CardPictureEdge::TurnEnded;
        case SessionState::Error:
            return CardPictureEdge::Error;
        case SessionState::NeedsApproval:
            return fromRunning ? CardPictureEdge::NeedsApproval : CardPictureEdge::None;
        default:
            return CardPictureEdge::None; // Running (an approval answered: the turn goes on), Done (claude exited: not asked for)
        }
    }

    /// <summary>
    /// Whether a session still shows what its edge's picture is for, read just before the screen is (§21.55): a turn's end while
    /// the session rests (Waiting for you, or Idle after its decay), an Error or a Needs approval while it lasts. A session that
    /// moved on during the settle - a new turn, an approval answered at the PC, an error the automatic restart healed - gets no
    /// picture of a screen that no longer shows that state. Pure.
    /// </summary>
    /// <param name="edge">The edge the picture is owed for.</param>
    /// <param name="now">The session's state now.</param>
    /// <returns>True when the screen still belongs to the edge; false for None.</returns>
    inline bool CardPictureEdgeStillHolds(CardPictureEdge edge, SessionState now) noexcept
    {
        switch (edge)
        {
        case CardPictureEdge::TurnEnded:
            return now == SessionState::WaitingForInput || now == SessionState::Idle;
        case CardPictureEdge::Error:
            return now == SessionState::Error;
        case CardPictureEdge::NeedsApproval:
            return now == SessionState::NeedsApproval;
        case CardPictureEdge::None:
        default:
            return false;
        }
    }

    /// <summary>An edge's word in hooks.log's "[msg] <sid8> card picture after …" lines and in the census ("turn-end", "error",
    /// "approval"; "none"). Pure.</summary>
    /// <param name="edge">The edge.</param>
    /// <returns>The word (a static string, no space).</returns>
    inline const wchar_t* CardPictureEdgeWord(CardPictureEdge edge) noexcept
    {
        switch (edge)
        {
        case CardPictureEdge::TurnEnded:
            return L"turn-end";
        case CardPictureEdge::Error:
            return L"error";
        case CardPictureEdge::NeedsApproval:
            return L"approval";
        case CardPictureEdge::None:
        default:
            return L"none";
        }
    }

    /// <summary>An edge as hooks.log's lines name the picture taken for it ("card picture after a turn's end: …"): a static string,
    /// so the read's wait can keep it as a view (MessagingService::PendingCardPicture::what). Pure.</summary>
    /// <param name="edge">The edge.</param>
    /// <returns>"after a turn's end", "after an error", "after an approval request" ("after a state change" for None).</returns>
    inline const wchar_t* CardPictureEdgePhrase(CardPictureEdge edge) noexcept
    {
        switch (edge)
        {
        case CardPictureEdge::TurnEnded:
            return L"after a turn's end";
        case CardPictureEdge::Error:
            return L"after an error";
        case CardPictureEdge::NeedsApproval:
            return L"after an approval request";
        case CardPictureEdge::None:
        default:
            return L"after a state change";
        }
    }

    /// <summary>
    /// How long after an edge its screen is read (§21.55): Claude Code ends a turn - and draws its idle prompt - only once its Stop
    /// hooks exited, while the hook's line reaches the engine as the hook runs, so a read at the edge itself could still show the
    /// turn's spinner (§21.42, 2026-11-01: the reason its settle look waited this long). 3 s covers Agentmaster's own hooks and the
    /// usual user hooks, and leaves the read and the drawing (~0.3-0.6 ms + ~90-185 ms) well inside the ring's wait
    /// (kCardRingWaitMs) and the card's re-post below the turn's reply (kCardStickyLagMs, 6 s after the reply).
    /// </summary>
    inline constexpr int64_t kCardPictureSettleMs = 3 * 1000;

    /// <summary>
    /// The longest a card's RING - its re-post WITH SOUND for a Needs approval or an Error the topic was not told about (§21.6) -
    /// waits for its edge's picture, counted from the edge (§21.55: the user's pick "Ring with the picture", 2026-11-14, so opening
    /// the notification shows the dialog's own screen). The settle plus a read and a drawing take ~3.5 s; a window that answers
    /// later (a busy UI thread answered 5-9 s late on 2026-11-01) lets the ring go without it, and the picture follows in an edit
    /// of its own. A read that brings no picture ends the wait at once.
    /// </summary>
    inline constexpr int64_t kCardRingWaitMs = 6 * 1000;

    /// <summary>
    /// The chat-wide gap between two edges' screen reads (§21.55): one per 10 s at most, so a burst of turn ends across the fleet
    /// never turns into a burst of uploads - the cadence that earned the 6-hour flood hold (§21.49.1). Measured before the change
    /// (production, 21.6 days): 6 edges in the busiest minute, at most 3 in any other, so the gap delays a picture by tens of
    /// seconds only in those rare minutes. A card whose ring waits goes first.
    /// </summary>
    inline constexpr int64_t kCardPictureEdgeGapMs = 10 * 1000;

    /// <summary>
    /// The longest an edge's picture waits for its read to START past its settle (the chat-wide gap, Telegram's flood pause, a 🖼️
    /// tap's read on the same card) before it is dropped (§21.55): a minute late, the user has likely looked already, and the
    /// card's 🖼️ takes a fresh one. The read itself is bounded by kCardPictureTapWaitMs, like a tap's.
    /// </summary>
    inline constexpr int64_t kCardPictureEdgeMaxWaitMs = 60 * 1000;

    /// <summary>
    /// The longest an edge's picture, once drawn, waits for the card's re-post that is coming anyway - the card sits above newer
    /// messages (the turn's reply, a question card), and it moves below them kCardStickyLagMs after they went quiet - instead of an
    /// edit of its own (§21.55: the picture rides a send that happens anyway). A topic whose messages keep coming past this bound
    /// gets the picture in its own edit.
    /// </summary>
    inline constexpr int64_t kCardEdgePictureRepostWaitMs = 15 * 1000;

    /// <summary>Whether an owed edge picture may start its screen read now (§21.55): its settle is over, the chat-wide gap since the
    /// last edge's read has passed, and Telegram is not holding the chat in a flood pause (a picture is the least urgent call there
    /// is). Pure.</summary>
    /// <param name="dueTickMs">When the settle ends (GetTickCount64 ms; 0 = no picture owed).</param>
    /// <param name="lastEdgeReadTickMs">When the last edge's read started, chat-wide (0 = none this run).</param>
    /// <param name="nowTickMs">Now, on the same monotonic clock.</param>
    /// <param name="floodPaused">A 429's pause holds, or the card edit method is parked.</param>
    /// <returns>True when the read may start.</returns>
    inline bool CardEdgePictureMayStart(int64_t dueTickMs, int64_t lastEdgeReadTickMs, int64_t nowTickMs, bool floodPaused) noexcept
    {
        return dueTickMs != 0 && nowTickMs >= dueTickMs && !floodPaused && (lastEdgeReadTickMs == 0 || nowTickMs - lastEdgeReadTickMs >= kCardPictureEdgeGapMs);
    }

    /// <summary>Whether an owed edge picture is dropped unread: kCardPictureEdgeMaxWaitMs past its due moment (§21.55). Pure.</summary>
    /// <param name="dueTickMs">When its settle ended (GetTickCount64 ms; 0 = none owed: never expired).</param>
    /// <param name="nowTickMs">Now, on the same monotonic clock.</param>
    /// <returns>True when the picture is to be dropped.</returns>
    inline bool CardEdgePictureExpired(int64_t dueTickMs, int64_t nowTickMs) noexcept
    {
        return dueTickMs != 0 && nowTickMs - dueTickMs >= kCardPictureEdgeMaxWaitMs;
    }

    /// <summary>Whether a card's ring (its re-post with sound) still waits for its edge's picture (§21.55): a wait was set at the
    /// edge, no picture is ready yet, and the wait has not run out (kCardRingWaitMs). Pure.</summary>
    /// <param name="ringWaitsUntilTickMs">The wait's end (GetTickCount64 ms; 0 = the ring waits for nothing).</param>
    /// <param name="nowTickMs">Now, on the same monotonic clock.</param>
    /// <param name="pictureReady">A new picture waits on the card for its send (the edge's - or a 🖼️ tap's, which serves as well).</param>
    /// <returns>True while the ring is held.</returns>
    inline bool CardRingWaitsForPicture(int64_t ringWaitsUntilTickMs, int64_t nowTickMs, bool pictureReady) noexcept
    {
        return ringWaitsUntilTickMs != 0 && !pictureReady && nowTickMs < ringWaitsUntilTickMs;
    }

    /// <summary>Whether an edge's drawn picture leaves its delivery to the card's coming re-post instead of an edit of its own
    /// (§21.55): newer messages sit below the card (it moves below them once they are quiet), and the picture has waited less than
    /// kCardEdgePictureRepostWaitMs. A 🖼️ tap's picture never waits: the user asked for it now. Pure.</summary>
    /// <param name="repostComing">The card has a message id and newer messages were posted after it (no history replay holds it).</param>
    /// <param name="readyTickMs">When the picture was drawn (GetTickCount64 ms; 0 = unknown: no wait).</param>
    /// <param name="nowTickMs">Now, on the same monotonic clock.</param>
    /// <returns>True when the picture waits for the re-post.</returns>
    inline bool CardEdgePictureWaitsForRepost(bool repostComing, int64_t readyTickMs, int64_t nowTickMs) noexcept
    {
        return repostComing && readyTickMs != 0 && nowTickMs - readyTickMs < kCardEdgePictureRepostWaitMs;
    }
}
