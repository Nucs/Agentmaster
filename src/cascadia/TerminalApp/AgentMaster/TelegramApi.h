// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — TelegramApi: the ONE HTTPS client of the Telegram connector (MESSAGING.md §10).
//
// It moves bytes and nothing else: a Bot API method name + a JSON object in, Telegram's answer out
// (ok / result / description / retry_after / the HTTP status). Every rule — who may talk to the bot,
// what a message means, pacing — lives in the service (Messaging.cpp), never here. WinHTTP, like
// Updater.h's HttpsGet, but POST with a JSON body (every Bot API method takes one), the error body
// READ (a 429's `retry_after` and a 400's `description` are in it; HttpsGet discards non-2xx
// bodies), and a request that another thread can ABORT (the long poll blocks up to a minute, and
// Stop() must not wait for it).
//
// The token never appears in a log line or an error text: it is part of every request URL, and
// Telegram's own error texts never echo it, so this class is the only place that holds it.
//
// Plain C++ + Win32 (no WinRT, no PCH); links winhttp through its #pragma in the .cpp. The m5
// harness compiles it (a LINK dependency of Messaging.cpp) but never calls the network.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "Json.h"

namespace Agentmaster::Msg
{
    // One Bot API answer.
    struct TgResult
    {
        bool ok{ false }; // Telegram's `ok` (false also for a transport failure)
        int httpStatus{ 0 }; // the HTTP status (0 when no response came back)
        int retryAfterSec{ 0 }; // a 429's `parameters.retry_after`, else 0
        bool transportFailed{ false }; // no HTTP response at all (DNS, TLS, timeout, aborted)
        std::wstring description; // Telegram's `description` on an error, or the transport error's note
        json::Value result; // Telegram's `result` on success (Null otherwise)

        // Telegram's "message is not modified" (an edit that changes nothing): harmless, not a failure.
        bool NotModified() const noexcept
        {
            return !ok && httpStatus == 400 && description.find(L"message is not modified") != std::wstring::npos;
        }
        /// <summary>
        /// Telegram's "TOPIC_NOT_MODIFIED": an editForumTopic that asked for the name and icon the topic shows already. The topic
        /// is as wanted, so the caller treats it as done (MESSAGING.md §21.29: the rename's change line is still told). It is not
        /// NotModified(), whose "message is not modified" Telegram answers only to a MESSAGE edit.
        /// </summary>
        /// <returns>True for that answer; false for every other one, a success included.</returns>
        bool TopicNotModified() const noexcept
        {
            return !ok && httpStatus == 400 && description.find(L"TOPIC_NOT_MODIFIED") != std::wstring::npos;
        }
        // Telegram's "message thread not found": the user deleted the topic (or it never existed here).
        bool ThreadNotFound() const noexcept
        {
            return !ok && httpStatus == 400 && (description.find(L"thread not found") != std::wstring::npos || description.find(L"TOPIC_ID_INVALID") != std::wstring::npos);
        }
    };

    class TelegramApi
    {
    public:
        /// <summary>Creates a client for one bot. Opens nothing until the first call.</summary>
        /// <param name="token">The bot token from @BotFather ("123456:ABC-…"). Kept in memory only.</param>
        /// <exception cref="std::bad_alloc">If the token cannot be copied.</exception>
        explicit TelegramApi(std::wstring token);
        ~TelegramApi();
        TelegramApi(const TelegramApi&) = delete;
        TelegramApi& operator=(const TelegramApi&) = delete;

        /// <summary>
        /// Calls one Bot API method with a JSON body (POST, `application/json`). Blocks for the
        /// round trip; call it on a worker thread only (H1).
        /// </summary>
        /// <param name="method">The method name ("sendMessage", "getUpdates", …).</param>
        /// <param name="params">The parameters as a JSON object (an empty object for none).</param>
        /// <param name="timeoutMs">The receive timeout; a long poll passes its poll timeout plus a margin.</param>
        /// <returns>The answer; `ok` false with a description on any failure. Never throws.</returns>
        /// <remarks>
        /// A failure is logged to messaging.log as `[tg] <method> …` with the status and Telegram's
        /// description — never the body or the token. A 429 returns retryAfterSec for the caller's pacing.
        /// </remarks>
        TgResult Call(std::wstring_view method, const json::Value& params, uint32_t timeoutMs = 30000) noexcept;

        /// <summary>
        /// Uploads a file with `sendDocument` (multipart/form-data): a transcript as a .md file, a long
        /// output. Blocks like Call.
        /// </summary>
        /// <param name="params">The other parameters (chat_id, message_thread_id, caption, disable_notification);
        /// each is sent as its own form field (numbers and booleans as text).</param>
        /// <param name="fileName">The file name Telegram shows (UTF-8 in the form; keep it ASCII-safe).</param>
        /// <param name="bytes">The file's bytes (Telegram's limit is 50 MB; the caller caps).</param>
        /// <param name="timeoutMs">The receive timeout (uploads are slower than calls).</param>
        /// <returns>The answer, as Call. Never throws.</returns>
        TgResult SendDocument(const json::Value& params, std::wstring_view fileName, const std::string& bytes, uint32_t timeoutMs = 60000) noexcept;

        /// <summary>
        /// Uploads a picture with `sendPhoto` (multipart/form-data, the field "photo"): the Telegram card's 🖼 Screen Image
        /// (MESSAGING.md §21.24). The apps show a photo inline and zoomable, where a document is a file to tap open; Telegram
        /// recompresses a photo, and refuses one past its photo limits (10 MB; width + height at most 10,000 pixels; one side
        /// at most 20 times the other) with a 400 — the caller then sends the same bytes as a document. Blocks like Call.
        /// </summary>
        /// <param name="params">The other parameters (chat_id, message_thread_id, caption, disable_notification), each sent as its
        /// own form field, as SendDocument.</param>
        /// <param name="fileName">The file name in the form (UTF-8; keep it ASCII-safe).</param>
        /// <param name="bytes">The picture's bytes (a PNG here).</param>
        /// <param name="timeoutMs">The receive timeout.</param>
        /// <returns>The answer, as Call. Never throws.</returns>
        TgResult SendPhoto(const json::Value& params, std::wstring_view fileName, const std::string& bytes, uint32_t timeoutMs = 60000) noexcept;
        /// <summary>
        /// Calls ANY method with one file attached (multipart/form-data): MESSAGING.md §21.42's card picture, a rich message
        /// (sendRichMessage / editMessageText) whose `rich_message.media` names the file as "attach://&lt;field&gt;". Every
        /// parameter goes as its own form field - objects and arrays (rich_message, reply_markup) as their JSON - and the file as
        /// the part named `field`. Blocks like Call; the send timeout grows with the body (_request).
        /// </summary>
        /// <param name="method">The Bot API method ("sendRichMessage", "editMessageText").</param>
        /// <param name="params">The other parameters, each a form field.</param>
        /// <param name="field">The file part's name: the attach name the parameters refer to ("screen" for "attach://screen").</param>
        /// <param name="fileContentType">The file part's Content-Type ("image/png").</param>
        /// <param name="fileName">The file name in the form (UTF-8; keep it ASCII-safe).</param>
        /// <param name="bytes">The file's bytes.</param>
        /// <param name="timeoutMs">The receive timeout (Telegram processes a photo before it answers: ~1.3 s for 4 MB, measured).</param>
        /// <returns>The answer, as Call. Never throws.</returns>
        TgResult CallWithFile(std::wstring_view method, const json::Value& params, std::string_view field, std::string_view fileContentType, std::wstring_view fileName, const std::string& bytes, uint32_t timeoutMs = 60000) noexcept;

        /// <summary>
        /// Downloads a file the bot received (MESSAGING.md §21.43, VOICE.md §6: a voice note): a GET of
        /// https://api.telegram.org/file/bot&lt;token&gt;/&lt;filePath&gt;, the path getFile named. Blocks like Call; call it on a
        /// worker thread only. Binary-safe: the bytes come back as they are.
        /// </summary>
        /// <param name="filePath">The `file_path` of getFile's answer ("voice/file_12.oga").</param>
        /// <param name="maxBytes">The most bytes accepted (the Bot API serves files up to 20 MB); a larger body is a failure.</param>
        /// <param name="out">Receives the bytes (cleared first).</param>
        /// <param name="timeoutMs">The receive timeout.</param>
        /// <returns>`ok` with httpStatus 200 on success (`result` stays Null); a failure says why. Never throws; the URL holds the
        /// token, so no failure text ever contains the URL.</returns>
        TgResult DownloadFile(std::wstring_view filePath, size_t maxBytes, std::string& out, uint32_t timeoutMs = 60000) noexcept;

        /// <summary>
        /// Ends the request in flight, if any, from ANOTHER thread: the blocked call returns with a
        /// transport failure. The long poll (getUpdates with a 50 s timeout) is the one call that needs
        /// it: Stop() must not wait a minute. Safe to call with nothing in flight. Never throws.
        /// </summary>
        void Abort() noexcept;

        /// <summary>Masks a token for a status line: "7221…bZ8" (the first 4 and last 3 characters).</summary>
        /// <param name="token">The token.</param>
        /// <returns>The masked text; "" for an empty token.</returns>
        /// <exception cref="std::bad_alloc">If the result cannot be allocated.</exception>
        static std::wstring MaskToken(std::wstring_view token);

    private:
        TgResult _request(std::wstring_view method, const std::string& body, const std::wstring& contentType, uint32_t timeoutMs) noexcept;
        /// <summary>
        /// The one multipart upload behind SendDocument, SendPhoto and CallWithFile (§21.24 split it out of SendDocument): every
        /// parameter as its own text field, then the file as the part named `field` with `fileContentType`, under a boundary no
        /// file can hold.
        /// </summary>
        /// <param name="method">The Bot API method ("sendDocument", "sendPhoto"; §21.42: "sendRichMessage", "editMessageText").</param>
        /// <param name="field">The file's form field ("document", "photo"; §21.42: the attach name "screen").</param>
        /// <param name="fileContentType">The file part's Content-Type ("application/octet-stream", "image/png").</param>
        /// <param name="params">The other parameters (each a text field).</param>
        /// <param name="fileName">The file name in the form (quotes, backslashes and line breaks become '_').</param>
        /// <param name="bytes">The file's bytes.</param>
        /// <param name="timeoutMs">The receive timeout.</param>
        /// <returns>The answer, as Call. Never throws: a failure while building the body is logged (Rule #18) and returned as a
        /// transport failure.</returns>
        TgResult _upload(std::wstring_view method, std::string_view field, std::string_view fileContentType, const json::Value& params, std::wstring_view fileName, const std::string& bytes, uint32_t timeoutMs) noexcept;

        std::wstring _token;
        std::mutex _mtx; // guards _inflight
        void* _inflight{ nullptr }; // the HINTERNET of the request in flight (Abort closes it)
        bool _aborted{ false }; // set by Abort while a request is in flight, so its failure is reported as such
    };
}
